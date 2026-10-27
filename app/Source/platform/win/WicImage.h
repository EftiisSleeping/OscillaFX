#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// Windows Imaging Component decoding for the gallery: whatever codecs this PC has (built in: PNG,
// JPEG, GIF, TIFF, BMP; WebP and HEIC from the Microsoft Store extensions on most Windows 10/11).
// Thread-safe; initialises COM on the calling thread as needed.
namespace WicImage
{
struct Info
{
    bool isDecodable = false;  // WIC has a decoder and frame 0 is readable
    juce::String extension;    // canonical: jpg png webp heic gif tiff; empty for other formats
    juce::int64 width = 0, height = 0;
};

// Container format and pixel size, without decoding the pixels.
Info probe (const juce::File&);

struct Grey
{
    int width = 0, height = 0;
    std::vector<juce::uint8> pixels;  // row-major luminance (Rec. 709 on the stored values) times alpha
    bool isValid() const { return width > 0 && height > 0; }
};

// Frame 0 upright (EXIF orientation applied), scaled so the longer side is at most `maxSide`
// (never enlarged). Invalid if WIC cannot decode it.
Grey decodeGrey (const juce::File&, int maxSide);

// False when this PC lacks the Store codec for "webp" / "heic"; true for the built-in formats.
bool hasCodec (const juce::String& extension);

// What the file's first bytes claim, for formats WIC may lack a codec for: "webp", "heic" or empty.
juce::String sniffOptionalFormat (const juce::File&);
} // namespace WicImage
