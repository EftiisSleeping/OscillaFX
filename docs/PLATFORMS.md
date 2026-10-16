# Platforms

OscillaFX builds on macOS (shipping) and Windows (in progress, x64). One CMake project, one source
tree: shared code plus small per-platform folders behind fixed interfaces.

## Layout

```
app/Source/
  audio/         shared: AudioEngine.h (public engine interface), DspParams, DriftResampler, Dynamics,
                 SpscRingBuffer, TripleBuffer, VisualTaps
    mac/         AudioEngine_mac.cpp (CoreAudio), CoreAudioHelpers, DeviceTraits_mac.cpp
    win/         AudioEngine_win.cpp (stub), DeviceTraits_win.cpp (stub)
  core/          shared: Controller, Presets, GalleryStore, AppPrefs, AppPaths.h, DeviceKind, StarterProfiles ...
  profiles/      shared: ProfileStore
  platform/      OS services, interfaces only + CrashWatchdog.cpp (shared restore record)
    mac/         CrashGuard_mac.cpp (+ CrashGuardMac.h), CrashWatchdog_mac.cpp, LoginItem_mac.mm,
                 MicrophoneAccess_mac.mm, ImageValidation_mac.mm
    win/         CrashGuard_win.cpp, CrashWatchdog_win.cpp, LoginItem_win.cpp,
                 MicrophoneAccess_win.cpp, ImageValidation_win.cpp
  ui/            shared JUCE UI; WindowChrome.h, DockReopen.h
    mac/         WindowChrome_mac.mm, DockReopen_mac.mm
    win/         WindowChrome_win.cpp, DockReopen_win.cpp (no-ops)
    crt/         shared CRT: CpuCrtRenderer, CrtTraces, Gallery.cpp, GpuCrtView.h
      mac/       MetalCrtView.mm, CrtShaders (MSL), GalleryDecode_mac.mm (ImageIO)
      win/       GalleryDecode_win.cpp (JUCE decoders)
engine/          DfxDsp (ported FxSound DSP), builds on both
  platform/dsp_platform.cpp   portable registry / wide file / string helpers, used on both
  shim/                       Windows SDK stand-ins, macOS only
helper/          OscillaWatchdog, macOS only
driver/          BlackHole-based HAL driver, macOS only
```

Rule: anything under a `mac/` or `win/` folder is built only on that OS (`OSCILLA_OS` in the
top-level `CMakeLists.txt`); everything else is built everywhere. Shared code includes only the
interface headers. `#if JUCE_MAC` / `JUCE_WINDOWS` in shared code is limited to a few lines
(`core/AppPaths.h`, `core/GalleryStore.cpp`, `ui/crt/GpuCrtView.h`, tests).

## Seams

| Interface | Shared signature | macOS | Windows now |
|---|---|---|---|
| `audio/AudioEngine.h` | `bool start(); void stop(); String lastError(); void applySettings(const DspSettings&); Array<OutputDevice> outputDevices(); OutputDevice currentOutputDevice(); void setOutputDevice(const String& uid); std::function<void(const OutputDevice&)> onOutputDeviceChanged; void getSpectrum(float* bands10); int getScopeSamples(float* l, float* r, int max);` | CoreAudio passthrough | stub: `start()` fails, `lastError()` = "Windows audio engine not implemented yet", no devices |
| `core/DeviceKind.h` | `Traits DeviceKinds::traitsFor(const String& uid, const String& name)` | CoreAudio transport + data source | name only |
| `platform/CrashGuard.h` | `void installQuitSignals(std::function<void()>); void installCrashHandlers();` | signals + dispatch; `setRestoreDevice(AudioObjectID)` in `platform/mac/CrashGuardMac.h` | no-op |
| `platform/CrashWatchdog.h` | `File restoreFile(); bool writeRestoreUid(const String&); String readRestoreUid(); void clearRestoreUid();` (shared) `int currentProcessId(); bool launch();` (per OS) | spawns `Contents/Helpers/OscillaWatchdog` | `launch()` returns false |
| `platform/LoginItem.h` | `bool isEnabled(); bool setEnabled(bool); void onLaunchFinished(std::function<void(bool launchedAtLogin)>);` | SMAppService / LaunchAgent | disabled; callback `false` |
| `platform/MicrophoneAccess.h` | `Status status(); void request(std::function<void(bool)>); void openSystemSettings();` | AVFoundation TCC | always granted; opens `ms-settings:privacy-microphone` |
| `platform/ImageValidation.h` | `Result inspect(const File&)` (extension decided from bytes) | ImageIO: jpg png webp heic gif tiff | JUCE: jpg png gif |
| `ui/crt/Gallery.h` | `LuminanceImage Crt::decodeLuminance(const File&, int maxSide)` | ImageIO thumbnailing | JUCE decode + rescale (png jpg gif) |
| `ui/crt/GpuCrtView.h` | `static create()`, `prefersReducedMotion()`, `render(const Crt::Scene&)`, `hasFailed()`, `drawableSize()`, `isVisibleOnScreen()`, `capture()` | `MetalCrtView` | `create()` returns nullptr: CPU renderer |
| `ui/WindowChrome.h` | `void useFullSizeContent(ComponentPeer&); void detentHaptic();` | full-size content view, trackpad haptic | no-op (native title bar) |
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
and it never writes to HKCU. On Windows its file helpers use `_wfopen`/`_wstat` directly and open
files for writing in binary mode, so presets keep LF line ends and stay byte-identical across
platforms (`.gitattributes` keeps `*.fac` unconverted on checkout). Its UTF-8/wide helpers handle
the Basic Multilingual Plane only on Windows (16-bit `wchar_t`).

