// Restores the default playback endpoint if OscillaFX dies while routing through FxSound's virtual
// endpoint ("FxSound Audio Enhancer"). Windows counterpart of helper/OscillaWatchdog.cpp. No JUCE.
//
//   OscillaWatchdog.exe <app-pid> <restore-file>     (spawned by the app: <app-pid> must be the parent)
//
// While routing, OscillaFX keeps <restore-file> = "<app pid>\n<real endpoint id>\n<roles>\n" and
// deletes it on a clean stop. <roles> is the default-device roles it took over (bits: 1 console,
// 2 multimedia, 4 communications); a record without that line means console + multimedia. If the
// file still names the watched pid when that process exits, the app crashed or was killed: if the
// virtual endpoint is still the default, make the saved one the default again for those roles only.
//
// Log: %LOCALAPPDATA%\OscillaFX\Logs\watchdog.log, or $OSCILLA_WATCHDOG_LOG.
// OSCILLA_WATCHDOG_DRYRUN=1 logs the decision without touching the default endpoint. In dry-run,
// OSCILLA_WATCHDOG_DRYRUN_DEFAULT="<id>|<name>" pretends that is the current default and
// OSCILLA_WATCHDOG_DRYRUN_ENDPOINTS="<id>|<name>;<id>|<name>..." the active endpoints (tests only).

#include "platform/win/WinEndpoints.h"

#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using WinEndpoints::Endpoint;

FILE* logFile = stderr;

std::wstring environment (const wchar_t* name)
{
    wchar_t buffer[4096];
    const auto length = GetEnvironmentVariableW (name, buffer, (DWORD) std::size (buffer));
    return length > 0 && length < std::size (buffer) ? std::wstring (buffer, length) : std::wstring();
}

std::string utf8 (const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte (CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0, nullptr, nullptr);
    std::string result ((size_t) size, '\0');
    WideCharToMultiByte (CP_UTF8, 0, text.data(), (int) text.size(), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide (const std::string& text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar (CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
    std::wstring result ((size_t) size, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, text.data(), (int) text.size(), result.data(), size);
    return result;
}

std::wstring defaultLogPath()
{
    const auto localAppData = environment (L"LOCALAPPDATA");
    if (localAppData.empty())
        return {};
    const auto dir = localAppData + L"\\OscillaFX";
    CreateDirectoryW (dir.c_str(), nullptr);
    CreateDirectoryW ((dir + L"\\Logs").c_str(), nullptr);
    return dir + L"\\Logs\\watchdog.log";
}

void openLog()
{
    auto path = environment (L"OSCILLA_WATCHDOG_LOG");
    if (path.empty())
        path = defaultLogPath();
    if (FILE* file = path.empty() ? nullptr : _wfopen (path.c_str(), L"ab"))
        logFile = file;
}

void logLine (const char* format, ...)
{
    char stamp[32];
    std::time_t now = std::time (nullptr);
    std::tm local {};
    localtime_s (&local, &now);
    std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", &local);
    std::fprintf (logFile, "%s [%lu] ", stamp, GetCurrentProcessId());
    va_list args;
    va_start (args, format);
    std::vfprintf (logFile, format, args);
    va_end (args);
    std::fputs ("\r\n", logFile);
    std::fflush (logFile);
}

bool isDryRun() { return ! environment (L"OSCILLA_WATCHDOG_DRYRUN").empty(); }

// ---------------------------------------------------------------- watching the app

enum class Watch { failed, watching, exited };

DWORD parentProcessId()
{
    const auto self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry { sizeof (entry) };
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW (snapshot, &entry); ok; ok = Process32NextW (snapshot, &entry))
        if (entry.th32ProcessID == self)
            parent = entry.th32ParentProcessID;
    CloseHandle (snapshot);
    return parent;
}

ULONGLONG creationTime (HANDLE process)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (! GetProcessTimes (process, &created, &exited, &kernel, &user))
        return 0;
    return ((ULONGLONG) created.dwHighDateTime << 32) | created.dwLowDateTime;
}

// Opens the parent for waiting. Fails (and logs) when `appPid` is not our parent. A parent that
// died before we got here is `exited`; a pid reused since then belongs to a process younger than us.
Watch watchParent (DWORD appPid, HANDLE& process)
{
    if (parentProcessId() != appPid)
    {
        logLine ("pid %lu is not my parent (parent %lu); exiting", appPid, parentProcessId());
        return Watch::failed;
    }
    process = OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, appPid);
    if (process == nullptr)
        return Watch::exited;
    if (creationTime (process) > creationTime (GetCurrentProcess()))
    {
        CloseHandle (process);
        process = nullptr;
        return Watch::exited;
    }
    return Watch::watching;
}

constexpr int DEFAULT_ROLES = 1 | 2;   // console | multimedia (CrashWatchdog::DEFAULT_ROLES)
constexpr int ALL_ROLES = 1 | 2 | 4;

struct PendingRestore
{
    std::wstring id;   // empty: nothing to restore
    int roles = DEFAULT_ROLES;
};

