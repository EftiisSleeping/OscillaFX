#include "ui/TrayIcon.h"
#include "ui/Brand.h"

TrayIcon::TrayIcon (Controller& c, std::function<void()> show, std::function<void()> tour, std::function<void()> quitApp)
    : controller (c), showWindow (std::move (show)), showTour (std::move (tour)), quit (std::move (quitApp))
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (&menuModel);
   #endif
    iconPowerOn = controller.settings().power;
   #if JUCE_WINDOWS
    iconForLightTaskbar = isTaskbarLight();
   #endif
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

#if JUCE_WINDOWS
namespace
{
constexpr int windowsIconSize = 32;   // Windows scales it to 16 / 20 / 24 px per DPI

bool isTaskbarLight()
{
    return juce::WindowsRegistry::getValue ("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize\\SystemUsesLightTheme",
                                            "0").getIntValue() != 0;
}

// The silhouette in a solid colour: white on a dark taskbar, near-black on a light one.
juce::Image tintedGlyph (juce::Colour colour)
{
    juce::Image icon (juce::Image::ARGB, windowsIconSize, windowsIconSize, true);
    juce::Graphics g (icon);
    g.setColour (colour);
    g.drawImage (Brand::trayGlyph(), icon.getBounds().toFloat(), juce::RectanglePlacement::centred, true);
    return icon;
}
}
#endif

// macOS: the butterfly silhouette is a template image, tinted by the system for light and dark menu
// bars. Windows has no template images: the glyph is drawn in the taskbar's contrast colour. Power off
// dims it (alpha only, so it stays a valid template).
juce::Image TrayIcon::drawIcon (bool powerOn)
{
   #if JUCE_WINDOWS
    auto glyph = tintedGlyph (isTaskbarLight() ? juce::Colour (0xff1a1a1a) : juce::Colours::white);
   #else
    auto glyph = Brand::trayGlyph().createCopy();
   #endif
    if (! powerOn)
        glyph.multiplyAllAlphas (0.45f);
    return glyph;
}

void TrayIcon::refreshIcon()
{
    const auto on = controller.settings().power;
   #if JUCE_WINDOWS
    const auto light = isTaskbarLight();
    if (on == iconPowerOn && light == iconForLightTaskbar)
        return;
    iconForLightTaskbar = light;
   #else
    if (on == iconPowerOn)
        return;
   #endif
    iconPowerOn = on;
    setIconImage (drawIcon (on), drawIcon (on));
}

void TrayIcon::mouseDown (const juce::MouseEvent& e)
{
    refreshIcon();
   #if JUCE_MAC
    juce::ignoreUnused (e);
    showDropdownMenu (buildMenu());
   #else
    if (! e.mods.isPopupMenu())   // Windows: left click opens the window, right click the menu
    {
        showWindow();
        return;
    }
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
   #if JUCE_WINDOWS   // Windows wording
    const char* const startAtLoginText = "Start with Windows";
    const char* const quitText = "Exit OscillaFX";
   #else
    const char* const startAtLoginText = "Start at Login";
    const char* const quitText = "Quit OscillaFX";
   #endif
    menu.addItem (startAtLoginText, true, startsAtLogin, [this, startsAtLogin] { controller.setLaunchAtLogin (! startsAtLogin); });
    menu.addSeparator();
    menu.addItem ("Show Tour", showTour);
    menu.addItem ("Show OscillaFX", showWindow);
    menu.addItem (quitText, quit);
    return menu;
}
