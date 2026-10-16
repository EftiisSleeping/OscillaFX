#pragma once

#include <functional>

// In-process crash protection: keeps the computer audible if OscillaFX dies while it has taken
// over the system output. Each audio backend tells its platform half which device to restore
// (macOS: platform/mac/CrashGuardMac.h).
namespace CrashGuard
{
// Termination requests (SIGTERM / SIGINT / SIGHUP on macOS) delivered on the message thread
// -> onQuit (normal shutdown restores output).
void installQuitSignals (std::function<void()> onQuit);

// Fatal signals / unhandled exceptions and std::terminate: best-effort switch of the system
// default output back to the saved real device, then the process dies as it would have.
void installCrashHandlers();
} // namespace CrashGuard
