#include "ui/MainPanel.h"
#include "ui/PanelArt.h"
#include "ui/PresetBlurbs.h"
#include "core/GalleryStore.h"

namespace
{
    namespace D = Theme::Design;

    constexpr int rebuildDelayMs = 150;
    constexpr int meterHz = 30;
    constexpr float effectMax = 10.0f;
    constexpr float gainRangeDb = 12.0f;
    constexpr float eqRangeDb = 12.0f;
    constexpr float powerSwitchWidth = 128.0f, powerSwitchHeight = 32.0f;
    constexpr float eqLabelWidth = 76.0f, eqLabelHeight = 22.0f;

    juce::String percentText (double v)
    {
        return juce::String (juce::roundToInt (v / effectMax * 100.0)) + " %";
    }

    juce::String gainText (double v)
    {
        return (v > 0.04 ? "+" : "") + juce::String (v, 1) + " dB";
    }

    juce::String balanceText (double v)
    {
        const auto percent = juce::roundToInt (std::abs (v) * 100.0);
        if (percent == 0)
            return "Centre";
        return juce::String (v < 0.0 ? "Left " : "Right ") + juce::String (percent) + " %";
    }

    // "Bass - ambience (Quizal)" -> "BASS - AMBIENCE": panel legends drop the author.
    juce::String presetLegend (const juce::String& name)
    {
        return name.upToFirstOccurrenceOf (" (", false, false).trim().toUpperCase();
    }

    // 62.5 -> "62", 396.85 -> "397", 1360.79 -> "1.4K", 16000 -> "16K"
    juce::String frequencyText (float hz)
    {
        if (hz < 1000.0f)
            return juce::String ((int) std::floor (hz + 0.25f));
        const auto kilo = hz / 1000.0f;
        return (kilo < 10.0f ? juce::String (kilo, 1) : juce::String (juce::roundToInt (kilo))) + "K";
    }

    juce::String frequencyWords (float hz)
    {
        if (hz < 100.0f)   return "rumble and deep bass";
        if (hz < 300.0f)   return "bass and warmth";
        if (hz < 800.0f)   return "body and fullness";
        if (hz < 2000.0f)  return "voices and instruments";
        if (hz < 5000.0f)  return "clarity and bite";
        if (hz < 10000.0f) return "crispness";
        return "air and sparkle";
    }

    juce::String frequencyTitle (float hz)
    {
        return "EQ " + frequencyText (hz) + " Hz";
    }

    juce::String equalizerHint (float hz)
    {
        return frequencyTitle (hz) + " - " + frequencyWords (hz) + ". Drag up for more, down for less";
    }
}

MainPanel::MainPanel (Controller& c)
    : controller (c), crt (c.engine()), scopeLeft ((size_t) meterWindowSamples), scopeRight ((size_t) meterWindowSamples)
{
    setSize ((int) D::width, (int) D::height);
    setOpaque (true);
    setTitle ("OscillaFX front panel");
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);

    configureFaders();
    configureEqualizer();
    configureMaster();
    configureDisplay();
    configurePresets();
    addControls();
    setFocusOrder();
    tooltips.setLookAndFeel (&lookAndFeel.get());
    resized();   // the controls were added after setSize()

    reloadPresetList();
    controller.addChangeListener (this);
    refreshFromController();
    meterTimer.startTimerHz (meterHz);
    if (controller.firstRunPending())
        showTour();
}

MainPanel::~MainPanel()
{
    meterTimer.stopTimer();
    controller.removeChangeListener (this);
    tooltips.setLookAndFeel (nullptr);
}

//==============================================================================
void MainPanel::describe (Knob& knob, const juce::String& text)
{
    knob.describe (text);
    hints.attach (knob, text);
}

void MainPanel::describe (Fader& fader, const juce::String& text)
{
    fader.describe (text);
    hints.attach (fader, text);
}

void MainPanel::describe (RotarySelector& selector, const juce::String& text)
{
    selector.describe (text);
    hints.attach (selector, text);
    hints.attach (selector.getKnob(), text);
}

void MainPanel::describe (juce::Button& button, const juce::String& text)
{
    button.setTooltip (text);
    button.setHelpText (text);
    hints.attach (button, text);
}

