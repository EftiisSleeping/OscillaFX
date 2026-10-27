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
$FxdevconTimeoutSeconds = 90
$DriverBlocked = $false
New-Item -ItemType Directory -Force $LogDir | Out-Null
$LogDir = (Resolve-Path $LogDir).Path

function Assert-True([bool]$Condition, [string]$What) {
    if (-not $Condition) { throw "FAILED: $What" }
    Write-Host "ok: $What"
}

# A driver install that waits on a prompt nobody can see (no interactive desktop on the runner, or
# Windows Server refusing the client-only attestation signature) is stopped after a while: setup then
# reports the driver failure and goes on, which is what this test checks. setupapi.dev.log shows why.
function Invoke-Setup([string]$Exe, [string[]]$Arguments) {
    $process = Start-Process $Exe -ArgumentList $Arguments -PassThru
    $null = $process.Handle # keeps ExitCode readable after exit
    $deadline = (Get-Date).AddMinutes(10)
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        Get-Process fxdevcon -ErrorAction SilentlyContinue |
            Where-Object { $_.StartTime -lt (Get-Date).AddSeconds(-$FxdevconTimeoutSeconds) } |
            ForEach-Object {
                Write-Warning "fxdevcon still running after $FxdevconTimeoutSeconds s: stopped (driver not installable here)"
                $script:DriverBlocked = $true
                Stop-Process $_ -Force
            }
        Start-Sleep -Seconds 1
    }
    Assert-True $process.HasExited "$(Split-Path $Exe -Leaf) finishes"
    Assert-True ($process.ExitCode -eq 0) "$(Split-Path $Exe -Leaf) $($Arguments -join ' ') exits 0 (got $($process.ExitCode))"
}

function Get-DriverDevices([switch]$WithDriver) {
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\MEDIA" -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ItemProperty $_.PSPath } |
        Where-Object { $_.HardwareID -contains $DriverHwid -and (-not $WithDriver -or $_.Service -eq "FXVAD") }
}

function Save-DriverDiagnostics {
    Get-Content "$env:windir\INF\setupapi.dev.log" -Tail 300 -ErrorAction SilentlyContinue |
        Set-Content "$LogDir\setupapi.dev.log"
    Get-PnpDevice -FriendlyName "FxSound*" -ErrorAction SilentlyContinue |
        Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-Host
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
$deviceCount = @(Get-DriverDevices -WithDriver).Count
Write-Host "driver: $deviceCount working $DriverHwid device(s), $(@(Get-DriverDevices).Count) devnode(s) after install (blocked: $DriverBlocked)"
Save-DriverDiagnostics
Assert-True ($deviceCount -le 1) "at most one driver device"

Write-Host "== upgrade over a running OscillaFX"
Start-Process "$InstallDir\OscillaFX.exe"
Start-Sleep -Seconds 5
Write-Host "running before upgrade: $(@(Get-AppProcesses).Count) process(es)"
Invoke-Setup $Setup ($SilentArgs + "/DIR=`"$InstallDir`"" + "/LOG=`"$LogDir\upgrade.log`"")
Assert-True (@(Get-AppProcesses).Count -eq 0) "setup quit OscillaFX"
Assert-Installed
Assert-True (@(Get-DriverDevices -WithDriver).Count -le 1) "upgrade adds no second driver device"

Write-Host "== uninstall"
Invoke-Setup "$InstallDir\unins000.exe" ($SilentArgs + "/LOG=`"$LogDir\uninstall.log`"")
Wait-Uninstalled
Assert-True (-not (Test-Path "$InstallDir\OscillaFX.exe")) "OscillaFX.exe removed"
Assert-True (-not (Test-Path "$InstallDir\Driver")) "Driver folder removed"
Assert-True ($null -eq (Get-RunValue)) "HKCU Run value removed"
Assert-True (@(Get-DriverDevices).Count -eq 0) "driver device removed"
Write-Host "driver: installed $deviceCount working device(s); blocked: $DriverBlocked; left after uninstall: 0"
