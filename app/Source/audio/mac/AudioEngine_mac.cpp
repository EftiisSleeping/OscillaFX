#include "audio/AudioEngine.h"

#include "audio/mac/CoreAudioHelpers.h"
#include "platform/mac/CrashGuardMac.h"
#include "platform/CrashWatchdog.h"
#include "audio/DriftResampler.h"
#include "audio/Dynamics.h"
#include "audio/DspParams.h"
#include "audio/SpscRingBuffer.h"
#include "audio/TripleBuffer.h"
#include "audio/VisualTaps.h"
#include "DfxDsp.h"

#include <juce_events/juce_events.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>

namespace CA = CoreAudioHelpers;

// Threading invariant (see engine/tests/mac_platform.cpp): the DSP's "registry" is thread_local.
// Every registry-touching call on the live DfxDsp (setters, and processAudio's registry refresh)
// happens on the capture IOProc thread. After each (re)start the first capture callback re-applies
// the full parameter set, so a new HAL IO thread starts from a complete table.

namespace
{
// Virtual device candidates, best first: our driver (UID is stable across the OscillaFX rename), then stock BlackHole.
const char* const VIRTUAL_UIDS[] = { "Oscilla_UID", "BlackHole2ch_UID" };
const char* const VIRTUAL_NAMES[] = { "OscillaFX Audio", "Oscilla Audio", "BlackHole 2ch" };
const char* const HIDDEN_UIDS[] = { "Oscilla_UID", "Oscilla_2_UID" };

constexpr int MAX_BLOCK_FRAMES = 4096;
constexpr size_t RING_FRAMES = 1 << 16;
constexpr size_t MIN_TARGET_FILL = 512;
constexpr float MAX_VOLUME_COMPENSATION = 1600.0f; // ~ +64 dB, the virtual device's range
constexpr int CONFIG_CHECK_DELAY_MS = 100;         // coalesces bursts of HAL notifications
constexpr int CAPTURE_START_TIMEOUT_MS = 2000;     // virtual device IOProc must fire by then

bool isHiddenUid (const juce::String& uid)
{
    for (auto* hidden : HIDDEN_UIDS)
        if (uid == hidden)
            return true;
    return false;
}

AudioObjectID findVirtualDevice()
{
    for (auto* uid : VIRTUAL_UIDS)
        if (auto id = CA::findDeviceByUid (uid))
            return id;
    for (auto* name : VIRTUAL_NAMES)
        if (auto id = CA::findDeviceByName (name))
            return id;
    return kAudioObjectUnknown;
}

// Where channel `channel` lives inside a (possibly non-interleaved) buffer list.
struct ChannelLocation
{
    const AudioBuffer* buffer = nullptr;
    UInt32 offset = 0;
};

ChannelLocation locateChannel (const AudioBufferList& list, UInt32 channel)
{
    for (UInt32 b = 0; b < list.mNumberBuffers; ++b)
    {
        const auto& buffer = list.mBuffers[b];
        if (channel < buffer.mNumberChannels)
            return { &buffer, channel };
        channel -= buffer.mNumberChannels;
    }
    return {};
}

int frameCount (const AudioBufferList& list)
{
    if (list.mNumberBuffers == 0 || list.mBuffers[0].mNumberChannels == 0)
        return 0;
    const auto& first = list.mBuffers[0];
    return (int) (first.mDataByteSize / (sizeof (float) * first.mNumberChannels));
}

// Device properties the running routing depends on; a change means restart.
struct RoutingConfig
{
    double inputRate = 0, outputRate = 0;
    UInt32 inputBuffer = 0, outputBuffer = 0;
    int inputChannels = 0, outputChannels = 0;

    static RoutingConfig read (AudioObjectID virtualId, AudioObjectID outputId)
    {
        return { CA::nominalSampleRate (virtualId), CA::nominalSampleRate (outputId),
                 CA::bufferFrameSize (virtualId), CA::bufferFrameSize (outputId),
                 CA::channelCount (virtualId, true), CA::channelCount (outputId, false) };
    }

    bool operator== (const RoutingConfig& o) const
    {
        return juce::exactlyEqual (inputRate, o.inputRate) && juce::exactlyEqual (outputRate, o.outputRate)
            && inputBuffer == o.inputBuffer && outputBuffer == o.outputBuffer
            && inputChannels == o.inputChannels && outputChannels == o.outputChannels;
    }
};
} // namespace