void MainPanel::bindSlider (juce::Slider& slider, float DspSettings::* field)
{
    // Volume and balance are not part of a preset, so moving them keeps the preset unmodified.
    const auto isPartOfPreset = field != &DspSettings::masterGainDb && field != &DspSettings::balance;
    slider.onValueChange = [this, &slider, field, isPartOfPreset]
    {
        const auto value = (float) slider.getValue();
        if (isPartOfPreset)
            markModified();
        controller.update ([field, value] (DspSettings& s) { s.*field = value; });
    };
}

void MainPanel::configureFaders()
{
    const DspSettings defaults;
    fidelity.configure (0.0, effectMax, defaults.fidelity, 0.1, percentText);
    ambience.configure (0.0, effectMax, defaults.ambience, 0.1, percentText);
    dynamicBoost.configure (0.0, effectMax, defaults.dynamicBoost, 0.1, percentText);
    widerSound.configure (0.0, effectMax, defaults.surround, 0.1, percentText);
    bass.configure (0.0, effectMax, defaults.bass, 0.1, percentText);

    balance.configure (-1.0, 1.0, defaults.balance, 0.02, balanceText);
    balance.setDetent (0.0, 0.06);

    for (auto* fader : { &fidelity, &ambience, &dynamicBoost, &widerSound, &bass, &balance })
        fader->setReadout (&readout);

    bindSlider (fidelity, &DspSettings::fidelity);
    bindSlider (ambience, &DspSettings::ambience);
    bindSlider (dynamicBoost, &DspSettings::dynamicBoost);
    bindSlider (widerSound, &DspSettings::surround);
    bindSlider (bass, &DspSettings::bass);
    bindSlider (balance, &DspSettings::balance);

    describe (fidelity, "Fidelity - brings back clarity and detail");
    describe (ambience, "Ambience - adds a sense of room and space");
    describe (dynamicBoost, "Dynamic boost - lifts quiet parts so everything sounds fuller");
    describe (widerSound, "Wider sound (2 speakers) - makes stereo feel bigger");
    describe (bass, "Bass - heavier thump");
    describe (balance, "Balance - moves the sound toward the left or right speaker");
}

void MainPanel::configureEqualizer()
{
    for (int i = 0; i < 5; ++i)
        effectLcds.push_back (std::make_unique<LcdReadout>());

    for (int band = 0; band < DspSettings::numEqBands; ++band)
    {
        eqLcds.push_back (std::make_unique<LcdReadout>());
        auto fader = std::make_unique<Fader> (frequencyTitle (DspSettings {}.eqFreqHz[(size_t) band]), Fader::Orientation::vertical);
        fader->configure (-eqRangeDb, eqRangeDb, 0.0, 0.1, gainText);
        fader->setDetent (0.0, 0.5);
        fader->setReadout (&readout);
        fader->onValueChange = [this, band, f = fader.get()]
        {
            const auto gain = (float) f->getValue();
            markModified();
            controller.update ([band, gain] (DspSettings& s) { s.eqGainDb[(size_t) band] = gain; });
        };
        describe (*fader, equalizerHint (DspSettings {}.eqFreqHz[(size_t) band]));
        eqFaders.push_back (std::move (fader));
        eqLabels.push_back (std::make_unique<PrintedLabel> (13.0f, true, juce::Justification::centred));
    }

    crt.onEqBandChanged = [this] (int band, float db)
    {
        markModified();
        controller.update ([band, db] (DspSettings& s) { s.eqGainDb[(size_t) band] = db; });
    };
}

