#include "platform/ImageValidation.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

namespace ImageValidation
{
namespace
{
constexpr int64_t MAX_PIXELS = 150'000'000;  // refuses decompression bombs before anything decodes them
constexpr int PROBE_THUMBNAIL_SIDE = 32;

struct FormatName { const char* uti; const char* extension; };
const FormatName FORMATS[] = {
    { "public.jpeg", "jpg" },       { "public.png", "png" },     { "org.webmproject.webp", "webp" },
    { "public.heic", "heic" },      { "public.heif", "heic" },   { "com.compuserve.gif", "gif" },
    { "public.tiff", "tiff" },
};

Result reject (const juce::String& why) { return { false, {}, why }; }

int64_t numberProperty (CFDictionaryRef properties, CFStringRef key)
{
    int64_t value = 0;
    if (auto number = (CFNumberRef) CFDictionaryGetValue (properties, key))
        CFNumberGetValue (number, kCFNumberSInt64Type, &value);
    return value;
}
} // namespace

Result inspect (const juce::File& file)
{
    @autoreleasepool
    {
        const auto path = file.getFullPathName().toStdString();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.data(), (CFIndex) path.size(), false);
        if (url == nullptr)
            return reject ("Cannot open the file.");
        CGImageSourceRef source = CGImageSourceCreateWithURL (url, nullptr);
        CFRelease (url);
        if (source == nullptr)
            return reject ("Not an image file.");

        Result result = reject ("Not an image file.");
        const auto type = CGImageSourceGetType (source);
        if (type != nullptr && CGImageSourceGetStatus (source) == kCGImageStatusComplete && CGImageSourceGetCount (source) > 0)
        {
            const auto uti = juce::String::fromCFString (type);
            const char* extension = nullptr;
            for (const auto& format : FORMATS)
                if (uti == format.uti)
                    extension = format.extension;

            if (extension == nullptr)
            {
                result = reject ("Unsupported image format (" + uti + "). Use JPEG, PNG, WebP, HEIC, GIF or TIFF.");
            }
            else if (auto properties = CGImageSourceCopyPropertiesAtIndex (source, 0, nullptr))
            {
                const auto width = numberProperty (properties, kCGImagePropertyPixelWidth);
                const auto height = numberProperty (properties, kCGImagePropertyPixelHeight);
                CFRelease (properties);
                if (width <= 0 || height <= 0)
                    result = reject ("The image has no pixels.");
                else if (width * height > MAX_PIXELS)
                    result = reject ("The image is too large (over 150 megapixels).");
                else
                {
                    // A header can be valid while the pixel data is broken: decode a tiny thumbnail.
                    NSDictionary* options = @{ (id) kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                                               (id) kCGImageSourceThumbnailMaxPixelSize: @(PROBE_THUMBNAIL_SIDE) };
                    if (auto probe = CGImageSourceCreateThumbnailAtIndex (source, 0, (CFDictionaryRef) options))
                    {
                        CGImageRelease (probe);
                        result = { true, extension, {} };
                    }
                    else
                        result = reject ("The image data is damaged.");
                }
            }
            else
                result = reject ("Cannot read the image properties.");
        }
        CFRelease (source);
        return result;
    }
}
} // namespace ImageValidation