The whole build uses the static CRT (`/MT`), `/utf-8`, `/MP`, `_USE_MATH_DEFINES`; x64 only.

## Tests

`ctest` runs the same set on both platforms: `dsp_offline`, `preset_tuning`,
`presets_match_make_preset` (`engine/tests/check_presets.cmake`, replaces the `diff` in
`engine/run_tests.sh`), `crt_trace_test`, `engine_selftest`. In `engine_selftest`, CoreAudio
device listing and live device-kind checks are macOS only; TIFF / WebP / HEIC import and the symlink
cases (need admin rights or developer mode on Windows) are skipped on Windows.
`engine_selftest` runs from the repo root (macOS `__FILE__` is repo-relative).
macOS also runs `engine/run_tests.sh` and `helper/test_watchdog.sh`.

CI: `.github/workflows/ci.yml` (macOS arm64 Release; Windows MSVC 2022 x64 Release, uploads the
built `OscillaFX.exe` folder).

## How Windows audio will work

Same design as FxSound, whose Windows passthrough is proven: FxSound's signed virtual audio driver
(`fxvad.sys`, `Installer/Drivers/Version14/win10/{x64,x86,arm64}` in the FxSound source) is made
the default playback endpoint; OscillaFX captures it in WASAPI shared loopback mode, runs DfxDsp
and renders to the user's real endpoint, restoring that endpoint as default on stop (FxSound:
`audiopassthru/src/sndDevices`, `IPolicyConfig::SetDefaultEndpoint`, `IMMNotificationClient` for
device changes). Copies of that code are already in `engine/support/src/{AudioPassthru,sndDevices}`
(not built).

The driver is signed by FxSound; its INF cannot be changed without breaking the signature, so the
endpoint shows as **"FxSound Audio Enhancer"** (hardware id `Root\FXVAD`), not "OscillaFX Audio".
The UI must call it that, and the engine must find it by that name / hardware id.

## Remaining Windows work

1. **Audio backend** (`app/Source/audio/win/AudioEngine_win.cpp`): implement `AudioEngine` as
   above. Keep the audio thread lock-free and allocation-free like the macOS engine: reuse
   `audio/DriftResampler`, `SpscRingBuffer`, `TripleBuffer`, `VisualTaps`, `Dynamics`, `DspParams`.
   Drive the DfxDsp registry from one thread (see `dsp_platform.cpp`). Exclude the FxSound endpoint
   from `outputDevices()`. Write the restore record (`CrashWatchdog::writeRestoreUid`) when routing
   starts. Also `audio/win/DeviceTraits_win.cpp`: map `PKEY_AudioEndpoint_FormFactor` and the
   enumerator (BTHENUM, USB, HDAUDIO/HDMI) onto `Traits` so starter-profile suggestions work.
2. **Crash safety**: `platform/win/CrashGuard_win.cpp` (`SetUnhandledExceptionFilter` +
   `std::set_terminate` restoring the default endpoint; add a `CrashGuardWin.h` with
   `setRestoreEndpoint(const wchar_t* id)` into a preallocated buffer, as `CrashGuardMac.h`), and a
   Windows watchdog (`platform/win/CrashWatchdog_win.cpp::launch()`): a small helper exe that waits
   on the app's process handle and restores the endpoint named in `restoreFile()` if the pid matches.
3. **Platform services**: `platform/win/LoginItem_win.cpp` (HKCU `...\CurrentVersion\Run\OscillaFX`
   = `"<exe>" --login`, `launchedAtLogin` from that argument); `--unregister-login-item` for the
   uninstaller already reaches `LoginItem::setEnabled(false)` in `Main.cpp`.
   `platform/win/MicrophoneAccess_win.cpp` may stay "granted" (loopback capture needs no consent).
4. **Images**: WIC in `platform/win/ImageValidation_win.cpp` and `ui/crt/win/GalleryDecode_win.cpp`
   for WebP / HEIC / TIFF and thumbnail decoding; then re-enable those formats in the self-test.
5. **UI polish**: `ui/win/WindowChrome_win.cpp` (frameless window or dark native title bar via
   `DwmSetWindowAttribute`); tray icon contrast (the glyph is a black macOS template image, invisible
   on a dark taskbar: `ui/TrayIcon.cpp`); copy that says "menu bar" / "System Settings"; optional
   GPU CRT (Direct3D 11 or OpenGL implementing the `GpuCrtView` interface) and
   `prefersReducedMotion()` from `SPI_GETCLIENTAREAANIMATION`.
6. **Installer**: install the signed driver with FxSound's `fxdevcon64.exe install fxvad.inf`
   (source `Installer/DfxInstall/fxdevcon`), install `OscillaFX.exe` + `Presets/` + `Gallery/`,
   uninstall with `fxdevcon64.exe remove` and `OscillaFX.exe --unregister-login-item`. AGPL-3.0
   notices for the FxSound driver and tools go into `NOTICE.md`.
