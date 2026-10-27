# Installing OscillaFX

- [macOS](#macos): macOS 13 (Ventura) or later, Apple silicon or Intel.
- [Windows](#windows): Windows 10 or 11, 64-bit (x64).

# macOS

Requires macOS 13 (Ventura) or later, Apple silicon or Intel.

## Install

1. Quit eqMac (and FxSound, Boom, SoundSource or any other system audio enhancer). Two
   enhancers that both capture system output fight over the default device. In eqMac use
   its menu > Quit, so it restores your speakers as the output first.
2. Double-click `OscillaFX-<version>.pkg` and follow the installer. It installs:
   - `/Applications/OscillaFX.app`
   - `/Library/Audio/Plug-Ins/HAL/Oscilla.driver` (the "OscillaFX Audio" virtual device)

   Audio drops for about two seconds at the end while Core Audio restarts. No reboot.

### "OscillaFX can't be opened" / "unidentified developer"

The package is not signed with an Apple Developer ID, so Gatekeeper may block it the first time:

- Right-click (or Control-click) the `.pkg` > **Open** > **Open**, or
- Open **System Settings > Privacy & Security**, scroll to the message about
  `OscillaFX-<version>.pkg`, click **Open Anyway**, then authenticate.

On recent macOS versions only the second route works. The same applies to `OscillaFX.app` if
macOS blocks it on first launch.

## First run

Open OscillaFX from Applications. macOS asks for **microphone** access: allow it. OscillaFX reads
the OscillaFX Audio virtual device through the input permission, processes it, and plays it on
your real output. Nothing is recorded or stored.

Until access is granted OscillaFX leaves your sound output alone and the screen shows
"MICROPHONE ACCESS NEEDED". If you denied it, enable OscillaFX in **System Settings > Privacy &
Security > Microphone**; OscillaFX starts by itself within a few seconds. Because the app is
ad-hoc signed, macOS may ask again after each update.

## Per-device suggestions, night mode, volume limiter

The first time an output device is used (speakers, headphones, AirPods, a display...) OscillaFX offers
five starting profiles tuned for that kind of device: Flat, Warm, Bass-heavy, Voice, Wide. Nothing
changes until you pick one; "don't ask again" is remembered per device.

**Night mode** (compressor: loud parts down, quiet parts up, bass kept in check) and the **volume
limiter** (a ceiling from -24 to 0 dBFS; 0 = off) are per-device settings and are off by default.
Presets and starter profiles never switch them off.

Your own pictures for the phosphor gallery live in `~/Library/Application Support/Oscilla/Gallery`
(JPEG, PNG, WebP, HEIC, GIF, TIFF, up to 50 MB each). App-wide choices (slideshow, first-run flag)
are in `~/Library/Application Support/Oscilla/prefs.json`.

## Start at login

Turn on **Start at login** in the menu bar menu. OscillaFX registers itself as a login item
(System Settings > General > Login Items lists it; macOS shows a "Background Items Added"
notice). When started at login it opens minimised to the menu bar; click the menu bar icon or
the Dock icon to show the window. If macOS refuses the registration (older macOS or signature
issues), OscillaFX falls back to `~/Library/LaunchAgents/com.oscilla.app.login.plist`.

## If OscillaFX crashes

While OscillaFX runs, the system output is the "OscillaFX Audio" device. If the app crashes or is
force-quit, a small helper (`OscillaFX.app/Contents/Helpers/OscillaWatchdog`) switches the system
output back to your real device, so the Mac never stays silent. It logs to
`~/Library/Logs/Oscilla/watchdog.log` and exits together with the app.

## Updating

Run the new `.pkg`. The installer quits a running OscillaFX first and replaces
app and driver. Your per-device profiles are kept.

## Uninstall

From the source tree:

```bash
packaging/uninstall.sh          # removes app + driver, keeps profiles
packaging/uninstall.sh --purge  # also deletes profiles and resets the microphone permission
```

It asks for your password, quits OscillaFX, removes its login item, removes the app and the driver and
restarts Core Audio. Profiles live in `~/Library/Application Support/Oscilla`.

## Building the installer

```bash
packaging/build_release.sh      # -> dist/OscillaFX-<version>.pkg (universal arm64 + x86_64)
ARCHS=arm64 packaging/build_release.sh   # native-only build
```

Needs Command Line Tools and CMake 3.22+. Presets come from `presets/` plus, if present,
`presets_fxsound/`; gallery images from `assets/gallery/`. The version comes
from `project(OscillaFX VERSION ...)` in `CMakeLists.txt`. The app icon is the 1024x1024
butterfly master `packaging/icon/OscillaFX.png` (JUCE builds the app icns from it; `driver/build.sh`
turns it into the driver's device icon `OscillaFX.icns` with sips/iconutil).

The driver is upstream BlackHole (GPL-3.0) in `driver/third_party/BlackHole`, left untouched:
`driver/build.sh` copies it into the build directory and applies `driver/patches/*.patch`
(OscillaFX names for device, box, manufacturer and plug-in factory). The upstream `LICENSE`
ships in `Oscilla.driver/Contents/Resources`.

# Windows

Requires Windows 10 or Windows 11, 64-bit (x64). Windows on ARM is not supported yet.

## Install

1. Quit FxSound if it runs (right-click its icon in the taskbar notification area > Exit), and any other
   system-wide audio enhancer. See [FxSound](#fxsound) below.
2. Run `OscillaFX-<version>-Setup-x64.exe` and confirm the administrator (UAC) prompt. It installs:
   - `C:\Program Files\OscillaFX`: `OscillaFX.exe`, `OscillaWatchdog.exe` (when the build has it),
     `Presets\`, `Gallery\`, `LICENSE`, `NOTICE.md`, and `Driver\` (driver files + `fxdevcon.exe`)
   - a Start menu shortcut, optionally a desktop shortcut
   - the virtual audio device **"FxSound Audio Enhancer"** (see below), unless it is already there
   - optionally "Start OscillaFX when I sign in to Windows"
     (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\OscillaFX`)

   No reboot is needed. If setup reports that the driver could not be installed, restart Windows and
   run the setup again.

### "Windows protected your PC" (SmartScreen)

The installer is not code-signed, so Microsoft Defender SmartScreen may stop it the first time:
click **More info**, then **Run anyway**. If the browser blocks the download, keep it from the
browser's download list (Edge: "..." > Keep > Show more > Keep anyway).

### The device is called "FxSound Audio Enhancer"

OscillaFX uses FxSound's virtual audio driver, which is signed by Microsoft for FxSound. Its name
is part of the signed package and cannot be changed, so Windows' sound settings show
**FxSound Audio Enhancer** (manufacturer "FxSound") while OscillaFX runs. That device is OscillaFX's
input; leave it alone and pick your real speakers or headphones inside OscillaFX.

### FxSound

FxSound and OscillaFX share that driver and both take over the default output, so do not run them at
the same time. Setup refuses to continue while FxSound runs and warns when FxSound is installed.
To keep FxSound installed, turn off its start with Windows. Uninstalling FxSound also removes the
shared driver: run the OscillaFX setup again afterwards to put it back. Uninstalling OscillaFX keeps
the driver when FxSound is installed.

## First run

Open OscillaFX from the Start menu. It routes system sound through the FxSound Audio Enhancer
device, processes it and plays it on your real output; nothing is recorded or stored. If the
visualizer stays flat or Windows reports that access is blocked, open **Settings > Privacy &
security > Microphone** and turn on **Microphone access** and **Let desktop apps access your
microphone**.

Settings, per-device profiles and your own gallery pictures live in `%APPDATA%\OscillaFX`.

## Updating

Run the new setup. It quits a running OscillaFX first (the watchdog then puts your real output back)
and keeps your settings.

## Uninstall

**Settings > Apps > Installed apps > OscillaFX > Uninstall** (or Control Panel > Programs). The
uninstaller quits OscillaFX, removes its login entry, removes the FxSound Audio Enhancer device
unless FxSound is installed, and asks whether to also delete your settings in `%APPDATA%\OscillaFX`
(default: keep). Windows then uses your real output device again.

## Building the installer

```powershell
cmake -S . -B build-win -A x64
cmake --build build-win --config Release
packaging\windows\build_installer.ps1     # -> dist\OscillaFX-<version>-Setup-x64.exe
```

Needs PowerShell 7+, Visual Studio 2022 or newer with C++, CMake 3.22+, Python 3 with Pillow (`pip install pillow`)
and Inno Setup 6.3+. The script builds `fxdevcon.exe` from FxSound's source
(`driver/windows/fxdevcon`), checks the vendored driver against `driver/windows/SHA256SUMS`,
turns `packaging/icon/OscillaFX.png` into the setup icon and runs `packaging/windows/OscillaFX.iss`.
The version comes from `project(OscillaFX VERSION ...)` in `CMakeLists.txt`.
`packaging/windows/test_installer.ps1` is the CI smoke test (silent install, upgrade, uninstall); it
changes the machine, so run it only on a throwaway VM.

CI (`.github/workflows/ci.yml`) builds and tests the installer on every push (artifact
`OscillaFX-windows-x64-setup`). `.github/workflows/release.yml` (tags `v*`, manual runs, packaging
changes) also builds the universal macOS `.pkg` (artifact `OscillaFX-macos-pkg`). Neither
publishes a release.
