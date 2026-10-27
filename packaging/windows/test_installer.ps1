# Smoke test of OscillaFX-<version>-Setup-x64.exe on a throwaway Windows machine (CI runner):
# silent install, upgrade over a running OscillaFX, silent uninstall. Changes the machine (installs the
# FxSound driver and an HKCU Run value): do not run it on your own PC.
# A driver that cannot be installed (no audio stack on the runner) is reported, not failed.
# Usage: packaging\windows\test_installer.ps1 -Setup dist\OscillaFX-0.4.0-Setup-x64.exe [-LogDir logs]
param(
    [Parameter(Mandatory)][string]$Setup,
    [string]$LogDir = "installer-logs"
)
$ErrorActionPreference = "Stop"

$InstallDir = "$env:SystemDrive\OscillaFX-test"
$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$DriverHwid = "Root\FXVAD"
$SilentArgs = @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART")
New-Item -ItemType Directory -Force $LogDir | Out-Null
$LogDir = (Resolve-Path $LogDir).Path

function Assert-True([bool]$Condition, [string]$What) {
    if (-not $Condition) { throw "FAILED: $What" }
    Write-Host "ok: $What"
}

function Invoke-Setup([string]$Exe, [string[]]$Arguments) {
    $process = Start-Process $Exe -ArgumentList $Arguments -Wait -PassThru
    Assert-True ($process.ExitCode -eq 0) "$(Split-Path $Exe -Leaf) $($Arguments -join ' ') exits 0 (got $($process.ExitCode))"
}

function Get-DriverDevices {
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\MEDIA" -ErrorAction SilentlyContinue |
        Where-Object { (Get-ItemProperty $_.PSPath -Name HardwareID -ErrorAction SilentlyContinue).HardwareID -contains $DriverHwid }
}

function Get-AppProcesses {
    Get-Process OscillaFX, OscillaWatchdog -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
}

function Get-RunValue { (Get-ItemProperty $RunKey -Name OscillaFX -ErrorAction SilentlyContinue).OscillaFX }

function Assert-Installed {
    foreach ($file in "OscillaFX.exe", "LICENSE", "NOTICE.md", "unins000.exe", "Driver\fxdevcon.exe",
                      "Driver\fxvad.inf", "Driver\fxvad.sys", "Driver\fxvadntamd64.cat") {
        Assert-True (Test-Path "$InstallDir\$file") "installed $file"
    }
    Assert-True ((Get-ChildItem "$InstallDir\Presets" -Filter *.fac).Count -gt 0) "installed Presets\*.fac"
    $expected = Get-Content "$PSScriptRoot\..\..\driver\windows\SHA256SUMS" | Where-Object { $_ -like "* x64/*" } |
        ForEach-Object { ($_ -split '\s+')[0].ToUpper() }
    $actual = Get-ChildItem "$InstallDir\Driver\fxvad*" | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash }
    Assert-True ((Compare-Object $expected $actual).Count -eq 0) "installed driver files are FxSound's signed bytes"
    Write-Host "installed files:"; Get-ChildItem $InstallDir -Recurse -File | ForEach-Object { "  $($_.FullName.Substring($InstallDir.Length + 1))  $($_.Length)" }
}

function Wait-Uninstalled {
    # unins000.exe hands over to a temporary copy and returns at once; wait for that copy to finish.
    for ($i = 0; $i -lt 240 -and (Test-Path "$InstallDir\unins000.exe"); $i++) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 2
}

Write-Host "== install"
Invoke-Setup $Setup ($SilentArgs + "/DIR=`"$InstallDir`"" + "/TASKS=startatlogin" + "/LOG=`"$LogDir\install.log`"")
Assert-Installed
Assert-True ((Get-RunValue) -eq "`"$InstallDir\OscillaFX.exe`" --launched-at-login") "HKCU Run value OscillaFX"
$deviceCount = @(Get-DriverDevices).Count
Write-Host "driver: $deviceCount $DriverHwid device(s) after install"
Get-PnpDevice -FriendlyName "FxSound*" -ErrorAction SilentlyContinue | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-Host

Write-Host "== upgrade over a running OscillaFX"
Start-Process "$InstallDir\OscillaFX.exe"
Start-Sleep -Seconds 5
Write-Host "running before upgrade: $(@(Get-AppProcesses).Count) process(es)"
Invoke-Setup $Setup ($SilentArgs + "/DIR=`"$InstallDir`"" + "/LOG=`"$LogDir\upgrade.log`"")
Assert-True (@(Get-AppProcesses).Count -eq 0) "setup quit OscillaFX"
Assert-Installed
Assert-True (@(Get-DriverDevices).Count -eq $deviceCount) "upgrade adds no second driver device"

Write-Host "== uninstall"
Invoke-Setup "$InstallDir\unins000.exe" ($SilentArgs + "/LOG=`"$LogDir\uninstall.log`"")
Wait-Uninstalled
Assert-True (-not (Test-Path "$InstallDir\OscillaFX.exe")) "OscillaFX.exe removed"
Assert-True (-not (Test-Path "$InstallDir\Driver")) "Driver folder removed"
Assert-True ($null -eq (Get-RunValue)) "HKCU Run value removed"
Assert-True (@(Get-DriverDevices).Count -eq 0) "driver device removed"
Write-Host "driver: installed $deviceCount device(s), uninstall left $(@(Get-DriverDevices).Count)"