struct AudioEngine::Impl
{
    explicit Impl (AudioEngine& o) : owner (o)
    {
        virtualId = findVirtualDevice(); // read-only lookup so device lists exclude it before start()
        virtualUid = CA::deviceUid (virtualId);
        DspParamsApply::configureBands (dsp);
        registry()[token] = this;
    }

    ~Impl()
    {
        stop();
        registry().erase (token);
    }

    // ---------------------------------------------------------------- devices

    bool containsVirtualDevice (AudioObjectID id) const
    {
        for (auto& uid : CA::subDeviceUids (id))
            if (uid == virtualUid || isHiddenUid (uid))
                return true;
        return false;
    }

    bool isUsableOutput (AudioObjectID id) const
    {
        return id != kAudioObjectUnknown && id != virtualId
            && ! isHiddenUid (CA::deviceUid (id)) && CA::outputChannelCount (id) > 0
            && ! containsVirtualDevice (id);
    }

    AudioObjectID usableByUid (const juce::String& uid) const
    {
        auto id = uid.isEmpty() ? kAudioObjectUnknown : CA::findDeviceByUid (uid);
        return isUsableOutput (id) ? id : kAudioObjectUnknown;
    }

    // Built-in output first, then the other usable outputs in HAL order.
    juce::Array<AudioObjectID> outputCandidates() const
    {
        juce::Array<AudioObjectID> builtIn, others;
        for (auto id : CA::allDevices())
            if (isUsableOutput (id))
                (CA::isBuiltIn (id) ? builtIn : others).add (id);
        builtIn.addArray (others);
        return builtIn;
    }

    AudioObjectID fallbackOutput() const
    {
        auto candidates = outputCandidates();
        return candidates.isEmpty() ? kAudioObjectUnknown : candidates.getFirst();
    }

    // Explicit choice, then the current system default, then the device saved before a crash.
    AudioObjectID resolveInitialOutput() const
    {
        if (auto id = usableByUid (preferredUid))
            return id;
        auto current = CA::defaultOutputDevice();
        if (isUsableOutput (current))
            return current;
        if (auto id = usableByUid (CrashWatchdog::readRestoreUid()))
            return id;
        return fallbackOutput();
    }

    static OutputDevice describe (AudioObjectID id)
    {
        if (id == kAudioObjectUnknown)
            return {};
        return { CA::deviceUid (id), CA::deviceName (id) };
    }

    juce::Array<OutputDevice> outputDevices() const
    {
        juce::Array<OutputDevice> result;
        for (auto id : CA::allDevices())
            if (isUsableOutput (id))
                result.add (describe (id));
        return result;
    }

    OutputDevice currentOutput() const
    {
        return describe (running ? outputId : resolveInitialOutput());
    }

    // ---------------------------------------------------------------- lifecycle

    bool start()
    {
        if (running)
            return true;
        error.clear();
        virtualId = findVirtualDevice();
        virtualUid = CA::deviceUid (virtualId);
        if (virtualId == kAudioObjectUnknown)
            return fail ("Virtual device \"OscillaFX Audio\" not found. Install the OscillaFX driver.");
        if (! CA::streamsAreFloat32 (virtualId, true))
            return failAndRestore ("Unsupported virtual device stream format (expected 32-bit float).");

        auto initial = resolveInitialOutput();
        if (initial == kAudioObjectUnknown)
            return failAndRestore ("No output device available.");

        running = true;
        captureCallbacks.store (0, std::memory_order_relaxed);
        if (! routeWithFallback (initial))
        {
            running = false;
            return failAndRestore (error);
        }
        CA::setDefaultOutputDevice (virtualId);
        addSystemListeners();
        captureWatch.startTimer (CAPTURE_START_TIMEOUT_MS);
        return true;
    }

    void stop()
    {
        if (! running)
            return;
        running = false;
        configCheck.stopTimer();
        captureWatch.stopTimer();
        removeListeners (outputListeners);
        removeListeners (systemListeners);
        stopRouting();
        restoreSystemDefault();
        CrashGuard::setRestoreDevice (kAudioObjectUnknown);
    }

    bool fail (const juce::String& message)
    {
        error = message;
        return false;
    }

    bool failAndRestore (const juce::String& message)
    {
        if (CA::defaultOutputDevice() == virtualId)
            restoreSystemDefault();
        return fail (message);
    }

    void restoreSystemDefault()
    {
        auto target = CA::isAlive (outputId) && isUsableOutput (outputId) ? outputId : resolveInitialOutput();
        if (target != kAudioObjectUnknown)
            CA::setDefaultOutputDevice (target);
        CrashWatchdog::clearRestoreUid();
    }

