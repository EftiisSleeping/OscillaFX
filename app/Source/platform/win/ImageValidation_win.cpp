#include "platform/ImageValidation.h"

#include <juce_graphics/juce_graphics.h>

namespace ImageValidation
{
namespace
{
constexpr juce::int64 MAX_PIXELS = 150'000'000;  // refuses decompression bombs before they decode

Result reject (const juce::String& why) { return { false, {}, why }; }

const char* extensionFor (juce::ImageFileFormat& format)
{
    if (dynamic_cast<juce::PNGImageFormat*> (&format) != nullptr)  return "png";
    if (dynamic_cast<juce::JPEGImageFormat*> (&format) != nullptr) return "jpg";
    if (dynamic_cast<juce::GIFImageFormat*> (&format) != nullptr)  return "gif";
    return nullptr;
}
} // namespace

// JUCE decoders only: PNG, JPEG, GIF. WebP / HEIC / TIFF need WIC (IWICImagingFactory).
Result inspect (const juce::File& file)
{
    juce::FileInputStream stream (file);
    if (! stream.openedOk())
        return reject ("Cannot open the file.");

    auto* format = juce::ImageFileFormat::findImageFormatForStream (stream);
    const char* extension = format != nullptr ? extensionFor (*format) : nullptr;
    if (extension == nullptr)
        return reject ("Not an image file. Use JPEG, PNG or GIF.");

    stream.setPosition (0);
    const auto image = format->decodeImage (stream);
    if (! image.isValid())
        return reject ("The image data is damaged.");
    if ((juce::int64) image.getWidth() * image.getHeight() > MAX_PIXELS)
        return reject ("The image is too large (over 150 megapixels).");
    return { true, extension, {} };
}
} // namespace ImageValidation
