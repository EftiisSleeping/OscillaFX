#include "ui/crt/mac/MetalCrtView.h"
#include "ui/crt/mac/CrtShaders.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>
#include <atomic>

// Manual retain/release (JUCE builds Objective-C++ without ARC).

@interface OscillaCrtMetalView : NSView
@end

@implementation OscillaCrtMetalView
- (BOOL) wantsUpdateLayer         { return YES; }
- (BOOL) isOpaque                 { return YES; }
- (CALayer*) makeBackingLayer     { return [CAMetalLayer layer]; }
- (NSView*) hitTest: (NSPoint) p  { juce::ignoreUnused (p); return nil; } // clicks go to the JUCE component
@end

namespace
{
    // ---- mirrors of the uniform structs in CrtShaders.cpp ----
    struct BeamGpu           { simd_float2 a, b; float density, sigma; };
    struct BeamUniforms      { simd_float2 viewSize, scale; float energy, unused; };
    struct PictureUniforms
    {
        simd_float4 galleryRect;
        simd_float2 drift, viewSize;
        float keep, pictureMix, overlayLevel, galleryGain;
        float cellSize, tear, tearY, tearHeight;
        float tearSeed, time, hasGallery, hasOverlay;
        float saturation, pad0, pad1, pad2;
    };
    struct CompositeUniforms
    {
        simd_float2 viewSize, collapse;
        float cornerRadius, brightness, bloom, afterglow;
        float degauss, time, illumination, faceGlow;
        float pxScale, scanPeriod, lineWidth, hintLevel;
        simd_float4 grid;
        float hasHint, pad0, pad1, pad2;
    };

    // Two frames in flight against three drawables: one is always free for nextDrawable, so it does
    // not block the message thread.
    constexpr int framesInFlight = 2;
    constexpr int64_t frameWaitNs = 4'000'000;      // give up on a frame rather than stall the UI
    constexpr int maxFailedFrames = 3;              // consecutive GPU errors before falling back to CPU
    constexpr int bloomLevels = 2;   // 1/4 and 1/8 resolution: a tight halo, not a haze
    constexpr float galleryRowsPerScreen = 92.0f;   // phosphor dot rows of a gallery picture

    template <typename T>
    void release (T& object)
    {
        [object release];
        object = nil;
    }

    NSString* toNSString (const char* utf8)
    {
        NSString* s = [NSString stringWithUTF8String: utf8];
        return s != nil ? s : @"";
    }

    void log (const juce::String& message)
    {
        juce::Logger::writeToLog ("CRT (Metal): " + message);
    }
}

