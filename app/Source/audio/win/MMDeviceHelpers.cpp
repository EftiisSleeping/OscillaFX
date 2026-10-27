#include "audio/win/MMDeviceHelpers.h"
#include "platform/win/WinEndpoints.h"

#include <devicetopology.h>
#include <mmreg.h>

#pragma comment (lib, "ole32.lib")

namespace MMDeviceHelpers
{
namespace
{
// Property keys, defined here so no INITGUID / extra import library is needed.
constexpr PROPERTYKEY KEY_DEVICE_FRIENDLY_NAME  { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
constexpr PROPERTYKEY KEY_DEVICE_ENUMERATOR     { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24 };
constexpr PROPERTYKEY KEY_ADAPTER_FRIENDLY_NAME { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
constexpr PROPERTYKEY KEY_ENDPOINT_FORM_FACTOR  { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

constexpr GUID SUBTYPE_PCM        { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
constexpr GUID SUBTYPE_IEEE_FLOAT { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// Name of the driver (FxSound's INF), and its pre-rename name still found on old installs.
const wchar_t* const FXSOUND_NAMES[] = { WinEndpoints::FXSOUND_ENDPOINT_NAME, L"DFX Audio Enhancer" };

constexpr DWORD STEREO_MASK = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

// ---------------------------------------------------------------- properties

juce::String takeString (LPWSTR text)
{
    juce::String result (text != nullptr ? juce::String (text) : juce::String());
    CoTaskMemFree (text);
    return result;
}

juce::String stringProperty (IPropertyStore& store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit (&value);
    juce::String result;
    if (SUCCEEDED (store.GetValue (key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        result = value.pwszVal;
    PropVariantClear (&value);
    return result;
}

int intProperty (IPropertyStore& store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit (&value);
    int result = -1;
    if (SUCCEEDED (store.GetValue (key, &value)) && value.vt == VT_UI4)
        result = (int) value.ulVal;
    PropVariantClear (&value);
    return result;
}

// "\\?\hdaudio#func_01&ven_10ec..." -> "HDAUDIO": the bus the endpoint's adapter sits on, for
// endpoints whose property store lacks the enumerator name.
juce::String enumeratorFromTopology (IMMDevice& device)
{
    ComPtr<IDeviceTopology> topology;
    ComPtr<IConnector> connector;
    LPWSTR adapterId = nullptr;
    if (FAILED (device.Activate (__uuidof (IDeviceTopology), CLSCTX_ALL, nullptr, (void**) topology.GetAddressOf()))
        || FAILED (topology->GetConnector (0, &connector))
        || FAILED (connector->GetDeviceIdConnectedTo (&adapterId)))
        return {};
    auto path = takeString (adapterId);
    return path.fromFirstOccurrenceOf ("\\\\?\\", false, false).upToFirstOccurrenceOf ("#", false, false).toUpperCase();
}
} // namespace

ComPtr<IMMDeviceEnumerator> createEnumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator));
    return enumerator;
}

ComPtr<IMMDevice> deviceById (IMMDeviceEnumerator& enumerator, const juce::String& id)
{
    ComPtr<IMMDevice> device;
    if (id.isNotEmpty())
        enumerator.GetDevice (id.toWideCharPointer(), &device);
    return device;
}

Endpoint describe (IMMDevice& device)
{
    Endpoint endpoint;
    LPWSTR id = nullptr;
    if (SUCCEEDED (device.GetId (&id)))
        endpoint.id = takeString (id);
    device.GetState (&endpoint.state);

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED (device.OpenPropertyStore (STGM_READ, &store)))
    {
        endpoint.name = stringProperty (*store.Get(), KEY_DEVICE_FRIENDLY_NAME);
        endpoint.adapterName = stringProperty (*store.Get(), KEY_ADAPTER_FRIENDLY_NAME);
        endpoint.enumerator = stringProperty (*store.Get(), KEY_DEVICE_ENUMERATOR).toUpperCase();
        endpoint.formFactor = intProperty (*store.Get(), KEY_ENDPOINT_FORM_FACTOR);
    }
    if (endpoint.enumerator.isEmpty() || endpoint.enumerator == "MMDEVAPI" || endpoint.enumerator == "SWD")
        endpoint.enumerator = enumeratorFromTopology (device);
    return endpoint;
}

juce::Array<Endpoint> renderEndpoints (IMMDeviceEnumerator& enumerator, DWORD stateMask)
{
    juce::Array<Endpoint> result;
    ComPtr<IMMDeviceCollection> collection;
    UINT count = 0;
    if (FAILED (enumerator.EnumAudioEndpoints (eRender, stateMask, &collection)) || FAILED (collection->GetCount (&count)))
        return result;
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED (collection->Item (i, &device)))
            result.add (describe (*device.Get()));
    }
    return result;
}

juce::String defaultRenderId (IMMDeviceEnumerator& enumerator, ERole role)
{
    ComPtr<IMMDevice> device;
    LPWSTR id = nullptr;
    if (FAILED (enumerator.GetDefaultAudioEndpoint (eRender, role, &device)) || FAILED (device->GetId (&id)))
        return {};
    return takeString (id);
}

bool isActiveRender (IMMDeviceEnumerator& enumerator, const juce::String& id)
{
    auto device = deviceById (enumerator, id);
    DWORD state = 0;
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow = eCapture;
    return device != nullptr && SUCCEEDED (device->GetState (&state)) && state == DEVICE_STATE_ACTIVE
        && SUCCEEDED (device.As (&endpoint)) && SUCCEEDED (endpoint->GetDataFlow (&flow)) && flow == eRender;
}

bool isFxSoundEndpoint (const Endpoint& endpoint)
{
    for (auto* name : FXSOUND_NAMES)
        if (endpoint.name.contains (juce::String (name)) || endpoint.adapterName.contains (juce::String (name)))
            return true;
    return false;
}

juce::String findFxSoundEndpoint (IMMDeviceEnumerator& enumerator)
{
    for (auto& endpoint : renderEndpoints (enumerator))
        if (isFxSoundEndpoint (endpoint))
            return endpoint.id;
    return {};
}

bool setDefaultEndpoint (const juce::String& id, ERole role)
{
    auto policy = WinEndpoints::detail::createPolicyConfig();
    return policy != nullptr && SUCCEEDED (policy->SetDefaultEndpoint (id.toWideCharPointer(), role));
}

bool matchDeviceFormat (const juce::String& id, int sampleRate, int channels)
{
    auto policy = WinEndpoints::detail::createPolicyConfig();
    WAVEFORMATEXTENSIBLE* format = nullptr;
    if (policy == nullptr || FAILED (policy->GetDeviceFormat (id.toWideCharPointer(), FALSE, (void**) &format)) || format == nullptr)
        return false;

    auto& base = format->Format;
    bool matches = (int) base.nSamplesPerSec == sampleRate && base.nChannels == channels;
    if (! matches)
    {
        base.nSamplesPerSec = (DWORD) sampleRate;
        base.nChannels = (WORD) channels;
        base.nBlockAlign = (WORD) (channels * base.wBitsPerSample / 8);
        base.nAvgBytesPerSec = base.nSamplesPerSec * base.nBlockAlign;
        if (base.wFormatTag == WAVE_FORMAT_EXTENSIBLE && channels == 2)
            format->dwChannelMask = STEREO_MASK;
        matches = SUCCEEDED (policy->SetDeviceFormat (id.toWideCharPointer(), format, nullptr));
    }
    CoTaskMemFree (format);
    return matches;
}

WasapiFormat::StreamFormat streamFormat (const WAVEFORMATEX& format)
{
    bool isFloat = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = format.wFormatTag == WAVE_FORMAT_PCM;
    DWORD mask = 0;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22)
    {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&> (format);
        isFloat = IsEqualGUID (extensible.SubFormat, SUBTYPE_IEEE_FLOAT) != 0;
        isPcm = IsEqualGUID (extensible.SubFormat, SUBTYPE_PCM) != 0;
        mask = extensible.dwChannelMask;
    }
    return WasapiFormat::describe (isFloat, isPcm, format.wBitsPerSample, format.nChannels,
                                   (double) format.nSamplesPerSec, (uint32_t) mask);
}

bool hasSystemBattery()
{
    SYSTEM_POWER_STATUS status {};
    constexpr BYTE NO_SYSTEM_BATTERY = 128;
    return ! GetSystemPowerStatus (&status) || status.BatteryFlag != NO_SYSTEM_BATTERY;
}
} // namespace MMDeviceHelpers
