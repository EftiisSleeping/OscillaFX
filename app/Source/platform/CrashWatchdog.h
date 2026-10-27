#pragma once

#include <juce_core/juce_core.h>

// Out-of-process half of crash protection (CrashGuard is the in-process half): a helper that
// outlives a crashed or force-killed OscillaFX and restores the system default output.
namespace CrashWatchdog
{
// Restore record "<pid>\n<real output UID>\n" (macOS), or "<pid>\n<endpoint id>\n<roles>\n" (Windows:
// the default-device roles OscillaFX took over, bits below; a 2-line record means console | multimedia).
// Written when routing starts, cleared on a clean stop. The helper acts only on a record carrying
// the pid it watches. Shared code (CrashWatchdog.cpp).
enum RoleBits { consoleRole = 1, multimediaRole = 2, communicationsRole = 4 };   // 1 << ERole
constexpr int DEFAULT_ROLES = consoleRole | multimediaRole;
constexpr int ALL_ROLES = consoleRole | multimediaRole | communicationsRole;

juce::File restoreFile();
bool writeRestoreUid (const juce::String& uid);              // 2 lines
bool writeRestoreUid (const juce::String& uid, int roles);   // 3 lines; roles masked to ALL_ROLES
juce::String readRestoreUid();  // also reads the legacy single-line format
int readRestoreRoles();         // DEFAULT_ROLES when the record has no (valid) roles line
void clearRestoreUid();

// Per platform (platform/<os>/CrashWatchdog_<os>.cpp).
int currentProcessId();

// Starts the helper to watch this process, and respawns it if it dies while the app runs.
// Message thread; at most one helper per process. macOS: Contents/Helpers/OscillaWatchdog.
bool launch();
} // namespace CrashWatchdog
