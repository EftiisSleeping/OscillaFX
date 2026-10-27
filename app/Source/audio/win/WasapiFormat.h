#pragma once

#include <cstdint>

// Shared-mode WASAPI stream layouts and the conversions between them and the interleaved stereo float
// the DSP runs on. Pure (no Windows headers), allocation-free, safe on the audio threads.
namespace WasapiFormat
{
enum class SampleType
{
    float32,
    int16,
    int24,       // packed, 3 bytes
    int32,       // 32-bit container, also 24-in-32 (valid bits are MSB-aligned)
    unsupported
};

struct StreamFormat
{
    SampleType type = SampleType::unsupported;
    int channels = 0;
    int bytesPerSample = 0;
    double sampleRate = 0;
    uint32_t channelMask = 0;   // WAVEFORMATEXTENSIBLE dwChannelMask, 0 if not given

    int bytesPerFrame() const { return channels * bytesPerSample; }
    bool isUsable() const { return type != SampleType::unsupported && channels > 0 && sampleRate > 0; }
};

// `isFloat` / `isPcm` come from the format tag or the extensible sub-format.
StreamFormat describe (bool isFloat, bool isPcm, int bitsPerSample, int channels, double sampleRate, uint32_t channelMask);

// Where stereo lives in a device frame. Speakers are ordered by their bit in the channel mask.
struct StereoMap
{
    bool mono = false;
    int left = 0, right = 1;
    int rearLeft = -1, rearRight = -1;   // quad only: stereo is copied to the back pair too (as FxSound)
};

StereoMap stereoMapFor (int channels, uint32_t channelMask);

float readSample (const uint8_t* source, SampleType);
void writeSample (uint8_t* destination, SampleType, float value);   // clamps to the integer range

// Capture side: how each source channel feeds left / right. Stereo is a straight copy, mono is
// duplicated, multichannel (when the virtual endpoint could not be set to stereo) is downmixed:
// centre and side / back speakers at -3 dB, LFE and back centre at -6 dB. The safety limiter at the
// end of the chain catches the extra headroom this can take.
constexpr int MAX_CHANNELS = 18;

struct Downmix
{
    int channels = 0;
    float left[MAX_CHANNELS] {}, right[MAX_CHANNELS] {};
};

Downmix downmixFor (int channels, uint32_t channelMask);

// Device frames -> interleaved stereo.
void toStereo (const uint8_t* source, const StreamFormat&, const Downmix&, float* stereo, int frames);

// Interleaved stereo -> device frames; channels outside the map are silent, mono gets (L + R) / 2.
void fromStereo (const float* stereo, const StreamFormat&, const StereoMap&, uint8_t* destination, int frames);
} // namespace WasapiFormat
