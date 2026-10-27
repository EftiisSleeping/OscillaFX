# Platforms

OscillaFX builds on macOS (shipping) and Windows (in progress, x64). One CMake project, one source
tree: shared code plus small per-platform folders behind fixed interfaces.

## Layout

```
app/Source/
  audio/         shared: AudioEngine.h (public engine interface), DspParams, DriftResampler, Dynamics,
                 SpscRingBuffer, TripleBuffer, VisualTaps
    mac/         AudioEngine_mac.cpp (CoreAudio), CoreAudioHelpers, DeviceTraits_mac.cpp
    win/         AudioEngine_win.cpp (WASAPI), MMDeviceHelpers, WasapiFormat, DefaultRoles,
                 EndpointTraits, DeviceTraits_win.cpp
  core/          shared: Controller, Presets, GalleryStore, AppPrefs, AppPaths.h, DeviceKind, StarterProfiles ...
  profiles/      shared: ProfileStore
  platform/      OS services, interfaces only + CrashWatchdog.cpp (shared restore record)
    mac/         CrashGuard_mac.cpp (+ CrashGuardMac.h), CrashWatchdog_mac.cpp, LoginItem_mac.mm,
                 MicrophoneAccess_mac.mm, ImageValidation_mac.mm
    win/         CrashGuard_win.cpp (+ CrashGuardWin.h), CrashWatchdog_win.cpp, LoginItem_win.cpp,
                 MicrophoneAccess_win.cpp, ImageValidation_win.cpp, WicImage_win.cpp,
                 WinEndpoints.h (default endpoint + IPolicyConfig, no JUCE; shared with the helper)
  ui/            shared JUCE UI; WindowChrome.h, DockReopen.h
    mac/         WindowChrome_mac.mm, DockReopen_mac.mm
    win/         WindowChrome_win.cpp (panel window keys), DockReopen_win.cpp (no-op)
    crt/         shared CRT: CpuCrtRenderer, CrtTraces, Gallery.cpp, GpuCrtView.h
      mac/       MetalCrtView.mm, CrtShaders (MSL), GalleryDecode_mac.mm (ImageIO)
      win/       GalleryDecode_win.cpp (WIC, JUCE fallback)
engine/          DfxDsp (ported FxSound DSP), builds on both
  platform/dsp_platform.cpp   portable registry / wide file / string helpers, used on both
  shim/                       Windows SDK stand-ins, macOS only
helper/          OscillaWatchdog (macOS); win/OscillaWatchdog.exe + watchdog_test (Windows)
driver/          BlackHole-based HAL driver, macOS only
  windows/       FxSound's signed virtual audio driver + fxdevcon source, Windows (README.md there)
packaging/       build_release.sh (.pkg, macOS); windows/ Inno Setup installer + build/test scripts
```

Rule: anything under a `mac/` or `win/` folder is built only on that OS (`OSCILLA_OS` in the
top-level `CMakeLists.txt`); everything else is built everywhere. Shared code includes only the
interface headers. `#if JUCE_MAC` / `JUCE_WINDOWS` in shared code is limited to a few lines
(`core/AppPaths.h`, `core/GalleryStore.cpp`, `ui/crt/GpuCrtView.h`, tests).

## Seams

