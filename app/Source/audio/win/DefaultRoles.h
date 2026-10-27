#pragma once

#include <juce_core/juce_core.h>
#include <array>

// Which endpoint Windows uses as the default playback device, per role, while OscillaFX runs.
// Pure policy over an `Endpoints` interface (IPolicyConfig + IMMDeviceEnumerator in the engine,
// a fake in the self-test). Message thread only.
namespace DefaultRoles
{
enum Role { console, multimedia, communications, numRoles };   // same order as ERole

struct Endpoints
{
    virtual ~Endpoints() = default;
    virtual juce::String defaultFor (Role) const = 0;              // endpoint id, empty if none
    virtual bool setDefault (const juce::String& id, Role) = 0;
    virtual bool isActive (const juce::String& id) const = 0;
};

class Takeover
{
public:
    // Remembers every role's default, then points console + multimedia at `virtualId` (what "Set
    // Default" in the Sound control panel changes). Communications follows only when it was on the
    // same endpoint as console, so a separate call headset stays as the user set it.
    bool begin (Endpoints&, const juce::String& virtualId);

    // The user chose a real endpoint for `role` while routing: communications is given up for good,
    // console / multimedia are pointed back at the virtual endpoint (the output follows the choice).
    void userChose (Endpoints&, Role, const juce::String& virtualId);

    // Hands every role still held back to `outputId` (the endpoint the user was hearing), or to the
    // role's original endpoint if the output is gone. Roles moved elsewhere meanwhile are left alone.
    void restore (Endpoints&, const juce::String& outputId);

    bool isActive() const { return active; }
    bool holds (Role role) const { return active && held[(size_t) role]; }
    juce::String original (Role role) const { return originals[(size_t) role]; }

private:
    bool active = false;
    juce::String virtualEndpoint;
    std::array<bool, numRoles> held {};
    std::array<juce::String, numRoles> originals;
};
} // namespace DefaultRoles
