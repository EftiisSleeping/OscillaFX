#include "ui/WindowChrome.h"
#include "ui/Hardware.h"
#include "ui/PanelWidgets.h"
#include "ui/Theme.h"

#include <windows.h>

namespace WindowChrome
{
namespace
{
// A second launch hands its command line to the running instance (anotherInstanceStarted) and exits.
// Windows only lets a process take the foreground if the foreground process allows it, so every
// launch allows it up front: the running instance can then bring its window to the front.
const bool allowsForegroundHandOver = AllowSetForegroundWindow (ASFW_ANY) != FALSE;

// Cream panel key with an engraved minimise bar or close cross, like the StepKey arrows.
class WindowKey : public PanelButton
{
public:
    enum class Kind { minimise, close };

    WindowKey (Kind k, std::function<void()> onPress) : PanelButton (k == Kind::minimise ? "Minimise" : "Close"), kind (k)
    {
        setTooltip (k == Kind::minimise ? "Minimise" : "Close - OscillaFX keeps running in the taskbar tray");
        setWantsKeyboardFocus (true);
        onClick = std::move (onPress);
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        Hardware::creamKey (g, area, isDown, isHighlighted, isEnabled());

        const auto glyph = juce::Rectangle<float> (12.0f, 12.0f).withCentre (area.getCentre().translated (0.0f, isDown ? 1.0f : 0.0f));
        const auto ink = kind == Kind::close && isHighlighted ? Theme::ledRed.darker (0.35f) : Theme::legendInk;
        g.setColour (ink.withAlpha (0.95f));
        if (kind == Kind::minimise)
            g.fillRect (glyph.withTop (glyph.getBottom() - 2.5f));
        else
        {
            juce::Path cross;
            cross.addLineSegment ({ glyph.getTopLeft(), glyph.getBottomRight() }, 2.4f);
            cross.addLineSegment ({ glyph.getTopRight(), glyph.getBottomLeft() }, 2.4f);
            g.fillPath (cross);
        }

        if (isShowingFocusRing())
            Hardware::focusRing (g, area.expanded (2.0f), 6.0f);
    }

private:
    Kind kind;
};

// Fills Theme::Design::windowButtons: Windows order, minimise then close, centred in the corner.
class WindowButtons : public juce::Component
{
public:
    WindowButtons (std::function<void()> onMinimise, std::function<void()> onClose)
        : minimise (WindowKey::Kind::minimise, std::move (onMinimise)), close (WindowKey::Kind::close, std::move (onClose))
    {
        setTitle ("Window controls");
        addAndMakeVisible (minimise);
        addAndMakeVisible (close);
    }

    void resized() override
    {
        constexpr float keyWidth = 38.0f, keyHeight = 32.0f, gap = 10.0f;
        auto row = juce::Rectangle<float> (keyWidth * 2.0f + gap, keyHeight).withCentre (getLocalBounds().toFloat().getCentre());
        minimise.setBounds (row.removeFromLeft (keyWidth).toNearestInt());
        close.setBounds (row.removeFromRight (keyWidth).toNearestInt());
    }

private:
    WindowKey minimise, close;
};
} // namespace

bool drawsOwnTitleBar() { return true; }

std::unique_ptr<juce::Component> createWindowButtons (std::function<void()> onMinimise, std::function<void()> onClose)
{
    return std::make_unique<WindowButtons> (std::move (onMinimise), std::move (onClose));
}

void useFullSizeContent (juce::ComponentPeer&) {}

void detentHaptic() {}
}
