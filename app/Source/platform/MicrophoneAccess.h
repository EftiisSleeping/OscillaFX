#pragma once

#include <functional>

// Permission to capture the virtual device. macOS (TCC microphone): without it the capture delivers
// silence, so OscillaFX must not take over the system output before it is granted.
// Windows: loopback/virtual-device capture needs no consent prompt; always granted.
namespace MicrophoneAccess
{
enum class Status { granted, undetermined, denied }; // denied includes "restricted"

Status status();

// Shows the system prompt; `onResult` runs later on the message thread.
void request (std::function<void (bool granted)> onResult);

void openSystemSettings(); // macOS: Privacy & Security > Microphone; Windows: ms-settings:privacy-microphone
} // namespace MicrophoneAccess
