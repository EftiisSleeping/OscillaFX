#pragma once

#include <juce_core/juce_core.h>

// Out-of-process half of crash protection (CrashGuard is the in-process half): a helper that
// outlives a crashed or force-killed OscillaFX and restores the system default output.
namespace CrashWatchdog
{
// Restore record "<pid>\n<real output UID>": written when routing starts, cleared on a clean stop.
// The helper acts only on a record carrying the pid it watches. Shared code (CrashWatchdog.cpp).
juce::File restoreFile();
bool writeRestoreUid (const juce::String& uid);
juce::String readRestoreUid(); // also reads the legacy single-line format
void clearRestoreUid();

// Per platform (platform/<os>/CrashWatchdog_<os>.cpp).
int currentProcessId();

// Starts the helper to watch this process, and respawns it if it dies while the app runs.
// Message thread; at most one helper per process. macOS: Contents/Helpers/OscillaWatchdog.
bool launch();
} // namespace CrashWatchdog