| Interface | Shared signature | macOS | Windows now |
|---|---|---|---|
| `audio/AudioEngine.h` | `bool start(); void stop(); String lastError(); void applySettings(const DspSettings&); Array<OutputDevice> outputDevices(); OutputDevice currentOutputDevice(); void setOutputDevice(const String& uid); std::function<void(const OutputDevice&)> onOutputDeviceChanged; void getSpectrum(float* bands10); int getScopeSamples(float* l, float* r, int max);` | CoreAudio passthrough | WASAPI passthrough from the FxSound endpoint (see "Windows audio") |
| `core/DeviceKind.h` | `Traits DeviceKinds::traitsFor(const String& uid, const String& name)` | CoreAudio transport + data source | endpoint form factor + enumerator (`audio/win/EndpointTraits`) |
| `platform/CrashGuard.h` | `void installQuitSignals(std::function<void()>); void installCrashHandlers();` | signals + dispatch; `setRestoreDevice(AudioObjectID)` in `platform/mac/CrashGuardMac.h` | console Ctrl-C/close, `WM_ENDSESSION` (hidden window), unhandled exception / terminate / abort restore; `setRestoreEndpoint(const wchar_t*)` in `platform/win/CrashGuardWin.h` |
| `platform/CrashWatchdog.h` | `File restoreFile(); bool writeRestoreUid(const String&); String readRestoreUid(); void clearRestoreUid();` (shared) `int currentProcessId(); bool launch();` (per OS) | spawns `Contents/Helpers/OscillaWatchdog` | spawns `OscillaWatchdog.exe` next to the exe, respawns it |
| `platform/LoginItem.h` | `bool isEnabled(); bool setEnabled(bool); void onLaunchFinished(std::function<void(bool launchedAtLogin)>);` | SMAppService / LaunchAgent | HKCU `Run\OscillaFX` = `"<exe>" --launched-at-login` |
| `platform/MicrophoneAccess.h` | `Status status(); void request(std::function<void(bool)>); void openSystemSettings();` | AVFoundation TCC | always granted; opens `ms-settings:privacy-microphone` |
| `platform/ImageValidation.h` | `Result inspect(const File&)` (extension decided from bytes) | ImageIO: jpg png webp heic gif tiff | WIC: jpg png gif tiff (+ webp heic with the Store codecs), JUCE fallback |
| `ui/crt/Gallery.h` | `LuminanceImage Crt::decodeLuminance(const File&, int maxSide)` | ImageIO thumbnailing | WIC scaler + EXIF orientation, JUCE fallback |
| `ui/crt/GpuCrtView.h` | `static create()`, `prefersReducedMotion()`, `render(const Crt::Scene&)`, `hasFailed()`, `drawableSize()`, `isVisibleOnScreen()`, `capture()` | `MetalCrtView` | `create()` returns nullptr: CPU renderer |
| `ui/WindowChrome.h` | `void useFullSizeContent(ComponentPeer&); void detentHaptic();` | full-size content view, trackpad haptic; `drawsOwnTitleBar()` false | borderless window, top strip = caption, `createWindowButtons()` minimise / close keys in `windowButtons` |
| `ui/DockReopen.h` | `void setHandler(std::function<void()>)` | Dock click | no-op (relaunch reaches `anotherInstanceStarted`) |

### Paths (`core/AppPaths.h`)

- `defaultDataDirectory()`: macOS `~/Library/Application Support/Oscilla` (unchanged),
  Windows `%APPDATA%\OscillaFX`. Holds `profiles/`, `prefs.json`, `Gallery/`, `restore_output_uid.txt`.
- `dataDirectory()`: the same, or `$OSCILLA_DATA_DIR` if absolute (the restore record ignores it).
- `resourcesDirectory()`: macOS `OscillaFX.app/Contents/Resources`, Windows the folder of
  `OscillaFX.exe`. The build copies `Presets/` and `Gallery/` there on both.

### Engine on Windows

The DSP compiles natively with MSVC (real `windows.h`, `UNICODE`, `/w`, as FxSound's
`DfxDsp.vcxproj`); `engine/shim` is not used. `engine/platform/dsp_platform.cpp` (the former
`tests/mac_platform.cpp`) is used on Windows too: its per-thread in-memory registry is what keeps
DfxDsp instances on different threads independent (`engine_selftest` "thread_local dsp registry"),
and it never writes to HKCU. On Windows its file helpers use `_wfopen`/`_wstat` directly; presets
are written in text mode as FxSound did (CRLF), and the preset check compares with line ends
normalised (`.gitattributes` keeps the committed `*.fac` unconverted on checkout). Its UTF-8/wide helpers use
surrogate pairs for characters outside the Basic Multilingual Plane on Windows (16-bit `wchar_t`).

