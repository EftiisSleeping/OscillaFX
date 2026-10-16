#pragma once

#include <juce_core/juce_core.h>

// What kind of output a device is, from CoreAudio facts. Drives the starter-profile suggestions.
enum class DeviceKind
{
    builtInSpeakers,      // laptop / iMac / Mac mini speakers
    headphonesWired,      // 3.5 mm jack
    headphonesUsb,        // USB headset, DAC or interface (assumed to drive headphones)
    headphonesBluetooth,  // generic Bluetooth headphones / earbuds
    airPods,              // AirPods, Beats and other Apple-tuned earbuds
    display,              // HDMI / DisplayPort: monitor or TV speakers, AV receiver
    externalSpeakers,     // Bluetooth / USB / AirPlay speakers, soundbars, line out
    unknown
};

namespace DeviceKinds
{
// The device facts the classifier looks at, in CoreAudio terms (other backends map their facts onto
// these codes or leave them 0 and rely on the name). Tests build them by hand.
struct Traits
{
    juce::String uid, name;
    juce::uint32 transport = 0;    // kAudioDeviceTransportType* four-char code
    juce::uint32 dataSource = 0;   // output data source four-char code ('ispk', 'hdpn', ...)
};

DeviceKind classify (const Traits&);              // pure
// Per audio backend (audio/<os>/DeviceTraits_<os>.cpp): reads the OS; name-only traits if the device is gone.
Traits traitsFor (const juce::String& uid, const juce::String& name);

juce::String id (DeviceKind);     // stable key: "builtin-speakers", "headphones", "usb-headphones", "bluetooth-headphones",
                                  // "airpods", "display", "external-speakers", "unknown"
juce::String label (DeviceKind);  // for the UI: "Built-in speakers", "Headphones", ...
} // namespace DeviceKinds
