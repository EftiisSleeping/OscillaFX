#pragma once

#include <functional>

// Start at login. The system registration is the source of truth: nothing is stored in OscillaFX's settings.
// macOS (mac/LoginItem_mac.mm): SMAppService.mainAppService on macOS 13+, falling back to a per-user
// LaunchAgent plist when the service is unavailable or refuses (e.g. ad-hoc signature).
// Windows (win/LoginItem_win.cpp): stub for now, always disabled.
namespace LoginItem
{
bool isEnabled();
bool setEnabled (bool shouldStartAtLogin); // false if neither mechanism took effect

// Calls back once on the message thread when launching finished, telling whether the OS started
// the app as a login item. Register in JUCEApplication::initialise(), before the run loop starts.
void onLaunchFinished (std::function<void (bool launchedAtLogin)>);
} // namespace LoginItem
