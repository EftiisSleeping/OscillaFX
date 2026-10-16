#include "ui/crt/Gallery.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

namespace Crt
{
// ImageIO thumbnailing: never decodes the full-size image.
LuminanceImage decodeLuminance (const juce::File& file, int maxSide)
{
    LuminanceImage result;
    @autoreleasepool
    {
        const auto path = file.getFullPathName().toStdString();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.data(), (CFIndex) path.size(), false);
        if (url == nullptr)
            return result;
        CGImageSourceRef source = CGImageSourceCreateWithURL (url, nullptr);
        CFRelease (url);
        if (source == nullptr)
            return result;

        NSDictionary* options = @{ (id) kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                                   (id) kCGImageSourceCreateThumbnailWithTransform: @YES,
                                   (id) kCGImageSourceShouldCacheImmediately: @YES,
                                   (id) kCGImageSourceThumbnailMaxPixelSize: @(maxSide) };
        CGImageRef thumbnail = CGImageSourceCreateThumbnailAtIndex (source, 0, (CFDictionaryRef) options);
        CFRelease (source);
        if (thumbnail == nullptr)
            return result;

        result.width = (int) CGImageGetWidth (thumbnail);
        result.height = (int) CGImageGetHeight (thumbnail);
        result.pixels.assign ((size_t) (result.width * result.height), 0);

        CGColorSpaceRef grey = CGColorSpaceCreateDeviceGray();
        CGContextRef context = CGBitmapContextCreate (result.pixels.data(), (size_t) result.width, (size_t) result.height,
                                                      8, (size_t) result.width, grey, (CGBitmapInfo) kCGImageAlphaNone);
        CGColorSpaceRelease (grey);
        if (context != nullptr)
        {
            CGContextDrawImage (context, CGRectMake (0, 0, result.width, result.height), thumbnail);
            CGContextRelease (context);
        }
        CGImageRelease (thumbnail);
        if (context == nullptr)
            return {};
    }
    return result;
}
}
