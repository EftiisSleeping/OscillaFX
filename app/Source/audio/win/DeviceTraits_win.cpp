#include "core/DeviceKind.h"
#include "audio/win/EndpointTraits.h"
#include "audio/win/MMDeviceHelpers.h"

namespace DeviceKinds
{
Traits traitsFor (const juce::String& uid, const juce::String& name)
{
    namespace MM = MMDeviceHelpers;
    MM::ComScope com;
    EndpointTraits::Facts facts;
    facts.uid = uid;
    facts.name = name;
    facts.hasSystemBattery = MM::hasSystemBattery();
    if (auto enumerator = MM::createEnumerator())
        if (auto device = MM::deviceById (*enumerator.Get(), uid))
        {
            const auto endpoint = MM::describe (*device.Get());
            facts.adapterName = endpoint.adapterName;
            facts.enumerator = endpoint.enumerator;
            facts.formFactor = endpoint.formFactor;
            if (facts.name.isEmpty())
                facts.name = endpoint.name;
        }
    return EndpointTraits::traitsFor (facts);
}
} // namespace DeviceKinds
