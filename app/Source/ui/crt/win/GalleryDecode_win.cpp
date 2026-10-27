#include "ui/crt/Gallery.h"
#include "platform/win/WicImage.h"

namespace Crt
{
namespace
{
// JUCE decoders (png / jpg / gif) when WIC cannot read the file: full decode, then scale down.
LuminanceImage decodeWithJuce (const juce::File& file, int maxSide)
{
    LuminanceImage result;
    auto image = juce::ImageFileFormat::loadFrom (file);
    if (! image.isValid())
        return result;

    const auto scale = juce::jmin (1.0, (double) maxSide / juce::jmax (image.getWidth(), image.getHeight()));
    result.width = juce::jmax (1, juce::roundToInt (image.getWidth() * scale));
    result.height = juce::jmax (1, juce::roundToInt (image.getHeight() * scale));
    if (scale < 1.0)
        image = image.rescaled (result.width, result.height, juce::Graphics::highResamplingQuality);

    result.pixels.resize ((size_t) (result.width * result.height));
    const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x)
        {
            const auto colour = pixels.getPixelColour (x, y);
            const auto luma = 0.2126f * colour.getFloatRed() + 0.7152f * colour.getFloatGreen() + 0.0722f * colour.getFloatBlue();
            result.pixels[(size_t) (y * result.width + x)] = (juce::uint8) juce::roundToInt (luma * colour.getFloatAlpha() * 255.0f);
        }
    return result;
}
} // namespace

// WIC: decodes straight to the thumbnail size (JPEG scales while decoding), EXIF orientation applied.
LuminanceImage decodeLuminance (const juce::File& file, int maxSide)
{
    auto grey = WicImage::decodeGrey (file, maxSide);
    if (! grey.isValid())
        return decodeWithJuce (file, maxSide);
    LuminanceImage result;
    result.width = grey.width;
    result.height = grey.height;
    result.pixels = std::move (grey.pixels);
    return result;
}
}
