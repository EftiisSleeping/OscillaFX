#include "platform/CrashWatchdog.h"

#include <juce_events/juce_events.h>
#include <windows.h>

// Spawns OscillaWatchdog.exe (helper/win, installed next to OscillaFX.exe), which waits on this
// process and restores the default endpoint named in restoreFile() if we die while routing.
namespace CrashWatchdog
{
namespace
{
constexpr int MAX_RESPAWNS = 5; // a helper that keeps dying is broken; stop trying

HANDLE helperProcess = nullptr; // message thread only
HANDLE exitWait = nullptr;
int respawns = 0;

juce::File helperExecutable()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
        .getSiblingFile ("OscillaWatchdog.exe");
}

// No console window, no inherited handles, and outside our job when allowed (a launcher's
// kill-on-close job must not take the helper down with the app). Returns the process handle.
HANDLE spawnDetached (const juce::File& executable, const juce::String& arguments)
{
    std::wstring commandLine = (executable.getFullPathName().quoted() + " " + arguments).toWideCharPointer();
    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION info {};
    const DWORD flags = CREATE_NO_WINDOW;
    const auto start = [&] (DWORD extraFlags)
    {
        return CreateProcessW (executable.getFullPathName().toWideCharPointer(), commandLine.data(), nullptr, nullptr,
                               FALSE, flags | extraFlags, nullptr, nullptr, &startup, &info) != FALSE;
    };
    if (! start (CREATE_BREAKAWAY_FROM_JOB) && ! start (0))
    {
        juce::Logger::writeToLog ("OscillaFX: cannot start crash watchdog: error " + juce::String ((int) GetLastError()));
        return nullptr;
    }
    CloseHandle (info.hThread);
    return info.hProcess;
}

bool spawnHelper();

void onHelperExit()
{
    UnregisterWait (exitWait);
    exitWait = nullptr;
    CloseHandle (helperProcess);
    helperProcess = nullptr;
    if (juce::MessageManager::getInstance()->hasStopMessageBeenSent() || respawns >= MAX_RESPAWNS)
        return;
    ++respawns;
    juce::Logger::writeToLog ("OscillaFX: crash watchdog exited unexpectedly; restarting it");
    spawnHelper();
}

void CALLBACK helperExited (void*, BOOLEAN)
{
    juce::MessageManager::callAsync (onHelperExit);
}

bool spawnHelper()
{
    const auto helper = helperExecutable();
    if (! helper.existsAsFile())
    {
        juce::Logger::writeToLog ("OscillaFX: crash watchdog missing at " + helper.getFullPathName());
        return false;
    }
    helperProcess = spawnDetached (helper, juce::String (currentProcessId()) + " " + restoreFile().getFullPathName().quoted());
    if (helperProcess == nullptr)
        return false;
    RegisterWaitForSingleObject (&exitWait, helperProcess, helperExited, nullptr, INFINITE, WT_EXECUTEONLYONCE);
    return true;
}
} // namespace

int currentProcessId() { return (int) GetCurrentProcessId(); }

bool launch()
{
    return helperProcess != nullptr || spawnHelper();
}
} // namespace CrashWatchdog
