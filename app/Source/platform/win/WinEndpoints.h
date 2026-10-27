#pragma once

// Default render endpoint access shared by the app (platform/win/CrashGuard_win.cpp) and the crash
// watchdog (helper/win/OscillaWatchdog.cpp). No JUCE; header-only. The caller initialises COM.
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cwctype>
#include <string>
#include <vector>

namespace WinEndpoints
{
// FxSound's signed virtual driver: its INF names the endpoint, OscillaFX cannot rename it.
constexpr const wchar_t* FXSOUND_ENDPOINT_NAME = L"FxSound Audio Enhancer";

struct Endpoint
{
    std::wstring id;    // IMMDevice::GetId
    std::wstring name;  // PKEY_Device_FriendlyName, e.g. "Speakers (FxSound Audio Enhancer)"
};

inline std::wstring lowercase (std::wstring text)
{
    for (auto& c : text)
        c = (wchar_t) std::towlower (c);
    return text;
}

inline bool isFxSound (const Endpoint& endpoint)
{
    return lowercase (endpoint.name).find (lowercase (FXSOUND_ENDPOINT_NAME)) != std::wstring::npos;
}

namespace detail
{
using Microsoft::WRL::ComPtr;

// Undocumented IPolicyConfigVista (Vista and later, as FxSound's audiopassthru/include/PolicyConfig.h):
// only the vtable order matters, the unused methods keep their slots.
struct __declspec (novtable) PolicyConfigVista : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat (PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat (PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod (PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod (PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint (PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility (PCWSTR, INT) = 0;
};

// CPolicyConfigVistaClient / IPolicyConfigVista
constexpr CLSID CLSID_POLICY_CONFIG_VISTA = { 0x294935ce, 0xf637, 0x4e7c, { 0xa4, 0x1b, 0xab, 0x25, 0x54, 0x60, 0xb8, 0x62 } };
constexpr IID IID_POLICY_CONFIG_VISTA = { 0x568b9108, 0x44bf, 0x40b4, { 0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20 } };

// PKEY_Device_FriendlyName, defined here so no translation unit needs INITGUID.
constexpr PROPERTYKEY FRIENDLY_NAME_KEY = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Also used by the audio engine (audio/win/MMDeviceHelpers.cpp): the one definition of the interface.
inline ComPtr<PolicyConfigVista> createPolicyConfig()
{
    ComPtr<PolicyConfigVista> policy;
    CoCreateInstance (CLSID_POLICY_CONFIG_VISTA, nullptr, CLSCTX_ALL, IID_POLICY_CONFIG_VISTA, (void**) policy.GetAddressOf());
    return policy;
}

inline ComPtr<IMMDeviceEnumerator> enumerator()
{
    ComPtr<IMMDeviceEnumerator> result;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&result));
    return result;
}

inline Endpoint describe (IMMDevice* device)
{
    Endpoint endpoint;
    LPWSTR id = nullptr;
    if (SUCCEEDED (device->GetId (&id)) && id != nullptr)
    {
        endpoint.id = id;
        CoTaskMemFree (id);
    }
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &properties)))
    {
        PROPVARIANT name;
        PropVariantInit (&name);
        if (SUCCEEDED (properties->GetValue (FRIENDLY_NAME_KEY, &name)) && name.vt == VT_LPWSTR)
            endpoint.name = name.pwszVal;
        PropVariantClear (&name);
    }
    return endpoint;
}
} // namespace detail

inline std::vector<Endpoint> activeRenderEndpoints()
{
    std::vector<Endpoint> result;
    auto devices = detail::enumerator();
    detail::ComPtr<IMMDeviceCollection> collection;
    if (devices == nullptr || FAILED (devices->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)))
        return result;
    UINT count = 0;
    collection->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        detail::ComPtr<IMMDevice> device;
        if (SUCCEEDED (collection->Item (i, &device)))
            result.push_back (detail::describe (device.Get()));
    }
    return result;
}

inline Endpoint defaultRenderEndpoint()
{
    auto devices = detail::enumerator();
    detail::ComPtr<IMMDevice> device;
    if (devices == nullptr || FAILED (devices->GetDefaultAudioEndpoint (eRender, eConsole, &device)))
        return {};
    return detail::describe (device.Get());
}

// Makes `endpointId` the default for console, multimedia and communications. True if all three took.
inline bool setDefaultForAllRoles (const wchar_t* endpointId)
{
    auto policy = detail::createPolicyConfig();
    if (policy == nullptr)
        return false;
    bool allSet = true;
    for (auto role : { eConsole, eMultimedia, eCommunications })
        allSet &= SUCCEEDED (policy->SetDefaultEndpoint (endpointId, role));
    return allSet;
}
} // namespace WinEndpoints
