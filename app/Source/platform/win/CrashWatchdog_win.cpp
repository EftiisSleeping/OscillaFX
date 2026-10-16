#include "platform/CrashWatchdog.h"

#include <windows.h>

namespace CrashWatchdog
{
int currentProcessId() { return (int) GetCurrentProcessId(); }

// Stub: no helper process yet. The real one watches the app with WaitForSingleObject on its
// process handle and restores the default endpoint named in restoreFile().
bool launch() { return false; }
} // namespace CrashWatchdog
