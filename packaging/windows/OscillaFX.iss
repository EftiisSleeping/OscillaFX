; OscillaFX for Windows: Inno Setup 6.3+ script -> OscillaFX-<version>-Setup-x64.exe
; Built by packaging/windows/build_installer.ps1, which passes:
;   /DAppVersion=<x.y.z>   project() VERSION in CMakeLists.txt
;   /DAppBuildDir=<dir>    folder with OscillaFX.exe (+ OscillaWatchdog.exe), Presets\, Gallery\
;   /DFxdevcon=<exe>       fxdevcon.exe built from driver/windows/fxdevcon
;   /DSetupIcon=<ico>      converted from packaging/icon/OscillaFX.png
; The driver is FxSound's signed virtual audio device (driver/windows/README.md): it shows as
; "FxSound Audio Enhancer" and is shared with FxSound, so it is installed only when missing and
; removed only when FxSound is not installed.

#if Ver < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or newer is required (x64compatible, ExecAndCaptureOutput)
#endif
#ifndef AppVersion
  #error Pass /DAppVersion=<x.y.z>
#endif
#ifndef AppBuildDir
  #error Pass /DAppBuildDir=<folder with OscillaFX.exe>
#endif
#ifndef Fxdevcon
  #error Pass /DFxdevcon=<path to fxdevcon.exe>
#endif
#ifndef SetupIcon
  #error Pass /DSetupIcon=<path to OscillaFX.ico>
#endif
#define RepoRoot AddBackslash(SourcePath) + "..\.."

