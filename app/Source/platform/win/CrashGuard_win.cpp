#include "platform/CrashGuard.h"

// Stub. Windows has no termination signals for a GUI app: shutdown arrives as WM_QUERYENDSESSION,
// which JUCE turns into systemRequestedQuit(). The audio backend adds the crash half
// (SetUnhandledExceptionFilter + std::set_terminate restoring the saved default endpoint).
namespace CrashGuard
{
void installQuitSignals (std::function<void()>) {}

void installCrashHandlers() {}
} // namespace CrashGuard
