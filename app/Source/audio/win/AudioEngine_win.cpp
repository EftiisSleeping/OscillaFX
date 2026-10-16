#include "audio/AudioEngine.h"

#include <algorithm>

// Stub so the app builds and runs on Windows. The real backend captures FxSound's virtual
// endpoint over WASAPI, runs DfxDsp and renders to the real endpoint (see docs/PLATFORMS.md).
struct AudioEngine::Impl
{
    juce::String lastError { "Windows audio engine not implemented yet" };
};

AudioEngine::AudioEngine() : impl (std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::start() { return false; }
void AudioEngine::stop() {}
juce::String AudioEngine::lastError() const { return impl->lastError; }

void AudioEngine::applySettings (const DspSettings&) {}

juce::Array<OutputDevice> AudioEngine::outputDevices() const { return {}; }
OutputDevice AudioEngine::currentOutputDevice() const { return {}; }
void AudioEngine::setOutputDevice (const juce::String&) {}

void AudioEngine::getSpectrum (float* bands10) const { std::fill (bands10, bands10 + 10, 0.0f); }
int AudioEngine::getScopeSamples (float*, float*, int) const { return 0; }
