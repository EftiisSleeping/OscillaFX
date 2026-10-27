#pragma once

#include "ui/MainPanel.h"

// Fixed-aspect, freely scalable window with no title-bar strip: the panel fills it edge to edge and
// the window buttons sit in its top-left corner (Theme::Design::windowButtons): macOS traffic lights
// floating over it, on Windows the panel's own keys (WindowChrome::createWindowButtons) in a
// borderless window whose top strip is the caption. Closing it only hides it; the app lives on in
// the menu bar / taskbar tray.
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (Controller&);
    ~MainWindow() override;

    void addToDesktop (int windowStyleFlags, void* nativeWindowToAttachTo = nullptr) override;

    void visibilityChanged() override;
    void closeButtonPressed() override;
    juce::BorderSize<int> getBorderThickness() const override;
    WindowControlKind findControlAtPoint (juce::Point<float>) const override;
    void present();   // show, un-minimise and focus
    void showTour();  // the first-run walk-through again

private:
    class ScaledHost : public juce::Component
    {
    public:
        ScaledHost (Controller&, juce::DocumentWindow&);
        void showTour() { panel.showTour(); }
        bool isCaptionAt (juce::Point<float>) const;  // bare panel in the top strip (no control there)
        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        MainPanel panel;
        std::unique_ptr<juce::Component> windowButtons;  // Windows only
    };

    // macOS hands the constrainer the whole frame, title bar included: keep the *content* at the
    // panel's aspect ratio at every size, so the panel is never letterboxed.
    class PanelConstrainer : public juce::ComponentBoundsConstrainer
    {
    public:
        explicit PanelConstrainer (juce::Component& w) : window (w) {}
        void checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
                          bool isStretchingTop, bool isStretchingLeft, bool isStretchingBottom, bool isStretchingRight) override;

    private:
        int titleBarHeight() const;
        juce::Component& window;
    };

    ScaledHost host;
    PanelConstrainer constrainer { *this };
};
