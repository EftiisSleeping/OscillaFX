#include "audio/win/EndpointTraits.h"

namespace EndpointTraits
{
namespace
{
using juce::uint32;

constexpr uint32 fourCC (const char (&c)[5])
{
    return ((uint32) (unsigned char) c[0] << 24) | ((uint32) (unsigned char) c[1] << 16)
         | ((uint32) (unsigned char) c[2] << 8) | (uint32) (unsigned char) c[3];
}

// The CoreAudio codes DeviceKinds::classify understands.
constexpr uint32 TRANSPORT_BUILT_IN = fourCC ("bltn"), TRANSPORT_USB = fourCC ("usb "),
                 TRANSPORT_BLUETOOTH = fourCC ("blue"), TRANSPORT_HDMI = fourCC ("hdmi");
constexpr uint32 SOURCE_INTERNAL_SPEAKER = fourCC ("ispk"), SOURCE_HEADPHONES = fourCC ("hdpn"),
                 SOURCE_EXTERNAL_SPEAKER = fourCC ("espk");

// Buses of the codec soldered on the mainboard (HD Audio, Intel SST, AMD ACP).
bool isOnboardEnumerator (const juce::String& enumerator)
{
    for (auto* name : { "HDAUDIO", "INTELAUDIO", "ACP", "ACPI", "PCI" })
        if (enumerator.equalsIgnoreCase (name))
            return true;
    return false;
}

uint32 onboardDataSource (const Facts& f)
{
    switch (f.formFactor)
    {
        case headphones:
        case headset:                   return SOURCE_HEADPHONES;
        case speakers:                  return f.hasSystemBattery ? SOURCE_INTERNAL_SPEAKER : SOURCE_EXTERNAL_SPEAKER;
        case lineLevel:
        case spdif:
        case unknownDigitalPassthrough: return SOURCE_EXTERNAL_SPEAKER;
        default:                        return 0;
    }
}

// Windows names most endpoints "Speakers (<adapter>)" whatever is plugged in; for USB the adapter
// name ("Logitech G Pro X", "Studio Display") says more.
juce::String usbName (const Facts& f)
{
    return f.formFactor == speakers && f.adapterName.isNotEmpty() ? f.adapterName : f.name;
}
} // namespace

DeviceKinds::Traits traitsFor (const Facts& f)
{
    DeviceKinds::Traits traits { f.uid, f.name, 0, 0 };
    if (f.formFactor == digitalAudioDisplayDevice)
        traits.transport = TRANSPORT_HDMI;
    else if (f.enumerator.startsWithIgnoreCase ("BTH"))
        traits.transport = TRANSPORT_BLUETOOTH;
    else if (f.enumerator.equalsIgnoreCase ("USB"))
    {
        traits.transport = TRANSPORT_USB;
        traits.name = usbName (f);
    }
    else if (isOnboardEnumerator (f.enumerator))
    {
        traits.transport = TRANSPORT_BUILT_IN;
        traits.dataSource = onboardDataSource (f);
    }
    return traits;
}
} // namespace EndpointTraits
