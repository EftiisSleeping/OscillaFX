#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

// Native window touches that JUCE does not expose. macOS: native title bar made transparent, traffic
// lights float over the panel. Windows: borderless window, the panel draws its own buttons.
namespace WindowChrome
{
    // True where the window has no native title bar (Windows): MainWindow then drops its border,
    // maps the panel's top strip to the system caption (drag, Aero Snap) and shows createWindowButtons().
    bool drawsOwnTitleBar();

    // Minimise / close keys for the panel's reserved top-left corner (Theme::Design::windowButtons,
    // in design units); nullptr on macOS, whose traffic lights sit there.
    std::unique_ptr<juce::Component> createWindowButtons (std::function<void()> onMinimise, std::function<void()> onClose);

    // Lets the panel fill the whole window: no title-bar strip, hidden title, and the traffic-light
    // buttons float over the top-left corner of the content. Safe to call again after the peer is recreated.
    void useFullSizeContent (juce::ComponentPeer&);

    // A light trackpad tap, for faders that snap to their detent. Does nothing without a haptic trackpad.
    void detentHaptic();
}
