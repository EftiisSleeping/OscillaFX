#include "audio/mac/CoreAudioHelpers.h"

namespace CoreAudioHelpers
{
namespace
{
constexpr AudioObjectPropertyScope OUTPUT = kAudioObjectPropertyScopeOutput;
constexpr AudioObjectPropertyScope GLOBAL = kAudioObjectPropertyScopeGlobal;
constexpr AudioObjectPropertyElement MAIN = kAudioObjectPropertyElementMain;
constexpr UInt32 STEREO_ELEMENTS[] = { 1, 2 };

AudioObjectPropertyAddress address (AudioObjectPropertySelector selector,
                                    AudioObjectPropertyScope scope = GLOBAL,
                                    AudioObjectPropertyElement element = MAIN)
{
    return { selector, scope, element };
}

template <typename T>
bool getValue (AudioObjectID id, const AudioObjectPropertyAddress& addr, T& out)
{
    UInt32 size = sizeof (T);
    return AudioObjectHasProperty (id, &addr)
        && AudioObjectGetPropertyData (id, &addr, 0, nullptr, &size, &out) == noErr;
}

template <typename T>
bool setValue (AudioObjectID id, const AudioObjectPropertyAddress& addr, const T& value)
{
    return AudioObjectSetPropertyData (id, &addr, 0, nullptr, sizeof (T), &value) == noErr;
}

bool isSettable (AudioObjectID id, const AudioObjectPropertyAddress& addr)
{
    Boolean settable = false;
    return AudioObjectHasProperty (id, &addr)
        && AudioObjectIsPropertySettable (id, &addr, &settable) == noErr && settable;
}

juce::String stringProperty (AudioObjectID id, AudioObjectPropertySelector selector)
{
    CFStringRef value = nullptr;
    if (! getValue (id, address (selector), value) || value == nullptr)
        return {};
    auto result = juce::String::fromCFString (value);
    CFRelease (value);
    return result;
}

// Elements carrying the output volume: master if settable, else the first stereo pair.
std::vector<UInt32> volumeElements (AudioObjectID id, AudioObjectPropertySelector selector)
{
    if (isSettable (id, address (selector, OUTPUT, MAIN)))
        return { MAIN };
    std::vector<UInt32> elements;
    for (auto element : STEREO_ELEMENTS)
        if (isSettable (id, address (selector, OUTPUT, element)))
            elements.push_back (element);
    return elements;
}

template <typename T>
T firstElementValue (AudioObjectID id, AudioObjectPropertySelector selector, T fallback)
{
    for (auto element : volumeElements (id, selector))
    {
        T value {};
        if (getValue (id, address (selector, OUTPUT, element), value))
            return value;
    }
    return fallback;
}
} // namespace

std::vector<AudioObjectID> allDevices()
{
    auto addr = address (kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    ids.resize (size / sizeof (AudioObjectID));
    return ids;
}

juce::String deviceUid (AudioObjectID id)  { return stringProperty (id, kAudioDevicePropertyDeviceUID); }
juce::String deviceName (AudioObjectID id) { return stringProperty (id, kAudioObjectPropertyName); }

AudioObjectID findDeviceByUid (const juce::String& uid)
{
    for (auto id : allDevices())
        if (deviceUid (id) == uid)
            return id;
    return kAudioObjectUnknown;
}

AudioObjectID findDeviceByName (const juce::String& name)
{
    for (auto id : allDevices())
        if (deviceName (id) == name)
            return id;
    return kAudioObjectUnknown;
}

int outputChannelCount (AudioObjectID id) { return channelCount (id, false); }

int channelCount (AudioObjectID id, bool isInput)
{
    auto addr = address (kAudioDevicePropertyStreamConfiguration, isInput ? kAudioObjectPropertyScopeInput : OUTPUT);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (id, &addr, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    juce::HeapBlock<char> storage (size);
    auto* list = reinterpret_cast<AudioBufferList*> (storage.get());
    if (AudioObjectGetPropertyData (id, &addr, 0, nullptr, &size, list) != noErr)
        return 0;
    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += (int) list->mBuffers[i].mNumberChannels;
    return channels;
}

juce::StringArray subDeviceUids (AudioObjectID id)
{
    juce::StringArray uids;
    CFArrayRef list = nullptr;
    if (! getValue (id, address (kAudioAggregateDevicePropertyFullSubDeviceList), list) || list == nullptr)
        return uids;
    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
        if (auto uid = (CFStringRef) CFArrayGetValueAtIndex (list, i); uid != nullptr && CFGetTypeID (uid) == CFStringGetTypeID())
            uids.add (juce::String::fromCFString (uid));
    CFRelease (list);
    return uids;
}

std::pair<int, int> preferredStereoChannels (AudioObjectID id)
{
    UInt32 channels[2] = { 1, 2 };
    auto addr = address (kAudioDevicePropertyPreferredChannelsForStereo, OUTPUT);
    UInt32 size = sizeof (channels);
    if (AudioObjectHasProperty (id, &addr))
        AudioObjectGetPropertyData (id, &addr, 0, nullptr, &size, channels);
    const int total = outputChannelCount (id);
    auto toIndex = [total] (UInt32 c, int fallback) { return (c >= 1 && (int) c <= total) ? (int) c - 1 : fallback; };
    return { toIndex (channels[0], 0), toIndex (channels[1], std::min (1, std::max (total - 1, 0))) };
}

bool isBuiltIn (AudioObjectID id)
{
    UInt32 transport = 0;
    return getValue (id, address (kAudioDevicePropertyTransportType), transport)
        && transport == kAudioDeviceTransportTypeBuiltIn;
}

UInt32 transportType (AudioObjectID id)
{
    UInt32 transport = 0;
    getValue (id, address (kAudioDevicePropertyTransportType), transport);
    return transport;
}

UInt32 outputDataSource (AudioObjectID id)
{
    UInt32 source = 0;
    getValue (id, address (kAudioDevicePropertyDataSource, OUTPUT), source);
    return source;
}

bool isAlive (AudioObjectID id)
{
    UInt32 alive = 0;
    return id != kAudioObjectUnknown && getValue (id, address (kAudioDevicePropertyDeviceIsAlive), alive) && alive != 0;
}

AudioObjectID defaultOutputDevice()
{
    AudioObjectID id = kAudioObjectUnknown;
    getValue (kAudioObjectSystemObject, address (kAudioHardwarePropertyDefaultOutputDevice), id);
    return id;
}

bool setDefaultOutputDevice (AudioObjectID id)
{
    return setValue (kAudioObjectSystemObject, address (kAudioHardwarePropertyDefaultOutputDevice), id);
}

double nominalSampleRate (AudioObjectID id)
{
    Float64 rate = 0;
    getValue (id, address (kAudioDevicePropertyNominalSampleRate), rate);
    return rate;
}

bool setNominalSampleRate (AudioObjectID id, double rate)
{
    if (juce::exactlyEqual (nominalSampleRate (id), rate))
        return true;
    return setValue (id, address (kAudioDevicePropertyNominalSampleRate), (Float64) rate);
}

UInt32 bufferFrameSize (AudioObjectID id)
{
    UInt32 frames = 0;
    getValue (id, address (kAudioDevicePropertyBufferFrameSize), frames);
    return frames;
}

bool streamsAreFloat32 (AudioObjectID id, bool isInput)
{
    auto scope = isInput ? kAudioObjectPropertyScopeInput : OUTPUT;
    auto addr = address (kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (id, &addr, 0, nullptr, &size) != noErr || size == 0)
        return false;
    std::vector<AudioStreamID> streams (size / sizeof (AudioStreamID));
    if (AudioObjectGetPropertyData (id, &addr, 0, nullptr, &size, streams.data()) != noErr)
        return false;
    for (auto stream : streams)
    {
        AudioStreamBasicDescription format {};
        if (! getValue (stream, address (kAudioStreamPropertyVirtualFormat), format))
            return false;
        const bool isFloat = format.mFormatID == kAudioFormatLinearPCM
                          && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0
                          && format.mBitsPerChannel == 32;
        if (! isFloat)
            return false;
    }
    return true;
}

void disableStreams (AudioObjectID id, AudioDeviceIOProcID proc, bool isInput)
{
    auto addr = address (kAudioDevicePropertyIOProcStreamUsage, isInput ? kAudioObjectPropertyScopeInput : OUTPUT);
    UInt32 size = 0;
    if (! AudioObjectHasProperty (id, &addr) || AudioObjectGetPropertyDataSize (id, &addr, 0, nullptr, &size) != noErr)
        return;
    juce::HeapBlock<char> storage (size, true);
    auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*> (storage.get());
    usage->mIOProc = reinterpret_cast<void*> (proc);
    if (AudioObjectGetPropertyData (id, &addr, 0, nullptr, &size, usage) != noErr)
        return;
    for (UInt32 i = 0; i < usage->mNumberStreams; ++i)
        usage->mStreamIsOn[i] = 0;
    AudioObjectSetPropertyData (id, &addr, 0, nullptr, size, usage);
}

bool hasSettableVolume (AudioObjectID id) { return ! volumeElements (id, kAudioDevicePropertyVolumeScalar).empty(); }
float volumeScalar (AudioObjectID id)     { return firstElementValue<Float32> (id, kAudioDevicePropertyVolumeScalar, 1.0f); }
float volumeDecibels (AudioObjectID id)   { return firstElementValue<Float32> (id, kAudioDevicePropertyVolumeDecibels, 0.0f); }

void setVolumeScalar (AudioObjectID id, float scalar)
{
    for (auto element : volumeElements (id, kAudioDevicePropertyVolumeScalar))
        setValue (id, address (kAudioDevicePropertyVolumeScalar, OUTPUT, element), (Float32) scalar);
}

bool hasSettableMute (AudioObjectID id) { return ! volumeElements (id, kAudioDevicePropertyMute).empty(); }
bool isMuted (AudioObjectID id)         { return firstElementValue<UInt32> (id, kAudioDevicePropertyMute, 0) != 0; }

void setMuted (AudioObjectID id, bool muted)
{
    for (auto element : volumeElements (id, kAudioDevicePropertyMute))
        setValue (id, address (kAudioDevicePropertyMute, OUTPUT, element), (UInt32) (muted ? 1 : 0));
}
} // namespace CoreAudioHelpers