    void rememberOutput()
    {
        outputUid = CA::deviceUid (outputId);
        preferredUid = outputUid;
        CrashGuard::setRestoreDevice (outputId);
        if (! CrashWatchdog::writeRestoreUid (outputUid))
            juce::Logger::writeToLog ("OscillaFX: could not write " + CrashWatchdog::restoreFile().getFullPathName());
    }

    // ---------------------------------------------------------------- routing

    // Stops routing, then opens `id`. Leaves routing stopped on failure.
    bool routeTo (AudioObjectID id)
    {
        removeListeners (outputListeners);
        stopRouting();
        outputId = id;
        if (! CA::streamsAreFloat32 (id, false) || ! startRouting())
            return false;
        rememberOutput();
        addOutputListeners();
        syncVolumeFromOutput();
        return true;
    }

    // Tries `first`, then built-in, then every other output. Sets `error` describing the outcome.
    bool routeWithFallback (AudioObjectID first)
    {
        auto candidates = outputCandidates();
        candidates.removeFirstMatchingValue (first);
        candidates.insert (0, first);
        for (auto id : candidates)
        {
            if (! routeTo (id))
                continue;
            error = id == first ? juce::String()
                                : "Could not start audio on " + CA::deviceName (first) + "; using " + CA::deviceName (id) + ".";
            return true;
        }
        error = "No output device could be started.";
        return false;
    }

    void switchOutput (AudioObjectID newId, bool force = false)
    {
        if (newId == kAudioObjectUnknown || (! force && newId == outputId && outputProcId != nullptr))
            return;
        if (! routeWithFallback (newId))
        {
            auto message = error;
            stop();
            error = message;
        }
        notifyOutputChanged();
    }

    bool startRouting()
    {
        CA::setNominalSampleRate (virtualId, CA::nominalSampleRate (outputId)); // async; rate listener re-checks
        routed = RoutingConfig::read (virtualId, outputId);
        if (routed.inputRate <= 0 || routed.outputRate <= 0 || routed.outputChannels <= 0)
            return false;

        prepareDsp();
        prepareOutputChannels();
        auto target = (size_t) 2 * (routed.inputBuffer + routed.outputBuffer);
        ring.reset();
        resampler.prepare (routed.inputRate, routed.outputRate, std::max (target, MIN_TARGET_FILL));

        if (! openIOProc (virtualId, captureProc, inputProcId, false)
            || ! openIOProc (outputId, renderProc, outputProcId, true))
        {
            stopRouting();
            return false;
        }
        return true;
    }

    // Message thread, IOProcs stopped. Registry-touching parameter writes are left to the capture thread.
    void prepareDsp()
    {
        dsp.setSignalFormat (32, SpscRingBuffer::NUM_CHANNELS, (int) routed.inputRate, 32);
        dynamics.prepare (routed.inputRate);
        needsFullApply.store (true, std::memory_order_release);
    }

    void prepareOutputChannels()
    {
        outputIsMono = routed.outputChannels == 1;
        auto [left, right] = CA::preferredStereoChannels (outputId);
        outputLeft = (UInt32) left;
        outputRight = (UInt32) right;
    }

    // `disableInput` true: render-only proc on the real device; false: capture-only on the virtual one.
    bool openIOProc (AudioObjectID device, AudioDeviceIOProc proc, AudioDeviceIOProcID& procId, bool disableInput)
    {
        if (AudioDeviceCreateIOProcID (device, proc, this, &procId) != noErr)
        {
            procId = nullptr;
            return false;
        }
        CA::disableStreams (device, procId, disableInput);
        return AudioDeviceStart (device, procId) == noErr;
    }

    void closeIOProc (AudioObjectID device, AudioDeviceIOProcID& procId)
    {
        if (procId == nullptr)
            return;
        AudioDeviceStop (device, procId);
        AudioDeviceDestroyIOProcID (device, procId);
        procId = nullptr;
    }

    void stopRouting()
    {
        closeIOProc (outputId, outputProcId);
        closeIOProc (virtualId, inputProcId);
        taps.clear();
    }

    void setOutputDevice (const juce::String& uid)
    {
        auto id = usableByUid (uid);
        if (id == kAudioObjectUnknown)
            return;
        preferredUid = uid;
        if (running)
            switchOutput (id);
        else
            notifyOutputChanged();
    }