void MainPanel::configureMaster()
{
    const DspSettings defaults;
    volume.configure (-gainRangeDb, gainRangeDb, defaults.masterGainDb, 0.5, gainText);
    volume.setReadout (&readout);
    bindSlider (volume, &DspSettings::masterGainDb);
    describe (volume, "Volume - overall loudness, from -12 to +12 dB");

    nightKey.setClickingTogglesState (true);
    nightKey.setLampShowsToggleState (true);
    describe (nightKey, "Night mode - evens out loud and quiet parts so you can listen quietly");
    nightKey.onClick = [this]
    {
        const auto isOn = nightKey.getToggleState();
        controller.update ([isOn] (DspSettings& s) { s.nightMode = isOn; });
    };

    limit.configure (DspSettings::limiterMinDb, 0.0, 0.0, 1.0, [] (double v) { return v > -0.5 ? juce::String ("Off") : juce::String (v, 0) + " dB"; });
    limit.setDetent (0.0, 0.0);
    limit.setReadout (&readout);
    limit.onValueChange = [this]
    {
        const auto ceiling = (float) limit.getValue();
        controller.update ([ceiling] (DspSettings& s) { s.limiterCeilingDb = ceiling; });
    };
    describe (limit, "Caps the loudest sound to protect your ears (far right is off)");

    describe (powerSwitch, "Power - turns the sound enhancement on or off");
    powerSwitch.onClick = [this]
    {
        const auto isOn = powerSwitch.getToggleState();
        controller.update ([isOn] (DspSettings& s) { s.power = isOn; });
    };

    describe (deviceButton, "Output device - the speakers or headphones OscillaFX plays through");
    deviceButton.onClick = [this] { showDeviceMenu(); };

    microphoneButton.setButtonText ("ALLOW MICROPHONE");
   #if JUCE_WINDOWS
    describe (microphoneButton, "OscillaFX needs microphone access to hear your PC's sound - opens "
                                "Settings > Privacy & security > Microphone");
   #else
    describe (microphoneButton, "OscillaFX needs microphone access to hear your Mac's sound - opens "
                                "System Settings > Privacy & Security > Microphone");
   #endif
    microphoneButton.onClick = [this] { controller.openMicrophoneSettings(); };
}

void MainPanel::configureDisplay()
{
    displayMode.setItems ({ "Spectrum", "Wave", "X-Y", "Gallery" }, 0);
    describe (displayMode, "Display - what the screen shows: spectrum, wave, X-Y or pictures");
    displayMode.setReadout (&readout);
    displayMode.onSelect = [this] (int index) { controller.update ([index] (DspSettings& s) { s.displayMode = index; }); };

    describe (screenSwitch, "Switch the screen between the live sound and your equalizer curve. This only changes the screen: the EQ sliders below always work");
    screenSwitch.onClick = [this]
    {
        const auto isEq = screenSwitch.getToggleState();
        controller.update ([isEq] (DspSettings& s) { s.eqEditMode = isEq; });
    };

    describe (nextImage, "Next image - shows the next picture (Gallery display only)");
    nextImage.onClick = [this] { showNextImage(); };

    describe (galleryAdd, "Add images - pick your own pictures for the gallery (or drop them on the window)");
    galleryAdd.setButtonText ("ADD IMAGES");
    galleryAdd.onClick = [this] { addPictures(); };
    describe (galleryRemove, "Remove image - deletes the shown picture, if it is one you added");
    galleryRemove.setButtonText ("REMOVE");
    galleryRemove.onClick = [this] { removeCurrentPicture(); };
    describe (gallerySlideshow, "Slideshow - changes the picture by itself; click to choose how often");
    gallerySlideshow.onClick = [this] { showSlideshowMenu(); };

    suggestionCard.onChoose = [this] (const juce::String& id) { controller.applySuggestion (id); };
    suggestionCard.onDismiss = [this] (bool dontAskAgain) { controller.dismissSuggestion (dontAskAgain); };

    tour.onFinished = [this] { controller.markFirstRunDone(); };

    crt.onGalleryIndexChanged = [this] (int index)
    {
        controller.update ([index] (DspSettings& s) { s.galleryIndex = index; });
    };
}

void MainPanel::configurePresets()
{
    presets.legendForItem = presetLegend;
    describe (presets, "Preset - a ready-made sound; turn the knob or click a name");
    presets.setReadout (&readout);
    presets.onSelect = [this] (int index)
    {
        isModifiedSincePreset = false; // re-selecting the same preset also discards edits
        controller.loadPreset (presetNames[index]);
        announcePreset (presetNames[index]);
    };

    describe (previousPreset, "Previous preset - go back one ready-made sound");
    describe (nextPreset, "Next preset - go forward one ready-made sound");
    describe (presetButton, "Preset list - click to pick any preset");
    previousPreset.onClick = [this] { stepPreset (-1); };
    nextPreset.onClick = [this] { stepPreset (1); };
    presetButton.onClick = [this] { showPresetMenu(); };
}

