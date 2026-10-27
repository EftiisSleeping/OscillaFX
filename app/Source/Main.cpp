#include "platform/CrashGuard.h"
#include "platform/CrashWatchdog.h"
#include "core/Controller.h"
#include "platform/LoginItem.h"
#include "ui/MainWindow.h"
#include "ui/TrayIcon.h"
#include "ui/dev/PreviewSnapshots.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace
{
// packaging/uninstall.sh runs the app binary with this to drop the SMAppService login item.
const char* const UNREGISTER_LOGIN_ITEM_ARG = "--unregister-login-item";

// The Windows installer runs `OscillaFX.exe --quit` before upgrading / uninstalling: the running
// instance quits normally (engine stop restores the default output); with none running, exits at once.
const char* const QUIT_ARG = "--quit";

bool hasArgument (const juce::String& commandLine, const char* argument)
{
    return juce::StringArray::fromTokens (commandLine, true).contains (argument);
}
}

class OscillaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
#if defined (OSCILLA_UI_STUB) && OSCILLA_UI_STUB
    bool moreThanOneInstanceAllowed() override { return true; }   // UI previews must not hand over to an installed OscillaFX
#else
    bool moreThanOneInstanceAllowed() override { return false; }
#endif

    void initialise (const juce::String& commandLine) override
    {
        if (hasArgument (commandLine, QUIT_ARG))   // no instance to stop
        {
            quit();
            return;
        }
        if (commandLine.contains (UNREGISTER_LOGIN_ITEM_ARG))
        {
            LoginItem::setEnabled (false);
            quit();
            return;
        }
        CrashGuard::installQuitSignals ([] { quit(); }); // SIGTERM/INT/HUP -> normal shutdown restores output
        CrashGuard::installCrashHandlers();
        CrashWatchdog::launch(); // restores the output even after SIGKILL / Force Quit
        controller = std::make_unique<Controller>();
        controller->startAudio(); // on failure the CRT shows controller->lastError()

        window = std::make_unique<MainWindow> (*controller);
        tray = std::make_unique<TrayIcon> (*controller, [this] { window->present(); }, [this] { window->present(); window->showTour(); }, [] { quit(); });
        LoginItem::onLaunchFinished ([this] (bool launchedAtLogin)
        {
            if (! launchedAtLogin && window != nullptr) // at login start minimised to the menu bar
                window->present();
        });
        PreviewSnapshots::scheduleIfRequested (*controller, *window->getContentComponent());
    }

    void shutdown() override
    {
        tray = nullptr;
        window = nullptr;
        controller = nullptr; // Controller's destructor stops the engine and restores the system output
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (hasArgument (commandLine, QUIT_ARG))
        {
            quit();   // shutdown() tears down the controller, which restores the system output
            return;
        }
        if (commandLine.contains (UNREGISTER_LOGIN_ITEM_ARG))
        {
            LoginItem::setEnabled (false);
            return;
        }
        if (window != nullptr)
            window->present();
    }

private:
    std::unique_ptr<Controller> controller;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
};

START_JUCE_APPLICATION (OscillaApplication)
