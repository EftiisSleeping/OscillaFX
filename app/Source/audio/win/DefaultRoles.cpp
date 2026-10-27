#include "audio/win/DefaultRoles.h"

namespace DefaultRoles
{
bool Takeover::begin (Endpoints& endpoints, const juce::String& virtualId)
{
    if (! active)
    {
        virtualEndpoint = virtualId;
        for (int role = 0; role < numRoles; ++role)
        {
            const auto current = endpoints.defaultFor ((Role) role);
            originals[(size_t) role] = current == virtualId ? juce::String() : current;
        }
        const auto& consoleOriginal = originals[console];
        const auto& callsOriginal = originals[communications];
        held = { true, true, callsOriginal.isEmpty() || callsOriginal == consoleOriginal };
    }
    active = true;

    bool ok = true;
    for (int role = 0; role < numRoles; ++role)
        if (held[(size_t) role] && endpoints.defaultFor ((Role) role) != virtualId)
            ok &= endpoints.setDefault (virtualId, (Role) role);
    return ok;
}

void Takeover::userChose (Endpoints& endpoints, Role role, const juce::String& virtualId)
{
    if (! holds (role))
        return;
    if (role == communications)
    {
        held[communications] = false;
        return;
    }
    endpoints.setDefault (virtualId, role);
}

void Takeover::restore (Endpoints& endpoints, const juce::String& outputId)
{
    if (! active)
        return;
    for (int role = 0; role < numRoles; ++role)
    {
        if (! held[(size_t) role])
            continue;
        const auto current = endpoints.defaultFor ((Role) role);
        if (current.isNotEmpty() && current != virtualEndpoint)
            continue;   // moved elsewhere by someone else meanwhile
        const auto& fallback = originals[(size_t) role];
        if (outputId.isNotEmpty() && endpoints.isActive (outputId))
            endpoints.setDefault (outputId, (Role) role);
        else if (fallback.isNotEmpty() && endpoints.isActive (fallback))
            endpoints.setDefault (fallback, (Role) role);
    }
    active = false;
    held = {};
}
} // namespace DefaultRoles