void MainPanel::addControls()
{
    for (auto* child : std::initializer_list<juce::Component*> {
             &crt, &vuLeft, &vuRight, &powerLed, &powerSwitch, &volume, &nightKey, &limit, &deviceButton,
             &displayMode, &screenSwitch, &nextImage,
             &presets, &previousPreset, &presetButton, &nextPreset, &presetBlurb,
             &fidelity, &ambience, &dynamicBoost, &widerSound, &bass, &balance })
        addAndMakeVisible (child);

    for (size_t band = 0; band < eqFaders.size(); ++band)
    {
        addAndMakeVisible (*eqFaders[band]);
        addAndMakeVisible (*eqLabels[band]);
        addAndMakeVisible (*eqLcds[band]);
    }
    for (auto& lcd : effectLcds)
        addAndMakeVisible (*lcd);
    addAndMakeVisible (balanceLcd);
    for (auto* child : { &galleryAdd, &galleryRemove, &gallerySlideshow })
        addChildComponent (child);
    addChildComponent (readout);
    addChildComponent (microphoneButton);
    addChildComponent (suggestionCard);
    addChildComponent (tour);
}

void MainPanel::setFocusOrder()
{
    int order = 1;
    const auto next = [&order] (juce::Component* c) { c->setExplicitFocusOrder (order++); };

    // The suggestion card (when showing) comes first, so keyboard users meet it without searching.
    for (size_t i = 0; i < suggestionCard.maxOptions; ++i)
        next (&suggestionCard.optionButton (i));
    next (&suggestionCard.keepButton());
    next (&suggestionCard.laterButton());

    for (auto* c : std::initializer_list<juce::Component*> {
             &microphoneButton, &powerSwitch, &volume, &deviceButton, &displayMode, &screenSwitch, &crt, &nextImage,
             &presets, &previousPreset, &presetButton, &nextPreset, &galleryAdd, &galleryRemove, &gallerySlideshow,
             &nightKey, &limit, &fidelity, &ambience, &dynamicBoost, &widerSound, &bass, &balance })
        next (c);

    for (auto& fader : eqFaders)
        next (fader.get());
}

//==============================================================================
void MainPanel::resized()
{
    crt.setBounds (D::glass.toNearestInt());
    microphoneButton.setBounds (D::micAlert.toNearestInt());

    powerLed.placeAt (D::powerLed);
    powerSwitch.setBounds (juce::Rectangle<float> (powerSwitchWidth, powerSwitchHeight).withCentre (D::powerSwitch).toNearestInt());
    volume.placeAt (D::volumeKnob);
    nightKey.setBounds (D::nightKey.toNearestInt());
    limit.setBounds (D::limitFader.toNearestInt());
    galleryAdd.setBounds (D::galleryAdd.toNearestInt());
    galleryRemove.setBounds (D::galleryRemove.toNearestInt());
    gallerySlideshow.setBounds (D::gallerySlideshow.toNearestInt());
    suggestionCard.setBounds (D::suggestionCard.toNearestInt());
    suggestionCard.setNotchX (D::devicePlate.getCentreX() - D::suggestionCard.getX());
    tour.setBounds (getLocalBounds());
    deviceButton.setBounds (D::devicePlate.toNearestInt());

    vuLeft.setBounds (D::vuLeft.toNearestInt());
    vuRight.setBounds (D::vuRight.toNearestInt());

    displayMode.placeAt (D::displaySelector);
    screenSwitch.setBounds (D::screenSwitch.toNearestInt());
    nextImage.setBounds (D::nextImage.toNearestInt());

    presets.placeAt (D::presetSelector);
    previousPreset.setBounds (D::presetPrev.toNearestInt());
    presetButton.setBounds (D::presetPlate.toNearestInt());
    nextPreset.setBounds (D::presetNext.toNearestInt());
    presetBlurb.setBounds (D::presetBlurb.toNearestInt());

    fidelity.setBounds (D::effectFader (0).toNearestInt());
    ambience.setBounds (D::effectFader (1).toNearestInt());
    dynamicBoost.setBounds (D::effectFader (2).toNearestInt());
    widerSound.setBounds (D::effectFader (3).toNearestInt());
    bass.setBounds (D::effectFader (4).toNearestInt());
    balance.setBounds (D::balanceFader.toNearestInt());
    for (size_t i = 0; i < effectLcds.size(); ++i)
        effectLcds[i]->setBounds (juce::Rectangle<float> (D::lcdWidth, D::lcdHeight).withCentre ({ D::effectFader ((int) i).getCentreX(), D::effectLcdY + D::lcdHeight * 0.5f }).toNearestInt());
    balanceLcd.setBounds (juce::Rectangle<float> (D::lcdWidth, D::lcdHeight).withCentre ({ D::balanceFader.getCentreX(), D::effectLcdY + D::lcdHeight * 0.5f }).toNearestInt());

    for (size_t band = 0; band < eqFaders.size(); ++band)
    {
        const auto bounds = D::eqFader ((int) band);
        eqFaders[band]->setBounds (bounds.toNearestInt());
        eqLcds[band]->setBounds (juce::Rectangle<float> (D::lcdWidth, D::lcdHeight).withCentre ({ bounds.getCentreX(), D::eqLcdY + D::lcdHeight * 0.5f }).toNearestInt());
        eqLabels[band]->setBounds (juce::Rectangle<float> (eqLabelWidth, eqLabelHeight).withCentre ({ bounds.getCentreX(), D::eqLabelY }).toNearestInt());
    }
}

