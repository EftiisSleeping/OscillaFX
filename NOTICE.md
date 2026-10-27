# Third-party notices

OscillaFX is distributed under the GNU Affero General Public License v3.0 (see `LICENSE`).
It includes or links the following works.

## FxSound (AGPL-3.0)
- Source: https://github.com/fxsound2/fxsound-app
- Copyright (C) FxSound LLC and contributors.
- Used in: `engine/dsp` and `engine/support` (the DfxDsp audio engine and support code, modified for macOS:
  Windows compatibility shim, 32-bit integer fixes, initialisation fixes, path handling),
  and `presets_fxsound/` (bonus presets; presets marked "(Quizal)" were contributed to FxSound by their author).
- Windows driver: `driver/windows/{x64,arm64,x86}` are FxSound's signed virtual audio driver package
  (`fxvad.inf`, `fxvad.sys`, `fxvadnt*.cat`, from `Installer/Drivers/Version14/win10`), copied unchanged. The
  device keeps FxSound's names ("FxSound Audio Enhancer", hardware id `Root\FXVAD`) because the signature covers the INF.
  The Windows installer ships the x64 set in `Driver\`.
- Windows device installer: `driver/windows/fxdevcon/src` is FxSound's `fxdevcon` tool (`Installer/DfxInstall`),
  unchanged, built from source by OscillaFX's CMake project as `fxdevcon.exe`.

## BlackHole (GPL-3.0)
- Source: https://github.com/ExistentialAudio/BlackHole
- Copyright (C) Existential Audio Inc.
- Used in: `driver/` as a git submodule, built into the "OscillaFX Audio" virtual device with the branding patch in
  `driver/patches/oscilla-branding.patch`. The upstream license text is shipped inside the driver bundle
  (`Oscilla.driver/Contents/Resources/LICENSE`).

## JUCE (AGPL-3.0 / commercial dual license)
- Source: https://github.com/juce-framework/JUCE (git submodule in `third_party/JUCE`)
- Used under the AGPL-3.0 option as the application framework.

## Gallery pictures
`assets/gallery/` contains demo pictures shown on the phosphor display. They are not covered by the AGPL license
and remain the property of their respective creators.