    // Delivered later on the message thread; reports the device current at delivery time.
    void notifyOutputChanged()
    {
        auto t = token;
        juce::MessageManager::callAsync ([t]
        {
            if (auto* live = lookup (t))
                if (live->owner.onOutputDeviceChanged)
                    live->owner.onOutputDeviceChanged (live->currentOutput());
        });
    }

    // ---------------------------------------------------------------- volume mirroring

    void syncVolumeFromOutput()
    {
        outputHasVolume = CA::hasSettableVolume (outputId) && CA::hasSettableVolume (virtualId);
        if (outputHasVolume)
            CA::setVolumeScalar (virtualId, CA::volumeScalar (outputId));
        if (CA::hasSettableMute (outputId) && CA::hasSettableMute (virtualId))
            CA::setMuted (virtualId, CA::isMuted (outputId));
        updateInputCompensation();
    }

    void mirrorVolumeToOutput()
    {
        if (outputHasVolume)
            CA::setVolumeScalar (outputId, CA::volumeScalar (virtualId));
        if (CA::hasSettableMute (outputId))
            CA::setMuted (outputId, CA::isMuted (virtualId));
        updateInputCompensation();
    }

    // The virtual device scales the captured signal by its own volume. When the real device
    // carries the volume instead, undo that so it is not applied twice (and DSP sees full level).
    void updateInputCompensation()
    {
        float gain = 1.0f;
        if (outputHasVolume)
            gain = std::min (MAX_VOLUME_COMPENSATION, std::pow (10.0f, -CA::volumeDecibels (virtualId) / 20.0f));
        inputGain.store (gain, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- HAL notifications

    struct Listener
    {
        AudioObjectID object;
        AudioObjectPropertyAddress address;
    };

    void addListener (std::vector<Listener>& list, AudioObjectID object, AudioObjectPropertySelector selector,
                      AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        Listener l { object, { selector, scope, kAudioObjectPropertyElementMain } };
        if (AudioObjectAddPropertyListener (object, &l.address, propertyListener, (void*) token) == noErr)
            list.push_back (l);
    }

    void removeListeners (std::vector<Listener>& list)
    {
        for (auto& l : list)
            AudioObjectRemovePropertyListener (l.object, &l.address, propertyListener, (void*) token);
        list.clear();
    }

    void addConfigListeners (std::vector<Listener>& list, AudioObjectID device, AudioObjectPropertyScope scope)
    {
        addListener (list, device, kAudioDevicePropertyNominalSampleRate);
        addListener (list, device, kAudioDevicePropertyBufferFrameSize);
        addListener (list, device, kAudioDevicePropertyStreamConfiguration, scope);
    }

    void addSystemListeners()
    {
        addListener (systemListeners, kAudioObjectSystemObject, kAudioHardwarePropertyDevices);
        addListener (systemListeners, kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
        addConfigListeners (systemListeners, virtualId, kAudioObjectPropertyScopeInput);
        addListener (systemListeners, virtualId, kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput);
        addListener (systemListeners, virtualId, kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput);
    }

    void addOutputListeners()
    {
        addConfigListeners (outputListeners, outputId, kAudioObjectPropertyScopeOutput);
        addListener (outputListeners, outputId, kAudioDevicePropertyDeviceIsAlive);
    }

    static OSStatus propertyListener (AudioObjectID object, UInt32 count,
                                      const AudioObjectPropertyAddress* addresses, void* client)
    {
        auto t = (uintptr_t) client;
        for (UInt32 i = 0; i < count; ++i)
        {
            auto selector = addresses[i].mSelector;
            juce::MessageManager::callAsync ([t, object, selector]
            {
                if (auto* live = lookup (t))
                    live->handleProperty (object, selector);
            });
        }
        return noErr;
    }

    void handleProperty (AudioObjectID object, AudioObjectPropertySelector selector)
    {
        if (! running)
            return;
        switch (selector)
        {
            case kAudioHardwarePropertyDefaultOutputDevice: onDefaultOutputChanged(); break;
            case kAudioHardwarePropertyDevices:
            case kAudioDevicePropertyDeviceIsAlive:         onDevicesChanged(); break;
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyStreamConfiguration:   configCheck.startTimer (CONFIG_CHECK_DELAY_MS); break;
            case kAudioDevicePropertyVolumeScalar:
            case kAudioDevicePropertyMute:                  if (object == virtualId) mirrorVolumeToOutput(); break;
            default: break;
        }
    }

    // A real device picked in macOS Sound settings becomes our output; the virtual device stays default.
    void onDefaultOutputChanged()
    {
        auto chosen = CA::defaultOutputDevice();
        if (chosen == virtualId || ! isUsableOutput (chosen))
            return;
        preferredUid = CA::deviceUid (chosen);
        switchOutput (chosen);
        if (running)
            CA::setDefaultOutputDevice (virtualId);
    }

    void onDevicesChanged()
    {
        if (! CA::isAlive (virtualId) || findVirtualDevice() != virtualId)
        {
            stop();
            error = "The virtual audio device disappeared.";
            notifyOutputChanged();
            return;
        }
        if (CA::findDeviceByUid (outputUid) == kAudioObjectUnknown || ! CA::isAlive (outputId))
            switchOutput (fallbackOutput());
    }

    void onConfigCheck()
    {
        configCheck.stopTimer();
        if (running && ! (RoutingConfig::read (virtualId, outputId) == routed))
            switchOutput (outputId, true);
    }

    // Safety net: the system output is already ours, so a capture IOProc that never runs means silence.
    void onCaptureWatch()
    {
        captureWatch.stopTimer();
        if (! running || captureCallbacks.load (std::memory_order_relaxed) > 0)
            return;
        stop();
        error = "No audio from the virtual device; system output restored.";
        notifyOutputChanged();
    }

    // ---------------------------------------------------------------- audio threads

    static OSStatus captureProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                                 const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* client)
    {
        auto* self = static_cast<Impl*> (client);
        self->captureCallbacks.fetch_add (1, std::memory_order_relaxed);
        if (input != nullptr)
            self->processCapture (*input);
        return noErr;
    }

    static OSStatus renderProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                                const AudioTimeStamp*, AudioBufferList* output, const AudioTimeStamp*, void* client)
    {
        if (output != nullptr)
            static_cast<Impl*> (client)->processRender (*output);
        return noErr;
    }

    void applyPendingParams()
    {
        if (needsFullApply.exchange (false, std::memory_order_acq_rel))
        {
            pendingParams.fetch (appliedParams);
            DspParamsApply::applyAll (dsp, appliedParams);
            dynamics.configure (appliedParams.nightMode, appliedParams.limiterCeilingDb);
            return;
        }
        DspParams latest;
        if (! pendingParams.fetch (latest))
            return;
        DspParamsApply::applyChanges (dsp, latest, appliedParams);
        appliedParams = latest;
        dynamics.configure (appliedParams.nightMode, appliedParams.limiterCeilingDb);
    }

    void processCapture (const AudioBufferList& input)
    {
        applyPendingParams();
        const int total = frameCount (input);
        for (int offset = 0; offset < total; offset += MAX_BLOCK_FRAMES)
        {
            const int frames = std::min (MAX_BLOCK_FRAMES, total - offset);
            readStereo (input, offset, frames);
            dsp.processAudio ((short*) captureScratch.data(), (short*) processedScratch.data(), frames, 0);
            dynamics.process (processedScratch.data(), frames); // night mode, then the safety limiter: last in the chain
            taps.writeScope (processedScratch.data(), frames);
            ring.write (processedScratch.data(), (size_t) frames);
        }
        float bands[VisualTaps::NUM_SPECTRUM_BANDS];
        dsp.getSpectrumBandValues (bands, VisualTaps::NUM_SPECTRUM_BANDS);
        taps.writeSpectrum (bands);
    }

    // Copies the first two channels (mono duplicated), ramping the volume compensation across the block.
    void readStereo (const AudioBufferList& input, int offset, int frames)
    {
        const float target = inputGain.load (std::memory_order_relaxed);
        const float step = (target - appliedInputGain) / (float) frames;
        for (UInt32 ch = 0; ch < 2; ++ch)
        {
            auto loc = locateChannel (input, ch);
            if (loc.buffer == nullptr)
                loc = locateChannel (input, 0);
            if (loc.buffer == nullptr)
                return;
            const auto* src = static_cast<const float*> (loc.buffer->mData);
            const auto stride = loc.buffer->mNumberChannels;
            for (int i = 0; i < frames; ++i)
            {
                const float gain = appliedInputGain + step * (float) (i + 1);
                captureScratch[(size_t) (2 * i) + ch] = gain * src[(size_t) (offset + i) * stride + loc.offset];
            }
        }
        appliedInputGain = target;
    }

    void processRender (AudioBufferList& output)
    {
        for (UInt32 b = 0; b < output.mNumberBuffers; ++b)
            std::memset (output.mBuffers[b].mData, 0, output.mBuffers[b].mDataByteSize);

        const int total = frameCount (output);
        for (int offset = 0; offset < total; offset += MAX_BLOCK_FRAMES)
        {
            const int frames = std::min (MAX_BLOCK_FRAMES, total - offset);
            resampler.render (ring, renderScratch.data(), frames);
            if (outputIsMono)
                writeMono (output, offset, frames);
            else
                writeStereo (output, offset, frames);
        }
    }

    void writeChannel (AudioBufferList& output, UInt32 channel, int offset, int frames, int sourceChannel)
    {
        auto loc = locateChannel (output, channel);
        if (loc.buffer == nullptr)
            return;
        auto* dst = static_cast<float*> (loc.buffer->mData);
        const auto stride = loc.buffer->mNumberChannels;
        for (int i = 0; i < frames; ++i)
            dst[(size_t) (offset + i) * stride + loc.offset] = renderScratch[(size_t) (2 * i + sourceChannel)];
    }

    void writeStereo (AudioBufferList& output, int offset, int frames)
    {
        writeChannel (output, outputLeft, offset, frames, 0);
        writeChannel (output, outputRight, offset, frames, 1);
    }

    void writeMono (AudioBufferList& output, int offset, int frames)
    {
        for (int i = 0; i < frames; ++i)
            renderScratch[(size_t) (2 * i)] = 0.5f * (renderScratch[(size_t) (2 * i)] + renderScratch[(size_t) (2 * i + 1)]);
        writeChannel (output, 0, offset, frames, 0);
    }

    // ---------------------------------------------------------------- listener dispatch

    static std::map<uintptr_t, Impl*>& registry()
    {
        static std::map<uintptr_t, Impl*> live;
        return live;
    }

    static Impl* lookup (uintptr_t t)
    {
        auto it = registry().find (t);
        return it == registry().end() ? nullptr : it->second;
    }

    static uintptr_t nextToken()
    {
        static uintptr_t counter = 0;
        return ++counter;
    }

    // ---------------------------------------------------------------- state

    AudioEngine& owner;
    const uintptr_t token = nextToken();

    // Message thread
    AudioObjectID virtualId = kAudioObjectUnknown, outputId = kAudioObjectUnknown;
    AudioDeviceIOProcID inputProcId = nullptr, outputProcId = nullptr;
    juce::String virtualUid, preferredUid, outputUid, error;
    RoutingConfig routed;
    bool running = false, outputHasVolume = false;
    std::vector<Listener> systemListeners, outputListeners;
    juce::TimedCallback configCheck { [this] { onConfigCheck(); } };
    juce::TimedCallback captureWatch { [this] { onCaptureWatch(); } };

    // Shared with audio threads (lock-free)
    TripleBuffer<DspParams> pendingParams;
    SpscRingBuffer ring { RING_FRAMES };
    VisualTaps taps;
    std::atomic<float> inputGain { 1.0f };
    std::atomic<bool> needsFullApply { true };
    std::atomic<uint32_t> captureCallbacks { 0 };

    // Capture thread only (setSignalFormat also from the message thread while stopped)
    DfxDsp dsp;
    DspParams appliedParams = DspParams::fromSettings ({});
    Dynamics::DynamicsChain dynamics;
    float appliedInputGain = 1.0f;
    std::vector<float> captureScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);
    std::vector<float> processedScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);

    // Render thread only (channel map set while stopped)
    DriftResampler resampler;
    std::vector<float> renderScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);
    bool outputIsMono = false;
    UInt32 outputLeft = 0, outputRight = 1;
};

AudioEngine::AudioEngine() : impl (std::make_unique<Impl> (*this)) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::start()                    { return impl->start(); }
void AudioEngine::stop()                     { impl->stop(); }
juce::String AudioEngine::lastError() const  { return impl->error; }

void AudioEngine::applySettings (const DspSettings& settings)
{
    impl->pendingParams.publish (DspParams::fromSettings (settings));
}

juce::Array<OutputDevice> AudioEngine::outputDevices() const { return impl->outputDevices(); }
OutputDevice AudioEngine::currentOutputDevice() const        { return impl->currentOutput(); }
void AudioEngine::setOutputDevice (const juce::String& uid)  { impl->setOutputDevice (uid); }

void AudioEngine::getSpectrum (float* bands10) const { impl->taps.readSpectrum (bands10); }

int AudioEngine::getScopeSamples (float* left, float* right, int maxSamples) const
{
    return impl->taps.readScope (left, right, maxSamples);
}