std::string withoutCarriageReturn (std::string line)
{
    if (! line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

// Restore left behind by an unclean exit of `appPid`; empty id if the app stopped cleanly or the
// file belongs to another process (including the legacy single-line format).
PendingRestore pendingRestore (const wchar_t* path, DWORD appPid)
{
    std::ifstream file (path, std::ios::binary);
    std::string pidLine, id, rolesLine;
    if (! std::getline (file, pidLine))
        return {};
    if (! std::getline (file, id) || (id = withoutCarriageReturn (id)).empty())
    {
        logLine ("ignoring restore file in an old or unknown format");
        return {};
    }
    if (std::strtoul (pidLine.c_str(), nullptr, 10) != appPid)
    {
        logLine ("ignoring restore file of pid %s", pidLine.c_str());
        return {};
    }
    PendingRestore restore { wide (id), DEFAULT_ROLES };
    if (std::getline (file, rolesLine))
        if (const auto roles = (int) std::strtol (rolesLine.c_str(), nullptr, 10) & ALL_ROLES; roles != 0)
            restore.roles = roles;
    return restore;
}

// ---------------------------------------------------------------- endpoints

Endpoint parseEndpoint (const std::wstring& text)
{
    const auto bar = text.find (L'|');
    return bar == std::wstring::npos ? Endpoint { text, {} } : Endpoint { text.substr (0, bar), text.substr (bar + 1) };
}

std::vector<Endpoint> parseEndpoints (const std::wstring& text)
{
    std::vector<Endpoint> result;
    size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find (L';', start);
        if (end == std::wstring::npos)
            end = text.size();
        if (end > start)
            result.push_back (parseEndpoint (text.substr (start, end - start)));
        start = end + 1;
    }
    return result;
}

Endpoint currentDefault()
{
    const auto pretend = environment (L"OSCILLA_WATCHDOG_DRYRUN_DEFAULT");
    return isDryRun() && ! pretend.empty() ? parseEndpoint (pretend) : WinEndpoints::defaultRenderEndpoint();
}

std::vector<Endpoint> activeEndpoints()
{
    const auto pretend = environment (L"OSCILLA_WATCHDOG_DRYRUN_ENDPOINTS");
    return isDryRun() && ! pretend.empty() ? parseEndpoints (pretend) : WinEndpoints::activeRenderEndpoints();
}

// Saved endpoint, else (unplugged during the crash) any other active real endpoint.
Endpoint restoreTarget (const std::wstring& savedId)
{
    const auto endpoints = activeEndpoints();
    for (const auto& endpoint : endpoints)
        if (endpoint.id == savedId && ! WinEndpoints::isFxSound (endpoint))
            return endpoint;
    for (const auto& endpoint : endpoints)
        if (! WinEndpoints::isFxSound (endpoint))
            return endpoint;
    return {};
}

void restoreOutput (const PendingRestore& restore)
{
    const auto& savedId = restore.id;
    const auto roles = WinEndpoints::roleNames (restore.roles);
    const auto current = currentDefault();
    if (! WinEndpoints::isFxSound (current))
        return logLine ("default output is %s, not the virtual device; nothing to do", utf8 (current.name).c_str());

    const auto target = restoreTarget (savedId);
    if (target.id.empty())
        return logLine ("no real output device found (saved id %s); nothing to restore", utf8 (savedId).c_str());

    if (isDryRun())
        return logLine ("DRY RUN: would set default output to %s (%s) for %s (saved id %s)",
                        utf8 (target.id).c_str(), utf8 (target.name).c_str(), roles.c_str(), utf8 (savedId).c_str());

    const bool ok = WinEndpoints::setDefaultForRoles (target.id.c_str(), restore.roles);
    logLine ("set default output to %s (%s) for %s (saved id %s): %s",
             utf8 (target.id).c_str(), utf8 (target.name).c_str(), roles.c_str(), utf8 (savedId).c_str(), ok ? "ok" : "failed");
}
} // namespace

int wmain (int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "usage: OscillaWatchdog <app-pid> <restore-file>\n");
        return 2;
    }
    openLog();
    const auto appPid = (DWORD) std::wcstoul (argv[1], nullptr, 10);
    const wchar_t* restorePath = argv[2];

    HANDLE process = nullptr;
    const auto watch = appPid > 4 ? watchParent (appPid, process) : Watch::failed;
    if (watch == Watch::failed)
        return 1;
    logLine ("watching pid %lu%s", appPid, isDryRun() ? " (dry run)" : "");
    if (watch == Watch::watching)
    {
        WaitForSingleObject (process, INFINITE);
        CloseHandle (process);
    }

    const auto restore = pendingRestore (restorePath, appPid);
    if (restore.id.empty())
    {
        logLine ("pid %lu exited cleanly", appPid);
        return 0;
    }
    logLine ("pid %lu exited without restoring output", appPid);
    const auto com = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    restoreOutput (restore);
    if (SUCCEEDED (com))
        CoUninitialize();
    return 0;
}
