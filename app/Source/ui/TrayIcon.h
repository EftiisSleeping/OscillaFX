#pragma once

#include "core/Controller.h"
#include <juce_gui_extra/juce_gui_extra.h>

// macOS menu bar item / Windows notification-area (tray) icon: power, presets, output device, night mode, volume limit, gallery slideshow,
// start at login, show tour, show window, quit.
class TrayIcon : public juce::SystemTrayIconComponent, private juce::Timer
{
public:
    TrayIcon (Controller&, std::function<void()> showWindow, std::function<void()> showTour, std::function<void()> quit);
    ~TrayIcon() override;

    void mouseDown (const juce::MouseEvent&) override;

private:
    // NSMenu items only fire their actions while a main-menu model is installed.
    class EmptyMenuModel : public juce::MenuBarModel
    {
    public:
        juce::StringArray getMenuBarNames() override { return {}; }
        juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
        void menuItemSelected (int, int) override {}
    };

    void timerCallback() override { refreshIcon(); }
    juce::PopupMenu buildMenu();
    static juce::Image drawIcon (bool powerOn);
    void refreshIcon();   // dims the glyph while enhancement is off; Windows: follows the taskbar theme

    Controller& controller;
    std::function<void()> showWindow, showTour, quit;
    EmptyMenuModel menuModel;
    bool iconPowerOn = true;
   #if JUCE_WINDOWS
    bool iconForLightTaskbar = false;
   #endif
};
