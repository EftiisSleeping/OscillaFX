#include "platform/ImageValidation.h"
#include "platform/win/ImageHeader.h"
#include "platform/win/WicImage.h"

#include <juce_graphics/juce_graphics.h>

// WIC first (JPEG PNG GIF TIFF built in; WebP / HEIC when the Store extensions are installed), JUCE's
// own decoders (JPEG PNG GIF) when WIC cannot open the file at all.
namespace ImageValidation
{
namespace
{
constexpr juce::int64 MAX_PIXELS = ImageHeader::MAX_PIXELS;  // refuses decompression bombs before they decode
constexpr int PROBE_SIDE = 32;
const char* const FORMAT_LIST = "Use JPEG, PNG, WebP, HEIC, GIF or TIFF.";

Result reject (const juce::String& why) { return { false, {}, why }; }

// Windows 10/11 usually ship these as Store apps; Windows Server and N editions often lack them.
juce::String storeCodecs (const juce::String& extension)
{
    return extension == "webp" ? "\"Webp Image Extensions\""
                               : "\"HEIF Image Extensions\" and \"HEVC Video Extensions\"";
}

bool isStoreFormat (const juce::String& extension) { return extension == "webp" || extension == "heic"; }

juce::String missingCodecMessage (const juce::String& extension)
{
    return "This PC cannot read " + extension.toUpperCase() + ": install " + storeCodecs (extension) + " from the Microsoft Store.";
}

juce::String undecodableMessage (const juce::String& extension)
{
    if (! isStoreFormat (extension))
        return "The image data is damaged.";
    return "Cannot decode this " + extension.toUpperCase() + " picture: it is damaged, or this PC needs "
         + storeCodecs (extension) + " from the Microsoft Store.";
}

Result inspectWithWic (const juce::File& file, const WicImage::Info& info)
{
    if (info.extension.isEmpty())
        return reject (juce::String ("Unsupported image format. ") + FORMAT_LIST);
    if (info.width <= 0 || info.height <= 0)
        return reject ("The image has no pixels.");
    if (info.width * info.height > MAX_PIXELS)
        return reject ("The image is too large (over 150 megapixels).");
    if (! WicImage::decodeGrey (file, PROBE_SIDE).isValid()) // a valid header can sit on broken pixel data
        return reject (undecodableMessage (info.extension));
    return { true, info.extension, {} };
}

const char* extensionFor (juce::ImageFileFormat& format)
{
    if (dynamic_cast<juce::PNGImageFormat*> (&format) != nullptr)  return "png";
    if (dynamic_cast<juce::JPEGImageFormat*> (&format) != nullptr) return "jpg";
    if (dynamic_cast<juce::GIFImageFormat*> (&format) != nullptr)  return "gif";
    return nullptr;
}

Result inspectWithJuce (juce::FileInputStream& stream)
{
    auto* format = juce::ImageFileFormat::findImageFormatForStream (stream);
    const char* extension = format != nullptr ? extensionFor (*format) : nullptr;
    if (extension == nullptr)
        return reject (juce::String ("Not an image file. ") + FORMAT_LIST);

    const auto pixels = ImageHeader::pixelCount (stream);   // before decoding: JUCE decodes it all at once
    if (pixels <= 0)
        return reject ("The image data is damaged.");
    if (pixels > MAX_PIXELS)
        return reject ("The image is too large (over 150 megapixels).");
    const auto image = format->decodeImage (stream);
    if (! image.isValid())
        return reject ("The image data is damaged.");
    return { true, extension, {} };
}
} // namespace

Result inspect (const juce::File& file)
{
    juce::FileInputStream stream (file);
    if (! stream.openedOk())
        return reject ("Cannot open the file.");

    if (const auto info = WicImage::probe (file); info.isDecodable)
        return inspectWithWic (file, info);
    if (const auto claimed = WicImage::sniffOptionalFormat (file); claimed.isNotEmpty())
        return reject (WicImage::hasCodec (claimed) ? undecodableMessage (claimed) : missingCodecMessage (claimed));
    return inspectWithJuce (stream);
}
} // namespace ImageValidation