[Setup]
AppId={{6F0B3C2E-8D4A-4E71-9B5C-2A7E1D9F4C38}
AppName=OscillaFX
AppVersion={#AppVersion}
AppVerName=OscillaFX {#AppVersion}
AppPublisher=OscillaFX
AppPublisherURL=https://github.com/evernoxxx/OscillaFX
AppSupportURL=https://github.com/evernoxxx/OscillaFX/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\OscillaFX
DefaultGroupName=OscillaFX
DisableProgramGroupPage=yes
OutputBaseFilename=OscillaFX-{#AppVersion}-Setup-x64
SetupIconFile={#SetupIcon}
UninstallDisplayIcon={app}\OscillaFX.exe
UninstallDisplayName=OscillaFX
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; PrepareToInstall quits OscillaFX first; Restart Manager is the fallback for anything still holding files.
CloseApplications=yes
CloseApplicationsFilter=OscillaFX.exe,OscillaWatchdog.exe
RestartApplications=no
; The login item lives in HKCU of the user running setup (as the app writes it).
UsedUserAreasWarning=no
SetupLogging=yes

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: startatlogin; Description: "Start OscillaFX when I sign in to Windows"; GroupDescription: "Startup:"; Flags: unchecked

[InstallDelete]
; Bundled presets and pictures are replaced as a whole, so ones dropped from a release disappear.
Type: filesandordirs; Name: "{app}\Presets"
Type: filesandordirs; Name: "{app}\Gallery"

[Files]
Source: "{#AppBuildDir}\*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppBuildDir}\Presets\*"; DestDir: "{app}\Presets"; Flags: ignoreversion recursesubdirs
Source: "{#AppBuildDir}\Gallery\*"; DestDir: "{app}\Gallery"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\driver\windows\x64\*"; DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "{#RepoRoot}\driver\windows\README.md"; DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "{#Fxdevcon}"; DestDir: "{app}\Driver"; DestName: "fxdevcon.exe"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\OscillaFX"; Filename: "{app}\OscillaFX.exe"
Name: "{autodesktop}\OscillaFX"; Filename: "{app}\OscillaFX.exe"; Tasks: desktopicon

[Registry]
; Same value platform/win/LoginItem_win.cpp manages from the app's "Start at login" switch.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "OscillaFX"; \
    ValueData: """{app}\OscillaFX.exe"" --launched-at-login"; Tasks: startatlogin; Flags: uninsdeletevalue

[Run]
Filename: "{app}\OscillaFX.exe"; Description: "{cm:LaunchProgram,OscillaFX}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
const
  APP_EXE = 'OscillaFX.exe';
  WATCHDOG_EXE = 'OscillaWatchdog.exe';
  FXSOUND_EXE = 'FxSound.exe';
  DRIVER_HWID = 'Root\FXVAD';
  DRIVER_NAME = 'FxSound Audio Enhancer';
  ROOT_MEDIA_DEVICES = 'SYSTEM\CurrentControlSet\Enum\ROOT\MEDIA';
  UNINSTALL_KEY = 'Software\Microsoft\Windows\CurrentVersion\Uninstall';
  RUN_KEY = 'Software\Microsoft\Windows\CurrentVersion\Run';
  QUIT_TIMEOUT_MS = 4000;

var
  RemoveUserSettings: Boolean;

function IsProcessRunning(const ExeName: string): Boolean;
var
  Locator, Service, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + ExeName + '''');
    Result := Processes.Count > 0;
  except
    Log('Process check for ' + ExeName + ' failed: ' + GetExceptionMessage);
  end;
end;

function WaitForExit(const ExeName: string; TimeoutMs: Integer): Boolean;
var
  WaitedMs: Integer;
begin
  WaitedMs := 0;
  while IsProcessRunning(ExeName) and (WaitedMs < TimeoutMs) do
  begin
    Sleep(250);
    WaitedMs := WaitedMs + 250;
  end;
  Result := not IsProcessRunning(ExeName);
end;

procedure TaskKill(const ExeName: string; Force: Boolean);
var
  Params: string;
  ResultCode: Integer;
begin
  Params := '/IM ' + ExeName;
  if Force then
    Params := '/F ' + Params;
  Exec(ExpandConstant('{sys}\taskkill.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('taskkill ' + Params + ' -> ' + IntToStr(ResultCode));
end;

{ Ask OscillaFX to close, then force it. The watchdog then restores the real output and exits on its
  own; it is only killed if it lingers. }
procedure QuitOscillaFX;
begin
  if IsProcessRunning(APP_EXE) then
  begin
    TaskKill(APP_EXE, False);
    if not WaitForExit(APP_EXE, QUIT_TIMEOUT_MS) then
    begin
      TaskKill(APP_EXE, True);
      WaitForExit(APP_EXE, QUIT_TIMEOUT_MS);
    end;
  end;
  if not WaitForExit(WATCHDOG_EXE, QUIT_TIMEOUT_MS) then
    TaskKill(WATCHDOG_EXE, True);
end;

function HasFxSoundUninstallEntry(RootKey: Integer): Boolean;
var
  Products: TArrayOfString;
  DisplayName: string;
  I: Integer;
begin
  Result := False;
  if not RegGetSubkeyNames(RootKey, UNINSTALL_KEY, Products) then
    Exit;
  for I := 0 to GetArrayLength(Products) - 1 do
    if RegQueryStringValue(RootKey, UNINSTALL_KEY + '\' + Products[I], 'DisplayName', DisplayName) and
       (CompareText(Copy(DisplayName, 1, 7), 'FxSound') = 0) then
    begin
      Log('FxSound found: ' + DisplayName + ' (' + Products[I] + ')');
      Result := True;
      Exit;
    end;
end;

function IsFxSoundInstalled: Boolean;
begin
  Result := HasFxSoundUninstallEntry(HKLM64) or HasFxSoundUninstallEntry(HKLM32) or
            HasFxSoundUninstallEntry(HKCU);
end;

{ fxdevcon creates root devnodes ROOT\MEDIA\<n>; HardwareID is a multi-string whose first entry is the id. }
function CountDriverDevices: Integer;
var
  Instances: TArrayOfString;
  HardwareIds: string;
  I: Integer;
begin
  Result := 0;
  if not RegGetSubkeyNames(HKLM64, ROOT_MEDIA_DEVICES, Instances) then
    Exit;
  for I := 0 to GetArrayLength(Instances) - 1 do
    if RegQueryMultiStringValue(HKLM64, ROOT_MEDIA_DEVICES + '\' + Instances[I], 'HardwareID', HardwareIds) and
       (Pos(Uppercase(DRIVER_HWID) + #0, Uppercase(HardwareIds) + #0) = 1) then
      Result := Result + 1;
end;

function DriverDir: string;
begin
  Result := ExpandConstant('{app}\Driver');
end;

procedure LogLines(const Lines: TArrayOfString);
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(Lines) - 1 do
    Log('  ' + Lines[I]);
end;

function RunFxdevcon(const Params: string): Boolean;
var
  Output: TExecOutput;
  ResultCode: Integer;
begin
  Log('fxdevcon ' + Params);
  Result := ExecAndCaptureOutput(DriverDir + '\fxdevcon.exe', Params, DriverDir, SW_HIDE,
                                 ewWaitUntilTerminated, ResultCode, Output) and (ResultCode = 0);
  LogLines(Output.StdOut);
  LogLines(Output.StdErr);
  Log('fxdevcon exit code ' + IntToStr(ResultCode));
end;

{ As FxSound: the driver never keeps the PC awake (empty request list removes the override). }
procedure SetPowerRequestOverride(const Requests: string);
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\powercfg.exe'), '/REQUESTSOVERRIDE DRIVER "' + DRIVER_NAME + '" ' + Requests,
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('powercfg request override "' + Requests + '" -> ' + IntToStr(ResultCode));
end;

procedure InstallDriver;
begin
  if CountDriverDevices > 0 then
  begin
    Log('The ' + DRIVER_HWID + ' device already exists (OscillaFX or FxSound): driver left as is');
    Exit;
  end;
  WizardForm.StatusLabel.Caption := 'Installing the audio driver (' + DRIVER_NAME + ')...';
  if RunFxdevcon('install "' + DriverDir + '\fxvad.inf" ' + DRIVER_HWID) then
  begin
    SetPowerRequestOverride('SYSTEM');
    Exit;
  end;
  SuppressibleMsgBox('OscillaFX is installed, but its audio driver ("' + DRIVER_NAME + '") could not be installed, ' +
    'so OscillaFX cannot process sound yet.' + #13#10#13#10 +
    'Restart Windows and run this setup again. If it still fails, see the setup log:' + #13#10 +
    ExpandConstant('{log}'), mbError, MB_OK, IDOK);
end;

{ fxdevcon removes one matching device per call; also clears duplicates left by older installs. }
procedure RemoveDriver;
var
  Attempt: Integer;
begin
  if IsFxSoundInstalled then
  begin
    Log('FxSound is installed and uses the same driver: driver kept');
    Exit;
  end;
  for Attempt := 1 to 8 do
  begin
    if CountDriverDevices = 0 then
      Break;
    if not RunFxdevcon('remove ' + DRIVER_HWID) then
      Break;
  end;
  SetPowerRequestOverride('');
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  while Result and IsProcessRunning(FXSOUND_EXE) do
    Result := SuppressibleMsgBox('FxSound is running. OscillaFX and FxSound use the same audio driver and cannot run at the same time.' +
      #13#10#13#10 + 'Quit FxSound (right-click its icon in the taskbar notification area > Exit), then click Retry.',
      mbError, MB_RETRYCANCEL, IDCANCEL) = IDRETRY;
  if Result and IsFxSoundInstalled then
    SuppressibleMsgBox('FxSound is installed on this PC. OscillaFX uses the same "' + DRIVER_NAME + '" driver.' + #13#10#13#10 +
      'Do not run both: turn off FxSound''s start with Windows, or uninstall FxSound. ' +
      'Uninstalling FxSound removes the shared driver; run this setup again afterwards to put it back.',
      mbInformation, MB_OK, IDOK);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  QuitOscillaFX;
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallDriver;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  case CurUninstallStep of
    usUninstall:
      begin
        RemoveUserSettings := (not UninstallSilent) and
          (MsgBox('Also remove your OscillaFX settings and per-device profiles?' + #13#10 +
                  ExpandConstant('{userappdata}\OscillaFX') + #13#10#13#10 + 'Choose No to keep them for a later install.',
                  mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES);
        QuitOscillaFX;
        RegDeleteValue(HKCU, RUN_KEY, 'OscillaFX');
        RemoveDriver;
      end;
    usPostUninstall:
      if RemoveUserSettings then
        DelTree(ExpandConstant('{userappdata}\OscillaFX'), True, True, True);
  end;
end;
