#include "ui/TrayIcon.h"
#include "ui/Brand.h"

TrayIcon::TrayIcon (Controller& c, std::function<void()> show, std::function<void()> tour, std::function<void()> quitApp)
    : controller (c), showWindow (std::move (show)), showTour (std::move (tour)), quit (std::move (quitApp))
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (&menuModel);
   #endif
    iconPowerOn = controller.settings().power;
    setIconImage (drawIcon (iconPowerOn), drawIcon (iconPowerOn));
    setIconTooltip ("OscillaFX");
    startTimer (1000);
}

TrayIcon::~TrayIcon()
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
}

// The butterfly silhouette is a template image: macOS tints it for light and dark menu bars. Power off
// dims it (alpha only, so it stays a valid template).
juce::Image TrayIcon::drawIcon (bool powerOn)
{
    auto glyph = Brand::trayGlyph().createCopy();
    if (! powerOn)
        glyph.multiplyAllAlphas (0.45f);
    return glyph;
}

void TrayIcon::refreshIcon()
{
    const auto on = controller.settings().power;
    if (on == iconPowerOn)
        return;
    iconPowerOn = on;
    setIconImage (drawIcon (on), drawIcon (on));
}

void TrayIcon::mouseDown (const juce::MouseEvent&)
{
    refreshIcon();
   #if JUCE_MAC
    showDropdownMenu (buildMenu());
   #else
    buildMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition());
   #endif
}

juce::PopupMenu TrayIcon::buildMenu()
{
    const auto& settings = controller.settings();
    juce::PopupMenu menu;
    if (controller.needsMicrophoneAccess())
    {
        menu.addItem (juce::String (juce::CharPointer_UTF8 ("Allow Microphone Access\xe2\x80\xa6")), [this] { controller.openMicrophoneSettings(); });
        menu.addSeparator();
    }

    const auto isOn = settings.power;
    menu.addItem ("Sound Enhancement", true, isOn, [this, isOn] { controller.update ([isOn] (DspSettings& s) { s.power = ! isOn; }); });
    menu.addSeparator();

    juce::PopupMenu presets;
    const auto presetNames = controller.presetNames();
    for (const auto& name : presetNames)
        presets.addItem (name, true, name == settings.presetName, [this, name] { controller.loadPreset (name); });
    menu.addSubMenu ("Preset", presets, ! presetNames.isEmpty());

    juce::PopupMenu devices;
    const auto active = controller.activeDevice().uid;
    const auto outputs = controller.outputDevices();
    for (const auto& device : outputs)
        devices.addItem (device.name, true, device.uid == active, [this, uid = device.uid] { controller.selectDevice (uid); });
    menu.addSubMenu ("Output Device", devices, ! outputs.isEmpty());

    menu.addSeparator();
    const auto isNight = settings.nightMode;
    menu.addItem ("Night Mode", true, isNight, [this, isNight] { controller.update ([isNight] (DspSettings& s) { s.nightMode = ! isNight; }); });

    juce::PopupMenu limits;
    for (const float ceiling : { 0.0f, -6.0f, -12.0f, -18.0f })
        limits.addItem (ceiling == 0.0f ? juce::String ("Off") : juce::String ((int) ceiling) + " dB", true,
                        std::abs (settings.limiterCeilingDb - ceiling) < 0.5f,
                        [this, ceiling] { controller.update ([ceiling] (DspSettings& s) { s.limiterCeilingDb = ceiling; }); });
    menu.addSubMenu ("Volume Limit", limits);

    const auto isSlideshow = controller.gallerySlideshow();
    menu.addItem ("Gallery Slideshow", true, isSlideshow, [this, isSlideshow] { controller.setGallerySlideshow (! isSlideshow); });

    menu.addSeparator();
    const auto startsAtLogin = controller.launchAtLogin();
    menu.addItem ("Start at Login", true, startsAtLogin, [this, startsAtLogin] { controller.setLaunchAtLogin (! startsAtLogin); });
    menu.addSeparator();
    menu.addItem ("Show Tour", showTour);
    menu.addItem ("Show OscillaFX", showWindow);
    menu.addItem ("Quit OscillaFX", quit);
    return menu;
}
