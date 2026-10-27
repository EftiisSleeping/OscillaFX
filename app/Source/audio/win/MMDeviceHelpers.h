#pragma once

#include "audio/win/WasapiFormat.h"

#include <juce_core/juce_core.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <atomic>

// Thin wrappers over the MMDevice API, IPolicyConfig and COM plumbing used by AudioEngine_win.cpp.
// Message thread unless noted.
namespace MMDeviceHelpers
{
using Microsoft::WRL::ComPtr;

// Initialises COM on this thread for the scope's lifetime (tolerates an existing apartment).
class ComScope
{
public:
    explicit ComScope (DWORD model = COINIT_APARTMENTTHREADED) : result (CoInitializeEx (nullptr, model)) {}
    ~ComScope() { if (SUCCEEDED (result)) CoUninitialize(); }
    ComScope (const ComScope&) = delete;
    ComScope& operator= (const ComScope&) = delete;

private:
    HRESULT result;
};

// Owns a Win32 event handle. Auto-reset releases one waiter per SetEvent (WASAPI buffer events);
// manual-reset stays signalled for every waiter until ResetEvent (a stop signal shared by threads).
class Event
{
public:
    explicit Event (bool manualReset = false) : handle (CreateEventW (nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr)) {}
    ~Event() { if (handle != nullptr) CloseHandle (handle); }
    Event (const Event&) = delete;
    Event& operator= (const Event&) = delete;
    HANDLE get() const { return handle; }

private:
    HANDLE handle;
};

// IUnknown for a single-interface COM object we implement; created with refcount 1.
template <typename Interface>
class ComObject : public Interface
{
public:
    virtual ~ComObject() = default;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        if (iid == __uuidof (IUnknown) || iid == __uuidof (Interface))
        {
            AddRef();
            *object = static_cast<Interface*> (this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

private:
    std::atomic<ULONG> references { 1 };
};

struct Endpoint
{
    juce::String id, name, adapterName, enumerator;
    int formFactor = -1;
    DWORD state = 0;
};

ComPtr<IMMDeviceEnumerator> createEnumerator();
ComPtr<IMMDevice> deviceById (IMMDeviceEnumerator&, const juce::String& id);
Endpoint describe (IMMDevice&);

// Render endpoints in `stateMask` (DEVICE_STATE_*), in enumeration order.
juce::Array<Endpoint> renderEndpoints (IMMDeviceEnumerator&, DWORD stateMask = DEVICE_STATE_ACTIVE);
juce::String defaultRenderId (IMMDeviceEnumerator&, ERole);
bool isActiveRender (IMMDeviceEnumerator&, const juce::String& id);

// FxSound's signed driver (hardware id Root\FXVAD): matched by name as FxSound does, the endpoint
// name may carry a "2- " prefix and the description part can be renamed by the user.
bool isFxSoundEndpoint (const Endpoint&);
juce::String findFxSoundEndpoint (IMMDeviceEnumerator&);   // active one, empty if none

// Undocumented IPolicyConfig (as FxSound and the Sound control panel use it).
bool setDefaultEndpoint (const juce::String& id, ERole);
// Sets the endpoint's shared-mode format to `sampleRate` / `channels`, keeping its sample size.
// Only writes when something differs. Returns true if the format now matches.
bool matchDeviceFormat (const juce::String& id, int sampleRate, int channels);

WasapiFormat::StreamFormat streamFormat (const WAVEFORMATEX&);
bool hasSystemBattery();
} // namespace MMDeviceHelpers
