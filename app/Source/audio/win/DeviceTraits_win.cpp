#include "core/DeviceKind.h"

namespace DeviceKinds
{
// TODO(windows audio): map the endpoint's PKEY_AudioEndpoint_FormFactor / PKEY_Device_EnumeratorName
// (Bluetooth, USB, HDMI, speakers, headphones) onto Traits::transport / dataSource codes.
Traits traitsFor (const juce::String& uid, const juce::String& name)
{
    return { uid, name, 0, 0 };
}
} // namespace DeviceKinds
