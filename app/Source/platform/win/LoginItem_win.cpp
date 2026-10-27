#include "platform/LoginItem.h"

#include <juce_events/juce_events.h>
#include <windows.h>

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run\OscillaFX = "<exe>" --launched-at-login.
// Per user, no admin rights. Task Manager > Startup apps can switch it off without touching that
// value (Explorer\StartupApproved\Run\OscillaFX, first byte odd = disabled); that counts as off.
namespace LoginItem
{
namespace
{
const char* const RUN_VALUE = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\OscillaFX";
const char* const APPROVAL_VALUE = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run\\OscillaFX";
const char* const LAUNCHED_AT_LOGIN_ARG = "--launched-at-login";

juce::String expectedCommand()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().quoted()
         + " " + LAUNCHED_AT_LOGIN_ARG;
}

bool isDisabledInTaskManager()
{
    juce::MemoryBlock approval;
    juce::WindowsRegistry::getBinaryValue (APPROVAL_VALUE, approval);
    return approval.getSize() > 0 && (static_cast<juce::uint8> (approval[0]) & 1) != 0;
}

bool wasLaunchedAtLogin()
{
    return juce::String (GetCommandLineW()).contains (LAUNCHED_AT_LOGIN_ARG);
}
} // namespace

// Registered for this copy of OscillaFX: an entry left by a moved or older copy counts as off,
// and turning it on rewrites it.
bool isEnabled()
{
    return juce::WindowsRegistry::getValue (RUN_VALUE).equalsIgnoreCase (expectedCommand()) && ! isDisabledInTaskManager();
}

bool setEnabled (bool shouldStartAtLogin)
{
    if (juce::WindowsRegistry::valueExists (APPROVAL_VALUE))
        juce::WindowsRegistry::deleteValue (APPROVAL_VALUE); // no entry = approved
    if (shouldStartAtLogin)
        return juce::WindowsRegistry::setValue (RUN_VALUE, expectedCommand()) && isEnabled();
    if (juce::WindowsRegistry::valueExists (RUN_VALUE))
        juce::WindowsRegistry::deleteValue (RUN_VALUE);
    return ! juce::WindowsRegistry::valueExists (RUN_VALUE);
}

void onLaunchFinished (std::function<void (bool)> callback)
{
    juce::MessageManager::callAsync ([callback = std::move (callback)] { callback (wasLaunchedAtLogin()); });
}
} // namespace LoginItem
