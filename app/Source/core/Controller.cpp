#include "Controller.h"
#include "platform/LoginItem.h"
#include "platform/MicrophoneAccess.h"
#include "Presets.h"

namespace
{
constexpr int SAVE_DEBOUNCE_MS = 500;
constexpr int MICROPHONE_POLL_MS = 2000; // picks up a grant made later in System Settings

juce::String microphoneDeniedMessage()
{
   #if JUCE_WINDOWS
    return juce::String (juce::CharPointer_UTF8 ("MICROPHONE ACCESS NEEDED \xe2\x80\x94 SETTINGS "
                                                 "\xe2\x96\xb8 PRIVACY & SECURITY \xe2\x96\xb8 MICROPHONE"));
   #else
    return juce::String (juce::CharPointer_UTF8 ("MICROPHONE ACCESS NEEDED \xe2\x80\x94 SYSTEM SETTINGS "
                                                 "\xe2\x96\xb8 PRIVACY \xe2\x96\xb8 MICROPHONE"));
   #endif
}

const char* const MICROPHONE_PENDING_MESSAGE = "ALLOW MICROPHONE ACCESS IN THE SYSTEM PROMPT";
}

Controller::Controller (const Locations& locations)
    : profiles (locations.profilesDirectory), prefs (locations.prefsFile),
      gallery (locations.galleryDirectory, locations.bundledGalleryDirectory)
{
    device = audio.currentOutputDevice();
    current = profiles.load (device.uid);
    audio.applySettings (current);
    refreshSuggestion (device);
    // Always notify: the device may be unchanged while lastError() changed (failed switch, stop).
    audio.onOutputDeviceChanged = [this] (const OutputDevice&)
    {
        if (! switchProfile (audio.currentOutputDevice()))
            sendChangeMessage();
    };
}

Controller::~Controller()
{
    audio.onOutputDeviceChanged = nullptr;
    saveNow();
    audio.stop();
}

bool Controller::startAudio()
{
    switch (MicrophoneAccess::status())
    {
        case MicrophoneAccess::Status::granted:      return startEngine();
        case MicrophoneAccess::Status::undetermined: requestMicrophoneAccess(); break;
        case MicrophoneAccess::Status::denied:       waitForMicrophoneAccess(); break;
    }
    return false;
}

bool Controller::startEngine()
{
    microphonePoll.stopTimer();
    permissionError.clear();
    const bool ok = audio.start();
    if (! switchProfile (audio.currentOutputDevice()))
        sendChangeMessage();
    return ok;
}

void Controller::requestMicrophoneAccess()
{
    permissionError = MICROPHONE_PENDING_MESSAGE;
    sendChangeMessage();
    juce::WeakReference<Controller> self (this);
    MicrophoneAccess::request ([self] (bool granted)
    {
        if (self == nullptr)
            return;
        if (granted)
            self->startEngine();
        else
            self->waitForMicrophoneAccess();
    });
}

void Controller::waitForMicrophoneAccess()
{
    permissionError = microphoneDeniedMessage();
    microphonePoll.startTimer (MICROPHONE_POLL_MS);
    sendChangeMessage();
}

void Controller::onMicrophonePoll()
{
    if (MicrophoneAccess::status() == MicrophoneAccess::Status::granted)
        startEngine();
}

juce::String Controller::lastError() const
{
    return permissionError.isNotEmpty() ? permissionError : audio.lastError();
}

bool Controller::needsMicrophoneAccess() const
{
    return MicrophoneAccess::status() == MicrophoneAccess::Status::denied;
}

void Controller::openMicrophoneSettings() { MicrophoneAccess::openSystemSettings(); }

void Controller::update (const std::function<void (DspSettings&)>& edit)
{
    edit (current);
    current.clampToRanges(); // the audio path trusts these ranges
    endSuggestionByUser();
    applyAndSave();
}

juce::StringArray Controller::presetNames() const { return Presets::names(); }

void Controller::loadPreset (const juce::String& name)
{
    if (auto loaded = Presets::load (name, current))
    {
        current = *loaded;
        endSuggestionByUser();
        applyAndSave();
    }
}

juce::Array<OutputDevice> Controller::outputDevices() const { return audio.outputDevices(); }
OutputDevice Controller::activeDevice() const               { return device; }

void Controller::selectDevice (const juce::String& uid)
{
    audio.setOutputDevice (uid);
    switchProfile (audio.currentOutputDevice());
}

std::optional<ProfileSuggestion> Controller::pendingSuggestion() const { return suggestions.pending(); }

void Controller::applySuggestion (const juce::String& starterId)
{
    if (auto applied = suggestions.apply (starterId, current))
    {
        current = *applied;
        audio.applySettings (current);
        sendChangeMessage();
    }
}

void Controller::dismissSuggestion (bool dontAskAgainForThisDevice)
{
    if (suggestions.dismiss (dontAskAgainForThisDevice, current))
        sendChangeMessage();
}

juce::Array<juce::File> Controller::galleryImages() const { return gallery.images(); }

int Controller::importGalleryImages (const juce::Array<juce::File>& files, juce::String& error)
{
    const int imported = gallery.importImages (files, error);
    if (imported > 0)
        sendChangeMessage();
    return imported;
}

bool Controller::removeGalleryImage (const juce::File& file)
{
    const bool removed = gallery.removeImage (file);
    if (removed)
        sendChangeMessage();
    return removed;
}

bool Controller::gallerySlideshow() const        { return prefs.gallerySlideshow(); }
int Controller::gallerySlideshowSeconds() const  { return prefs.gallerySlideshowSeconds(); }
bool Controller::firstRunPending() const         { return ! prefs.firstRunDone(); }

void Controller::setGallerySlideshow (bool enabled)
{
    prefs.setGallerySlideshow (enabled);
    sendChangeMessage();
}

void Controller::setGallerySlideshowSeconds (int seconds)
{
    prefs.setGallerySlideshowSeconds (seconds);
    sendChangeMessage();
}

void Controller::markFirstRunDone()
{
    prefs.setFirstRunDone (true);
    sendChangeMessage();
}

bool Controller::launchAtLogin() const { return LoginItem::isEnabled(); }

void Controller::setLaunchAtLogin (bool shouldStart)
{
    LoginItem::setEnabled (shouldStart);
    sendChangeMessage();
}

void Controller::applyAndSave()
{
    audio.applySettings (current);
    saveTimer.startTimer (SAVE_DEBOUNCE_MS);
    sendChangeMessage();
}

void Controller::saveNow()
{
    saveTimer.stopTimer();
    if (device.uid.isNotEmpty())
        profiles.save (device.uid, current);
}

// Idempotent: the engine also reports changes we initiated ourselves. True if the profile changed.
bool Controller::switchProfile (const OutputDevice& newDevice)
{
    if (newDevice.uid.isEmpty() || newDevice.uid == device.uid)
        return false;
    saveNow();
    device = newDevice;
    current = profiles.load (device.uid);
    audio.applySettings (current);
    refreshSuggestion (device);
    sendChangeMessage();
    return true;
}

// Silent: callers send the change message themselves.
void Controller::refreshSuggestion (const OutputDevice& active)
{
    suggestions.deviceActivated (DeviceKinds::traitsFor (active.uid, active.name));
}

void Controller::endSuggestionByUser()
{
    suggestions.userTookControl (current);
}