void MainPanel::paint (juce::Graphics& g)
{
    const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! background.isValid())
    {
        backgroundScale = scale;
        background = juce::Image (juce::Image::RGB, juce::roundToInt (D::width * scale), juce::roundToInt (D::height * scale), false);
        juce::Graphics bg (background);
        bg.addTransform (juce::AffineTransform::scale (scale));
        PanelArt::paint (bg);
    }
    else if (! juce::approximatelyEqual (scale, backgroundScale))
    {
        startTimer (rebuildDelayMs); // keep drawing the stale bitmap while a live resize is in progress
    }

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (background, juce::AffineTransform::scale (1.0f / backgroundScale));
}

void MainPanel::timerCallback()
{
    stopTimer();
    background = {};
    repaint();
}

void MainPanel::mouseDown (const juce::MouseEvent& e)
{
    if (auto* window = getTopLevelComponent(); window != nullptr && ! e.mods.isPopupMenu())
        windowDragger.startDraggingComponent (window, e);
}

void MainPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (auto* window = getTopLevelComponent(); window != nullptr && ! e.mods.isPopupMenu())
        windowDragger.dragComponent (window, e, nullptr);
}

//==============================================================================
void MainPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromController();
}

void MainPanel::refreshFromController()
{
    const auto& s = controller.settings();
    refreshSliders (s);
    refreshEqualizer (s);
    refreshReadouts (s);

    powerSwitch.setToggleState (s.power, juce::dontSendNotification);
    powerLed.setLit (s.power);
    crt.setPowered (s.power);
    crt.setEqFrequencies (s.eqFreqHz);
    crt.setEqGains (s.eqGainDb);
    crt.setMessage (controller.lastError());

    nightKey.setToggleState (s.nightMode, juce::dontSendNotification);

    refreshDisplay (s);
    refreshMicrophoneAlert();
    refreshSuggestion();
    refreshDevice();     // before presets: a new device brings its own, unmodified profile
    refreshPresets (s);
}

void MainPanel::refreshSliders (const DspSettings& s)
{
    const auto sync = [] (juce::Slider& slider, float value)
    {
        if (! slider.isMouseButtonDown())
            slider.setValue (value, juce::dontSendNotification);
    };

    sync (fidelity, s.fidelity);
    sync (ambience, s.ambience);
    sync (dynamicBoost, s.dynamicBoost);
    sync (widerSound, s.surround);
    sync (bass, s.bass);
    sync (volume, s.masterGainDb);
    sync (balance, s.balance);
    sync (limit, s.limiterCeilingDb);

    for (auto* fader : { &fidelity, &ambience, &dynamicBoost, &widerSound, &bass, &balance })
        fader->setLampsOn (s.power);
}

void MainPanel::refreshReadouts (const DspSettings& s)
{
    const float effects[] = { s.fidelity, s.ambience, s.dynamicBoost, s.surround, s.bass };
    for (size_t i = 0; i < effectLcds.size(); ++i)
        effectLcds[i]->setText (juce::String (effects[i], 1));

    const auto percent = juce::roundToInt (std::abs (s.balance) * 100.0f);
    balanceLcd.setText (percent == 0 ? juce::String ("C") : (s.balance < 0.0f ? "L " : "R ") + juce::String (percent));

    for (size_t band = 0; band < eqLcds.size(); ++band)
        eqLcds[band]->setText ((s.eqGainDb[band] > 0.04f ? "+" : "") + juce::String (s.eqGainDb[band], 1));
}

