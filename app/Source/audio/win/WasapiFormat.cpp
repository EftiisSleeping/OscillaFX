#include "audio/win/WasapiFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace WasapiFormat
{
namespace
{
constexpr uint32_t SPEAKER_FRONT_LEFT = 0x1, SPEAKER_FRONT_RIGHT = 0x2,
                   SPEAKER_BACK_LEFT = 0x10, SPEAKER_BACK_RIGHT = 0x20;

constexpr float MINUS_3_DB = 0.70710678f, MINUS_6_DB = 0.5f;

struct Gains { float left, right; };

// ksmedia.h speaker bits in channel order; height speakers are left out.
Gains speakerGains (uint32_t speaker)
{
    switch (speaker)
    {
        case 0x1:   return { 1.0f, 0.0f };              // front left
        case 0x2:   return { 0.0f, 1.0f };              // front right
        case 0x4:   return { MINUS_3_DB, MINUS_3_DB };  // front centre
        case 0x8:   return { MINUS_6_DB, MINUS_6_DB };  // LFE
        case 0x10:  return { MINUS_3_DB, 0.0f };        // back left
        case 0x20:  return { 0.0f, MINUS_3_DB };        // back right
        case 0x40:  return { MINUS_3_DB, 0.0f };        // front left of centre
        case 0x80:  return { 0.0f, MINUS_3_DB };        // front right of centre
        case 0x100: return { MINUS_6_DB, MINUS_6_DB };  // back centre
        case 0x200: return { MINUS_3_DB, 0.0f };        // side left
        case 0x400: return { 0.0f, MINUS_3_DB };        // side right
        default:    return { 0.0f, 0.0f };
    }
}

// Windows' default layouts when a format carries no channel mask.
uint32_t defaultMask (int channels)
{
    switch (channels)
    {
        case 4:  return 0x33;   // quad
        case 6:  return 0x3f;   // 5.1
        case 8:  return 0x63f;  // 7.1
        default: return 0x3;    // the first two channels as front left / right
    }
}

constexpr double INT16_SCALE = 32767.0, INT24_SCALE = 8388607.0, INT32_SCALE = 2147483647.0;

// Position of `speaker` in a frame, or -1 if the mask lacks it or the frame is too short.
int speakerIndex (uint32_t mask, uint32_t speaker, int channels)
{
    if ((mask & speaker) == 0)
        return -1;
    int index = 0;
    for (uint32_t bit = 1; bit < speaker; bit <<= 1)
        if (mask & bit)
            ++index;
    return index < channels ? index : -1;
}

int32_t toInteger (float value, double scale)
{
    const double clamped = std::clamp ((double) value, -1.0, 1.0);
    return (int32_t) std::lround (clamped * scale);
}

int32_t readLittleEndian24 (const uint8_t* p)
{
    const int32_t raw = (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
    return (raw & 0x800000) ? raw - 0x1000000 : raw;
}
} // namespace

StreamFormat describe (bool isFloat, bool isPcm, int bitsPerSample, int channels, double sampleRate, uint32_t channelMask)
{
    StreamFormat format;
    format.channels = channels;
    format.sampleRate = sampleRate;
    format.channelMask = channelMask;
    format.bytesPerSample = bitsPerSample / 8;
    if (isFloat && bitsPerSample == 32)
        format.type = SampleType::float32;
    else if (isPcm && bitsPerSample == 16)
        format.type = SampleType::int16;
    else if (isPcm && bitsPerSample == 24)
        format.type = SampleType::int24;
    else if (isPcm && bitsPerSample == 32)
        format.type = SampleType::int32;
    return format;
}

StereoMap stereoMapFor (int channels, uint32_t channelMask)
{
    StereoMap map;
    if (channels <= 1)
    {
        map.mono = true;
        map.left = map.right = 0;
        return map;
    }
    const int left = speakerIndex (channelMask, SPEAKER_FRONT_LEFT, channels);
    const int right = speakerIndex (channelMask, SPEAKER_FRONT_RIGHT, channels);
    map.left = left >= 0 ? left : 0;
    map.right = right >= 0 && right != map.left ? right : (map.left == 0 ? 1 : 0);
    if (channels == 4)
    {
        const int rearLeft = speakerIndex (channelMask, SPEAKER_BACK_LEFT, channels);
        const int rearRight = speakerIndex (channelMask, SPEAKER_BACK_RIGHT, channels);
        map.rearLeft = channelMask == 0 ? 2 : rearLeft;
        map.rearRight = channelMask == 0 ? 3 : rearRight;
    }
    return map;
}

float readSample (const uint8_t* source, SampleType type)
{
    switch (type)
    {
        case SampleType::float32: { float v; std::memcpy (&v, source, 4); return v; }
        case SampleType::int16:   { int16_t v; std::memcpy (&v, source, 2); return (float) (v / 32768.0); }
        case SampleType::int24:   return (float) (readLittleEndian24 (source) / 8388608.0);
        case SampleType::int32:   { int32_t v; std::memcpy (&v, source, 4); return (float) (v / 2147483648.0); }
        case SampleType::unsupported: break;
    }
    return 0.0f;
}

void writeSample (uint8_t* destination, SampleType type, float value)
{
    switch (type)
    {
        case SampleType::float32: std::memcpy (destination, &value, 4); break;
        case SampleType::int16:
        {
            const auto v = (int16_t) toInteger (value, INT16_SCALE);
            std::memcpy (destination, &v, 2);
            break;
        }
        case SampleType::int24:
        {
            const auto v = (uint32_t) toInteger (value, INT24_SCALE);
            destination[0] = (uint8_t) v;
            destination[1] = (uint8_t) (v >> 8);
            destination[2] = (uint8_t) (v >> 16);
            break;
        }
        case SampleType::int32:
        {
            const auto v = toInteger (value, INT32_SCALE);
            std::memcpy (destination, &v, 4);
            break;
        }
        case SampleType::unsupported: break;
    }
}

Downmix downmixFor (int channels, uint32_t channelMask)
{
    Downmix mix;
    mix.channels = std::clamp (channels, 0, MAX_CHANNELS);
    if (mix.channels == 1)
    {
        mix.left[0] = mix.right[0] = 1.0f;
        return mix;
    }
    if (mix.channels == 0)
        return mix;

    const uint32_t mask = channelMask != 0 ? channelMask : defaultMask (mix.channels);
    int index = 0;
    for (uint32_t bit = 1; bit != 0 && index < mix.channels; bit <<= 1)
    {
        if ((mask & bit) == 0)
            continue;
        const auto gains = speakerGains (bit);
        mix.left[index] = gains.left;
        mix.right[index] = gains.right;
        ++index;
    }
    if (std::all_of (mix.left, mix.left + mix.channels, [] (float g) { return g <= 0.0f; })
        || std::all_of (mix.right, mix.right + mix.channels, [] (float g) { return g <= 0.0f; }))
    {
        mix = Downmix { mix.channels };   // no usable front pair: take the first two channels
        mix.left[0] = 1.0f;
        mix.right[1] = 1.0f;
    }
    return mix;
}

void toStereo (const uint8_t* source, const StreamFormat& format, const Downmix& mix, float* stereo, int frames)
{
    const int frameBytes = format.bytesPerFrame(), channels = std::min (format.channels, mix.channels);
    for (int i = 0; i < frames; ++i, source += frameBytes)
    {
        float left = 0.0f, right = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float sample = readSample (source + ch * format.bytesPerSample, format.type);
            left += mix.left[ch] * sample;
            right += mix.right[ch] * sample;
        }
        stereo[2 * i] = left;
        stereo[2 * i + 1] = right;
    }
}

void fromStereo (const float* stereo, const StreamFormat& format, const StereoMap& map, uint8_t* destination, int frames)
{
    const int frameBytes = format.bytesPerFrame(), sampleBytes = format.bytesPerSample;
    std::memset (destination, 0, (size_t) frameBytes * (size_t) std::max (frames, 0));
    for (int i = 0; i < frames; ++i, destination += frameBytes)
    {
        const float left = stereo[2 * i], right = stereo[2 * i + 1];
        if (map.mono)
        {
            writeSample (destination, format.type, 0.5f * (left + right));
            continue;
        }
        writeSample (destination + map.left * sampleBytes, format.type, left);
        writeSample (destination + map.right * sampleBytes, format.type, right);
        if (map.rearLeft >= 0)
            writeSample (destination + map.rearLeft * sampleBytes, format.type, left);
        if (map.rearRight >= 0)
            writeSample (destination + map.rearRight * sampleBytes, format.type, right);
    }
}
} // namespace WasapiFormat
