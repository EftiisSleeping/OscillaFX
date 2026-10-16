#pragma once

#include <CoreAudio/CoreAudio.h>
#include <juce_core/juce_core.h>
#include <utility>
#include <vector>

// Thin HAL property wrappers. Message thread only.
namespace CoreAudioHelpers
{
std::vector<AudioObjectID> allDevices();
juce::String deviceUid (AudioObjectID);
juce::String deviceName (AudioObjectID);
AudioObjectID findDeviceByUid (const juce::String& uid);
AudioObjectID findDeviceByName (const juce::String& name);
int channelCount (AudioObjectID, bool isInput);
int outputChannelCount (AudioObjectID);
juce::StringArray subDeviceUids (AudioObjectID); // aggregate / multi-output members, else empty
std::pair<int, int> preferredStereoChannels (AudioObjectID); // zero-based, defaults to {0, 1}
bool isBuiltIn (AudioObjectID);
UInt32 transportType (AudioObjectID);       // kAudioDeviceTransportType* four-char code, 0 if unknown
UInt32 outputDataSource (AudioObjectID);    // active output data source ('ispk' speaker, 'hdpn' headphones...), 0 if none
bool isAlive (AudioObjectID);

AudioObjectID defaultOutputDevice();
bool setDefaultOutputDevice (AudioObjectID);

double nominalSampleRate (AudioObjectID);
bool setNominalSampleRate (AudioObjectID, double rate); // async in the HAL; watch the rate listener
UInt32 bufferFrameSize (AudioObjectID);
bool streamsAreFloat32 (AudioObjectID, bool isInput);
// Turns off one direction for an IOProc (e.g. never open the mic of a headset we only play to).
void disableStreams (AudioObjectID, AudioDeviceIOProcID, bool isInput);

// Output-scope master volume, falling back to channels 1+2 when there is no master control.
bool hasSettableVolume (AudioObjectID);
float volumeScalar (AudioObjectID);
float volumeDecibels (AudioObjectID);
void setVolumeScalar (AudioObjectID, float scalar);
bool hasSettableMute (AudioObjectID);
bool isMuted (AudioObjectID);
void setMuted (AudioObjectID, bool);
} // namespace CoreAudioHelpers