void MainPanel::refreshEqualizer (const DspSettings& s)
{
    for (size_t band = 0; band < eqFaders.size(); ++band)
    {
        auto& fader = *eqFaders[band];
        const auto hz = s.eqFreqHz[band];
        if (! fader.isMouseButtonDown())
            fader.setValue (s.eqGainDb[band], juce::dontSendNotification);
        fader.setLampsOn (s.power);

        eqLabels[band]->setText (frequencyText (hz));
        const auto title = frequencyTitle (hz);
        if (title != fader.getTitle()) // presets move the band frequencies
        {
            fader.setTitle (title);
            fader.describe (equalizerHint (hz));
            hints.setHint (fader, equalizerHint (hz));
        }
    }
}

void MainPanel::refreshDisplay (const DspSettings& s)
{
    const auto mode = juce::jlimit (0, DspSettings::numDisplayModes - 1, s.displayMode);
    displayMode.setSelectedIndex (mode);
    crt.setMode (static_cast<CrtDisplay::Mode> (mode));
    crt.setGalleryIndex (s.galleryIndex);
    nextImage.setEnabled (mode == (int) CrtDisplay::Mode::gallery); // the picture count is checked on click

    screenSwitch.setToggleState (s.eqEditMode, juce::dontSendNotification);
    if (s.eqEditMode != isEqEditingShown) // setEqEditing() resets an in-progress band drag, so only call it on change
    {
        isEqEditingShown = s.eqEditMode;
        crt.setEqEditing (s.eqEditMode);
    }
    refreshGalleryControls (s);
}

void MainPanel::refreshMicrophoneAlert()
{
    microphoneButton.setVisible (controller.needsMicrophoneAccess()); // the CRT shows lastError() alongside
}

void MainPanel::refreshSuggestion()
{
    const auto suggestion = controller.pendingSuggestion();
    if (! suggestion.has_value())
    {
        suggestedDeviceUid = {};
        suggestionCard.hideCard();
        return;
    }

    if (suggestionCard.isVisible() && suggestedDeviceUid == suggestion->deviceUid)
        return;

    suggestedDeviceUid = suggestion->deviceUid;
    suggestionCard.showFor (*suggestion);
    for (size_t i = 0; i < suggestionCard.numOptions(); ++i)
        hints.attach (suggestionCard.optionButton (i), suggestionCard.optionDescription (i));
    hints.attach (suggestionCard.keepButton(), "Keep my settings - leave this device as it is and do not ask again");
    hints.attach (suggestionCard.laterButton(), "Not now - close this; it may come back next time you start OscillaFX");
}

juce::File MainPanel::currentPicture() const
{
    const auto pictures = controller.galleryImages();
    if (pictures.isEmpty())
        return {};
    const auto count = pictures.size();
    return pictures[((controller.settings().galleryIndex % count) + count) % count];
}

void MainPanel::refreshGalleryControls (const DspSettings& s)
{
    const auto isOn = controller.gallerySlideshow();
    const auto seconds = controller.gallerySlideshowSeconds();
    if (isOn != slideshowShown || seconds != slideshowSecondsShown)
    {
        slideshowShown = isOn;
        slideshowSecondsShown = seconds;
        crt.setGallerySlideshow (isOn, seconds);
    }

    const auto isGallery = s.displayMode == (int) CrtDisplay::Mode::gallery && ! s.eqEditMode;
    const auto showStrip = isGallery;
    for (auto* button : { &galleryAdd, &galleryRemove, &gallerySlideshow })
        button->setVisible (showStrip);
    if (! showStrip)
        return;

    galleryRemove.setEnabled (currentPicture().getParentDirectory() == GalleryStore::defaultUserDirectory());
    gallerySlideshow.setButtonText (isOn ? "EVERY " + juce::String (seconds) + " S" : juce::String ("SLIDESHOW OFF"));
}

void MainPanel::refreshDevice()
{
    const auto active = controller.activeDevice();
    if (active.uid != deviceUid)
    {
        deviceUid = active.uid;
        isModifiedSincePreset = false;
    }

    const auto name = active.name.isEmpty() ? juce::String ("No output device") : active.name;
    deviceButton.setButtonText (name.toUpperCase());
    deviceButton.setTitle ("Output device: " + name);
}

