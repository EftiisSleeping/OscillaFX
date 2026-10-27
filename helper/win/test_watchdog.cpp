// Dry-run tests for OscillaWatchdog.exe: a stand-in "app" (this exe with --fake-app) spawns the
// helper, writes a restore record and kills itself. The default endpoint is never changed
// (OSCILLA_WATCHDOG_DRYRUN=1, fake endpoint lists).
//   watchdog_test <path to OscillaWatchdog.exe>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

std::wstring tempDirectory()
{
    wchar_t buffer[MAX_PATH];
    GetTempPathW (MAX_PATH, buffer);
    std::wstring dir = std::wstring (buffer) + L"oscilla_watchdog_test_" + std::to_wstring (GetCurrentProcessId());
    CreateDirectoryW (dir.c_str(), nullptr);
    return dir;
}

std::string readFile (const std::wstring& path)
{
    std::ifstream file (path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool waitForText (const std::wstring& path, const std::string& text, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 50)
    {
        if (readFile (path).find (text) != std::string::npos)
            return true;
        Sleep (50);
    }
    return false;
}

void replaceAll (std::wstring& text, const std::wstring& from, const std::wstring& to)
{
    for (size_t at = text.find (from); at != std::wstring::npos; at = text.find (from, at + to.size()))
        text.replace (at, from.size(), to);
}

// Starts `commandLine` without a window and without inheriting handles. Returns the process handle.
HANDLE spawn (std::wstring commandLine)
{
    STARTUPINFOW startup { sizeof (startup) };
    PROCESS_INFORMATION info {};
    if (! CreateProcessW (nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return nullptr;
    CloseHandle (info.hThread);
    return info.hProcess;
}

std::wstring quoted (const std::wstring& text) { return L"\"" + text + L"\""; }

// --fake-app <watchdog> <record> <content>: content uses \n for line breaks and %PID% for our pid;
// empty content removes the record (clean stop).
int runFakeApp (const std::wstring& watchdog, const std::wstring& record, std::wstring content)
{
    const auto pid = std::to_wstring (GetCurrentProcessId());
    HANDLE helper = spawn (quoted (watchdog) + L" " + pid + L" " + quoted (record));
    if (helper == nullptr)
        return 3;
    CloseHandle (helper);

    wchar_t log[MAX_PATH] {};
    GetEnvironmentVariableW (L"OSCILLA_WATCHDOG_LOG", log, MAX_PATH);
    waitForText (log, "watching pid", 5000);

    replaceAll (content, L"%PID%", pid);
    replaceAll (content, L"\\r", L"\r");
    replaceAll (content, L"\\n", L"\n");
    DeleteFileW (record.c_str());
    if (! content.empty())
    {
        std::ofstream file (record, std::ios::binary);
        for (auto c : content)
            file.put ((char) c); // ASCII test data
    }
    TerminateProcess (GetCurrentProcess(), 9); // as a crash or End task would
    return 0;
}

struct Fakes
{
    const wchar_t* defaultEndpoint;
    const wchar_t* endpoints;
};

class Runner
{
public:
    explicit Runner (std::wstring watchdogPath) : watchdog (std::move (watchdogPath)), dir (tempDirectory())
    {
        GetModuleFileNameW (nullptr, self, MAX_PATH);
        SetEnvironmentVariableW (L"OSCILLA_WATCHDOG_DRYRUN", L"1");
    }

    void crashCase (const char* name, const std::string& expected, const std::wstring& record, const Fakes& fakes)
    {
        const auto log = startCase (fakes);
        HANDLE app = spawn (quoted (self) + L" --fake-app " + quoted (watchdog) + L" " + quoted (recordPath()) + L" " + quoted (record));
        if (app != nullptr)
        {
            WaitForSingleObject (app, 10000);
            CloseHandle (app);
        }
        report (name, app != nullptr && waitForText (log, expected, 5000), expected);
    }

    void notMyParentCase()
    {
        const auto log = startCase ({ nullptr, nullptr });
        HANDLE stranger = spawn (quoted (self) + L" --sleep");
        const auto strangerPid = std::to_wstring (GetProcessId (stranger));
        HANDLE helper = spawn (quoted (watchdog) + L" " + strangerPid + L" " + quoted (recordPath()));
        DWORD status = 0;
        if (helper != nullptr)
        {
            WaitForSingleObject (helper, 5000);
            GetExitCodeProcess (helper, &status);
            CloseHandle (helper);
        }
        TerminateProcess (stranger, 0);
        CloseHandle (stranger);
        report ("pid that is not the parent is refused (exit 1)", status == 1, "exit code 1");
        report ("pid that is not the parent is logged", waitForText (log, "is not my parent", 1000), "is not my parent");
    }

    void finish() const
    {
        std::printf ("%s\n", failures == 0 ? "ALL PASS" : "FAILED");
    }

private:
    std::wstring recordPath() const { return dir + L"\\restore_output_uid.txt"; }

    std::wstring startCase (const Fakes& fakes)
    {
        const auto log = dir + L"\\watchdog_" + std::to_wstring (++caseNumber) + L".log";
        SetEnvironmentVariableW (L"OSCILLA_WATCHDOG_LOG", log.c_str());
        SetEnvironmentVariableW (L"OSCILLA_WATCHDOG_DRYRUN_DEFAULT", fakes.defaultEndpoint);
        SetEnvironmentVariableW (L"OSCILLA_WATCHDOG_DRYRUN_ENDPOINTS", fakes.endpoints);
        DeleteFileW (recordPath().c_str());
        return log;
    }

    static void report (const char* name, bool ok, const std::string& expected)
    {
        if (ok)
            std::printf ("[PASS] %s\n", name);
        else
            std::printf ("[FAIL] %s (expected: %s)\n", name, expected.c_str());
        failures += ok ? 0 : 1;
    }

    std::wstring watchdog, dir;
    wchar_t self[MAX_PATH] {};
    int caseNumber = 0;
};
} // namespace

int wmain (int argc, wchar_t** argv)
{
    if (argc == 5 && std::wstring (argv[1]) == L"--fake-app")
        return runFakeApp (argv[2], argv[3], argv[4]);
    if (argc == 2 && std::wstring (argv[1]) == L"--sleep")
        return (int) SleepEx (30000, FALSE);
    if (argc != 2)
    {
        std::fprintf (stderr, "usage: watchdog_test <OscillaWatchdog.exe>\n");
        return 2;
    }

    const wchar_t* const FX = L"{0.0.0.00000000}.{fx}|Speakers (FxSound Audio Enhancer)";
    const Fakes fxDefault { FX, L"{0.0.0.00000000}.{fx}|Speakers (FxSound Audio Enhancer);{real-a}|Headphones (USB DAC);{real-b}|Speakers (Realtek(R) Audio)" };

    Runner run (argv[1]);
    run.crashCase ("crash restores saved endpoint", "would set default output to {real-b} ", L"%PID%\\n{real-b}\\n", fxDefault);
    run.crashCase ("saved endpoint gone -> other real endpoint", "would set default output to {real-a} ", L"%PID%\\n{unplugged}\\n", fxDefault);
    run.crashCase ("CRLF record (text mode) is read", "would set default output to {real-b} ", L"%PID%\\r\\n{real-b}\\r\\n", fxDefault);
    run.crashCase ("default not the virtual endpoint -> untouched", "not the virtual device; nothing to do", L"%PID%\\n{real-b}\\n",
                   { L"{real-b}|Speakers (Realtek(R) Audio)", fxDefault.endpoints });
    run.crashCase ("only the virtual endpoint left -> nothing to restore", "no real output device found", L"%PID%\\n{real-b}\\n",
                   { FX, FX });
    run.crashCase ("clean stop (record removed)", "exited cleanly", L"", fxDefault);
    run.crashCase ("record of another pid ignored", "ignoring restore file of pid 1", L"1\\n{real-b}\\n", fxDefault);
    run.crashCase ("legacy single-line record ignored", "old or unknown format", L"{real-b}", fxDefault);
    run.notMyParentCase();
    run.finish();
    return failures > 0 ? 1 : 0;
}
