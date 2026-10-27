#include "ui/MainWindow.h"
#include "ui/DockReopen.h"
#include "ui/Theme.h"
#include "ui/WindowChrome.h"

namespace
{
    namespace D = Theme::Design;

    constexpr float preferredScale = 0.76f;
    constexpr float minimumScale = 0.58f;   // below this the engraved legends become unreadable
    constexpr float maximumScale = 1.5f;
    constexpr int screenMargin = 60;

    constexpr float captionBottom = 50.0f;   // design units: the badge / title / device-plate strip
    constexpr int resizeEdge = 6;            // window px left to the edge-resize emulation at the very top

    int heightForWidth (int width) { return juce::roundToInt ((float) width * D::height / D::width); }
    int widthForHeight (int height) { return juce::roundToInt ((float) height * D::width / D::height); }
}

MainWindow::ScaledHost::ScaledHost (Controller& c, juce::DocumentWindow& window)
    : panel (c),
      windowButtons (WindowChrome::createWindowButtons ([&window] { window.setMinimised (true); },
                                                        [&window] { window.closeButtonPressed(); }))
{
    addAndMakeVisible (panel);
    if (windowButtons != nullptr)
    {
        panel.addAndMakeVisible (*windowButtons);
        windowButtons->setBounds (D::windowButtons.toNearestInt());
    }
}

bool MainWindow::ScaledHost::isCaptionAt (juce::Point<float> position) const
{
    const auto design = position.transformedBy (panel.getTransform().inverted());
    if (design.y >= captionBottom || position.y < (float) resizeEdge)
        return false;
    // getComponentAt only hit-tests locally (never the OS), so it is safe inside findControlAtPoint.
    return const_cast<MainPanel&> (panel).getComponentAt (design) == &panel;
}

void MainWindow::ScaledHost::resized()
{
    // Letterbox as a fallback: programmatic sizes do not pass through the constrainer.
    const auto scale = juce::jmin ((float) getWidth() / D::width, (float) getHeight() / D::height);
    const auto x = ((float) getWidth() - D::width * scale) * 0.5f;
    const auto y = ((float) getHeight() - D::height * scale) * 0.5f;
    panel.setTransform (juce::AffineTransform::scale (scale).translated (x, y));
}

void MainWindow::ScaledHost::paint (juce::Graphics& g)
{
    g.fillAll (Theme::desk);
}

//==============================================================================
int MainWindow::PanelConstrainer::titleBarHeight() const
{
    if (auto* peer = window.getPeer())
        if (const auto frame = peer->getFrameSizeIfPresent())
            return frame->getTop();
    return 0;
}

void MainWindow::PanelConstrainer::checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>&, const juce::Rectangle<int>& limits,
                                                bool isStretchingTop, bool isStretchingLeft, bool isStretchingBottom, bool isStretchingRight)
{
    const auto titleBar = titleBarHeight();
    const auto isOnlyVertical = (isStretchingTop || isStretchingBottom) && ! (isStretchingLeft || isStretchingRight);
    const auto proposedWidth = isOnlyVertical ? widthForHeight (bounds.getHeight() - titleBar) : bounds.getWidth();
    const auto widestOnScreen = limits.isEmpty() ? getMaximumWidth() : widthForHeight (limits.getHeight() - titleBar);
    const auto maxWidth = juce::jmax (getMinimumWidth(), juce::jmin (getMaximumWidth(), widestOnScreen));
    const auto width = juce::jlimit (getMinimumWidth(), maxWidth, proposedWidth);
    const auto height = heightForWidth (width) + titleBar;

    if (isStretchingLeft)
        bounds.setLeft (bounds.getRight() - width);
    else
        bounds.setWidth (width);

    if (isStretchingTop)
        bounds.setTop (bounds.getBottom() - height);
    else
        bounds.setHeight (height);
}

//==============================================================================
MainWindow::MainWindow (Controller& controller)
    : juce::DocumentWindow ("OscillaFX", Theme::desk, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      host (controller, *this)
{
    setUsingNativeTitleBar (! WindowChrome::drawsOwnTitleBar());
    if (WindowChrome::drawsOwnTitleBar())
        setTitleBarHeight (0);
    if (auto* peer = getPeer())
        WindowChrome::useFullSizeContent (*peer);
    setContentNonOwned (&host, false);

    auto scale = preferredScale;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        scale = juce::jlimit (minimumScale, preferredScale, (float) (display->userArea.getHeight() - screenMargin) / D::height);

    constrainer.setSizeLimits ((int) (D::width * minimumScale), heightForWidth ((int) (D::width * minimumScale)),
                               (int) (D::width * maximumScale), heightForWidth ((int) (D::width * maximumScale)));
    setConstrainer (&constrainer);
    setResizable (true, false);

    centreWithSize (juce::roundToInt (D::width * scale), heightForWidth (juce::roundToInt (D::width * scale)));
    DockReopen::setHandler ([this] { present(); });
}

void MainWindow::addToDesktop (int windowStyleFlags, void* nativeWindowToAttachTo)
{
    juce::DocumentWindow::addToDesktop (windowStyleFlags, nativeWindowToAttachTo);
    if (auto* peer = getPeer())
        WindowChrome::useFullSizeContent (*peer);   // every time JUCE recreates the native window
}

MainWindow::~MainWindow()
{
    DockReopen::setHandler (nullptr);
    setConstrainer (nullptr);
}

void MainWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged();
    if (auto* peer = getPeer())
        WindowChrome::useFullSizeContent (*peer);
}

void MainWindow::closeButtonPressed()
{
    setVisible (false); // keeps running in the menu bar / tray; the Dock icon, tray icon or a relaunch brings it back
}

juce::BorderSize<int> MainWindow::getBorderThickness() const
{
    // Windows: the panel reaches the window edge; JUCE emulates the resize border inside it.
    return WindowChrome::drawsOwnTitleBar() ? juce::BorderSize<int>() : juce::DocumentWindow::getBorderThickness();
}

juce::Component::WindowControlKind MainWindow::findControlAtPoint (juce::Point<float> position) const
{
    if (! WindowChrome::drawsOwnTitleBar())
        return juce::DocumentWindow::findControlAtPoint (position);
    return host.isCaptionAt (position) ? WindowControlKind::caption : WindowControlKind::client;
}

void MainWindow::present()
{
    if (isMinimised())
        setMinimised (false);
    setVisible (true);
    toFront (true);
    juce::Process::makeForegroundProcess();
}

void MainWindow::showTour()
{
    host.showTour();
}