void MainPanel::refreshPresets (const DspSettings& s)
{
    if (s.presetName != loadedPresetName) // loaded here, from the menu bar or with a new device
    {
        loadedPresetName = s.presetName;
        isModifiedSincePreset = false;
    }

    presets.setSelectedIndex (presetNames.indexOf (s.presetName));

    const auto hasPresets = ! presetNames.isEmpty();
    presetButton.setEnabled (hasPresets);
    previousPreset.setEnabled (presetNames.size() > 1);
    nextPreset.setEnabled (presetNames.size() > 1);

    const auto edited = isModifiedSincePreset ? juce::String (" (edited)") : juce::String();
    const auto label = hasPresets ? presetLegend (s.presetName) + edited.toUpperCase() : juce::String ("NO PRESETS FOUND");
    presetButton.setButtonText (label);
    presetButton.setTitle ("Preset: " + s.presetName + edited);
    presetButton.setDescription (PresetBlurbs::forPreset (s.presetName));
    presetBlurb.setText (hasPresets ? PresetBlurbs::forPreset (s.presetName) : juce::String());
}

void MainPanel::reloadPresetList()
{
    presetNames = controller.presetNames(); // scans the preset folder: only on construction / explicit reload
    presets.setItems (presetNames, presetNames.indexOf (controller.settings().presetName));
}

void MainPanel::markModified()
{
    if (isModifiedSincePreset)
        return;
    isModifiedSincePreset = true;
    refreshPresets (controller.settings());
}

//==============================================================================
void MainPanel::updateMeters()
{
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto seconds = (float) juce::jlimit (0.001, 0.25, now - lastMeterTime);
    lastMeterTime = now;
    if (! isShowing())   // hidden in the menu bar: nothing to draw
        return;

    const auto count = controller.engine().getScopeSamples (scopeLeft.data(), scopeRight.data(), meterWindowSamples);
    const auto measure = [count] (const std::vector<float>& samples, float& peak)
    {
        float sum = 0.0f;
        peak = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const auto magnitude = std::abs (samples[(size_t) i]);
            sum += magnitude;
            peak = juce::jmax (peak, magnitude);
        }
        return count > 0 ? sum / (float) count : 0.0f;
    };

    float peakLeft = 0.0f, peakRight = 0.0f;
    const auto meanLeft = measure (scopeLeft, peakLeft), meanRight = measure (scopeRight, peakRight);

    const auto hold = [seconds] (float& remaining, float peak)
    {
        remaining = peak >= Vu::peakThreshold ? Vu::peakHoldSeconds : juce::jmax (0.0f, remaining - seconds);
        return remaining > 0.0f;
    };

    const auto leftPeakLit = hold (leftPeakHold, peakLeft), rightPeakLit = hold (rightPeakHold, peakRight);
    vuLeft.setReading (leftBallistics.step (meanLeft, seconds), leftPeakLit);
    vuRight.setReading (rightBallistics.step (meanRight, seconds), rightPeakLit);
}

//==============================================================================
void MainPanel::stepPreset (int delta)
{
    const auto count = presetNames.size();
    if (count == 0)
        return;

    const auto current = presets.getSelectedIndex();
    const auto next = current < 0 ? (delta > 0 ? 0 : count - 1) : (current + delta + count) % count;
    isModifiedSincePreset = false;
    controller.loadPreset (presetNames[next]);
    announcePreset (presetNames[next]);
}

void MainPanel::showPresetMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel.get());
    menu.addSectionHeader ("PRESET");

    juce::Component::SafePointer<MainPanel> safe (this);
    const auto current = controller.settings().presetName;
    for (const auto& name : presetNames)
        menu.addItem (name, true, name == current, [safe, name]
        {
            if (safe == nullptr)
                return;
            safe->isModifiedSincePreset = false;
            safe->controller.loadPreset (name);
            safe->announcePreset (name);
        });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton));
}

void MainPanel::showDeviceMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel.get());
    menu.addSectionHeader ("OUTPUT DEVICE");

    auto& ctrl = controller;
    const auto active = controller.activeDevice().uid;
    const auto devices = controller.outputDevices();
    for (const auto& device : devices)
        menu.addItem (device.name, true, device.uid == active, [&ctrl, uid = device.uid] { ctrl.selectDevice (uid); });

    if (devices.isEmpty())
        menu.addItem ("No output devices found", false, false, nullptr);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&deviceButton));
}

