#include "platform/CrashWatchdog.h"

#include <juce_events/juce_events.h>
#include <crt_externs.h>
#include <csignal>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace CrashWatchdog
{
namespace
{
constexpr int MAX_RESPAWNS = 5; // a helper that keeps dying is broken; stop trying

dispatch_source_t exitWatch = nullptr; // main queue only
int respawns = 0;

juce::File helperExecutable()
{
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile)
        .getChildFile ("Contents/Helpers/OscillaWatchdog");
}

// Own process group (a terminal Ctrl-C on a dev build must not take it down with the app), default
// signal dispositions and mask (the app ignores SIGTERM/INT/HUP for its dispatch sources), and no
// inherited descriptors except stdio pointed at /dev/null. Returns the child pid, or 0.
pid_t spawnDetached (const juce::File& executable, const juce::StringArray& args)
{
    std::vector<std::string> storage { executable.getFullPathName().toStdString() };
    for (auto& arg : args)
        storage.push_back (arg.toStdString());
    std::vector<char*> argv;
    for (auto& s : storage)
        argv.push_back (s.data());
    argv.push_back (nullptr);

    sigset_t appIgnored, noSignals;
    sigemptyset (&appIgnored);
    for (int sig : { SIGTERM, SIGINT, SIGHUP, SIGPIPE })
        sigaddset (&appIgnored, sig);
    sigemptyset (&noSignals);
    posix_spawnattr_t attributes;
    posix_spawnattr_init (&attributes);
    posix_spawnattr_setflags (&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT
                                               | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup (&attributes, 0);
    posix_spawnattr_setsigdefault (&attributes, &appIgnored);
    posix_spawnattr_setsigmask (&attributes, &noSignals);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    for (int fd : { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO })
        posix_spawn_file_actions_addopen (&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);

    pid_t child = 0;
    int error = posix_spawn (&child, argv[0], &actions, &attributes, argv.data(), *_NSGetEnviron());
    posix_spawn_file_actions_destroy (&actions);
    posix_spawnattr_destroy (&attributes);
    if (error != 0)
        juce::Logger::writeToLog ("OscillaFX: cannot start crash watchdog: " + juce::String (strerror (error)));
    return error == 0 ? child : 0;
}

bool spawnHelper();

void onHelperExit (void* context)
{
    auto pid = (pid_t) (intptr_t) context;
    waitpid (pid, nullptr, WNOHANG);
    dispatch_source_cancel (exitWatch);
    dispatch_release (exitWatch);
    exitWatch = nullptr;
    if (juce::MessageManager::getInstance()->hasStopMessageBeenSent() || respawns >= MAX_RESPAWNS)
        return;
    ++respawns;
    juce::Logger::writeToLog ("OscillaFX: crash watchdog exited unexpectedly; restarting it");
    spawnHelper();
}

void watchHelper (pid_t pid)
{
    exitWatch = dispatch_source_create (DISPATCH_SOURCE_TYPE_PROC, (uintptr_t) pid, DISPATCH_PROC_EXIT,
                                        dispatch_get_main_queue());
    dispatch_set_context (exitWatch, (void*) (intptr_t) pid);
    dispatch_source_set_event_handler_f (exitWatch, onHelperExit);
    dispatch_resume (exitWatch);
}

bool spawnHelper()
{
    auto helper = helperExecutable();
    if (! helper.existsAsFile())
    {
        juce::Logger::writeToLog ("OscillaFX: crash watchdog missing at " + helper.getFullPathName());
        return false;
    }
    auto pid = spawnDetached (helper, { juce::String (getpid()), restoreFile().getFullPathName() });
    if (pid != 0)
        watchHelper (pid);
    return pid != 0;
}
} // namespace

int currentProcessId() { return (int) getpid(); }

bool launch()
{
    return exitWatch != nullptr || spawnHelper();
}
} // namespace CrashWatchdog