The whole build uses the static CRT (`/MT`), `/utf-8`, `/MP`, `_USE_MATH_DEFINES`; x64 only.

## Tests

`ctest` runs the same set on both platforms: `dsp_offline`, `preset_tuning`,
`presets_match_make_preset` (`engine/tests/check_presets.cmake`, the ctest form of the `diff` in
`engine/run_tests.sh`), `crt_trace_test`, `engine_selftest`. In `engine_selftest`, CoreAudio
device listing and live device-kind checks are macOS only; the symlink cases (need admin rights or
developer mode on Windows) are skipped on Windows.
`engine_selftest` runs from the repo root (macOS `__FILE__` is repo-relative).
macOS also runs `engine/run_tests.sh` and `helper/test_watchdog.sh`. Windows CI also runs `engine_selftest --live-audio` after installing
FxSound's driver on the runner (audio services started, `OSCILLA_TEST_VIRTUAL_OUTPUT=1`: the runner
has no real output, so the FxSound endpoint is routed onto itself, a self-test-only hook): two real
start / stop rounds with the restore record checked; skipped if no endpoint appears. Windows adds `watchdog_test`
(dry-run, fake app that kills itself) and, in `engine_selftest`, the HKCU Run round trip and WIC
decoding, plus the audio backend's pure parts (sample conversion, channel maps, endpoint classifier,
default-role policy against a fake); WebP / HEIC import is expected only where the Store codec decodes the fixture, otherwise the
error must name the Store extension. `crt_cpu_bench` times the CPU CRT renderer at 100/150/200 %.

CI: `.github/workflows/ci.yml` (macOS arm64 Release; Windows MSVC x64 Release, uploads the
built `OscillaFX.exe` folder, builds the installer, smoke-tests it with a silent install / upgrade /
uninstall and uploads `OscillaFX-<version>-Setup-x64.exe`). `.github/workflows/release.yml` (tags `v*`,
manual) reuses it and adds the universal macOS `.pkg`.

## Windows audio

Same design as FxSound, whose Windows passthrough is proven (`audiopassthru/src/sndDevices` in the
FxSound source; unbuilt copies in `engine/support/src`). FxSound's signed virtual driver (`fxvad.sys`)
is made the default playback endpoint; OscillaFX captures it in WASAPI shared loopback, runs DfxDsp and
renders to the user's real endpoint.

The driver is signed by FxSound; its INF cannot be changed without breaking the signature, so the
endpoint shows as **"FxSound Audio Enhancer"** (hardware id `Root\FXVAD`), not "OscillaFX Audio".
The UI calls it that; the engine finds it by that name (endpoint or adapter friendly name, also the
old "DFX Audio Enhancer"), as FxSound does.

- **Pipeline** (`audio/win/AudioEngine_win.cpp`): loopback capture thread (event-driven, 10 ms wait
  timeout as fallback) converts to stereo float (`WasapiFormat`), runs DfxDsp + `Dynamics`, feeds the
  visual taps and the `SpscRingBuffer`; the render thread (event-driven shared mode, mix format or the
  closest supported one; float / int16 / int24 / int32, any channel count) pulls through the
  `DriftResampler`. Both threads run under MMCSS "Pro Audio", never allocate, lock or log, and report
  faults through atomics; the message thread polls them and reopens the streams. The render stream is
  stopped after 2 s of silence (the PC can sleep) and restarted when audio arrives.
- **Formats**: the FxSound endpoint is set to stereo at 48 kHz (48 kHz-family outputs) or 44.1 kHz via
  `IPolicyConfig::SetDeviceFormat` (only when it differs, before any stream is opened), as FxSound
  does; stereo goes to the output's front pair (quad: also the back pair), mono gets (L + R) / 2. If the
  endpoint stays multichannel the capture is downmixed (centre and surrounds -3 dB, LFE -6 dB).
