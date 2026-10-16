#include "core/DeviceKind.h"
#include "audio/mac/CoreAudioHelpers.h"

namespace DeviceKinds
{
Traits traitsFor (const juce::String& uid, const juce::String& name)
{
    Traits traits { uid, name, 0, 0 };
    if (auto device = CoreAudioHelpers::findDeviceByUid (uid); device != kAudioObjectUnknown)
    {
        traits.transport = CoreAudioHelpers::transportType (device);
        traits.dataSource = CoreAudioHelpers::outputDataSource (device);
        if (traits.name.isEmpty())
            traits.name = CoreAudioHelpers::deviceName (device);
    }
    return traits;
}
} // namespace DeviceKinds
