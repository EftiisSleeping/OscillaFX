# OscillaFX backend API (for the UI)

Everything below lives on `Controller` (`app/Source/core/Controller.h`) unless noted. Message thread only.
Every setter or action that changes state fires the Controller's change message, so the UI re-reads
state in its `changeListenerCallback`. Headers: `core/StarterProfiles.h` (`ProfileSuggestion`, `StarterProfile`),
`core/DspSettings.h`.

## 1. Per-device starter profile suggestions

A device that has no handled profile yet (first time it is the active output) gets a suggestion. It keeps
a copy of the last-used settings and **nothing is applied until the user chooses**.

```cpp
std::optional<ProfileSuggestion> pendingSuggestion() const;
void applySuggestion (const juce::String& starterId);
void dismissSuggestion (bool dontAskAgainForThisDevice);

struct StarterProfile {
    juce::String id;           // "flat" | "warm" | "bass-heavy" | "voice" | "wide"
    juce::String name;         // "Flat" | "Warm" | "Bass-heavy" | "Voice" | "Wide"
    juce::String description;  // one line
    DspSettings settings;      // effects + EQ (preview values)
};
struct ProfileSuggestion {
    juce::String deviceUid, deviceName;
    juce::String deviceKind;       // "builtin-speakers" | "headphones" | "usb-headphones" | "bluetooth-headphones"
                                   // | "airpods" | "display" | "external-speakers" | "unknown"
    juce::String deviceKindLabel;  // "Built-in speakers", "Headphones", "USB headphones / DAC", "Bluetooth headphones",
                                   // "AirPods", "HDMI / display speakers", "External speakers", "Output device"
    juce::Array<StarterProfile> options; // always 5, Flat first
};
```

- `pendingSuggestion()`: set while the active device is unhandled and not dismissed this session. Fires a change message whenever it appears or goes away (device switch, apply, dismiss, manual edit).
- `applySuggestion(id)`: starter effects + EQ + preset name on top of the current settings (power, master gain, balance, night mode, limiter, display state are kept). Saves, marks the device handled. Unknown id or nothing pending: no-op.
- `dismissSuggestion(false)`: closes it, settings unchanged; it can return after an app restart. `dismissSuggestion(true)`: never again for this device.
- Any `update()` or `loadPreset()` on a device with a pending suggestion counts as "the user took control": the suggestion ends and the device is marked handled.
- "Flat" is the device-appropriate neutral voicing (laptop speakers: low-cut protection + presence; headphones: slight bass + air; Bluetooth/AirPods: near neutral with headroom), not a bypass. Show it first / as the default choice.
- Devices with a profile from an older profile (no flag in the file) are treated as handled and are never suggested.
- Device kind comes from CoreAudio transport type, output data source and name (`core/DeviceKind.h`, unit-tested table).

## 2. Gallery (pictures for the phosphor display)

```cpp
juce::Array<juce::File> galleryImages() const;
int  importGalleryImages (const juce::Array<juce::File>&, juce::String& error);
bool removeGalleryImage (const juce::File&);
```

- `galleryImages()`: user folder `~/Library/Application Support/Oscilla/Gallery` plus bundled `Contents/Resources/Gallery`, sorted by file name (natural). Same set `ui/crt/Gallery.cpp` scans. Re-lists on every call.
- `importGalleryImages(files, error)`: copies jpg / png / webp / heic / gif (first frame is shown) / tiff into the user folder. Validated by content (ImageIO), not extension; max 50 MB each; stored under a sanitised unique name (`name.png`, `name-2.png`, ...) with the extension the content really has. **Returns the number imported** (0 = none), not a bool; `error` holds one line per rejected file, empty when all went in. Fires a change message if anything was imported.
- `removeGalleryImage(file)`: deletes only a picture directly inside the user folder (`..` and symlinks resolved). Bundled pictures and anything else: `false`, untouched. Fires a change message on success.

## 3. App preferences (`prefs.json`, not per device)

```cpp
bool gallerySlideshow() const;          void setGallerySlideshow (bool);          // default false
int  gallerySlideshowSeconds() const;   void setGallerySlideshowSeconds (int);    // 3..600, default 20, clamped
bool firstRunPending() const;           void markFirstRunDone();                  // true until marked, survives restarts
```

Stored in `~/Library/Application Support/Oscilla/prefs.json`. Each setter notifies listeners. There is no auto-skip for upgraders: `firstRunPending()` is true for v2 users until the welcome flow calls `markFirstRunDone()`.

## 4. Night mode and volume safety limiter

Two new fields in `DspSettings` (per device profile, edited with `Controller::update`):

```cpp
bool  nightMode = false;          // gentle compressor + safety ceiling
float limiterCeilingDb = 0.0f;    // -24..0 dBFS, >= -0.05 = off (settings().limiterEnabled())
```

- Night mode: compressor after the DSP (threshold -24 dBFS, 3:1, attack 5 ms, release 200 ms, soft knee, auto makeup +4 dB so quiet passages come up and loud ones go down), Dynamic Boost raised to at least 6, Bass capped at 3, EQ boost under 200 Hz capped at +3 dB. A -1 dBFS safety ceiling is always on while night mode is on. The stored effect/EQ values are untouched (turning night mode off restores them). Inactive while `power` is off.
- Limiter: last stage of the chain (after master gain). Peaks never exceed the ceiling. It stays active when `power` is off (safety net).
- Both fade/switch click-free. Presets (`loadPreset`) and starter profiles never reset them.
- Both are off by default and absent in old profile files.

## 5. Misc

- `Controller::update(edit)` now clamps the result into valid ranges (`DspSettings::clampToRanges()`), so UI code cannot push the audio path out of range.
- `Controller(const Locations&)`: folders and files the controller uses, so tests and previews can run on temp locations. The default `Controller()` is unchanged.
- `$OSCILLA_DATA_DIR` (absolute path) replaces `~/Library/Application Support/Oscilla` for profiles, prefs and gallery. Use it for UI preview builds and screenshots so they never touch the user's real data (the crash-watchdog record stays in the real location on purpose).
- `OutputDevice` is unchanged. Use `pendingSuggestion()->deviceKindLabel` for a device description; or `DeviceKinds::classify(DeviceKinds::traitsFor(uid, name))` from `core/DeviceKind.h` for any device.

## Tests

`engine/run_tests.sh` (DSP + preset + starter tuning), `engine_selftest` (everything above, temp folders only), `helper/test_watchdog.sh <OscillaWatchdog>`.
