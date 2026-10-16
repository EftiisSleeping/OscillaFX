#include "DeviceKind.h"

namespace
{
using juce::uint32;

constexpr uint32 fourCC (const char (&c)[5])
{
    return ((uint32) (unsigned char) c[0] << 24) | ((uint32) (unsigned char) c[1] << 16)
         | ((uint32) (unsigned char) c[2] << 8) | (uint32) (unsigned char) c[3];
}

constexpr uint32 TRANSPORT_BUILT_IN = fourCC ("bltn"), TRANSPORT_USB = fourCC ("usb "),
                 TRANSPORT_BLUETOOTH = fourCC ("blue"), TRANSPORT_BLUETOOTH_LE = fourCC ("blea"),
                 TRANSPORT_HDMI = fourCC ("hdmi"), TRANSPORT_DISPLAY_PORT = fourCC ("dprt"),
                 TRANSPORT_AIRPLAY = fourCC ("airp"), TRANSPORT_THUNDERBOLT = fourCC ("thun"),
                 TRANSPORT_FIREWIRE = fourCC ("1394"), TRANSPORT_PCI = fourCC ("pci ");
constexpr uint32 SOURCE_HEADPHONES = fourCC ("hdpn"), SOURCE_EXTERNAL_SPEAKER = fourCC ("espk");

bool containsAny (const juce::String& text, std::initializer_list<const char*> needles)
{
    for (auto* needle : needles)
        if (text.containsIgnoreCase (needle))
            return true;
    return false;
}

// Whole-word match for short needles that would otherwise hit inside other words ("tv", "avr").
bool hasWord (const juce::String& text, const char* word)
{
    return juce::StringArray::fromTokens (text.toLowerCase(), " -_/.,()[]:", "").contains (word);
}

bool isAppleBuds (const juce::String& name)
{
    return containsAny (name, { "airpods", "beats", "powerbeats", "earpods" });
}

bool nameSaysHeadphones (const juce::String& name)
{
    return containsAny (name, { "headphone", "headset", "earphone", "earbud", "in-ear" })
        || hasWord (name, "buds") || hasWord (name, "iem");
}

bool nameSaysSpeakers (const juce::String& name)
{
    return (containsAny (name, { "speaker", "soundbar", "sound bar", "monitor", "homepod", "boombox", "sonos", "line out", "line-out" })
            || hasWord (name, "echo"))
        && ! nameSaysHeadphones (name);
}

bool nameSaysBuiltIn (const juce::String& name)
{
    return containsAny (name, { "macbook", "imac", "mac mini", "mac studio", "mac pro", "built-in", "internal speaker" });
}

bool nameSaysDisplay (const juce::String& name)
{
    return containsAny (name, { "hdmi", "displayport", "display", "receiver" }) || hasWord (name, "tv") || hasWord (name, "avr");
}

bool isBluetooth (uint32 transport) { return transport == TRANSPORT_BLUETOOTH || transport == TRANSPORT_BLUETOOTH_LE; }

bool isWiredBus (uint32 transport)
{
    return transport == TRANSPORT_USB || transport == TRANSPORT_THUNDERBOLT
        || transport == TRANSPORT_FIREWIRE || transport == TRANSPORT_PCI;
}
} // namespace

namespace DeviceKinds
{
DeviceKind classify (const Traits& t)
{
    if (isBluetooth (t.transport))
    {
        if (isAppleBuds (t.name))
            return DeviceKind::airPods;
        return nameSaysSpeakers (t.name) ? DeviceKind::externalSpeakers : DeviceKind::headphonesBluetooth;
    }
    if (t.transport == TRANSPORT_HDMI || t.transport == TRANSPORT_DISPLAY_PORT)
        return DeviceKind::display;
    if (t.transport == TRANSPORT_AIRPLAY)
        return DeviceKind::externalSpeakers;

    if (t.transport == TRANSPORT_BUILT_IN)
    {
        if (t.dataSource == SOURCE_EXTERNAL_SPEAKER || containsAny (t.name, { "line out", "line-out" }))
            return DeviceKind::externalSpeakers;
        if (t.dataSource == SOURCE_HEADPHONES || t.uid.containsIgnoreCase ("headphone") || nameSaysHeadphones (t.name))
            return DeviceKind::headphonesWired;
        return DeviceKind::builtInSpeakers;
    }

    if (isWiredBus (t.transport))
    {
        if (nameSaysDisplay (t.name) && ! nameSaysHeadphones (t.name))
            return DeviceKind::display;
        if (nameSaysHeadphones (t.name))
            return DeviceKind::headphonesUsb;
        if (nameSaysSpeakers (t.name))
            return DeviceKind::externalSpeakers;
        return DeviceKind::headphonesUsb;
    }

    // Virtual, aggregate or unreported transport: only an unmistakable name helps.
    if (isAppleBuds (t.name))
        return DeviceKind::airPods;
    if (nameSaysHeadphones (t.name))
        return DeviceKind::headphonesWired;
    if (nameSaysBuiltIn (t.name) && ! nameSaysDisplay (t.name))
        return DeviceKind::builtInSpeakers;
    if (nameSaysDisplay (t.name))
        return DeviceKind::display;
    if (nameSaysSpeakers (t.name))
        return DeviceKind::externalSpeakers;
    return DeviceKind::unknown;
}

juce::String id (DeviceKind kind)
{
    switch (kind)
    {
        case DeviceKind::builtInSpeakers:     return "builtin-speakers";
        case DeviceKind::headphonesWired:     return "headphones";
        case DeviceKind::headphonesUsb:       return "usb-headphones";
        case DeviceKind::headphonesBluetooth: return "bluetooth-headphones";
        case DeviceKind::airPods:             return "airpods";
        case DeviceKind::display:             return "display";
        case DeviceKind::externalSpeakers:    return "external-speakers";
        case DeviceKind::unknown:             break;
    }
    return "unknown";
}

juce::String label (DeviceKind kind)
{
    switch (kind)
    {
        case DeviceKind::builtInSpeakers:     return "Built-in speakers";
        case DeviceKind::headphonesWired:     return "Headphones";
        case DeviceKind::headphonesUsb:       return "USB headphones / DAC";
        case DeviceKind::headphonesBluetooth: return "Bluetooth headphones";
        case DeviceKind::airPods:             return "AirPods";
        case DeviceKind::display:             return "HDMI / display speakers";
        case DeviceKind::externalSpeakers:    return "External speakers";
        case DeviceKind::unknown:             break;
    }
    return "Output device";
}
} // namespace DeviceKinds
