# Package an already built OscillaFX (cmake --build <BuildDir> --config Release) into
# <OutDir>\OscillaFX-<version>-Setup-x64.exe. Builds fxdevcon.exe, checks the vendored driver hashes,
# converts the icon and runs Inno Setup 6.3+ (ISCC). Installs nothing.
# Needs: Visual Studio (C++), CMake, Python 3 with Pillow, Inno Setup 6.3+.
# Usage: packaging\windows\build_installer.ps1 [-BuildDir build-win] [-OutDir dist]
param(
    [string]$BuildDir = "build-win",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$BuildDir = [IO.Path]::GetFullPath([IO.Path]::Combine($Root, $BuildDir))
$OutDir = [IO.Path]::GetFullPath([IO.Path]::Combine($Root, $OutDir))
$AppBuildDir = "$BuildDir\OscillaFX_artefacts\Release"
$DriverDir = "$Root\driver\windows"

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

function Get-ProjectVersion {
    $match = Select-String -Path "$Root\CMakeLists.txt" -Pattern '^project\(OscillaFX VERSION ([0-9.]+)'
    if (-not $match) { throw "cannot read project VERSION from CMakeLists.txt" }
    $match.Matches[0].Groups[1].Value
}

# The driver catalog signs these exact bytes; a changed line ending would make Windows refuse it.
function Assert-DriverUnchanged {
    foreach ($line in Get-Content "$DriverDir\SHA256SUMS") {
        $expected, $file = $line -split '\s+', 2
        $actual = (Get-FileHash "$DriverDir\$file" -Algorithm SHA256).Hash
        if ($actual -ne $expected.ToUpper()) { throw "driver/windows/$file differs from FxSound's signed file" }
    }
}

function Build-Fxdevcon {
    Invoke-Native cmake @("-S", "$DriverDir\fxdevcon", "-B", "$BuildDir\fxdevcon", "-A", "x64")
    Invoke-Native cmake @("--build", "$BuildDir\fxdevcon", "--config", "Release")
    "$BuildDir\fxdevcon\Release\fxdevcon.exe"
}

function New-SetupIcon {
    $ico = "$BuildDir\OscillaFX.ico"
    Invoke-Native python @("$PSScriptRoot\make_icon.py", "$Root\packaging\icon\OscillaFX.png", $ico)
    $ico
}

function Find-Iscc {
    $onPath = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($dir in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs")) {
        if ($dir -and (Test-Path "$dir\Inno Setup 6\ISCC.exe")) { return "$dir\Inno Setup 6\ISCC.exe" }
    }
    throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup, or choco install innosetup)"
}

if (-not (Test-Path "$AppBuildDir\OscillaFX.exe")) {
    throw "$AppBuildDir\OscillaFX.exe missing: cmake -S . -B build-win -A x64; cmake --build build-win --config Release"
}
$version = Get-ProjectVersion
Assert-DriverUnchanged
$fxdevcon = Build-Fxdevcon
$icon = New-SetupIcon
Invoke-Native (Find-Iscc) @(
    "/DAppVersion=$version", "/DAppBuildDir=$AppBuildDir", "/DFxdevcon=$fxdevcon", "/DSetupIcon=$icon",
    "/O$OutDir", "$PSScriptRoot\OscillaFX.iss")
$setup = Get-Item "$OutDir\OscillaFX-$version-Setup-x64.exe"
Write-Host ("built: {0} ({1:N1} MB)" -f $setup.FullName, ($setup.Length / 1MB))