struct MetalCrtView::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> picturePipe = nil, beamPipe = nil, quarterPipe = nil, downPipe = nil, upPipe = nil, compositePipe = nil;
    OscillaCrtMetalView* view = nil;
    CAMetalLayer* layer = nil;

    id<MTLTexture> phosphor[2] = { nil, nil };
    id<MTLTexture> bloomDown[bloomLevels] = {};
    id<MTLTexture> bloomUp[bloomLevels - 1] = {};
    id<MTLTexture> hint = nil, overlay = nil, gallery = nil, blank = nil;
    int hintVersion = -1, overlayVersion = -1, galleryId = -1;
    int current = 0;
    CGSize size = CGSizeZero;
    bool hasContent = false;

    id<MTLBuffer> beamBuffers[framesInFlight] = {};
    dispatch_semaphore_t inFlight = dispatch_semaphore_create (framesInFlight);
    int frameSlot = 0;
    CompositeUniforms lastComposite {};

    // Written from Metal's threads, read on the message thread.
    std::shared_ptr<std::atomic<int>> failedFrames = std::make_shared<std::atomic<int>> (0);
    std::shared_ptr<std::atomic<bool>> deviceLost = std::make_shared<std::atomic<bool>> (false);
    id<NSObject> deviceObserver = nil;

    ~Impl()
    {
        if (deviceObserver != nil)
        {
            JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations") // still needed on Intel Macs
            MTLRemoveDeviceObserver (deviceObserver);
            JUCE_END_IGNORE_WARNINGS_GCC_LIKE
            release (deviceObserver);
        }

        // let in-flight frames finish before their resources go away
        for (int i = 0; i < framesInFlight; ++i)
            dispatch_semaphore_wait (inFlight, dispatch_time (DISPATCH_TIME_NOW, NSEC_PER_SEC));
        for (int i = 0; i < framesInFlight; ++i)
            dispatch_semaphore_signal (inFlight);
        dispatch_release (inFlight);

        releaseTargets();
        release (hint); release (overlay); release (gallery); release (blank);
        for (auto& b : beamBuffers) release (b);
        release (picturePipe); release (beamPipe); release (quarterPipe); release (downPipe); release (upPipe); release (compositePipe);
        release (queue); release (view); release (device);
    }

    void releaseTargets()
    {
        for (auto& t : phosphor) release (t);
        for (auto& t : bloomDown) release (t);
        for (auto& t : bloomUp) release (t);
    }

    //==========================================================================================
    // Prefer the integrated GPU on dual-GPU Macs so the CRT never keeps the discrete one awake;
    // watch for the device going away (eGPU unplugged) so the caller can fall back to the CPU.
    id<MTLDevice> chooseDevice()
    {
        auto lost = deviceLost;
        __block id<MTLDevice> chosen = nil;
        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations") // device notifications matter on Intel Macs
        NSArray<id<MTLDevice>>* all = MTLCopyAllDevicesWithObserver (&deviceObserver, ^(id<MTLDevice> removed, MTLDeviceNotificationName name)
        {
            if (removed == chosen && name != MTLDeviceWasAddedNotification)
                lost->store (true);
        });
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE

        for (id<MTLDevice> candidate in all)
            if (candidate.isLowPower && ! candidate.isRemovable)
                chosen = candidate;
        if (chosen == nil)
            chosen = MTLCreateSystemDefaultDevice();
        else
            [chosen retain];

        [deviceObserver retain];
        [all release];
        return chosen;
    }

    bool build()
    {
        device = chooseDevice();
        if (device == nil)
            return fail ("no Metal device");
        log (juce::String ("using ") + [[device name] UTF8String]);
        queue = [device newCommandQueue];

        NSError* error = nil;
        const auto source = Crt::metalShaderSource();
        id<MTLLibrary> library = [device newLibraryWithSource: toNSString (source.c_str()) options: nil error: &error];
        if (library == nil)
            return fail ("shader compile failed: " + juce::String ([[error localizedDescription] UTF8String]));

        picturePipe = makePipeline (library, "fullscreenVertex", "pictureFragment", MTLPixelFormatR16Float, false);
        beamPipe = makePipeline (library, "beamVertex", "beamFragment", MTLPixelFormatR16Float, true);
        quarterPipe = makePipeline (library, "fullscreenVertex", "bloomQuarterFragment", MTLPixelFormatR16Float, false);
        downPipe = makePipeline (library, "fullscreenVertex", "bloomDownFragment", MTLPixelFormatR16Float, false);
        upPipe = makePipeline (library, "fullscreenVertex", "bloomUpFragment", MTLPixelFormatR16Float, false);
        compositePipe = makePipeline (library, "fullscreenVertex", "compositeFragment", MTLPixelFormatBGRA8Unorm, false);
        [library release];
        if (picturePipe == nil || beamPipe == nil || quarterPipe == nil || downPipe == nil || upPipe == nil || compositePipe == nil)
            return false;

        const juce::uint8 zero = 0;
        blank = makeTexture (1, 1, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead, cpuVisibleStorage());
        [blank replaceRegion: MTLRegionMake2D (0, 0, 1, 1) mipmapLevel: 0 withBytes: &zero bytesPerRow: 1];

        view = [[OscillaCrtMetalView alloc] initWithFrame: NSMakeRect (0, 0, 16, 16)];
        [view setWantsLayer: YES];
        layer = (CAMetalLayer*) [view layer];
        if (! [layer isKindOfClass: [CAMetalLayer class]])
            return fail ("view is not layer-backed by CAMetalLayer");
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        layer.maximumDrawableCount = 3;
        layer.allowsNextDrawableTimeout = YES;
        return true;
    }

    bool fail (const juce::String& why)
    {
        log (why);
        return false;
    }

    id<MTLRenderPipelineState> makePipeline (id<MTLLibrary> library, const char* vertexName, const char* fragmentName,
                                             MTLPixelFormat format, bool additive)
    {
        id<MTLFunction> vertexFn = [library newFunctionWithName: toNSString (vertexName)];
        id<MTLFunction> fragmentFn = [library newFunctionWithName: toNSString (fragmentName)];
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vertexFn;
        desc.fragmentFunction = fragmentFn;
        desc.colorAttachments[0].pixelFormat = format;
        if (additive)
        {
            auto* attachment = desc.colorAttachments[0];
            attachment.blendingEnabled = YES;
            attachment.rgbBlendOperation = MTLBlendOperationAdd;
            attachment.alphaBlendOperation = MTLBlendOperationAdd;
            attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
            attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
            attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
            attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
        }

        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor: desc error: &error];
        if (state == nil)
            log (juce::String ("pipeline ") + fragmentName + " failed: " + [[error localizedDescription] UTF8String]);
        [desc release];
        [vertexFn release];
        [fragmentFn release];
        return state;
    }

    // Textures the CPU writes or reads: shared on Apple GPUs, managed everywhere else (Intel/AMD
    // GPUs do not support shared textures, even with unified memory).
    bool isAppleGpu() const { return [device supportsFamily: MTLGPUFamilyApple1]; }

    MTLStorageMode cpuVisibleStorage() const
    {
        if (isAppleGpu())
            return MTLStorageModeShared;
        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
        return MTLStorageModeManaged;
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE
    }

    void synchronise (id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture) const
    {
        if (isAppleGpu())
            return;
        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
        [blit synchronizeResource: texture];
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE
    }

    id<MTLTexture> makeTexture (int w, int h, MTLPixelFormat format, MTLTextureUsage usage, MTLStorageMode storage)
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat: format
                                                                                         width: (NSUInteger) juce::jmax (1, w)
                                                                                        height: (NSUInteger) juce::jmax (1, h)
                                                                                     mipmapped: NO];
        desc.usage = usage;
        desc.storageMode = storage;
        return [device newTextureWithDescriptor: desc];
    }

    //==========================================================================================
    double backingScale() const { return view.window != nil ? view.window.backingScaleFactor : 2.0; }

    CGSize expectedSize() const
    {
        const auto scale = backingScale();
        const auto bounds = view.bounds.size;
        return CGSizeMake (std::round (bounds.width * scale), std::round (bounds.height * scale));
    }

    bool updateSize()
    {
        const auto drawable = expectedSize();
        if (drawable.width < 4 || drawable.height < 4)
            return false;
        if (CGSizeEqualToSize (drawable, size))
            return true;

        size = drawable;
        layer.contentsScale = backingScale();
        layer.drawableSize = drawable;
        createTargets ((int) drawable.width, (int) drawable.height);
        return true;
    }

    void createTargets (int w, int h)
    {
        releaseTargets();
        const auto rw = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        for (auto& t : phosphor)
            t = makeTexture (w, h, MTLPixelFormatR16Float, rw, MTLStorageModePrivate);
        for (int i = 0; i < bloomLevels; ++i)
            bloomDown[i] = makeTexture (w >> (i + 2), h >> (i + 2), MTLPixelFormatR16Float, rw, MTLStorageModePrivate);
        for (int i = 0; i < bloomLevels - 1; ++i)
            bloomUp[i] = makeTexture (w >> (i + 2), h >> (i + 2), MTLPixelFormatR16Float, rw, MTLStorageModePrivate);

        // start from a dark screen
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        for (auto& t : phosphor)
            [[cmd renderCommandEncoderWithDescriptor: passFor (t, MTLLoadActionClear)] endEncoding];
        [cmd commit];
        hasContent = false;
    }

    static MTLRenderPassDescriptor* passFor (id<MTLTexture> target, MTLLoadAction load)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = load;
        pass.colorAttachments[0].clearColor = MTLClearColorMake (0, 0, 0, 1);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        return pass;
    }

    //==========================================================================================
    // Uploads a CPU image (SingleChannel) into a fresh R8 texture when its version changed. Never
    // rewrites a texture in place: frames still in flight may be reading the old one (they retain it).
    void syncImage (id<MTLTexture>& texture, int& version, const Crt::Overlay& source)
    {
        if (source.image == nullptr || ! source.image->isValid())
        {
            version = -1;
            return;
        }
        if (source.version == version && texture != nil)
            return;

        const auto& image = *source.image;
        release (texture);
        texture = makeTexture (image.getWidth(), image.getHeight(), MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead, cpuVisibleStorage());

        const juce::Image::BitmapData px (image, juce::Image::BitmapData::readOnly);
        jassert (px.pixelStride == 1);
        [texture replaceRegion: MTLRegionMake2D (0, 0, (NSUInteger) px.width, (NSUInteger) px.height)
                   mipmapLevel: 0 withBytes: px.data bytesPerRow: (NSUInteger) px.lineStride];
        version = source.version;
    }

    void syncGallery (const Crt::LuminanceImage* picture)
    {
        if (picture == nullptr || picture->id == galleryId)
            return;
        release (gallery);
        gallery = makeTexture (picture->width, picture->height, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead, cpuVisibleStorage());
        [gallery replaceRegion: MTLRegionMake2D (0, 0, (NSUInteger) picture->width, (NSUInteger) picture->height)
                   mipmapLevel: 0 withBytes: picture->pixels.data() bytesPerRow: (NSUInteger) picture->width];
        galleryId = picture->id;
    }

    id<MTLBuffer> beamBufferFor (const std::vector<Crt::BeamSegment>& beams)
    {
        const auto bytes = juce::jmax ((size_t) 1, beams.size()) * sizeof (BeamGpu);
        auto& buffer = beamBuffers[frameSlot];
        if (buffer == nil || buffer.length < bytes)
        {
            release (buffer);
            buffer = [device newBufferWithLength: bytes * 2 options: MTLResourceStorageModeShared];
        }
        auto* out = static_cast<BeamGpu*> (buffer.contents);
        for (const auto& b : beams)
            *out++ = { simd_make_float2 (b.a.x, b.a.y), simd_make_float2 (b.b.x, b.b.y), b.density, b.sigma };
        return buffer;
    }

    //==========================================================================================
    bool render (const Crt::Scene& scene, simd_float2 scale)
    {
        if (dispatch_semaphore_wait (inFlight, dispatch_time (DISPATCH_TIME_NOW, frameWaitNs)) != 0)
            return false;   // the GPU is behind: drop this frame
        frameSlot = (frameSlot + 1) % framesInFlight;

        syncImage (hint, hintVersion, scene.hint);
        syncImage (overlay, overlayVersion, scene.overlay);
        syncGallery (scene.gallery.current);

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        const auto next = 1 - current;
        encodePhosphor (cmd, scene, scale, phosphor[current], phosphor[next]);
        current = next;
        encodeBloom (cmd);

        lastComposite = compositeUniforms (scene, scale);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable != nil)
        {
            encodeComposite (cmd, drawable.texture, lastComposite);
            [cmd presentDrawable: drawable];
        }

        dispatch_semaphore_t semaphore = inFlight;
        auto failures = failedFrames;
        [cmd addCompletedHandler: ^(id<MTLCommandBuffer> done)
        {
            if (done.status == MTLCommandBufferStatusError)
                failures->fetch_add (1);
            else
                failures->store (0);
            dispatch_semaphore_signal (semaphore);
        }];
        [cmd commit];
        hasContent = true;
        return true;
    }

    bool hasFailed() const
    {
        return deviceLost->load() || failedFrames->load() >= maxFailedFrames;
    }

    void encodePhosphor (id<MTLCommandBuffer> cmd, const Crt::Scene& scene, simd_float2 scale, id<MTLTexture> from, id<MTLTexture> to)
    {
        const auto viewSize = simd_make_float2 ((float) size.width, (float) size.height);
        const auto& g = scene.gallery;
        const auto hasGallery = g.current != nullptr && gallery != nil && g.current->id == galleryId;
        const auto hasOverlay = overlayVersion >= 0 && overlay != nil
                                && (int) overlay.width == (int) size.width && (int) overlay.height == (int) size.height;

        PictureUniforms pu {};
        pu.galleryRect = simd_make_float4 (g.area.getX() * scale.x, g.area.getY() * scale.y, g.area.getWidth() * scale.x, g.area.getHeight() * scale.y);
        pu.drift = simd_make_float2 (g.drift.x * scale.x, g.drift.y * scale.y);
        pu.viewSize = viewSize;
        pu.keep = std::exp (-scene.dt / juce::jmax (0.001f, scene.persistence));
        pu.pictureMix = 1.0f - std::exp (-scene.dt / juce::jmax (0.001f, scene.pictureResponse));
        pu.overlayLevel = scene.overlay.level;
        pu.galleryGain = g.gain;
        pu.cellSize = juce::jmax (2.0f, (float) size.height / galleryRowsPerScreen);
        pu.tear = g.tear;
        pu.tearY = g.tearY;
        pu.tearHeight = g.tearHeight;
        pu.tearSeed = g.tearSeed;
        pu.time = (float) std::fmod (scene.time, 1000.0);
        pu.hasGallery = hasGallery ? 1.0f : 0.0f;
        pu.hasOverlay = hasOverlay ? 1.0f : 0.0f;
        pu.saturation = scene.saturation;

        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor: passFor (to, MTLLoadActionDontCare)];
        [enc setRenderPipelineState: picturePipe];
        [enc setFragmentBytes: &pu length: sizeof (pu) atIndex: 0];
        [enc setFragmentTexture: from atIndex: 0];
        [enc setFragmentTexture: hasOverlay ? overlay : blank atIndex: 1];
        [enc setFragmentTexture: hasGallery ? gallery : blank atIndex: 2];
        [enc drawPrimitives: MTLPrimitiveTypeTriangle vertexStart: 0 vertexCount: 3];

        if (! scene.beams.empty())
        {
            // A beam's density is the energy a line drawn every frame settles at: each frame deposits
            // the fraction the phosphor loses in that frame, so brightness does not depend on the
            // persistence or the frame rate.
            BeamUniforms bu { viewSize, scale, scene.brightness * juce::jlimit (0.04f, 1.0f, 1.0f - pu.keep), 0.0f };
            [enc setRenderPipelineState: beamPipe];
            [enc setVertexBuffer: beamBufferFor (scene.beams) offset: 0 atIndex: 0];
            [enc setVertexBytes: &bu length: sizeof (bu) atIndex: 1];
            [enc drawPrimitives: MTLPrimitiveTypeTriangleStrip vertexStart: 0 vertexCount: 4 instanceCount: scene.beams.size()];
        }
        [enc endEncoding];
    }

    void encodeBloom (id<MTLCommandBuffer> cmd)
    {
        auto pass = [&] (id<MTLRenderPipelineState> pipe, id<MTLTexture> target, id<MTLTexture> a, id<MTLTexture> b)
        {
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor: passFor (target, MTLLoadActionDontCare)];
            [enc setRenderPipelineState: pipe];
            [enc setFragmentTexture: a atIndex: 0];
            if (b != nil)
                [enc setFragmentTexture: b atIndex: 1];
            [enc drawPrimitives: MTLPrimitiveTypeTriangle vertexStart: 0 vertexCount: 3];
            [enc endEncoding];
        };

        pass (quarterPipe, bloomDown[0], phosphor[current], nil);
        for (int i = 1; i < bloomLevels; ++i)
            pass (downPipe, bloomDown[i], bloomDown[i - 1], nil);

        pass (upPipe, bloomUp[bloomLevels - 2], bloomDown[bloomLevels - 1], bloomDown[bloomLevels - 2]);
        for (int i = bloomLevels - 3; i >= 0; --i)
            pass (upPipe, bloomUp[i], bloomUp[i + 1], bloomDown[i]);
    }

    CompositeUniforms compositeUniforms (const Crt::Scene& scene, simd_float2 scale) const
    {
        const auto hasHint = hintVersion >= 0 && hint != nil && scene.hint.level > 0.0f
                             && (int) hint.width == (int) size.width && (int) hint.height == (int) size.height;
        CompositeUniforms cu {};
        cu.viewSize = simd_make_float2 ((float) size.width, (float) size.height);
        cu.collapse = simd_make_float2 (juce::jmax (0.002f, scene.collapseX), juce::jmax (0.002f, scene.collapseY));
        cu.cornerRadius = scene.cornerRadius * scale.x;
        cu.brightness = scene.brightness;
        cu.bloom = scene.bloom;
        cu.afterglow = scene.afterglow;
        cu.degauss = scene.reducedMotion ? 0.0f : scene.degauss;
        cu.time = (float) std::fmod (scene.time, 1000.0);
        cu.illumination = scene.scaleIllumination;
        cu.faceGlow = scene.faceGlow;
        cu.pxScale = scale.x;
        cu.scanPeriod = juce::jmax (2.0f, std::round (2.0f * scale.x));
        cu.lineWidth = juce::jmax (1.0f, std::round (0.75f * scale.x));
        cu.hintLevel = scene.hint.level;
        cu.grid = simd_make_float4 (scene.graticule.getX() * scale.x, scene.graticule.getY() * scale.y,
                                    scene.graticule.getWidth() * scale.x, scene.graticule.getHeight() * scale.y);
        cu.hasHint = hasHint ? 1.0f : 0.0f;
        return cu;
    }

    void encodeComposite (id<MTLCommandBuffer> cmd, id<MTLTexture> target, const CompositeUniforms& cu)
    {
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor: passFor (target, MTLLoadActionDontCare)];
        [enc setRenderPipelineState: compositePipe];
        [enc setFragmentBytes: &cu length: sizeof (cu) atIndex: 0];
        [enc setFragmentTexture: phosphor[current] atIndex: 0];
        [enc setFragmentTexture: bloomUp[0] atIndex: 1];
        [enc setFragmentTexture: hintVersion >= 0 ? hint : blank atIndex: 2];
        [enc drawPrimitives: MTLPrimitiveTypeTriangle vertexStart: 0 vertexCount: 3];
        [enc endEncoding];
    }

    //==========================================================================================
    juce::Image capture()
    {
        if (! hasContent)
            return {};

        const auto w = (int) size.width, h = (int) size.height;
        id<MTLTexture> target = makeTexture (w, h, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget, cpuVisibleStorage());
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        encodeComposite (cmd, target, lastComposite);
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        synchronise (blit, target);
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        juce::Image image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            const juce::Image::BitmapData px (image, juce::Image::BitmapData::writeOnly);
            [target getBytes: px.data bytesPerRow: (NSUInteger) px.lineStride
                  fromRegion: MTLRegionMake2D (0, 0, (NSUInteger) w, (NSUInteger) h) mipmapLevel: 0];
        }
        [target release];
        return image;
    }
};

