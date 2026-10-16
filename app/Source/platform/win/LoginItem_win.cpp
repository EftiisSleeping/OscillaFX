#include "platform/LoginItem.h"

#include <juce_events/juce_events.h>

// Stub. The real version registers "<exe>" --login under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run\OscillaFX and reports launchedAtLogin from that argument.
namespace LoginItem
{
bool isEnabled() { return false; }

bool setEnabled (bool shouldStartAtLogin) { return ! shouldStartAtLogin; }

void onLaunchFinished (std::function<void (bool)> callback)
{
    juce::MessageManager::callAsync ([callback = std::move (callback)] { callback (false); });
}
} // namespace LoginItem