void MainPanel::showNextImage()
{
    const auto count = crt.galleryCount(); // live: the gallery folder can change while the app runs
    if (count < 2)
        return;
    // Wrap the saved index the same way the CRT does, so "next" is the picture after the one shown.
    controller.update ([count] (DspSettings& s) { s.galleryIndex = ((s.galleryIndex % count) + count + 1) % count; });
}

//==============================================================================
void MainPanel::announcePreset (const juce::String& presetName)
{
    flashMessage (presetLegend (presetName).toLowerCase().replaceCharacter ('_', ' ') + " - " + PresetBlurbs::forPreset (presetName));
}

void MainPanel::flashMessage (const juce::String& text)
{
    crt.setHoverHint (text);
    juce::Component::SafePointer<MainPanel> safe (this);
    juce::Timer::callAfterDelay (4500, [safe]
    {
        if (safe != nullptr)
            safe->crt.setHoverHint ({});
    });
}

void MainPanel::addPictures()
{
    pictureChooser = std::make_unique<juce::FileChooser> ("Choose pictures to add to the gallery",
                                                          juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                                          "*.jpg;*.jpeg;*.png;*.webp;*.heic;*.gif;*.tif;*.tiff");
    juce::Component::SafePointer<MainPanel> safe (this);
    pictureChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                     | juce::FileBrowserComponent::canSelectMultipleItems,
                                 [safe] (const juce::FileChooser& chooser)
                                 {
                                     if (safe != nullptr && chooser.getResults().size() > 0)
                                         safe->importPictures (chooser.getResults());
                                 });
}

void MainPanel::importPictures (const juce::Array<juce::File>& files)
{
    juce::String error;
    const auto added = controller.importGalleryImages (files, error);
    crt.reloadGallery();

    auto message = added > 0 ? "Added " + juce::String (added) + (added == 1 ? " picture" : " pictures") : juce::String ("No pictures added");
    if (error.isNotEmpty())
        message += " - " + error.upToFirstOccurrenceOf ("\n", false, false);
    flashMessage (message);
}

void MainPanel::filesDropped (const juce::StringArray& paths, int, int)
{
    juce::Array<juce::File> files;
    for (const auto& path : paths)
        files.add (juce::File (path));
    importPictures (files);
}

void MainPanel::removeCurrentPicture()
{
    const auto picture = currentPicture();
    const auto isRemoved = picture != juce::File() && controller.removeGalleryImage (picture);
    crt.reloadGallery();
    flashMessage (isRemoved ? "Picture removed" : "Only pictures you added can be removed");
}

void MainPanel::showSlideshowMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel.get());
    menu.addSectionHeader ("SLIDESHOW");
    const auto isOn = controller.gallerySlideshow();
    const auto current = controller.gallerySlideshowSeconds();
    auto& ctrl = controller;
    menu.addItem ("Off", true, ! isOn, [&ctrl] { ctrl.setGallerySlideshow (false); });
    for (const int seconds : { 5, 10, 20, 30, 60 })
        menu.addItem ("Change picture every " + juce::String (seconds) + " seconds", true, isOn && seconds == current,
                      [&ctrl, seconds] { ctrl.setGallerySlideshowSeconds (seconds); ctrl.setGallerySlideshow (true); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gallerySlideshow));
}

void MainPanel::hideTour()
{
    tour.setVisible (false);
}

void MainPanel::showTour()
{
    tour.setAvoidArea (D::glass.expanded (20.0f));
    tour.start ({
        { "Switch it on",
         #if JUCE_WINDOWS
          "Flip POWER to ON. Everything your PC plays now goes through OscillaFX.",
         #else
          "Flip POWER to ON. If your Mac asks for the microphone, choose Allow (or press ALLOW MICROPHONE).",
         #endif
          juce::Rectangle<float> (powerSwitchWidth, powerSwitchHeight).withCentre (D::powerSwitch).expanded (18.0f, 36.0f) },
        { "Pick a sound",
          "Turn PRESET or press the arrows to choose Movies, Music and more.",
          D::presetBox.reduced (6.0f) },
        { "Choose your output",
          "Pick your speakers or headphones. OscillaFX remembers each one.",
          D::devicePlate } });
}
