#pragma once

#include <juce_core/juce_core.h>

// Pixel size read from the file header, so the JUCE decoders (which decode everything at once) never
// see a decompression bomb. PNG, GIF and JPEG, the formats they read.
namespace ImageHeader
{
constexpr juce::int64 MAX_PIXELS = 150'000'000;

namespace detail
{
inline int bigEndian16 (juce::InputStream& in)  { const int hi = in.readByte() & 0xff; return (hi << 8) | (in.readByte() & 0xff); }

inline juce::int64 jpegPixels (juce::InputStream& in)
{
    in.setPosition (2);   // after FF D8
    while (! in.isExhausted())
    {
        if ((in.readByte() & 0xff) != 0xff)
            return -1;
        int marker = in.readByte() & 0xff;
        while (marker == 0xff && ! in.isExhausted())   // fill bytes
            marker = in.readByte() & 0xff;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8))
            continue;   // no length field
        if (marker == 0xd9 || marker == 0xda)
            return -1;  // image end / scan start before any frame header
        const auto length = bigEndian16 (in);
        const bool isFrameHeader = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (isFrameHeader)
        {
            in.readByte();   // sample precision
            const juce::int64 height = bigEndian16 (in);
            const juce::int64 width = bigEndian16 (in);
            return width * height;
        }
        if (length < 2 || ! in.setPosition (in.getPosition() + length - 2))
            return -1;
    }
    return -1;
}
} // namespace detail

// Width x height, or -1 if the header is not a PNG / GIF / JPEG one or is cut short.
inline juce::int64 pixelCount (juce::InputStream& in)
{
    in.setPosition (0);
    juce::uint8 head[24] {};
    const auto got = in.read (head, (int) sizeof (head));
    const auto u32 = [&head] (int i) { return (juce::int64) ((juce::uint32) head[i] << 24 | (juce::uint32) head[i + 1] << 16 | (juce::uint32) head[i + 2] << 8 | head[i + 3]); };
    juce::int64 pixels = -1;
    if (got >= 24 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G')
        pixels = u32 (16) * u32 (20);   // IHDR is always the first chunk
    else if (got >= 10 && head[0] == 'G' && head[1] == 'I' && head[2] == 'F')
        pixels = (juce::int64) (head[6] | head[7] << 8) * (head[8] | head[9] << 8);   // logical screen
    else if (got >= 4 && head[0] == 0xff && head[1] == 0xd8)
        pixels = detail::jpegPixels (in);
    in.setPosition (0);
    return pixels;
}
} // namespace ImageHeader
