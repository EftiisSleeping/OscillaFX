#pragma once

#include "core/DeviceKind.h"

// Maps the facts Windows reports for a render endpoint onto the CoreAudio-style DeviceKinds::Traits
// the shared classifier reads. Pure; DeviceTraits_win.cpp reads the facts from the property store.
namespace EndpointTraits
{
struct Facts
{
    juce::String uid, name;          // endpoint id, PKEY_Device_FriendlyName ("Speakers (Realtek(R) Audio)")
    juce::String adapterName;        // PKEY_DeviceInterface_FriendlyName ("Realtek(R) Audio")
    juce::String enumerator;         // PKEY_Device_EnumeratorName: "HDAUDIO", "USB", "BTHENUM", "BTHHFENUM" ...
    int formFactor = -1;             // PKEY_AudioEndpoint_FormFactor (EndpointFormFactor), -1 unknown
    bool hasSystemBattery = true;    // laptop: HD Audio "Speakers" are the built-in ones
};

// EndpointFormFactor values (mmdeviceapi.h).
enum FormFactor
{
    remoteNetworkDevice = 0, speakers = 1, lineLevel = 2, headphones = 3, microphone = 4, headset = 5,
    handset = 6, unknownDigitalPassthrough = 7, spdif = 8, digitalAudioDisplayDevice = 9
};

DeviceKinds::Traits traitsFor (const Facts&);
} // namespace EndpointTraits
