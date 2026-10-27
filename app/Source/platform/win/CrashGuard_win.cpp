#include "platform/win/CrashGuardWin.h"

#include <juce_events/juce_events.h>
#include "platform/win/WinEndpoints.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <exception>

// Windows has no termination signals for a GUI app. Logoff / shutdown / Restart Manager arrive as
// WM_QUERYENDSESSION (JUCE turns it into systemRequestedQuit(), which quits normally) and then
// WM_ENDSESSION, after which the process may be killed before the quit message is ever handled:
// a hidden window restores the endpoint synchronously there. Console Ctrl-C / close (dev builds run
// from a terminal) quit normally. Crashes restore the saved endpoint from a helper thread.
namespace CrashGuard
{
namespace
{
constexpr DWORD RESTORE_TIMEOUT_MS = 3000;  // a crashed process may hold a lock COM needs: never hang
constexpr DWORD CLOSE_GRACE_MS = 4000;      // console close: time for the normal shutdown to finish
constexpr const wchar_t* SESSION_WINDOW_CLASS = L"OscillaFXSessionEnd";

wchar_t restoreId[512] {};
std::atomic<bool> hasRestoreId { false };
std::atomic<bool> restoring { false };
std::function<void()> quitCallback; // message thread only
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;

DWORD WINAPI restoreOnThisThread (void*)
{
    const auto com = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    WinEndpoints::setDefaultForAllRoles (restoreId);
    if (SUCCEEDED (com))
        CoUninitialize();
    return 0;
}

// On a fresh thread: the caller may be a crashing thread with COM in an unknown state.
void restoreDefaultOutput()
{
    if (! hasRestoreId.load() || restoring.exchange (true))
        return;
    if (HANDLE thread = CreateThread (nullptr, 0, restoreOnThisThread, nullptr, 0, nullptr))
    {
        WaitForSingleObject (thread, RESTORE_TIMEOUT_MS);
        CloseHandle (thread);
    }
}

LONG WINAPI onUnhandledException (EXCEPTION_POINTERS* info)
{
    restoreDefaultOutput();
    return previousFilter != nullptr ? previousFilter (info) : EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate()
{
    restoreDefaultOutput();
    std::abort();
}

void onAbort (int)
{
    restoreDefaultOutput();
    std::signal (SIGABRT, SIG_DFL);
    std::raise (SIGABRT);
}

void postQuit()
{
    juce::MessageManager::callAsync ([] { if (quitCallback) quitCallback(); });
}

BOOL WINAPI onConsoleEvent (DWORD event)
{
    postQuit();
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT)
    {
        restoreDefaultOutput(); // the process ends when this handler returns
        Sleep (CLOSE_GRACE_MS);
    }
    return TRUE;
}

LRESULT CALLBACK sessionWindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ENDSESSION && wParam != FALSE)
        restoreDefaultOutput();
    return DefWindowProcW (window, message, wParam, lParam);
}

// Hidden top-level window (message-only windows get no session messages). Never destroyed.
void createSessionWindow()
{
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = sessionWindowProc;
    windowClass.hInstance = GetModuleHandleW (nullptr);
    windowClass.lpszClassName = SESSION_WINDOW_CLASS;
    RegisterClassW (&windowClass);
    CreateWindowExW (WS_EX_TOOLWINDOW, SESSION_WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0,
                     nullptr, nullptr, windowClass.hInstance, nullptr);
}
} // namespace

void installQuitSignals (std::function<void()> onQuit)
{
    quitCallback = std::move (onQuit);
    SetConsoleCtrlHandler (onConsoleEvent, TRUE);
    createSessionWindow();
}

void installCrashHandlers()
{
    previousFilter = SetUnhandledExceptionFilter (onUnhandledException);
    std::set_terminate (onTerminate);
    std::signal (SIGABRT, onAbort);
}

void setRestoreEndpoint (const wchar_t* endpointId)
{
    hasRestoreId.store (false);
    if (endpointId == nullptr || *endpointId == L'\0')
        return;
    wcsncpy_s (restoreId, endpointId, _TRUNCATE);
    restoring.store (false);
    hasRestoreId.store (true);
}
} // namespace CrashGuard
