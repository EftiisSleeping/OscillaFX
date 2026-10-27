# FxSound virtual audio driver (Windows)

OscillaFX on Windows routes system sound through FxSound's virtual audio driver, the same way FxSound
does. The files here are copied from the FxSound source repository; the signed content is unchanged (see the line-ending note below).

- Origin: https://github.com/fxsound2/fxsound-app (AGPL-3.0), `Installer/Drivers/Version14/win10/{x64,arm64,x86}`
  and `Installer/DfxInstall/{fxdevcon,DfxInstall}`. Copyright (C) FxSound LLC.
- `x64/`, `arm64/`, `x86/`: `fxvad.inf`, `fxvad.sys`, `fxvadnt*.cat`. FxSound's driver package for
  Windows 10 and 11, attestation-signed through Microsoft (the catalog's signer is "Microsoft Windows
  Hardware Compatibility Publisher"). The catalog hashes the exact INF and SYS bytes: changing any
  byte, including line endings, breaks the signature and Windows refuses the driver.
  The INFs were signed with CRLF line endings, but FxSound's git repository stores them with LF, so a
  plain copy from the source tree fails on Windows ("Driver package INF file hash is not present in
  catalog file", 0xE000024B). The INFs here are those files with CRLF restored and nothing else
  changed; their SHA-256 is then exactly the one inside each catalog. The `.sys` and `.cat` files are
  byte-identical copies. `.gitattributes` keeps this folder unconverted; `SHA256SUMS` lists the
  hashes and `packaging/windows/build_installer.ps1` checks them, and each INF against its catalog,
  before packaging.
- Because the INF cannot change, the device keeps FxSound's names: the playback device is
  **"FxSound Audio Enhancer"**, provider/manufacturer "FxSound", service `FXVAD`, hardware id `Root\FXVAD`.
- FxSound's prebuilt `fxdevcon64.exe` / `fxdevcon32.exe` are deliberately left out. `fxdevcon/` builds
  the same tool from FxSound's source (`fxdevcon.cpp` + `DfxInstall.cpp`, unchanged) as `fxdevcon.exe`.
  `DfxInstall.cpp` also contains FxSound's MSI custom actions; fxdevcon only uses `cmdInstall` and
  `cmdRemove`.

## How FxSound installs it, and what OscillaFX's installer does

FxSound's Advanced Installer project (`Installer/fxsound.aip`) calls `DfxInstall.dll`:

1. Install (`InstallDFXDriver`, after files are copied): picks `win10\<arch>` (Windows version string
   `10.*`, which includes Windows 11) or `win7\<arch>`, removes the legacy `*DFX12` driver if present,
   runs `fxdevcon64.exe install "<dir>\fxvad.inf"` (hardware id `Root\FXVAD`; ARM64 passes
   `root\fxvad`), waits one second, re-enables the "FxSound Audio Enhancer" endpoint if it is disabled,
   stores the endpoint GUID under `HKLM\Software\DFX\23\devices`, and runs
   `powercfg -REQUESTSOVERRIDE DRIVER "FxSound Audio Enhancer" SYSTEM` (the driver never blocks sleep).
2. Uninstall (`UninstallFxSoundDriver`, before files are removed): `fxdevcon64.exe remove`
   (`Root\FXVAD`), unconditionally. No reboot is requested (`REBOOT=Suppress`).

`fxdevcon install` always creates a new devnode, so running it twice gives two devices. OscillaFX's
installer (`packaging/windows/OscillaFX.iss`) therefore only runs it when no `Root\FXVAD` device exists
(for example when FxSound already installed it), then applies the same `powercfg` override. Its
uninstaller removes the device only when FxSound is not installed. Uninstalling FxSound removes the
shared device; run the OscillaFX setup again afterwards to put it back.

OscillaFX supports Windows 10/11 x64 only and packages `x64/`. The ARM64 and x86 sets are kept as
shipped by FxSound for a later ARM64 build.