- **Default device** (`audio/win/DefaultRoles`): on start every role's default is remembered and
  console + multimedia point at the FxSound endpoint; communications follows only if it was on the
  same endpoint as console. A real endpoint picked in Windows settings while running becomes the
  output and the default goes back to FxSound. Stop / destructor hand the roles back to the output.
  The single `IPolicyConfigVista` definition lives in `platform/win/WinEndpoints.h`.
- **Volume**: on start the real endpoint's level and mute are copied onto the FxSound endpoint (the
  speakers keep their level, as on macOS); from then on the FxSound endpoint's volume (keyboard keys,
  tray) is mirrored to the real endpoint and back (`IAudioEndpointVolumeCallback`, own context GUID).
  The real endpoint's own level is saved and put back when OscillaFX leaves it (never louder than now
  on stop), as FxSound does. The captured signal gets no gain compensation, as in FxSound: WASAPI
  loopback taps the endpoint mix before its endpoint volume, so the level is applied once, on the
  real endpoint (macOS compensates because its HAL driver scales its own stream).
- **Devices**: `IMMNotificationClient` events are marshalled to the message thread and coalesced
  (100 ms); a vanished output falls back to the original default or the first active endpoint, a
  vanished FxSound endpoint stops routing. `outputDevices()` excludes the FxSound endpoint;
  `OutputDevice.uid` is the endpoint id.
- **Crash safety**: when routing starts or the output changes, `CrashWatchdog::writeRestoreUid(id)` and
  `CrashGuard::setRestoreEndpoint(id)`; a clean stop clears both.
- **Errors**: missing endpoint: "Virtual device "FxSound Audio Enhancer" not found. Reinstall
  OscillaFX." (defaults untouched); `E_ACCESSDENIED` at capture: points to Settings > Privacy >
  Microphone; FxSound.exe running: "FxSound is running. Quit FxSound to use OscillaFX — they share the
  same virtual device."; a capture thread that has not polled within 2 s stops routing and restores
  the default.
- **Threads** (`audio/win/StreamThreads.h`): both stream threads wait on their own auto-reset WASAPI
  event and one shared manual-reset stop event, so stop() always joins both.

## Remaining Windows work

1. **Audio backend** (done, see above; needs verification on real hardware with the driver installed).
2. **Crash safety** (done): when routing starts, the engine calls
   `CrashWatchdog::writeRestoreUid(<real endpoint id, IMMDevice::GetId, UTF-8>)` and
   `CrashGuard::setRestoreEndpoint(id)` (`platform/win/CrashGuardWin.h`); on a clean stop
   `clearRestoreUid()` and `setRestoreEndpoint(nullptr)`. The watchdog acts only if the default render
   endpoint's friendly name contains "FxSound Audio Enhancer"; it restores the saved endpoint (or any
   other active one) for all three roles. Log: `%LOCALAPPDATA%\OscillaFX\Logs\watchdog.log`.
3. **Platform services** (done): login item (`--launched-at-login` starts in the tray; a Task Manager
   "Disabled" counts as off). `MicrophoneAccess_win.cpp` stays "granted" (loopback capture needs no consent).
4. **Images** (done): WIC. WebP / HEIC need the Store "Webp Image Extensions" / "HEIF Image Extensions"
   + "HEVC Video Extensions"; without them the import error says so.
5. **UI** (done): borderless window with panel keys, tray glyph in the taskbar's contrast colour
   (`SystemUsesLightTheme`), left click shows the window, right click the menu; Windows wording.
   Still open: optional GPU CRT (Direct3D 11 or OpenGL implementing `GpuCrtView`),
   `prefersReducedMotion()` from `SPI_GETCLIENTAREAANIMATION`.
6. **Installer** (done): `packaging/windows/OscillaFX.iss` (see `docs/INSTALL.md`, Windows). Installs
   every exe of the build folder (`OscillaFX.exe`, `OscillaWatchdog.exe`), `Presets/`, `Gallery/` and the
   driver; stops both exes before upgrade / uninstall. The login item it writes is the same one
   `LoginItem_win.cpp` manages: `HKCU\...\Run\OscillaFX` = `"<app>\OscillaFX.exe" --launched-at-login`.