//==============================================================================================
std::unique_ptr<MetalCrtView> MetalCrtView::create()
{
    if (juce::SystemStats::getEnvironmentVariable ("OSCILLA_CRT_CPU", {}).isNotEmpty())
    {
        log ("disabled by OSCILLA_CRT_CPU, using the CPU renderer");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    @autoreleasepool
    {
        if (! impl->build())
        {
            log ("unavailable, using the CPU renderer");
            return nullptr;
        }
    }
    return std::unique_ptr<MetalCrtView> (new MetalCrtView (std::move (impl)));
}

MetalCrtView::MetalCrtView (std::unique_ptr<Impl> i) : impl (std::move (i))
{
    setInterceptsMouseClicks (false, false);
    setView (impl->view);
}

MetalCrtView::~MetalCrtView()
{
    setView (nullptr);
}

void MetalCrtView::render (const Crt::Scene& scene)
{
    @autoreleasepool
    {
        if (impl->updateSize() && getWidth() > 0 && getHeight() > 0)
            impl->render (scene, simd_make_float2 ((float) impl->size.width / (float) getWidth(),
                                                   (float) impl->size.height / (float) getHeight()));
    }
}

bool MetalCrtView::hasFailed() const
{
    return impl->hasFailed();
}

juce::Point<int> MetalCrtView::drawableSize() const
{
    const auto size = impl->expectedSize();
    return { (int) size.width, (int) size.height };
}

bool MetalCrtView::isVisibleOnScreen() const
{
    NSWindow* window = impl->view.window;
    return window != nil && window.isVisible && ! window.isMiniaturized
           && (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

juce::Image MetalCrtView::capture()
{
    @autoreleasepool
    {
        return impl->capture();
    }
}

bool MetalCrtView::prefersReducedMotion()
{
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}
