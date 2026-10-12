; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup script of the Windows installer (CI job "Windows app", docs/WINDOWS.md).
;
;   iscc /DAppVersion=1.1.0 /DSourceDir=<deployed folder> /DOutputDir=<dir> /DIconFile=<Lumaflip.ico> Lumaflip.iss
;
; File types: Lumaflip registers as a program that can open images ("Open with", Default
; apps in Settings: RegisteredApplications + Capabilities, OpenWithProgids) - it never takes
; over a file type itself. The user chooses Lumaflip as default in Settings → Apps →
; Default apps (the app opens that page: Settings → General → "Als Standard-App festlegen").
;
; Updates (WinSparkle, docs/WINDOWS.md): the same AppId upgrades the installation in place,
; in the mode (per user / all users) and folder of the previous installation. WinSparkle starts
; the downloaded installer with the arguments of the appcast (release.yml, tools/appcast_windows.py):
;   /SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1
; and asks Lumaflip to quit; Restart Manager closes a copy that is still running, and
; /RELAUNCH=1 starts the updated app again when the silent installation is done.
;
; Signing: release builds sign Lumaflip.exe, the helper and the DLLs before packaging and
; this installer afterwards (Azure Artifact Signing, release.yml); CI builds are unsigned.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Lumaflip"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

#define AppName "Lumaflip"
#define AppExe "Lumaflip.exe"
#define ProgId "Lumaflip.Image"

; Image types offered in "Open with" (keep in sync with WinShell.cpp imageDocumentTypes()).
#dim Exts[30] {"jpg", "jpeg", "jpe", "jfif", "png", "gif", "bmp", "dib", "tif", "tiff", "webp", "heic", "heif", \
  "avif", "jxl", "svg", "svgz", "ico", "jxr", "wdp", "dds", "psd", "exr", "dng", "cr2", "cr3", "nef", "arw", \
  "raf", "orf"}
; Archives (browsed like folders, F-OPEN-10): only offered in "Open with", never registered
; as a default-app capability (the archive tool stays the default). No PDF: the Windows build
; has no PDF renderer (docs/WINDOWS.md).
#define ContainerProgId "Lumaflip.Container"
#dim ContainerExts[6] {"zip", "cbz", "rar", "cbr", "7z", "cb7"}

[Setup]
AppId={{6E0D5D8A-6A43-4C6B-9C6F-4C1A5B7E1F2D}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Stefan Richter
AppPublisherURL=https://github.com/brokoskokoli/lumaflip
AppSupportURL=https://github.com/brokoskokoli/lumaflip/issues
AppUpdatesURL=https://github.com/brokoskokoli/lumaflip/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Per user by default (no administrator rights); "for all users" on request.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
ChangesAssociations=yes
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=Lumaflip-{#AppVersion}-windows-x64-setup
#ifdef IconFile
SetupIconFile={#IconFile}
#endif
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Upgrades: running copies are closed (Restart Manager) instead of failing on files in use.
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousPrivileges=yes

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; ProgID used by the "Open with" entries.
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "Bild (Lumaflip)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Bild (Lumaflip)"
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
; The application itself ("Open with" list, Explorer's app picker).
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
; Default apps in Settings (RegisteredApplications -> Capabilities).
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Schneller Bildbetrachter, der Ordner in der Reihenfolge des Dateimanagers durchblättert."
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationIcon"; ValueData: """{app}\{#AppExe}"",0"
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#AppName}"; ValueData: "Software\{#AppName}\Capabilities"; Flags: uninsdeletevalue
#sub AddExtension
Root: HKA; Subkey: "Software\Classes\.{#Exts[i]}\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".{#Exts[i]}"; ValueData: ""
Root: HKA; Subkey: "Software\{#AppName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".{#Exts[i]}"; ValueData: "{#ProgId}"
#endsub
#define i
#for {i = 0; i < DimOf(Exts); i++} AddExtension
; ProgID and "Open with" entries for archives (no FileAssociations capability).
Root: HKA; Subkey: "Software\Classes\{#ContainerProgId}"; ValueType: string; ValueName: ""; ValueData: "Archiv (Lumaflip)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#ContainerProgId}"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Archiv (Lumaflip)"
Root: HKA; Subkey: "Software\Classes\{#ContainerProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"
Root: HKA; Subkey: "Software\Classes\{#ContainerProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
#sub AddContainerExtension
Root: HKA; Subkey: "Software\Classes\.{#ContainerExts[i]}\OpenWithProgids"; ValueType: string; ValueName: "{#ContainerProgId}"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".{#ContainerExts[i]}"; ValueData: ""
#endsub
#for {i = 0; i < DimOf(ContainerExts); i++} AddContainerExtension

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; Update started by WinSparkle (/SILENT ... /RELAUNCH=1): start the updated app again.
Filename: "{app}\{#AppExe}"; Flags: nowait runasoriginaluser skipifnotsilent; Check: RelaunchAfterUpdate

[Code]
function RelaunchAfterUpdate: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;
