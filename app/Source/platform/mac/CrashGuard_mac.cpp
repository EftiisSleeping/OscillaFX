#include "platform/mac/CrashGuardMac.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <exception>

namespace CrashGuard
{
namespace
{
constexpr int QUIT_SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };
constexpr int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };

std::atomic<AudioObjectID> restoreDevice { kAudioObjectUnknown };
std::atomic<bool> restoring { false };
std::function<void()> quitCallback; // main queue only

// Not formally async-signal-safe, but the process is dying anyway; one HAL call, no allocation by us.
void restoreDefaultOutput()
{
    if (restoring.exchange (true))
        return;
    AudioObjectID device = restoreDevice.load();
    if (device == kAudioObjectUnknown)
        return;
    AudioObjectPropertyAddress addr { kAudioHardwarePropertyDefaultOutputDevice,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, sizeof (device), &device);
}

void onCrashSignal (int sig)
{
    restoreDefaultOutput();
    std::signal (sig, SIG_DFL); // SA_RESETHAND already did this; be explicit
    std::raise (sig);
}

void onTerminate()
{
    restoreDefaultOutput();
    std::abort();
}

void onQuitSignal (void*)
{
    if (quitCallback)
        quitCallback();
}
} // namespace

void installQuitSignals (std::function<void()> onQuit)
{
    quitCallback = std::move (onQuit);
    for (int sig : QUIT_SIGNALS)
    {
        std::signal (sig, SIG_IGN); // dispatch source receives it instead of the default handler
        auto source = dispatch_source_create (DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t) sig, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler_f (source, onQuitSignal);
        dispatch_resume (source); // intentionally never released: active for the process lifetime
    }
}

void installCrashHandlers()
{
    struct sigaction action {};
    action.sa_handler = onCrashSignal;
    action.sa_flags = SA_RESETHAND;
    sigemptyset (&action.sa_mask);
    for (int sig : CRASH_SIGNALS)
        sigaction (sig, &action, nullptr);
    std::set_terminate (onTerminate);
}

void setRestoreDevice (AudioObjectID device) { restoreDevice.store (device); }
} // namespace CrashGuard
