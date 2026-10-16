#include "ui/crt/Gallery.h"

namespace Crt
{
// JUCE decoders (png / jpg / gif): decodes the full image, then scales it down. WIC would add
// WebP / HEIC / TIFF and thumbnail decoding.
LuminanceImage decodeLuminance (const juce::File& file, int maxSide)
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
}
