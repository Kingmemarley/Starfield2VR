; Starfield2VR installer (Inno Setup 7).
;
; Two install types:
;  - "Star Wars Genesis (Mod Organizer 2)": the mod is added to a portable Mod Organizer 2 instance as its own mod,
;    mods\[NoDelete] Starfield2VR\Root\, and turned on in every profile. Root Builder (part of Star Wars Genesis) copies
;    the files in its Root folder next to Starfield.exe while the game runs. The [NoDelete] prefix keeps Wabbajack from
;    deleting the mod when the modlist is updated.
;  - "Steam game folder": dxgi.dll and openxr_loader.dll are copied next to Starfield.exe.
;
; Nothing in the game or Mod Organizer folders is written before the "Preparing to install" step. A file of another mod
; (or another OpenXR loader) that is in the way is only moved aside, into the installer's own folder ({app}\backup), and
; the uninstaller puts it back. What was done is recorded in {app}\install-state.txt, one action per line; the
; uninstaller reads it and only deletes files it recognises as Starfield2VR's.
;
; A failed or cancelled install puts back everything it had changed. The mod's dxgi.dll needs the Microsoft Visual C++
; v14 runtime (x64) 14.51 or newer; if it is missing or older, the installer offers to download Microsoft's installer
; for it, checks that it is signed by Microsoft, and runs it.
;
; Build: see installer\README.md. Unattended use (also for testing):
;   Starfield2VR-Setup.exe /VERYSILENT /NORESTART /INSTALLTYPE=genesis|steam /TARGET="<folder>" [/INSTALLANYWAY]
;     [/MOVEFOREIGN] [/ENABLEALL] [/DISABLEOLD] [/VCREDIST]
;   unins000.exe /VERYSILENT [/REMOVESETTINGS]
; (/NORESTART: without it a /VERYSILENT install restarts Windows by itself if the runtime installer asks for that.)
; Exit codes: 0 installed, 10 installed but the Visual C++ runtime is still missing, 7 nothing installed (see the log).
; (/TYPE is Inno Setup's own parameter for setup types, so the install type is /INSTALLTYPE.)

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#define AppName "Starfield2VR"
#define AppPublisher "Kingmemarley"
#define AppURL "https://github.com/Kingmemarley/Starfield2VR"

; The game version the bundled dxgi.dll is built for. On any other version the mod switches its game features off.
#ifndef ExpectedGameVersion
  #define ExpectedGameVersion "1.16.244"
#endif

#define RepoDir AddBackslash(SourcePath) + "..\"
#ifndef DxgiSource
  #define DxgiSource RepoDir + "build\build-release-no-debug-msvc-1.16.244\dxgi.dll"
#endif
#ifndef OpenXrSource
  #define OpenXrSource RepoDir + "shipping\openxr_loader.dll"
#endif

#if !FileExists(DxgiSource)
  #error dxgi.dll not found: build the 1.16.244 preset first, or pass /DDxgiSource=<path>
#endif
#if !FileExists(OpenXrSource)
  #error openxr_loader.dll not found (shipping\openxr_loader.dll)
#endif

; Hashes of the files this installer carries. The uninstaller only deletes a dxgi.dll / openxr_loader.dll it can
; recognise as Starfield2VR's: by one of these hashes, by a hash recorded at install time, or (dxgi.dll) by its version
; resource's ProductName "Starfield2VR".
#define DxgiHash LowerCase(GetSHA256OfFile(DxgiSource))
#define OpenXrHash LowerCase(GetSHA256OfFile(OpenXrSource))
; SHA-256 hashes of earlier released dxgi.dll builds, separated by spaces: v1.0.0 (Starfield 1.16.244, no version resource).
#ifndef ExtraKnownHashes
  #define ExtraKnownHashes "fbd17617e38d8fa2ab99622dd64dee8879a35de907c84785f46cb57c562354c9"
#endif

#if GetStringFileInfo(DxgiSource, "ProductName") != "Starfield2VR"
  #pragma message "Note: this dxgi.dll has no version resource with ProductName Starfield2VR; it is recognised by its hash only."
#endif

[Setup]
#ifdef TestRoot
; A test build (ISCC /DTestRoot=<folder>): its own AppId, and it refuses every folder outside the test folder.
AppId={{5D3A8E2C-71B4-4C69-9F0E-2B7C1A6D4E93}
OutputDir={#TestRoot}\out
OutputBaseFilename=Starfield2VR-Setup-test
DefaultDirName={#TestRoot}\app
#else
AppId={{8C7B2511-BD18-4F92-A403-289411DC3EAA}
OutputDir=..\build\installer
OutputBaseFilename=Starfield2VR-Setup-{#AppVersion}
DefaultDirName={autopf}\{#AppName}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
; The installer's own folder: the uninstaller, the license texts, the record of what was done and the backups.
; The mod itself goes into the game or Mod Organizer folder chosen on the wizard's pages.
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
LicenseFile=..\LICENSE
; No administrator rights: Mod Organizer 2 folders and Steam libraries can be written by the user (Steam gives its
; folder's users full control). If a folder can't be written, the installer says to run it as administrator.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
SetupArchitecture=x64
WizardStyle=modern dynamic
Compression=lzma2
SolidCompression=yes
; The installer checks for Mod Organizer 2 and the game itself (they must be closed); Windows' Restart Manager isn't used.
CloseApplications=no
SetupLogging=yes
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} installer

[Files]
; The two files the game loads. They are not installed by Inno's own file list: the [Code] section copies them to the
; chosen place (after the checks and backups), so Inno's uninstaller can never delete a file that isn't Starfield2VR's.
Source: "{#DxgiSource}"; DestName: "dxgi.dll"; Flags: dontcopy
Source: "{#OpenXrSource}"; DestName: "openxr_loader.dll"; Flags: dontcopy
; License texts, in the installer's own folder (a file in a Mod Organizer mod outside its Root folder would show up in
; the game's Data folder).
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\sdk-lite\COPYING"; DestDir: "{app}\licenses\sdk-lite"; Flags: ignoreversion
Source: "..\sdk-lite\EXCEPTIONS"; DestDir: "{app}\licenses\sdk-lite"; Flags: ignoreversion
Source: "..\sdk-lite\LICENSE.md"; DestDir: "{app}\licenses\sdk-lite"; Flags: ignoreversion

[Dirs]
; The [Code] section creates this folder before Inno's own files are installed (for the record of what was done), so
; it is listed here to be removed by the uninstaller once it is empty.
Name: "{app}"; Flags: uninsalwaysuninstall

[UninstallDelete]
; Written by [Code], so not in Inno's own list. The uninstaller's code has put the backups back before this runs.
Type: files; Name: "{app}\install-state.txt"
; {app}\backup is deleted by [Code] at usPostUninstall, unless a file in it could not be put back anywhere.

[Code]
const
  ModFolderName = '[NoDelete] Starfield2VR';
  ExpectedGameVersion = '{#ExpectedGameVersion}';
  OurProductName = 'Starfield2VR';
  DxgiHash = '{#DxgiHash}';
  OpenXrHash = '{#OpenXrHash}';
  KnownHashes = '{#DxgiHash} {#OpenXrHash} {#ExtraKnownHashes}';
  StateFileName = 'install-state.txt';
  SteamAppId = '1716740';
  // Star Wars Genesis's own setup (StarWarsGenesisSetup.exe) is an Inno Setup installer with this AppId.
  GenesisSetupKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{44F78ABB-D635-476A-88BF-2E749C0010B6}_is1';

  // The Microsoft Visual C++ v14 runtime (x64): the mod's dxgi.dll imports msvcp140.dll, msvcp140_atomic_wait.dll,
  // concrt140.dll, vcruntime140.dll and vcruntime140_1.dll. It is built with the 14.51 toolset, and a program needs a
  // runtime at least as new as its toolset (std::mutex from a 14.40+ toolset crashes on an older msvcp140.dll, for one).
  // Microsoft's link for the current v14 runtime (the older vs/17 link serves 14.44, too old for this build).
  VcRedistUrl = 'https://aka.ms/vc14/vc_redist.x64.exe';
  VcRedistFile = 'vc_redist.x64.exe';
  VcMinMajor = 14;
  VcMinMinor = 51;

  // What a file found where Starfield2VR's file goes turned out to be.
  fcMissing = 0;   // no file there
  fcSame = 1;      // the very file this installer carries
  fcOurs = 2;      // another Starfield2VR build
  fcOpenXR = 3;    // an OpenXR loader from somewhere else (any loader works; it is moved aside and put back later)
  fcForeign = 4;   // something else: ReShade, an upscaler... (only moved aside if the user agrees)

var
  TypePage: TInputOptionWizardPage;
  FolderPage: TInputDirWizardPage;
  FolderHelp, FolderInfo: TNewStaticText;
  CheckPage: TOutputMsgMemoWizardPage;
  // The choices on the Checks page: a box and its (wrapping) label, shown only when they apply.
  InstallAnywayBox, MoveForeignBox, EnableAllBox, DisableOldBox, VcRedistBox: TNewCheckBox;
  InstallAnywayText, MoveForeignText, EnableAllText, DisableOldText, VcRedistText: TNewStaticText;
  DownloadPage: TDownloadWizardPage;

  // The folder typed or found for each install type (kept when the user switches type).
  GenesisDirValue, SteamDirValue: String;
  ShownFolderType: String;
  // Mod Organizer 2 instances found on this PC (to warn when a Steam-folder install goes into an MO2 game folder).
  KnownMO2: TArrayOfString;

  // Results of the last check of the chosen folder (Analyze).
  AType: String;            // 'genesis' or 'steam'
  ATarget: String;          // the MO2 instance folder or the game folder
  AOk: Boolean;
  AError: String;
  AGameDir, AGameVersion: String;
  AVersionOk: Boolean;
  AModsDir, AProfilesDir, AOverwriteDir: String;
  AProfiles: TArrayOfString;  // every profile's modlist.txt
  ANotes: TArrayOfString;     // warnings and notes for the check page
  // Files to move aside before installing: "path|restore|consent|what it is".
  AMoves: TArrayOfString;
  ANeedsConsent: Boolean;
  AProfilesOff: TArrayOfString;  // profiles (modlist.txt) where the player turned the mod off
  AOldMods: TArrayOfString;      // other mods that are an older Starfield2VR install, turned on in some profile
  AOldModsAll: TArrayOfString;   // every other mod that is a Starfield2VR install, on or off

  // The Visual C++ runtime: what this PC has, whether it must be installed, and what happened.
  VcVersion: String;
  VcNeeded, VcDownloaded, VcRan, VcRunOk, VcRestart: Boolean;

  // Backups only needed to undo this run (copies of files this installer put there before); deleted once it is done.
  RunUndoOnly: TArrayOfString;

{ ---------- small helpers ---------- }

procedure AddStr(var A: TArrayOfString; const S: String);
var
  N: Integer;
begin
  N := GetArrayLength(A);
  SetArrayLength(A, N + 1);
  A[N] := S;
end;

function StartsText(const Prefix, S: String): Boolean;
begin
  Result := CompareText(Copy(S, 1, Length(Prefix)), Prefix) = 0;
end;

function EndsText(const Suffix, S: String): Boolean;
begin
  Result := (Length(S) >= Length(Suffix)) and
    (CompareText(Copy(S, Length(S) - Length(Suffix) + 1, Length(Suffix)), Suffix) = 0);
end;

// Field Index (0-based) of a "a|b|c" line.
function Field(S: String; Index: Integer): String;
var
  P, I: Integer;
begin
  for I := 1 to Index do begin
    P := Pos('|', S);
    if P = 0 then begin
      Result := '';
      Exit;
    end;
    Delete(S, 1, P);
  end;
  P := Pos('|', S);
  if P > 0 then
    Result := Copy(S, 1, P - 1)
  else
    Result := S;
end;

function SamePath(A, B: String): Boolean;
begin
  A := RemoveBackslashUnlessRoot(Trim(A));
  B := RemoveBackslashUnlessRoot(Trim(B));
  Result := (A <> '') and (CompareText(A, B) = 0);
end;

function JoinLines(const A: TArrayOfString): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(A) - 1 do
    Result := Result + A[I] + #13#10;
end;

// A command-line switch: /NAME or /NAME=value (any case). Works in Setup and in the uninstaller.
function HasParam(const Name: String): Boolean;
var
  I: Integer;
  P: String;
begin
  Result := False;
  for I := 1 to ParamCount do begin
    P := ParamStr(I);
    if (CompareText(P, '/' + Name) = 0) or StartsText('/' + Name + '=', P) then begin
      Result := True;
      Exit;
    end;
  end;
end;

function ParamValue(const Name: String): String;
var
  I: Integer;
  P: String;
begin
  Result := '';
  for I := 1 to ParamCount do begin
    P := ParamStr(I);
    if StartsText('/' + Name + '=', P) then begin
      Result := Copy(P, Length(Name) + 3, MaxInt);
      if (Length(Result) >= 2) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
        Result := Copy(Result, 2, Length(Result) - 2);
      Result := RemoveBackslashUnlessRoot(Trim(Result));
      Exit;
    end;
  end;
end;

function LoadLines(const FileName: String; var Lines: TArrayOfString): Boolean;
begin
  SetArrayLength(Lines, 0);
  Result := FileExists(FileName) and LoadStringsFromFile(FileName, Lines);
end;

// One value of an INI file read as UTF-8 lines (Mod Organizer writes its INI files as UTF-8).
function IniValue(const Lines: TArrayOfString; const Section, Key: String): String;
var
  I, P: Integer;
  Cur, L: String;
begin
  Result := '';
  Cur := '';
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    L := Trim(Lines[I]);
    if (Length(L) >= 2) and (L[1] = '[') and (L[Length(L)] = ']') then
      Cur := Copy(L, 2, Length(L) - 2)
    else if CompareText(Cur, Section) = 0 then begin
      P := Pos('=', L);
      if (P > 0) and (CompareText(Trim(Copy(L, 1, P - 1)), Key) = 0) then begin
        Result := Trim(Copy(L, P + 1, MaxInt));
        Exit;
      end;
    end;
  end;
end;

function Unquote(S: String): String;
begin
  S := Trim(S);
  if (Length(S) >= 2) and (S[1] = '"') and (S[Length(S)] = '"') then
    S := Copy(S, 2, Length(S) - 2);
  Result := S;
end;

function HexVal(C: Char): Integer;
begin
  case C of
    '0'..'9': Result := Ord(C) - Ord('0');
    'a'..'f': Result := Ord(C) - Ord('a') + 10;
    'A'..'F': Result := Ord(C) - Ord('A') + 10;
  else
    Result := -1;
  end;
end;

// One Unicode code point as a string (a surrogate pair above U+FFFF).
function CodePointStr(CP: Integer): String;
begin
  if CP > $FFFF then
    Result := Chr($D800 + ((CP - $10000) shr 10)) + Chr($DC00 + ((CP - $10000) and $3FF))
  else
    Result := Chr(CP);
end;

// A value as Qt writes it in an INI file: the whole value may be in quotes (when it has , ; or =), backslash escapes
// (\\ \" \xHH... \0 \n...), and @ByteArray(...) holds bytes (Mod Organizer's paths: UTF-8, every byte >= 0x7F as \xHH).
function QtIniValue(V: String): String;
var
  I, N, Val, K, CP, Need: Integer;
  IsBytes: Boolean;
  Units: array of Integer;  // the unescaped values: bytes (@ByteArray) or UTF-16 code units
begin
  V := Unquote(Trim(V));
  IsBytes := StartsText('@ByteArray(', V) and (Length(V) > 11) and (V[Length(V)] = ')');
  if IsBytes then
    V := Copy(V, 12, Length(V) - 12);
  SetArrayLength(Units, 0);
  I := 1;
  while I <= Length(V) do begin
    Val := Ord(V[I]);
    if (V[I] = '\') and (I < Length(V)) then begin
      I := I + 1;
      case V[I] of
        'x', 'X': begin
            Val := 0;
            while (I < Length(V)) and (HexVal(V[I + 1]) >= 0) do begin
              I := I + 1;
              Val := Val * 16 + HexVal(V[I]);
            end;
          end;
        '0'..'7': begin
            Val := Ord(V[I]) - Ord('0');
            while (I < Length(V)) and (V[I + 1] >= '0') and (V[I + 1] <= '7') do begin
              I := I + 1;
              Val := Val * 8 + Ord(V[I]) - Ord('0');
            end;
          end;
        'a': Val := 7;
        'b': Val := 8;
        'f': Val := 12;
        'n': Val := 10;
        'r': Val := 13;
        't': Val := 9;
        'v': Val := 11;
      else
        Val := Ord(V[I]);  // \\ \" \' \? and anything else: the character itself
      end;
    end;
    N := GetArrayLength(Units);
    SetArrayLength(Units, N + 1);
    Units[N] := Val;
    I := I + 1;
  end;
  Result := '';
  if not IsBytes then begin
    for I := 0 to GetArrayLength(Units) - 1 do
      Result := Result + Chr(Units[I] and $FFFF);
    Exit;
  end;
  // UTF-8 bytes to text (an invalid byte is kept as the same-numbered character).
  I := 0;
  while I < GetArrayLength(Units) do begin
    // A character that was written as itself rather than as \x bytes is kept as it is.
    if Units[I] > $FF then begin
      Result := Result + Chr(Units[I] and $FFFF);
      I := I + 1;
      Continue;
    end;
    Val := Units[I] and $FF;
    Need := 0;
    if Val >= $F0 then begin Need := 3; CP := Val and $07; end
    else if Val >= $E0 then begin Need := 2; CP := Val and $0F; end
    else if Val >= $C0 then begin Need := 1; CP := Val and $1F; end
    else CP := Val;
    if (Need > 0) and (I + Need < GetArrayLength(Units)) then begin
      for K := 1 to Need do
        if (Units[I + K] and $C0) = $80 then
          CP := (CP shl 6) or (Units[I + K] and $3F)
        else
          Need := -1;
    end else if Need > 0 then
      Need := -1;
    if Need < 0 then begin
      Result := Result + Chr(Val);
      I := I + 1;
    end else begin
      Result := Result + CodePointStr(CP);
      I := I + 1 + Need;
    end;
  end;
end;

// A path as Mod Organizer stores it: @ByteArray(D:\\Games\\Starfield), "@ByteArray(D:\\Games, Copy)" or D:/Games/Starfield.
function QtIniPath(V: String): String;
begin
  V := QtIniValue(V);
  StringChangeEx(V, '/', '\', True);
  Result := RemoveBackslashUnlessRoot(V);
end;

{ ---------- version resources and processes ---------- }

function GetFileVersionInfoSizeW(lptstrFilename: String; var lpdwHandle: Cardinal): Cardinal;
  external 'GetFileVersionInfoSizeW@version.dll stdcall';
function GetFileVersionInfoW(lptstrFilename: String; dwHandle: Cardinal; dwLen: Cardinal; var lpData: Byte): Boolean;
  external 'GetFileVersionInfoW@version.dll stdcall';
function VerQueryValueW(var pBlock: Byte; lpSubBlock: String; var lplpBuffer: NativeUInt; var puLen: Cardinal): Boolean;
  external 'VerQueryValueW@version.dll stdcall';
procedure RtlMoveMemoryToString(Dest: String; Source: NativeUInt; Len: NativeUInt);
  external 'RtlMoveMemory@kernel32.dll stdcall';
procedure RtlMoveMemoryToBytes(var Dest: Byte; Source: NativeUInt; Len: NativeUInt);
  external 'RtlMoveMemory@kernel32.dll stdcall';

// A string from a file's version resource (ProductName, FileDescription...); '' if there is none.
function GetVersionString(const FileName, Name: String): String;
var
  Size, Handle, Len: Cardinal;
  Info: array of Byte;
  Lang: array of Byte;
  Ptr: NativeUInt;
  Block: String;
begin
  Result := '';
  if not FileExists(FileName) then
    Exit;
  try
    Handle := 0;
    Size := GetFileVersionInfoSizeW(FileName, Handle);
    if Size = 0 then
      Exit;
    SetArrayLength(Info, Size);
    if not GetFileVersionInfoW(FileName, 0, Size, Info[0]) then
      Exit;
    Block := '040904b0';
    Ptr := 0;
    Len := 0;
    if VerQueryValueW(Info[0], '\VarFileInfo\Translation', Ptr, Len) and (Len >= 4) then begin
      SetArrayLength(Lang, 4);
      RtlMoveMemoryToBytes(Lang[0], Ptr, 4);
      Block := Format('%.2x%.2x%.2x%.2x', [Lang[1], Lang[0], Lang[3], Lang[2]]);
    end;
    Ptr := 0;
    Len := 0;
    if VerQueryValueW(Info[0], '\StringFileInfo\' + Block + '\' + Name, Ptr, Len) and (Len > 1) and (Ptr <> 0) then begin
      Result := StringOfChar(' ', Len - 1);
      RtlMoveMemoryToString(Result, Ptr, (Len - 1) * 2);
      Result := Trim(Result);
    end;
  except
    Log('Reading the version resource of ' + FileName + ' failed: ' + GetExceptionMessage);
    Result := '';
  end;
end;

function DescribeFile(const FileName: String): String;
var
  Product, Desc, Ver: String;
begin
  Product := GetVersionString(FileName, 'ProductName');
  Desc := GetVersionString(FileName, 'FileDescription');
  Ver := GetVersionString(FileName, 'FileVersion');
  Result := '';
  if Product <> '' then
    Result := Product;
  if (Desc <> '') and (CompareText(Desc, Product) <> 0) then begin
    if Result <> '' then Result := Result + ', ';
    Result := Result + Desc;
  end;
  if Ver <> '' then begin
    if Result <> '' then Result := Result + ', ';
    Result := Result + 'version ' + Ver;
  end;
  if Result = '' then
    Result := 'no version information';
end;

function IsProcessRunning(const ExeName: String): Boolean;
var
  Locator, Service, Items: Variant;
  RC, I: Integer;
  Output: TExecOutput;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Items := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + ExeName + '''');
    Result := Items.Count > 0;
  except
    // No WMI: ask tasklist instead.
    Log('WMI process query failed (' + GetExceptionMessage + '), using tasklist.');
    if ExecAndCaptureOutput(ExpandConstant('{sys}\tasklist.exe'), '/FI "IMAGENAME eq ' + ExeName + '" /NH /FO CSV', '',
      SW_SHOWNORMAL, ewWaitUntilTerminated, RC, Output) then
      for I := 0 to GetArrayLength(Output.StdOut) - 1 do
        if Pos('"' + Lowercase(ExeName) + '"', Lowercase(Output.StdOut[I])) > 0 then
          Result := True;
  end;
end;

// The programs that must be closed: Mod Organizer 2 rewrites modlist.txt itself, and a running game holds dxgi.dll.
function RunningPrograms: String;
begin
  Result := '';
  if IsProcessRunning('ModOrganizer.exe') then Result := Result + '  Mod Organizer 2 (ModOrganizer.exe)' + #13#10;
  if IsProcessRunning('Starfield.exe') then Result := Result + '  Starfield (Starfield.exe)' + #13#10;
  if IsProcessRunning('sfse_loader.exe') then Result := Result + '  SFSE (sfse_loader.exe)' + #13#10;
end;

{ ---------- the Microsoft Visual C++ runtime ---------- }

// The version of this PC's 64-bit msvcp140.dll, '' if there is none.
function GetVcVersion: String;
begin
#ifdef TestRoot
  // The test build can pretend another version (/FAKEVCVERSION=14.38.0.0, or =none).
  if HasParam('FAKEVCVERSION') then begin
    Result := ParamValue('FAKEVCVERSION');
    if Result = 'none' then Result := '';
    Exit;
  end;
#endif
  if not GetVersionNumbersString(ExpandConstant('{sys}\msvcp140.dll'), Result) then
    Result := '';
end;

function VersionAtLeast(V: String; MinMajor, MinMinor: Integer): Boolean;
var
  P, Major, Minor: Integer;
begin
  Result := False;
  P := Pos('.', V);
  if P = 0 then
    Exit;
  Major := StrToIntDef(Copy(V, 1, P - 1), -1);
  Delete(V, 1, P);
  P := Pos('.', V);
  if P > 0 then
    V := Copy(V, 1, P - 1);
  Minor := StrToIntDef(V, -1);
  Result := (Major > MinMajor) or ((Major = MinMajor) and (Minor >= MinMinor));
end;

// Every runtime DLL the mod imports is there, and msvcp140.dll is new enough.
function VcRuntimeOk(const V: String): Boolean;
var
  Sys: String;
begin
  Result := VersionAtLeast(V, VcMinMajor, VcMinMinor);
#ifdef TestRoot
  if HasParam('FAKEVCVERSION') then
    Exit;
#endif
  Sys := ExpandConstant('{sys}\');
  Result := Result and FileExists(Sys + 'msvcp140_atomic_wait.dll') and FileExists(Sys + 'concrt140.dll') and
    FileExists(Sys + 'vcruntime140.dll') and FileExists(Sys + 'vcruntime140_1.dll');
end;

function VcUrl: String;
begin
  Result := VcRedistUrl;
#ifdef TestRoot
  if ParamValue('VCURL') <> '' then
    Result := ParamValue('VCURL');
#endif
end;

// The runtime the mod needs is still missing or too old after this install (not counting one waiting for a restart).
function VcStillMissing: Boolean;
begin
  Result := VcNeeded and not VcRestart and not VcRuntimeOk(GetVcVersion);
#ifdef TestRoot
  // The test build never runs Microsoft's installer: a pretended successful run counts as installed.
  if VcRunOk then
    Result := False;
#endif
end;

// True if the file has a valid Authenticode signature (a trusted chain, the file unchanged since it was signed) by
// Microsoft Corporation, as Windows' own Get-AuthenticodeSignature sees it.
function SignedByMicrosoft(const FileName: String): Boolean;
var
  RC: Integer;
  Output: TExecOutput;
  P, Status, Subject: String;
begin
  Result := False;
  Status := '';
  Subject := '';
  P := FileName;
  StringChangeEx(P, '''', '''''', True);
  if ExecAndCaptureOutput(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$s = Get-AuthenticodeSignature -LiteralPath ''' + P +
    '''; $s.Status.ToString(); if ($s.SignerCertificate) { $s.SignerCertificate.Subject }"', '', SW_SHOWNORMAL,
    ewWaitUntilTerminated, RC, Output) then begin
    if GetArrayLength(Output.StdOut) >= 1 then Status := Trim(Output.StdOut[0]);
    if GetArrayLength(Output.StdOut) >= 2 then Subject := Trim(Output.StdOut[1]);
  end else
    Log('Could not start PowerShell to check a signature: ' + SysErrorMessage(RC));
  Result := (Status = 'Valid') and StartsText('CN=Microsoft Corporation, O=Microsoft Corporation,', Subject);
  Log('Signature of ' + FileName + ': "' + Status + '", signer "' + Subject + '": ' + IntToStr(Ord(Result)));
end;

{ ---------- the record of what was installed ---------- }

function StateFile: String;
begin
  // In Setup the wizard's folder value is used: {app} is only set once the (hidden) folder page has been passed.
  if IsUninstaller then
    Result := ExpandConstant('{app}\') + StateFileName
  else
    Result := AddBackslash(WizardDirValue) + StateFileName;
end;

function LoadState(var Lines: TArrayOfString): Boolean;
begin
  Result := LoadLines(StateFile, Lines);
end;

// Adds a line unless the same line is already there.
procedure AddStateLine(var Lines: TArrayOfString; const S: String);
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(Lines) - 1 do
    if CompareText(Lines[I], S) = 0 then
      Exit;
  AddStr(Lines, S);
end;

function IsKnownHash(const Hash: String; const State: TArrayOfString): Boolean;
var
  I: Integer;
begin
  Result := (Hash <> '') and (Pos(' ' + Lowercase(Hash) + ' ', ' ' + Lowercase(KnownHashes) + ' ') > 0);
  if not Result then
    for I := 0 to GetArrayLength(State) - 1 do
      if CompareText(State[I], 'hash|' + Hash) = 0 then begin
        Result := True;
        Exit;
      end;
end;

function FileHash(const FileName: String): String;
begin
  Result := '';
  try
    Result := Lowercase(GetSHA256OfFile(FileName));
  except
    Log('Could not hash ' + FileName + ': ' + GetExceptionMessage);
  end;
end;

// What the file at Path is. IsDxgi: dxgi.dll, else openxr_loader.dll.
function ClassifyFile(const Path: String; IsDxgi: Boolean; const State: TArrayOfString): Integer;
var
  H: String;
begin
  if not FileExists(Path) then begin
    Result := fcMissing;
    Exit;
  end;
  H := FileHash(Path);
  if (IsDxgi and (H = DxgiHash)) or ((not IsDxgi) and (H = OpenXrHash)) then
    Result := fcSame
  else if IsKnownHash(H, State) then
    Result := fcOurs
  else if IsDxgi and (CompareText(GetVersionString(Path, 'ProductName'), OurProductName) = 0) then
    Result := fcOurs
  else if (not IsDxgi) and (CompareText(GetVersionString(Path, 'ProductName'), 'OpenXR Loader') = 0) then
    Result := fcOpenXR
  else
    Result := fcForeign;
end;

{ ---------- Mod Organizer 2 ---------- }

// Mod Organizer's mods/profiles/overwrite folder: the one set in ModOrganizer.ini if it exists, else the default one
// in the instance (an instance moved to another drive can still name its old folders).
function MO2Dir(const Ini: TArrayOfString; const Instance, Key, DefaultName: String): String;
var
  Base, V: String;
begin
  Base := QtIniPath(IniValue(Ini, 'Settings', 'base_directory'));
  if (Base = '') or not DirExists(Base) then
    Base := Instance;
  V := QtIniPath(IniValue(Ini, 'Settings', Key));
  StringChangeEx(V, '%BASE_DIR%', Base, True);
  if (V = '') or not DirExists(V) then
    V := AddBackslash(Base) + DefaultName;
  Result := RemoveBackslashUnlessRoot(V);
end;

// A portable Mod Organizer 2 instance managing Starfield: its game folder, or '' if it isn't one.
function MO2GamePath(const Instance: String): String;
var
  Ini: TArrayOfString;
begin
  Result := '';
  if FileExists(AddBackslash(Instance) + 'ModOrganizer.exe') and
     LoadLines(AddBackslash(Instance) + 'ModOrganizer.ini', Ini) and
     (CompareText(Unquote(IniValue(Ini, 'General', 'gameName')), 'Starfield') = 0) then
    Result := QtIniPath(IniValue(Ini, 'General', 'gamePath'));
end;

procedure AddMO2Candidate(var List: TArrayOfString; Dir: String);
var
  I: Integer;
begin
  Dir := RemoveBackslashUnlessRoot(Trim(Dir));
  if (Dir = '') or (MO2GamePath(Dir) = '') then
    Exit;
  for I := 0 to GetArrayLength(List) - 1 do
    if SamePath(List[I], Dir) then
      Exit;
  AddStr(List, Dir);
end;

// The folder of the first quoted (or space-separated) exe path in a command line.
function CommandDir(Cmd: String): String;
var
  P: Integer;
begin
  Cmd := Trim(Cmd);
  if (Cmd <> '') and (Cmd[1] = '"') then begin
    Delete(Cmd, 1, 1);
    P := Pos('"', Cmd);
    if P > 0 then
      Cmd := Copy(Cmd, 1, P - 1);
  end else begin
    P := Pos(' ', Cmd);
    if P > 0 then
      Cmd := Copy(Cmd, 1, P - 1);
  end;
  Result := ExtractFileDir(Cmd);
end;

// Star Wars Genesis installs into <drive>:\Star Wars Genesis\Game (its guide says not to move it); its setup and
// Mod Organizer's link handler for nexus downloads also name the folder.
procedure FindMO2Instances(var List: TArrayOfString);
var
  C: Integer;
  S: String;
  Ini: TArrayOfString;
  I, N: Integer;
begin
  for C := Ord('C') to Ord('Z') do
    AddMO2Candidate(List, Chr(C) + ':\Star Wars Genesis\Game');
  if RegQueryStringValue(HKCU, GenesisSetupKey, 'InstallLocation', S) then AddMO2Candidate(List, AddBackslash(S) + 'Game');
  if RegQueryStringValue(HKLM64, GenesisSetupKey, 'InstallLocation', S) then AddMO2Candidate(List, AddBackslash(S) + 'Game');
  if RegQueryStringValue(HKLM32, GenesisSetupKey, 'InstallLocation', S) then AddMO2Candidate(List, AddBackslash(S) + 'Game');
  if RegQueryStringValue(HKCU, 'Software\Classes\nxm\shell\open\command', '', S) then begin
    S := CommandDir(S);
    AddMO2Candidate(List, S);
    // nxmhandler.ini lists every instance registered for nexus links: "N\executable=...\ModOrganizer.exe".
    if LoadLines(AddBackslash(S) + 'nxmhandler.ini', Ini) then begin
      N := StrToIntDef(IniValue(Ini, 'handlers', 'size'), 0);
      for I := 1 to N do
        AddMO2Candidate(List, ExtractFileDir(QtIniPath(IniValue(Ini, 'handlers', IntToStr(I) + '\executable'))));
    end;
  end;
end;

{ ---------- Steam ---------- }

function VdfValue(const Line, Key: String): String;
var
  S: String;
  P: Integer;
begin
  Result := '';
  S := Trim(Line);
  if not StartsText('"' + Key + '"', S) then
    Exit;
  Delete(S, 1, Length(Key) + 2);
  P := Pos('"', S);
  if P = 0 then Exit;
  Delete(S, 1, P);
  P := Pos('"', S);
  if P = 0 then Exit;
  Result := Copy(S, 1, P - 1);
  StringChangeEx(Result, '\\', '\', True);
end;

procedure AddSteamCandidate(var List: TArrayOfString; Dir: String);
var
  I: Integer;
begin
  Dir := RemoveBackslashUnlessRoot(Trim(Dir));
  StringChangeEx(Dir, '/', '\', True);
  if (Dir = '') or not FileExists(AddBackslash(Dir) + 'Starfield.exe') then
    Exit;
  for I := 0 to GetArrayLength(List) - 1 do
    if SamePath(List[I], Dir) then
      Exit;
  AddStr(List, Dir);
end;

// Each Steam library in libraryfolders.vdf; Starfield is where its appmanifest's "installdir" says.
procedure AddSteamLibraries(var List: TArrayOfString; SteamRoot: String);
var
  Vdf, Acf: TArrayOfString;
  I, J: Integer;
  Lib, InstallDir: String;
begin
  StringChangeEx(SteamRoot, '/', '\', True);
  SteamRoot := RemoveBackslashUnlessRoot(Trim(SteamRoot));
  if SteamRoot = '' then
    Exit;
  AddSteamCandidate(List, AddBackslash(SteamRoot) + 'steamapps\common\Starfield');
  if not LoadLines(AddBackslash(SteamRoot) + 'steamapps\libraryfolders.vdf', Vdf) then
    if not LoadLines(AddBackslash(SteamRoot) + 'config\libraryfolders.vdf', Vdf) then
      Exit;
  for I := 0 to GetArrayLength(Vdf) - 1 do begin
    Lib := VdfValue(Vdf[I], 'path');
    if Lib = '' then
      Continue;
    InstallDir := 'Starfield';
    if LoadLines(AddBackslash(Lib) + 'steamapps\appmanifest_' + SteamAppId + '.acf', Acf) then
      for J := 0 to GetArrayLength(Acf) - 1 do
        if VdfValue(Acf[J], 'installdir') <> '' then
          InstallDir := VdfValue(Acf[J], 'installdir');
    AddSteamCandidate(List, AddBackslash(Lib) + 'steamapps\common\' + InstallDir);
  end;
end;

procedure FindSteamGame(var List: TArrayOfString);
var
  S: String;
  Key: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Steam App ' + SteamAppId;
  if RegQueryStringValue(HKLM64, Key, 'InstallLocation', S) then AddSteamCandidate(List, S);
  if RegQueryStringValue(HKLM32, Key, 'InstallLocation', S) then AddSteamCandidate(List, S);
  if RegQueryStringValue(HKCU, Key, 'InstallLocation', S) then AddSteamCandidate(List, S);
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) then AddSteamLibraries(List, S);
  if RegQueryStringValue(HKLM32, 'Software\Valve\Steam', 'InstallPath', S) then AddSteamLibraries(List, S);
  if RegQueryStringValue(HKLM64, 'Software\Valve\Steam', 'InstallPath', S) then AddSteamLibraries(List, S);
end;

{ ---------- Mod Organizer's modlist.txt ---------- }

{ modlist.txt is edited as bytes: the mod's line is plain ASCII, so every other line (whatever its encoding), the line
  breaks and a BOM stay exactly as they were. }
type
  TRawLines = array of AnsiString;

// Each line with its own line break (the last line may have none).
procedure SplitRaw(const Raw: AnsiString; var Lines: TRawLines);
var
  I, Start, N: Integer;
begin
  SetArrayLength(Lines, 0);
  Start := 1;
  for I := 1 to Length(Raw) do
    if Ord(Raw[I]) = 10 then begin
      N := GetArrayLength(Lines);
      SetArrayLength(Lines, N + 1);
      Lines[N] := Copy(Raw, Start, I - Start + 1);
      Start := I + 1;
    end;
  if Start <= Length(Raw) then begin
    N := GetArrayLength(Lines);
    SetArrayLength(Lines, N + 1);
    Lines[N] := Copy(Raw, Start, MaxInt);
  end;
end;

// A line without its line break.
function RawBody(const L: AnsiString): AnsiString;
begin
  Result := L;
  while (Length(Result) > 0) and ((Ord(Result[Length(Result)]) = 10) or (Ord(Result[Length(Result)]) = 13)) do
    SetLength(Result, Length(Result) - 1);
end;

function IsOurModLine(const L: AnsiString): Boolean;
var
  B: AnsiString;
begin
  B := RawBody(L);
  Result := (Length(B) > 1) and ((Ord(B[1]) = Ord('+')) or (Ord(B[1]) = Ord('-'))) and
    (CompareText(String(Copy(B, 2, MaxInt)), ModFolderName) = 0);
end;

// Loads modlist.txt as bytes, splits off a UTF-8 BOM, and returns the line break the file uses.
function LoadModList(const ModList: String; var Bom: AnsiString; var Lines: TRawLines; var Eol: AnsiString): Boolean;
var
  Raw: AnsiString;
begin
  Bom := '';
  Eol := #13#10;
  Result := FileExists(ModList) and LoadStringFromFile(ModList, Raw);
  if not Result then
    Exit;
  if (Length(Raw) >= 3) and (Ord(Raw[1]) = $EF) and (Ord(Raw[2]) = $BB) and (Ord(Raw[3]) = $BF) then begin
    Bom := Copy(Raw, 1, 3);
    Delete(Raw, 1, 3);
  end;
  if (Pos(#10, Raw) > 0) and (Pos(#13#10, Raw) = 0) then
    Eol := #10;
  SplitRaw(Raw, Lines);
end;

// UTF-8 bytes as text (a byte that isn't valid UTF-8 is kept as the same-numbered character).
function DecodeUtf8(const B: AnsiString): String;
var
  I, K, Need, CP, Val: Integer;
begin
  Result := '';
  I := 1;
  while I <= Length(B) do begin
    Val := Ord(B[I]);
    Need := 0;
    if Val >= $F0 then begin Need := 3; CP := Val and $07; end
    else if Val >= $E0 then begin Need := 2; CP := Val and $0F; end
    else if Val >= $C0 then begin Need := 1; CP := Val and $1F; end
    else CP := Val;
    if (Need > 0) and (I + Need <= Length(B)) then begin
      for K := 1 to Need do
        if (Ord(B[I + K]) and $C0) = $80 then
          CP := (CP shl 6) or (Ord(B[I + K]) and $3F)
        else
          Need := -1;
    end else if Need > 0 then
      Need := -1;
    if Need < 0 then begin
      Result := Result + Chr(Val);
      I := I + 1;
    end else begin
      Result := Result + CodePointStr(CP);
      I := I + 1 + Need;
    end;
  end;
end;

// The mod a modlist.txt line is about, and its mark: '+' on, '-' off; '' for a comment, a separator-less blank...
function ModLineName(const L: AnsiString; var Mark: String): String;
var
  B: AnsiString;
begin
  Result := '';
  Mark := '';
  B := RawBody(L);
  if (Length(B) > 1) and ((Ord(B[1]) = Ord('+')) or (Ord(B[1]) = Ord('-'))) then begin
    if Ord(B[1]) = Ord('+') then Mark := '+' else Mark := '-';
    Result := DecodeUtf8(Copy(B, 2, MaxInt));
  end;
end;

// Whether Starfield2VR's mod is on ('+'), off ('-') or not listed ('') in a modlist.txt.
function ModStateIn(const ModList: String): String;
var
  Lines: TRawLines;
  Bom, Eol: AnsiString;
  I: Integer;
  Mark: String;
begin
  Result := '';
  if not LoadModList(ModList, Bom, Lines, Eol) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if CompareText(ModLineName(Lines[I], Mark), ModFolderName) = 0 then begin
      Result := Mark;
      if Mark = '+' then
        Exit;
    end;
end;

// Whether the player has Starfield2VR turned off in this profile: its mod's line is "-", or it isn't listed but an
// earlier Starfield2VR install is, and only as off.
function ProfileOff(const ModList: String): Boolean;
var
  Lines: TRawLines;
  Bom, Eol: AnsiString;
  I, J: Integer;
  Cur, Mark, Name: String;
  OldOn, OldOff: Boolean;
begin
  Cur := ModStateIn(ModList);
  if Cur <> '' then begin
    Result := Cur = '-';
    Exit;
  end;
  Result := False;
  if not LoadModList(ModList, Bom, Lines, Eol) then
    Exit;
  OldOn := False;
  OldOff := False;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Name := ModLineName(Lines[I], Mark);
    if Mark <> '' then
      for J := 0 to GetArrayLength(AOldModsAll) - 1 do
        if CompareText(Name, AOldModsAll[J]) = 0 then begin
          if Mark = '+' then OldOn := True else OldOff := True;
        end;
  end;
  Result := OldOff and not OldOn;
end;
// "Default, Alt" from a list of profiles\<name>\modlist.txt paths.
function ProfileNames(const ModLists: TArrayOfString): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(ModLists) - 1 do begin
    if Result <> '' then Result := Result + ', ';
    Result := Result + ExtractFileName(ExtractFileDir(ModLists[I]));
  end;
end;

{ ---------- checking the chosen folder ---------- }

function IsGamePassFolder(const Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'MicrosoftGame.config') or FileExists(AddBackslash(Dir) + 'appxmanifest.xml') or
    (Pos('\windowsapps\', Lowercase(AddBackslash(Dir))) > 0);
end;

function GameVersionOf(const Dir: String): String;
begin
  if not GetVersionNumbersString(AddBackslash(Dir) + 'Starfield.exe', Result) then
    Result := '';
end;

function VersionMatches(const V: String): Boolean;
begin
  Result := (V = ExpectedGameVersion) or StartsText(ExpectedGameVersion + '.', V);
end;

function CanWriteTo(const Dir: String): Boolean;
var
  T: String;
begin
  T := AddBackslash(Dir) + 'starfield2vr-write-test.tmp';
  Result := SaveStringToFile(T, 'test', False);
  if Result then
    DeleteFile(T);
end;

procedure Note(const S: String);
begin
  AddStr(ANotes, S);
end;

procedure PlanMove(const Path: String; Restore, Consent: Boolean; const What: String);
var
  R, C: String;
begin
  if Restore then R := 'restore' else R := 'drop';
  if Consent then begin
    C := 'consent';
    ANeedsConsent := True;
  end else
    C := 'auto';
  AddStr(AMoves, Path + '|' + R + '|' + C + '|' + What);
end;

// The Starfield version a Starfield2VR dxgi.dll is made for, from its version resource ("... for Starfield 1.16.236").
function BuildTargetOf(const Path: String): String;
var
  D: String;
  P: Integer;
begin
  Result := '';
  D := GetVersionString(Path, 'FileDescription');
  P := Pos('for Starfield ', D);
  if P > 0 then
    Result := Trim(Copy(D, P + Length('for Starfield '), MaxInt));
end;

// A strong note when a Starfield2VR build already there is made for this game's version and the bundled one isn't.
procedure NoteMatchingBuild(const Path: String);
var
  V: String;
begin
  if AVersionOk or not FileExists(Path) then
    Exit;
  V := BuildTargetOf(Path);
  if (V <> '') and ((AGameVersion = V) or StartsText(V + '.', AGameVersion)) then
    Note('YOUR CURRENT STARFIELD2VR BUILD MATCHES YOUR GAME: ' + Path + ' is made for Starfield ' + V + ', your ' +
      'game''s version. This installer''s build is made for Starfield ' + ExpectedGameVersion + ', so on your game its ' +
      'features stay off. Click Cancel to keep using your current build. (If you install anyway, your current file ' +
      'is kept and put back when you uninstall Starfield2VR.)');
end;
// Looks at one place Starfield2VR's file goes and plans what to do with a file already there.
// InOverwrite: it is Mod Organizer's overwrite\Root, which beats every mod, so even a file identical to ours is moved.
function RecordedFile(const Path: String; const State: TArrayOfString): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to GetArrayLength(State) - 1 do
    if (Field(State[I], 0) = 'file') and SamePath(Field(State[I], 1), Path) then begin
      Result := True;
      Exit;
    end;
end;

procedure CheckPlace(const Path: String; IsDxgi, InOverwrite: Boolean; const State: TArrayOfString);
var
  K, KD: Integer;
  Name: String;
begin
  Name := ExtractFileName(Path);
  K := ClassifyFile(Path, IsDxgi, State);
  // The stock OpenXR loader is the same file in many VR mods: one this installer didn't put there belongs to whatever
  // dxgi.dll is next to it, unless that dxgi.dll is Starfield2VR's.
  if (not IsDxgi) and ((K = fcSame) or (K = fcOurs)) and not RecordedFile(Path, State) then begin
    KD := ClassifyFile(AddBackslash(ExtractFileDir(Path)) + 'dxgi.dll', True, State);
    if (KD <> fcSame) and (KD <> fcOurs) then
      K := fcOpenXR;
  end;
  case K of
    fcSame:
      if InOverwrite then begin
        PlanMove(Path, False, False, 'a copy of the same Starfield2VR ' + Name);
        Note('Mod Organizer''s overwrite\Root holds a copy of the same Starfield2VR ' + Name + '. Files in overwrite ' +
          'are used instead of every mod''s, so it is moved out (into the installer''s backup folder) and the mod''s ' +
          'own file is used.');
      end;
    fcOurs:
      // Another Starfield2VR build (perhaps for another game version): never lost, put back on uninstall.
      if InOverwrite then begin
        PlanMove(Path, True, False, 'another Starfield2VR build: ' + DescribeFile(Path));
        Note('Mod Organizer''s overwrite\Root holds another Starfield2VR ' + Name + ' (' + DescribeFile(Path) + '). ' +
          'Files in overwrite are used instead of every mod''s, so it is moved aside (into the installer''s backup ' +
          'folder) and put back when Starfield2VR is uninstalled.');
      end else if not RecordedFile(Path, State) then
        Note('There is another Starfield2VR ' + Name + ' at ' + Path + ' (' + DescribeFile(Path) + '). It is moved ' +
          'aside and put back when Starfield2VR is uninstalled.');
    fcOpenXR: begin
        PlanMove(Path, True, False, DescribeFile(Path));
        Note('There is another OpenXR loader at ' + Path + ' (' + DescribeFile(Path) +
          '). It is moved aside and put back when Starfield2VR is uninstalled.');
      end;
    fcForeign: begin
        PlanMove(Path, True, True, DescribeFile(Path));
        Note('ANOTHER MOD''S FILE: ' + Path + ' (' + DescribeFile(Path) + '). Starfield2VR needs this file name. ' +
          'Tick the box below to move it aside (it is put back when Starfield2VR is uninstalled); ' +
          'the other mod will not work while Starfield2VR is installed.');
      end;
  end;
end;

// Mod names turned on ("+Name") in any profile, lower case.
function EnabledModsText: String;
var
  I, J: Integer;
  Lines: TArrayOfString;
begin
  Result := #10;
  for I := 0 to GetArrayLength(AProfiles) - 1 do
    if LoadLines(AProfiles[I], Lines) then
      for J := 0 to GetArrayLength(Lines) - 1 do
        if (Length(Lines[J]) > 1) and (Lines[J][1] = '+') then
          Result := Result + Lowercase(Copy(Lines[J], 2, MaxInt)) + #10;
end;

procedure AnalyzeGenesis(const Instance: String; const State: TArrayOfString; CheckWrite: Boolean);
var
  Ini: TArrayOfString;
  FR: TFindRec;
  Excl, Enabled, ModDir, CopyFiles: String;
  P, K: Integer;
begin
  if not FileExists(AddBackslash(Instance) + 'ModOrganizer.exe') then begin
    AError := 'There is no ModOrganizer.exe in this folder. Choose the folder that holds ModOrganizer.exe ' +
      '(for Star Wars Genesis: <drive>:\Star Wars Genesis\Game).';
    Exit;
  end;
  if not LoadLines(AddBackslash(Instance) + 'ModOrganizer.ini', Ini) then begin
    AError := 'There is no ModOrganizer.ini next to ModOrganizer.exe, so this is not a portable Mod Organizer 2 ' +
      'instance (Star Wars Genesis installs one). This installer only supports portable instances.';
    Exit;
  end;
  if CompareText(Unquote(IniValue(Ini, 'General', 'gameName')), 'Starfield') <> 0 then begin
    AError := 'This Mod Organizer 2 manages ' + Unquote(IniValue(Ini, 'General', 'gameName')) + ', not Starfield.';
    Exit;
  end;
  AGameDir := QtIniPath(IniValue(Ini, 'General', 'gamePath'));
  if (AGameDir = '') or not FileExists(AddBackslash(AGameDir) + 'Starfield.exe') then begin
    AError := 'Mod Organizer 2''s game folder (' + AGameDir + ') has no Starfield.exe. Start Mod Organizer 2 once ' +
      'and check its game folder, then run this installer again.';
    Exit;
  end;
  if IsGamePassFolder(AGameDir) then begin
    AError := 'Mod Organizer 2''s game folder is the Game Pass / Microsoft Store version of Starfield, which ' +
      'Starfield2VR does not support (only the Steam version).';
    Exit;
  end;
  if not DirExists(AddBackslash(Instance) + 'plugins\rootbuilder') then begin
    AError := 'Root Builder is not installed in this Mod Organizer 2 (no plugins\rootbuilder folder). Star Wars ' +
      'Genesis includes it; without it, dxgi.dll can''t reach the game folder. Use the "Steam game folder" install ' +
      'type instead, or install Root Builder.';
    Exit;
  end;
  // Root Builder copies a mod's Root files into the game folder unless they are excluded.
  Excl := ',' + Lowercase(Unquote(IniValue(Ini, 'Plugins', 'RootBuilder\exclusions'))) + ',';
  StringChangeEx(Excl, ' ', '', True);
  if (Pos(',dxgi.dll,', Excl) > 0) or (Pos(',openxr_loader.dll,', Excl) > 0) or (Pos(',*.dll,', Excl) > 0) then begin
    AError := 'Root Builder''s Exclusions list contains dxgi.dll or openxr_loader.dll, so Root Builder would not copy ' +
      'Starfield2VR''s files. Remove them from Root Builder''s Exclusions, then run this installer again.';
    Exit;
  end;
  CopyFiles := Unquote(IniValue(Ini, 'Plugins', 'RootBuilder\copyfiles'));
  if (CopyFiles <> '') and (Pos('**', CopyFiles) = 0) and (Pos('*.dll', Lowercase(CopyFiles)) = 0) and
     (Pos('dxgi.dll', Lowercase(CopyFiles)) = 0) then
    Note('Root Builder''s Copy list is "' + CopyFiles + '". Star Wars Genesis copies every file (**); if dxgi.dll is ' +
      'linked or virtualised instead, the game may not load it.');

  AModsDir := MO2Dir(Ini, Instance, 'mod_directory', 'mods');
  AProfilesDir := MO2Dir(Ini, Instance, 'profiles_directory', 'profiles');
  AOverwriteDir := MO2Dir(Ini, Instance, 'overwrite_directory', 'overwrite');
  if not DirExists(AModsDir) then begin
    AError := 'Mod Organizer 2''s mods folder was not found (' + AModsDir + ').';
    Exit;
  end;
  SetArrayLength(AProfiles, 0);
  if FindFirst(AddBackslash(AProfilesDir) + '*', FR) then begin
    try
      repeat
        if (FR.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (FR.Name <> '.') and (FR.Name <> '..') and
           FileExists(AddBackslash(AProfilesDir) + FR.Name + '\modlist.txt') then
          AddStr(AProfiles, AddBackslash(AProfilesDir) + FR.Name + '\modlist.txt');
      until not FindNext(FR);
    finally
      FindClose(FR);
    end;
  end;
  if GetArrayLength(AProfiles) = 0 then begin
    AError := 'No Mod Organizer 2 profile (profiles\<name>\modlist.txt) was found in ' + AProfilesDir + '.';
    Exit;
  end;

  AGameVersion := GameVersionOf(AGameDir);
  AVersionOk := VersionMatches(AGameVersion);
#ifdef TestRoot
  if not StartsText('{#TestRoot}\', AddBackslash(ExpandFileName(AModsDir))) or
     not StartsText('{#TestRoot}\', AddBackslash(ExpandFileName(AProfilesDir))) or
     not StartsText('{#TestRoot}\', AddBackslash(ExpandFileName(AOverwriteDir))) then begin
    AError := 'Test build: Mod Organizer''s folders are outside the test folder.';
    Exit;
  end;
#endif
  if CheckWrite and not CanWriteTo(AModsDir) then begin
    AError := 'Windows does not let this account write to ' + AModsDir + '. Start the installer again with ' +
      'right-click > Run as administrator.';
    Exit;
  end;
  // Every file that will be changed must be writable before anything is changed (appending nothing changes nothing).
  if CheckWrite then
    for P := 0 to GetArrayLength(AProfiles) - 1 do
      if not SaveStringToFile(AProfiles[P], '', True) then begin
        AError := 'Windows does not let this account change ' + AProfiles[P] + ' (is it read-only?). Nothing was ' +
          'changed.';
        Exit;
      end;

  // Mod Organizer's overwrite\Root is copied in after every mod, so a dxgi.dll there wins over the mod's.
  CheckPlace(AddBackslash(AOverwriteDir) + 'Root\dxgi.dll', True, True, State);
  CheckPlace(AddBackslash(AOverwriteDir) + 'Root\openxr_loader.dll', False, True, State);
  NoteMatchingBuild(AddBackslash(AOverwriteDir) + 'Root\dxgi.dll');
  NoteMatchingBuild(AddBackslash(AGameDir) + 'dxgi.dll');

  // Other turned-on mods with their own dxgi.dll: Starfield2VR is put above them, so its file is the one used. One
  // whose dxgi.dll is Starfield2VR's is an earlier install of this mod (by hand, as a mod with a Root folder).
  SetArrayLength(AOldMods, 0);
  SetArrayLength(AOldModsAll, 0);
  Enabled := EnabledModsText;
  if FindFirst(AddBackslash(AModsDir) + '*', FR) then begin
    try
      repeat
        if (FR.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (FR.Name <> '.') and (FR.Name <> '..') and
           (CompareText(FR.Name, ModFolderName) <> 0) then begin
          ModDir := AddBackslash(AModsDir) + FR.Name;
          K := ClassifyFile(ModDir + '\Root\dxgi.dll', True, State);
          if (K = fcSame) or (K = fcOurs) then
            AddStr(AOldModsAll, FR.Name);
          if (FileExists(ModDir + '\Root\dxgi.dll') or FileExists(ModDir + '\Root\openxr_loader.dll')) and
             (Pos(#10 + Lowercase(FR.Name) + #10, Enabled) > 0) then begin
            if (K = fcSame) or (K = fcOurs) then begin
              AddStr(AOldMods, FR.Name);
              Note('The mod "' + FR.Name + '" is another install of Starfield2VR (its Root\dxgi.dll is Starfield2VR''s: ' +
                DescribeFile(ModDir + '\Root\dxgi.dll') + '). With the box below ticked it is turned off in every ' +
                'profile where this one is on (and turned back on if you uninstall Starfield2VR); its files are not ' +
                'touched.');
              NoteMatchingBuild(ModDir + '\Root\dxgi.dll');
            end else
              Note('The mod "' + FR.Name + '" also has a dxgi.dll or openxr_loader.dll in its Root folder. ' +
                'Starfield2VR is put above it in the mod list, so the game gets Starfield2VR''s file and that mod''s file ' +
                'does not load.');
          end;
        end;
      until not FindNext(FR);
    finally
      FindClose(FR);
    end;
  end;

  // Profiles where the player turned the mod off are left off unless they tick "turn on in every profile".
  SetArrayLength(AProfilesOff, 0);
  for P := 0 to GetArrayLength(AProfiles) - 1 do
    if ProfileOff(AProfiles[P]) then
      AddStr(AProfilesOff, AProfiles[P]);
  if GetArrayLength(AProfilesOff) > 0 then
    Note('Starfield2VR is turned off in these profiles: ' + ProfileNames(AProfilesOff) + '. They are left as they ' +
      'are, unless you tick "Turn Starfield2VR on in every profile" below. (A Wabbajack update of Star Wars Genesis ' +
      'also turns it off.)');
  // A dxgi.dll in the real game folder. When the game starts through Mod Organizer 2, Root Builder backs it up (in
  // its own data folder) and copies the mod's file over it. When the game closes it deletes what it copied and, from
  // its backup, puts back the files on its list of the game's own files (made the first time it saw this game folder
  // and version); other files it finds new in the game folder are moved into overwrite\Root.
  P := ClassifyFile(AddBackslash(AGameDir) + 'dxgi.dll', True, State);
  if P = fcForeign then
    Note('The game folder itself has another dxgi.dll (' + DescribeFile(AddBackslash(AGameDir) + 'dxgi.dll') +
      '). When you play through Mod Organizer 2, Root Builder copies Starfield2VR''s file over it for the session ' +
      'and keeps its own backup of it. Starfield2VR''s file is the one the game loads.')
  else if (P = fcSame) or (P = fcOurs) then
    Note('Starfield2VR''s dxgi.dll is also in the game folder itself (from a "Steam game folder" install). It does no ' +
      'harm: when you play through Mod Organizer 2, Root Builder copies the mod''s file over it, and when the game ' +
      'closes it may delete that copy or move it into overwrite\Root. Uninstalling Starfield2VR removes it (and ' +
      'running this installer again moves a copy in overwrite\Root out of the way).');

  if DirExists(AddBackslash(AOverwriteDir) + 'Root\sfvr') then
    Note('Your Starfield2VR settings and logs (overwrite\Root\sfvr) are kept.');
  Note('If you update Star Wars Genesis with Wabbajack, the mod folder "' + ModFolderName + '" is kept but turned off ' +
    '(and Wabbajack clears the overwrite folder, which holds the mod''s settings). Afterwards, turn the mod on again in ' +
    'Mod Organizer 2, or run this installer again and tick "Turn Starfield2VR on in every profile".');
  AOk := True;
end;

procedure AnalyzeSteam(const Dir: String; const State: TArrayOfString; CheckWrite: Boolean);
var
  I: Integer;
begin
  if not FileExists(AddBackslash(Dir) + 'Starfield.exe') then begin
    AError := 'There is no Starfield.exe in this folder. Choose the folder that holds Starfield.exe ' +
      '(usually ...\steamapps\common\Starfield in your Steam library).';
    Exit;
  end;
  if IsGamePassFolder(Dir) then begin
    AError := 'This is the Game Pass / Microsoft Store version of Starfield, which Starfield2VR does not support ' +
      '(only the Steam version).';
    Exit;
  end;
  AGameDir := Dir;
  AGameVersion := GameVersionOf(Dir);
  AVersionOk := VersionMatches(AGameVersion);
  if CheckWrite and not CanWriteTo(Dir) then begin
    AError := 'Windows does not let this account write to ' + Dir + '. Start the installer again with ' +
      'right-click > Run as administrator.';
    Exit;
  end;
  if not FileExists(AddBackslash(Dir) + 'steam_api64.dll') then
    Note('There is no steam_api64.dll here, so this may not be the Steam version of Starfield. Starfield2VR is made ' +
      'for the Steam version.');
  CheckPlace(AddBackslash(Dir) + 'dxgi.dll', True, False, State);
  CheckPlace(AddBackslash(Dir) + 'openxr_loader.dll', False, False, State);
  NoteMatchingBuild(AddBackslash(Dir) + 'dxgi.dll');
  for I := 0 to GetArrayLength(KnownMO2) - 1 do
    if SamePath(MO2GamePath(KnownMO2[I]), Dir) then
      Note('This folder is also the game folder of the Mod Organizer 2 in ' + KnownMO2[I] + '. If you also play ' +
        'through Mod Organizer 2, Root Builder may move dxgi.dll and openxr_loader.dll into its overwrite\Root ' +
        'folder when the game closes (it keeps in place only the files it listed as the game''s own the first time it ' +
        'saw this folder). To play through Mod Organizer 2, choose the "Star Wars Genesis (Mod Organizer 2)" install ' +
        'type instead.');
  if DirExists(AddBackslash(Dir) + 'sfvr') then
    Note('Your Starfield2VR settings and logs (the sfvr folder) are kept.');
  AOk := True;
end;

// Checks the chosen folder and plans the install. Nothing is written, except (CheckWrite, only just before
// installing) a test file created and deleted again to see that the folder can be written.
function Analyze(const InstallType, Target: String; CheckWrite: Boolean): Boolean;
var
  State: TArrayOfString;
begin
  AType := InstallType;
  ATarget := Trim(Target);
  if ATarget <> '' then
    ATarget := RemoveBackslashUnlessRoot(ExpandFileName(ATarget));
  AOk := False;
  AError := '';
  AGameDir := '';
  AGameVersion := '';
  AVersionOk := False;
  AModsDir := '';
  AProfilesDir := '';
  AOverwriteDir := '';
  ANeedsConsent := False;
  SetArrayLength(AProfiles, 0);
  SetArrayLength(ANotes, 0);
  SetArrayLength(AMoves, 0);
  SetArrayLength(AProfilesOff, 0);
  SetArrayLength(AOldMods, 0);
  SetArrayLength(AOldModsAll, 0);
  LoadState(State);
  if ATarget = '' then
    AError := 'Choose a folder.'
  else if not DirExists(ATarget) then
    AError := 'The folder ' + ATarget + ' does not exist.'
#ifdef TestRoot
  else if not StartsText('{#TestRoot}\', AddBackslash(ATarget)) then
    AError := 'Test build: only folders inside {#TestRoot} are accepted.'
#endif
  else if InstallType = 'genesis' then
    AnalyzeGenesis(ATarget, State, CheckWrite)
  else
    AnalyzeSteam(ATarget, State, CheckWrite);
  Result := AOk;
  Log('Check of ' + ATarget + ' (' + InstallType + '): ok=' + IntToStr(Ord(AOk)) + ' version=' + AGameVersion +
    ' error=' + AError);
  Log('Notes:'#13#10 + JoinLines(ANotes));
  Log('Planned moves:'#13#10 + JoinLines(AMoves));
end;

function SelectedType: String;
begin
  if TypePage.SelectedValueIndex = 1 then
    Result := 'steam'
  else
    Result := 'genesis';
end;

{ ---------- installing ---------- }

// Copies a file and checks the copy by its hash.
function CopyChecked(const Src, Dest: String): Boolean;
begin
  Result := CopyFile(Src, Dest, False) and (FileHash(Dest) = FileHash(Src));
  if not Result then
    Log('Copy failed: ' + Src + ' -> ' + Dest);
end;

// Moves a file into the installer's backup folder (a copy and a delete: the backup folder may be on another drive).
function MoveToBackup(const Path: String; var Backup: String): Boolean;
var
  Dir: String;
  N: Integer;
begin
  N := 1;
  repeat
    Dir := ExpandConstant('{app}\backup\') + GetDateTimeString('yyyymmdd-hhnnss', #0, #0) + '-' + IntToStr(N);
    N := N + 1;
  until not DirExists(Dir);
  Backup := Dir + '\' + ExtractFileName(Path);
  Result := ForceDirectories(Dir) and CopyChecked(Path, Backup) and DeleteFile(Path);
  if Result then
    Log('Moved ' + Path + ' to ' + Backup);
end;

function DoMoves(var State: TArrayOfString; var Journal: TArrayOfString): String;
var
  I: Integer;
  Path, Backup: String;
begin
  Result := '';
  for I := 0 to GetArrayLength(AMoves) - 1 do begin
    Path := Field(AMoves[I], 0);
    if not FileExists(Path) then
      Continue;
    if not MoveToBackup(Path, Backup) then begin
      Result := 'Could not move ' + Path + ' aside.';
      Exit;
    end;
    AddStr(Journal, 'moved|' + Backup + '|' + Path);
    if Field(AMoves[I], 1) = 'restore' then
      AddStateLine(State, 'restore|' + Backup + '|' + Path)
    else
      AddStr(RunUndoOnly, Backup);
  end;
end;

// True if the record says a file moved aside is to be put back at Path on uninstall.
function HasRestoreFor(const Path: String; const State: TArrayOfString): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to GetArrayLength(State) - 1 do
    if (Field(State[I], 0) = 'restore') and SamePath(Field(State[I], 2), Path) then begin
      Result := True;
      Exit;
    end;
end;

function InstallFile(const Name, DestDir: String; var State: TArrayOfString; var Journal: TArrayOfString): String;
var
  Src, Dest, Backup, H: String;
  KeepOnUninstall: Boolean;
begin
  Result := '';
  Src := ExpandConstant('{tmp}\') + Name;
  Dest := AddBackslash(DestDir) + Name;
  // The same OpenXR loader already next to a Starfield2VR dxgi.dll that will be put back on uninstall belongs to that
  // one: it stays when this install is uninstalled.
  KeepOnUninstall := (CompareText(Name, 'openxr_loader.dll') = 0) and FileExists(Dest) and
    (FileHash(Dest) = FileHash(Src)) and not RecordedFile(Dest, State) and
    HasRestoreFor(AddBackslash(DestDir) + 'dxgi.dll', State);
  // A different file already there is moved aside first, so an undo puts it back. One this installer put there before
  // is only kept until the install is done; any other (e.g. a Starfield2VR build installed by hand, perhaps for another
  // game version) is also put back when Starfield2VR is uninstalled.
  if FileExists(Dest) then begin
    H := FileHash(Dest);
    if H <> FileHash(Src) then begin
      if not MoveToBackup(Dest, Backup) then begin
        Result := 'Could not move ' + Dest + ' aside.';
        Exit;
      end;
      AddStr(Journal, 'moved|' + Backup + '|' + Dest);
      if RecordedFile(Dest, State) and IsKnownHash(H, State) then
        AddStr(RunUndoOnly, Backup)
      else
        AddStateLine(State, 'restore|' + Backup + '|' + Dest);
    end;
  end;
  if not DirExists(DestDir) then
    AddStr(Journal, 'dir|' + DestDir);
  if not FileExists(Dest) then
    AddStr(Journal, 'created|' + Dest);
  if not ForceDirectories(DestDir) or not CopyChecked(Src, Dest) then begin
    Result := 'Could not write ' + Dest + '.';
    Exit;
  end;
  if KeepOnUninstall then
    Log('Kept ' + Dest + ' (already there, for the Starfield2VR build that is put back on uninstall)')
  else begin
    AddStateLine(State, 'file|' + Dest);
    AddStateLine(State, 'hash|' + FileHash(Dest));
    Log('Installed ' + Dest);
  end;
end;

// Mod Organizer's information file for the mod. False if it could not be written.
function WriteMetaIni(const ModDir: String): Boolean;
var
  Lines: TArrayOfString;
  I: Integer;
  F: String;
begin
  F := AddBackslash(ModDir) + 'meta.ini';
  if LoadLines(F, Lines) then begin
    // Keep what Mod Organizer added (notes, colour...); only the version changes.
    for I := 0 to GetArrayLength(Lines) - 1 do
      if StartsText('version=', Lines[I]) then
        Lines[I] := 'version={#AppVersion}';
  end else begin
    SetArrayLength(Lines, 0);
    AddStr(Lines, '[General]');
    AddStr(Lines, 'gameName=Starfield');
    AddStr(Lines, 'modid=0');
    AddStr(Lines, 'version={#AppVersion}');
    AddStr(Lines, 'newestVersion=');
    AddStr(Lines, 'category="-1,"');
    AddStr(Lines, 'nexusFileStatus=1');
    AddStr(Lines, 'installationFile=');
    AddStr(Lines, 'repository=');
    AddStr(Lines, 'ignoredVersion=');
    AddStr(Lines, 'comments=VR mod for Starfield, added by the Starfield2VR installer');
    AddStr(Lines, 'notes=');
    AddStr(Lines, 'nexusDescription=');
    AddStr(Lines, 'url={#AppURL}');
    AddStr(Lines, 'hasCustomURL=true');
    AddStr(Lines, 'converted=false');
    AddStr(Lines, 'validated=true');
    AddStr(Lines, '');
    AddStr(Lines, '[installedFiles]');
    AddStr(Lines, 'size=0');
  end;
  Result := SaveStringsToUTF8FileWithoutBOM(F, Lines, False);
end;

// modlist.txt: the first line after the header comment is the highest priority (the bottom of Mod Organizer's left
// list); "+" turns a mod on, "-" off. The mod is added, turned on, where it isn't listed. Where the player turned it
// off it stays off, unless EnableOff (its old line is then replaced by a "+" line at the top). With DisableOld, the
// other Starfield2VR installs in AOldMods are turned off where this one ends up on (recorded, so the uninstaller turns
// them back on); a profile where the player turned this one off keeps the one they use.
function UpdateProfile(const ModList: String; EnableOff, DisableOld: Boolean; var State: TArrayOfString;
  var Journal: TArrayOfString): String;
var
  Lines: TRawLines;
  Bom, Eol, NewRaw, Ours, L, Minus: AnsiString;
  I, J: Integer;
  Cur, Mark, Name, Backup: String;
  Insert, Inserted, Changed, DisableHere: Boolean;
begin
  Result := '';
  if not LoadModList(ModList, Bom, Lines, Eol) then begin
    Result := 'Could not read ' + ModList + '.';
    Exit;
  end;
  Cur := ModStateIn(ModList);
  // Added (on) unless the player has it off here (then only with EnableOff).
  Insert := (Cur <> '+') and (EnableOff or not ProfileOff(ModList));
  DisableHere := DisableOld and ((Cur = '+') or Insert);
  Changed := Insert;
  Ours := '+' + ModFolderName + Eol;
  Minus := '-';
  NewRaw := Bom;
  Inserted := False;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    L := Lines[I];
    if Insert and IsOurModLine(L) then
      Continue;
    if Insert and (not Inserted) and not ((Length(L) > 0) and (Ord(L[1]) = Ord('#'))) then begin
      NewRaw := NewRaw + Ours;
      Inserted := True;
    end;
    if DisableHere then begin
      Name := ModLineName(L, Mark);
      if Mark = '+' then
        for J := 0 to GetArrayLength(AOldMods) - 1 do
          if CompareText(Name, AOldMods[J]) = 0 then begin
            L := Minus + Copy(L, 2, MaxInt);
            Changed := True;
            AddStateLine(State, 'turnedoff|' + ModList + '|' + AOldMods[J]);
            Log('Turned off the other Starfield2VR install "' + AOldMods[J] + '" in ' + ModList);
          end;
    end;
    NewRaw := NewRaw + L;
  end;
  if Insert and not Inserted then begin
    if (Length(NewRaw) > Length(Bom)) and (Ord(NewRaw[Length(NewRaw)]) <> 10) then
      NewRaw := NewRaw + Eol;
    NewRaw := NewRaw + Ours;
  end;
  if not Changed then begin
    if Cur = '+' then
      Log('Already on in ' + ModList + '; left as it is.')
    else
      Log('Turned off by the player in ' + ModList + '; left as it is.');
    Exit;
  end;
  Backup := ExpandConstant('{app}\backup\profiles\') + GetDateTimeString('yyyymmdd-hhnnss', #0, #0) + '\' +
    ExtractFileName(ExtractFileDir(ModList));
  if not ForceDirectories(Backup) or not CopyChecked(ModList, Backup + '\modlist.txt') then begin
    Result := 'Could not back up ' + ModList + '.';
    Exit;
  end;
  AddStr(Journal, 'profile|' + ModList + '|' + Backup + '\modlist.txt');
  if not SaveStringToFile(ModList, NewRaw, False) then begin
    Result := 'Could not write ' + ModList + '.';
    Exit;
  end;
  AddStateLine(State, 'profile|' + ATarget + '|' + ModList);
  if Insert then
    Log('Turned the mod on in ' + ModList + ' (backup in ' + Backup + ')')
  else
    Log('Changed ' + ModList + ' (backup in ' + Backup + ')');
end;

var
  // What this run changed, in order, so a failed or cancelled install can be undone: "moved|backup|original",
  // "created|file", "dir|folder", "profile|file|its backup" (a changed file: modlist.txt or meta.ini).
  RunJournal: TArrayOfString;
  RunStateBefore: TArrayOfString;
  RunStateFile, RunAppDir: String;
  RunChanged, RunCommitted: Boolean;
  // What an undo could not put back, for the message.
  RunLeft: TArrayOfString;

// Undoes everything this run did, newest first, and puts the record of earlier installs back as it was. Nothing is
// lost if a step fails: a moved file's backup is kept (and recorded, so the uninstaller puts it back later), a file
// that can't be deleted is recorded for the uninstaller, and the installer's folder is kept.
procedure UndoThisRun;
var
  I: Integer;
  L, Kind, Target: String;
  Failed: Boolean;
begin
  Failed := False;
  SetArrayLength(RunLeft, 0);
  Target := ATarget;
  for I := GetArrayLength(RunJournal) - 1 downto 0 do begin
    L := RunJournal[I];
    Kind := Field(L, 0);
    if Kind = 'profile' then begin
      if not CopyChecked(Field(L, 2), Field(L, 1)) then begin
        Failed := True;
        Log('UNDO FAILED: ' + Field(L, 1) + ' (its earlier copy is ' + Field(L, 2) + ')');
        AddStr(RunLeft, Field(L, 1) + ' could not be put back as it was; its earlier copy is ' + Field(L, 2));
        AddStateLine(RunStateBefore, 'profile|' + Target + '|' + Field(L, 1));
      end;
    end else if Kind = 'created' then begin
      if FileExists(Field(L, 1)) and not DeleteFile(Field(L, 1)) then begin
        Failed := True;
        Log('UNDO FAILED: could not delete ' + Field(L, 1));
        AddStr(RunLeft, Field(L, 1) + ' could not be deleted');
        AddStateLine(RunStateBefore, 'file|' + Field(L, 1));
        AddStateLine(RunStateBefore, 'hash|' + FileHash(Field(L, 1)));
      end;
    end else if Kind = 'dir' then begin
      if DirExists(Field(L, 1)) and not RemoveDir(Field(L, 1)) then begin
        Log('Could not remove the folder ' + Field(L, 1) + ' (not empty)');
        if CompareText(ExtractFileName(Field(L, 1)), ModFolderName) = 0 then
          AddStateLine(RunStateBefore, 'moddir|' + Target + '|' + Field(L, 1));
      end;
    end else if Kind = 'moved' then begin
      if not FileExists(Field(L, 2)) and CopyChecked(Field(L, 1), Field(L, 2)) then
        DeleteFile(Field(L, 1))
      else begin
        // Keep the backup, the only copy, and let the uninstaller put it back.
        Failed := True;
        Log('UNDO FAILED: ' + Field(L, 2) + ' (it is kept in ' + Field(L, 1) + ')');
        AddStr(RunLeft, Field(L, 2) + ' could not be put back; it is kept in ' + Field(L, 1));
        AddStateLine(RunStateBefore, 'restore|' + Field(L, 1) + '|' + Field(L, 2));
      end;
    end;
    Log('Undone: ' + L);
  end;
  SetArrayLength(RunJournal, 0);
  if Failed or (GetArrayLength(RunStateBefore) > 0) then
    SaveStringsToUTF8FileWithoutBOM(RunStateFile, RunStateBefore, False)
  else begin
    // A first install: nothing of the installer's own folder is left either (its backups were put back above).
    DeleteFile(RunStateFile);
    if not FileExists(AddBackslash(RunAppDir) + 'unins000.exe') then begin
      DelTree(AddBackslash(RunAppDir) + 'backup', True, True, True);
      RemoveDir(RunAppDir);
    end;
  end;
  if Failed then
    AddStr(RunLeft, 'They are listed in ' + RunStateFile + ': run this installer again and then uninstall Starfield2VR ' +
      'to clear them up.');
  RunChanged := False;
end;

// What an undo reports: all put back, or what was left and where.
function UndoMessage: String;
begin
  if GetArrayLength(RunLeft) = 0 then
    Result := 'Everything this installer had changed was put back.'
  else
    Result := 'Not everything could be put back:' + #13#10 + JoinLines(RunLeft);
end;

function DoInstall: String;
var
  State: TArrayOfString;
  ModDir, Meta: String;
  I: Integer;
begin
  Result := '';
  if not ForceDirectories(ExpandConstant('{app}')) then begin
    Result := 'Could not create ' + ExpandConstant('{app}') + '.';
    Exit;
  end;
  ExtractTemporaryFile('dxgi.dll');
  ExtractTemporaryFile('openxr_loader.dll');
  RunAppDir := ExpandConstant('{app}');
  RunStateFile := StateFile;
  LoadState(State);
  LoadState(RunStateBefore);
  SetArrayLength(RunJournal, 0);
  SetArrayLength(RunUndoOnly, 0);
  RunChanged := True;
  Result := DoMoves(State, RunJournal);
  if Result = '' then begin
    if AType = 'genesis' then begin
      ModDir := AddBackslash(AModsDir) + ModFolderName;
      AddStateLine(State, 'genesis|' + ATarget);
      AddStateLine(State, 'moddir|' + ATarget + '|' + ModDir);
      AddStateLine(State, 'profilesdir|' + ATarget + '|' + AProfilesDir);
      AddStateLine(State, 'overwrite|' + ATarget + '|' + AOverwriteDir);
      if not DirExists(ModDir) then
        AddStr(RunJournal, 'dir|' + ModDir);
      Result := InstallFile('dxgi.dll', ModDir + '\Root', State, RunJournal);
      if Result = '' then Result := InstallFile('openxr_loader.dll', ModDir + '\Root', State, RunJournal);
      if Result = '' then begin
        Meta := AddBackslash(ModDir) + 'meta.ini';
        if not FileExists(Meta) then
          AddStr(RunJournal, 'created|' + Meta)
        else if ForceDirectories(AddBackslash(RunAppDir) + 'backup') and
                CopyChecked(Meta, AddBackslash(RunAppDir) + 'backup\meta.ini.before') then
          AddStr(RunJournal, 'profile|' + Meta + '|' + AddBackslash(RunAppDir) + 'backup\meta.ini.before');
        if not WriteMetaIni(ModDir) then
          Result := 'Could not write ' + Meta + '.';
      end;
      for I := 0 to GetArrayLength(AProfiles) - 1 do
        if Result = '' then
          Result := UpdateProfile(AProfiles[I], EnableAllBox.Checked, DisableOldBox.Checked, State, RunJournal);
    end else begin
      AddStateLine(State, 'steam|' + ATarget);
      Result := InstallFile('dxgi.dll', ATarget, State, RunJournal);
      if Result = '' then Result := InstallFile('openxr_loader.dll', ATarget, State, RunJournal);
    end;
  end;
#ifdef TestRoot
  // The test build can fail here on purpose, after every change, to test that a failed install is undone.
  if (Result = '') and HasParam('TESTFAIL') then
    Result := 'Test build: failing on purpose (/TESTFAIL).';
#endif
  if (Result = '') and not SaveStringsToUTF8FileWithoutBOM(RunStateFile, State, False) then
    Result := 'Could not write ' + RunStateFile + '.';
  if Result <> '' then begin
    UndoThisRun;
    Result := Result + ' ' + UndoMessage;
  end;
end;

// The install only counts once Inno has written the uninstaller; a cancel or error before that undoes this run.
procedure CurStepChanged(CurStep: TSetupStep);
var
  I: Integer;
begin
#ifdef TestRoot
  // The test build can stop the install after "Preparing to install" (as a cancel or an error there would).
  if (CurStep = ssInstall) and HasParam('TESTABORT') then
    Abort;
#endif
  if CurStep = ssPostInstall then begin
    RunCommitted := True;
    // The install is done: backups that were only kept to undo it go.
    for I := 0 to GetArrayLength(RunUndoOnly) - 1 do
      if DeleteFile(RunUndoOnly[I]) then
        RemoveDir(ExtractFileDir(RunUndoOnly[I]));
  end;
end;

procedure DeinitializeSetup;
begin
  if RunChanged and not RunCommitted then begin
    Log('Setup did not finish: undoing this run.');
    UndoThisRun;
    Log(UndoMessage);
    if (GetArrayLength(RunLeft) > 0) and not WizardSilent then
      MsgBox('Setup did not finish, and its changes were undone. ' + UndoMessage, mbError, MB_OK);
  end;
end;

{ ---------- wizard ---------- }

var
  VcError: String;

function CheckText: String;
var
  S: String;
  I: Integer;
begin
  if AType = 'genesis' then
    S := 'Install type: Star Wars Genesis (Mod Organizer 2)' + #13#10 + 'Mod Organizer 2: ' + ATarget + #13#10 +
      'Game folder (from Mod Organizer 2): ' + AGameDir + #13#10
  else
    S := 'Install type: Steam game folder' + #13#10 + 'Game folder: ' + ATarget + #13#10;
  if AGameVersion = '' then
    S := S + 'Starfield version: unknown' + #13#10
  else
    S := S + 'Starfield version: ' + AGameVersion + #13#10;
  S := S + #13#10;
  if AVersionOk then
    S := S + 'This is the Starfield version this build of Starfield2VR is made for (' + ExpectedGameVersion + ').' + #13#10
  else
    S := S + 'WRONG STARFIELD VERSION: this build of Starfield2VR is made for Starfield ' + ExpectedGameVersion +
      '. On this version the mod loads, but all of its game features stay off (motion controls, body, weapons, VR ' +
      'menus); the log and the F11 window say so. Install anyway only if you know you want that.' + #13#10;
  if VcNeeded then begin
    S := S + #13#10 + 'MICROSOFT VISUAL C++ RUNTIME: Starfield2VR needs the Visual C++ v14 runtime (x64), version ' +
      IntToStr(VcMinMajor) + '.' + IntToStr(VcMinMinor) + ' or newer. ';
    if VcVersion = '' then
      S := S + 'This PC doesn''t have it. '
    else
      S := S + 'This PC has ' + VcVersion + ' (or some of its files are missing). ';
    S := S + 'Without it the game does not start with the mod. With the box below ticked, the installer downloads ' +
      'Microsoft''s installer for it (' + VcRedistUrl + ', about 19 MB), checks Microsoft''s signature on it and runs it; Windows asks for administrator ' +
      'permission.' + #13#10;
  end;
  for I := 0 to GetArrayLength(ANotes) - 1 do
    S := S + #13#10 + ANotes[I] + #13#10;
  Result := S;
end;

procedure UpdateFolderInfo;
var
  Dir, Gp, V: String;
begin
  Dir := RemoveBackslashUnlessRoot(Trim(FolderPage.Values[0]));
  if Dir = '' then
    FolderInfo.Caption := ''
  else if SelectedType = 'genesis' then begin
    Gp := MO2GamePath(Dir);
    if Gp = '' then
      FolderInfo.Caption := 'No Mod Organizer 2 for Starfield found in this folder (it needs ModOrganizer.exe and ' +
        'ModOrganizer.ini).'
    else begin
      V := GameVersionOf(Gp);
      if V = '' then V := 'not found';
      FolderInfo.Caption := 'Mod Organizer 2 for Starfield found.' + #13#10 + 'Starfield.exe version: ' + V;
      if VersionMatches(V) then
        FolderInfo.Caption := FolderInfo.Caption + ' (the version this build is made for)'
      else
        FolderInfo.Caption := FolderInfo.Caption + ' (this build is made for ' + ExpectedGameVersion + ')';
      FolderInfo.Caption := FolderInfo.Caption + #13#10 + 'Its game folder: ' +
        MinimizePathName(Gp, FolderInfo.Font, FolderInfo.Width - ScaleX(100));
    end;
  end else begin
    V := GameVersionOf(Dir);
    if not FileExists(AddBackslash(Dir) + 'Starfield.exe') then
      FolderInfo.Caption := 'No Starfield.exe in this folder.'
    else begin
      if V = '' then V := 'unknown';
      FolderInfo.Caption := 'Starfield.exe found, version ' + V;
      if VersionMatches(V) then
        FolderInfo.Caption := FolderInfo.Caption + ' (the version this build is made for)'
      else
        FolderInfo.Caption := FolderInfo.Caption + ' (this build is made for ' + ExpectedGameVersion + ')';
    end;
  end;
end;

procedure FolderEditChange(Sender: TObject);
begin
  UpdateFolderInfo;
end;

procedure SetUpFolderPage;
var
  T: String;
begin
  T := SelectedType;
  // Remember what was typed for the other type.
  if ShownFolderType = 'genesis' then GenesisDirValue := FolderPage.Values[0];
  if ShownFolderType = 'steam' then SteamDirValue := FolderPage.Values[0];
  if T = 'genesis' then begin
    FolderPage.Caption := 'Mod Organizer 2 folder';
    FolderPage.Description := 'Where is your Star Wars Genesis install?';
    FolderPage.SubCaptionLabel.Caption := 'Select the folder that holds ModOrganizer.exe, then click Next.';
    FolderHelp.Caption := 'For Star Wars Genesis this is <drive>:\Star Wars Genesis\Game. Starfield2VR is added ' +
      'there as its own mod, "' + ModFolderName + '", and turned on in your profiles. Close Mod Organizer 2 and the ' +
      'game first.';
    FolderPage.Values[0] := GenesisDirValue;
  end else begin
    FolderPage.Caption := 'Starfield folder';
    FolderPage.Description := 'Where is Starfield installed?';
    FolderPage.SubCaptionLabel.Caption := 'Select the folder that holds Starfield.exe, then click Next.';
    FolderHelp.Caption := 'Usually ...\steamapps\common\Starfield in your Steam library. dxgi.dll and ' +
      'openxr_loader.dll are copied there. Close the game first.';
    FolderPage.Values[0] := SteamDirValue;
  end;
  ShownFolderType := T;
  UpdateFolderInfo;
end;

// Clicking a choice's text ticks its box.
procedure ChoiceTextClick(Sender: TObject);
begin
  if Sender = InstallAnywayText then InstallAnywayBox.Checked := not InstallAnywayBox.Checked;
  if Sender = MoveForeignText then MoveForeignBox.Checked := not MoveForeignBox.Checked;
  if Sender = EnableAllText then EnableAllBox.Checked := not EnableAllBox.Checked;
  if Sender = DisableOldText then DisableOldBox.Checked := not DisableOldBox.Checked;
  if Sender = VcRedistText then VcRedistBox.Checked := not VcRedistBox.Checked;
end;

// A choice on the Checks page: a box, with its text next to it in a label that wraps onto a second line.
procedure MakeChoice(var Box: TNewCheckBox; var Txt: TNewStaticText; const Caption: String; Checked: Boolean);
begin
  Box := TNewCheckBox.Create(CheckPage);
  Box.Parent := CheckPage.Surface;
  Box.Caption := '';
  Box.Left := 0;
  Box.Width := ScaleX(18);
  Box.Height := ScaleY(17);
  Box.Checked := Checked;
  Box.Visible := False;
  Txt := TNewStaticText.Create(CheckPage);
  Txt.Parent := CheckPage.Surface;
  Txt.AutoSize := False;
  Txt.WordWrap := True;
  Txt.Left := ScaleX(20);
  Txt.Width := CheckPage.SurfaceWidth - ScaleX(20);
  Txt.Caption := Caption;
  Txt.Height := ScaleY(28);
  Txt.OnClick := @ChoiceTextClick;
  Txt.Visible := False;
end;

// Shows or hides a choice; shown ones are stacked upwards from Y (the bottom of the page).
procedure PlaceChoice(Box: TNewCheckBox; Txt: TNewStaticText; Show: Boolean; var Y: Integer);
begin
  Box.Visible := Show;
  Txt.Visible := Show;
  if Show then begin
    Txt.AdjustHeight;
    Y := Y - Txt.Height - ScaleY(6);
    Txt.Top := Y;
    Box.Top := Y;
  end;
end;

procedure InitializeWizard;
var
  List: TArrayOfString;
  T, Target: String;
begin
  // After the (hidden) folder page for the installer's own folder, so {app} is known on these pages.
  TypePage := CreateInputOptionPage(wpSelectDir, 'Install type', 'How do you play Starfield?',
    'Choose where Starfield2VR goes. You can run this installer again later to repair, update or add the other type.',
    True, False);
  TypePage.Add('Star Wars Genesis (Mod Organizer 2 with Root Builder)');
  TypePage.Add('Steam game folder (no mod manager)');

  // One line above the folder box (the page places the box under it); the explanation and what was found go below.
  FolderPage := CreateInputDirPage(TypePage.ID, 'Folder', '', 'Select the folder, then click Next.', False, '');
  FolderPage.Add('');
  FolderHelp := TNewStaticText.Create(FolderPage);
  FolderHelp.Parent := FolderPage.Surface;
  FolderHelp.AutoSize := False;
  FolderHelp.WordWrap := True;
  FolderHelp.Left := 0;
  FolderHelp.Top := FolderPage.Edits[0].Top + FolderPage.Edits[0].Height + ScaleY(12);
  FolderHelp.Width := FolderPage.SurfaceWidth;
  FolderHelp.Height := ScaleY(48);
  FolderInfo := TNewStaticText.Create(FolderPage);
  FolderInfo.Parent := FolderPage.Surface;
  FolderInfo.AutoSize := False;
  FolderInfo.WordWrap := True;
  FolderInfo.Left := 0;
  FolderInfo.Top := FolderHelp.Top + FolderHelp.Height + ScaleY(10);
  FolderInfo.Width := FolderPage.SurfaceWidth;
  FolderInfo.Height := ScaleY(64);
  FolderPage.Edits[0].OnChange := @FolderEditChange;

  CheckPage := CreateOutputMsgMemoPage(FolderPage.ID, 'Checks', 'What the installer found. Please read it.',
    'Read this, tick a box below if one is shown, then click Next.', '');
  // Unattended installs: the boxes are the command-line switches. In the wizard, turning other Starfield2VR installs
  // off and installing the runtime are ticked to begin with.
  MakeChoice(InstallAnywayBox, InstallAnywayText, 'Install anyway. On this Starfield version the mod''s game features ' +
    'stay off (motion controls, body, weapons, VR menus).', HasParam('INSTALLANYWAY'));
  MakeChoice(MoveForeignBox, MoveForeignText, 'Move the other mod''s file aside. It is kept in the installer''s folder ' +
    'and put back when Starfield2VR is uninstalled; that mod does not work meanwhile.', HasParam('MOVEFOREIGN'));
  MakeChoice(EnableAllBox, EnableAllText, 'Turn Starfield2VR on in every profile, also where it was turned off.',
    HasParam('ENABLEALL'));
  MakeChoice(DisableOldBox, DisableOldText, 'Turn off the other Starfield2VR install(s) named above, in every profile ' +
    'where this one is on (they are turned back on if you uninstall Starfield2VR).',
    HasParam('DISABLEOLD') or not WizardSilent);
  MakeChoice(VcRedistBox, VcRedistText, 'Download and install the Microsoft Visual C++ runtime (from Microsoft, about ' +
    '19 MB; Windows asks for administrator permission).', HasParam('VCREDIST') or not WizardSilent);

  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading the Microsoft Visual C++ runtime',
    nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;

  VcVersion := GetVcVersion;
  VcNeeded := not VcRuntimeOk(VcVersion);
  Log('Visual C++ runtime: msvcp140.dll ' + VcVersion + ', needs installing: ' + IntToStr(Ord(VcNeeded)));

  // Folders: the command line, then the last install, then what is found on this PC.
  SetArrayLength(KnownMO2, 0);
  AddMO2Candidate(KnownMO2, GetPreviousData('GenesisDir', ''));
  FindMO2Instances(KnownMO2);
  SetArrayLength(List, 0);
  AddSteamCandidate(List, GetPreviousData('SteamDir', ''));
  FindSteamGame(List);
  Log('Mod Organizer 2 instances found:'#13#10 + JoinLines(KnownMO2));
  Log('Starfield folders found:'#13#10 + JoinLines(List));
  if GetArrayLength(KnownMO2) > 0 then GenesisDirValue := KnownMO2[0];
  if GetArrayLength(List) > 0 then SteamDirValue := List[0];
  T := Lowercase(ParamValue('INSTALLTYPE'));
  if T = '' then
    T := GetPreviousData('InstallType', '');
  if (T = 'steam') then
    TypePage.SelectedValueIndex := 1
  else if (T = 'genesis') then
    TypePage.SelectedValueIndex := 0
  else if (GetArrayLength(KnownMO2) = 0) and (GetArrayLength(List) > 0) then
    TypePage.SelectedValueIndex := 1
  else
    TypePage.SelectedValueIndex := 0;
  Target := ParamValue('TARGET');
  if Target <> '' then begin
    Target := RemoveBackslashUnlessRoot(ExpandFileName(Target));
    if TypePage.SelectedValueIndex = 1 then
      SteamDirValue := Target
    else
      GenesisDirValue := Target;
  end;
  ShownFolderType := '';
  SetUpFolderPage;
end;

procedure CurPageChanged(CurPageID: Integer);
var
  Y: Integer;
  S: String;
begin
  if CurPageID = FolderPage.ID then
    SetUpFolderPage
  else if CurPageID = CheckPage.ID then begin
    // Through RTFText, as the page itself sets its text (so it gets the wizard's text colours).
    CheckPage.RichEditViewer.RTFText := CheckText;
    Y := CheckPage.SurfaceHeight;
    PlaceChoice(VcRedistBox, VcRedistText, VcNeeded, Y);
    PlaceChoice(DisableOldBox, DisableOldText, GetArrayLength(AOldMods) > 0, Y);
    PlaceChoice(EnableAllBox, EnableAllText, GetArrayLength(AProfilesOff) > 0, Y);
    PlaceChoice(MoveForeignBox, MoveForeignText, ANeedsConsent, Y);
    PlaceChoice(InstallAnywayBox, InstallAnywayText, not AVersionOk, Y);
    CheckPage.RichEditViewer.Height := Y - CheckPage.RichEditViewer.Top - ScaleY(6);
  end else if CurPageID = wpFinished then begin
    if AType = 'genesis' then
      S := 'Starfield2VR is installed as the mod "' + ModFolderName + '" and turned on in your profiles.' + #13#10#13#10 +
        'Before you play, make your headset''s software the active OpenXR runtime (for example Virtual Desktop''s ' +
        'VDXR, Meta Quest Link or SteamVR). Then start Mod Organizer 2 and launch the game as usual.'
    else
      S := 'Starfield2VR is installed in the Starfield folder.' + #13#10#13#10 + 'Before you play, make your ' +
        'headset''s software the active OpenXR runtime (for example Virtual Desktop''s VDXR, Meta Quest Link or ' +
        'SteamVR). Then start Starfield from Steam.';
    if VcStillMissing then
      S := S + #13#10#13#10 + 'The Microsoft Visual C++ runtime is still missing or too old: install it from ' +
        VcRedistUrl + ' before you play.';
    if VcRestart then
      S := S + #13#10#13#10 + 'Microsoft''s Visual C++ runtime installer needs Windows to restart to finish installing ' +
        'the runtime. Restart before you play.';
    WizardForm.FinishedLabel.Caption := S;
    WizardForm.AdjustLabelHeight(WizardForm.FinishedLabel);
    // The restart choice (shown when a restart is needed) goes below the text.
    WizardForm.YesRadio.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(12);
    WizardForm.NoRadio.Top := WizardForm.YesRadio.Top + WizardForm.YesRadio.Height + ScaleY(4);
  end;
end;

// Downloads Microsoft's installer for the Visual C++ runtime into the temporary folder. False (and VcError) if not.
function DownloadVcRedist: Boolean;
begin
  Result := False;
  VcError := '';
  DownloadPage.Clear;
  DownloadPage.Add(VcUrl, VcRedistFile, '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      // What came back must be Microsoft's installer for this runtime, signed by Microsoft.
      if Pos('Microsoft Visual C++', GetVersionString(ExpandConstant('{tmp}\') + VcRedistFile, 'ProductName')) <> 1 then
        RaiseException('the downloaded file is not Microsoft''s Visual C++ runtime installer');
      if not SignedByMicrosoft(ExpandConstant('{tmp}\') + VcRedistFile) then
        RaiseException('the downloaded file does not carry a valid signature from Microsoft');
      Result := True;
      Log('Downloaded ' + VcUrl);
    except
      if DownloadPage.AbortedByUser then
        VcError := 'the download was cancelled'
      else
        VcError := GetExceptionMessage;
      Log('Visual C++ runtime download failed: ' + VcError);
    end;
  finally
    DownloadPage.Hide;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Running: String;
begin
  Result := True;
  if CurPageID = FolderPage.ID then begin
    // Silent installs: the checks that matter are repeated in PrepareToInstall, which stops with an error message.
    if not Analyze(SelectedType, FolderPage.Values[0], False) and not WizardSilent then begin
      MsgBox(AError, mbError, MB_OK);
      Result := False;
    end;
  end else if (CurPageID = CheckPage.ID) and not WizardSilent then begin
    if InstallAnywayBox.Visible and not InstallAnywayBox.Checked then begin
      MsgBox('This build of Starfield2VR is made for Starfield ' + ExpectedGameVersion + ', and this Starfield is ' +
        AGameVersion + '. Tick "Install anyway" to install it all the same, or click Back.', mbError, MB_OK);
      Result := False;
    end else if MoveForeignBox.Visible and not MoveForeignBox.Checked then begin
      MsgBox('Another mod''s file is where Starfield2VR''s file must go. Tick the box to move it aside (it is put ' +
        'back when Starfield2VR is uninstalled), or click Cancel.', mbError, MB_OK);
      Result := False;
    end else begin
      if VcRedistBox.Visible and not VcRedistBox.Checked then
        if MsgBox('Without the Microsoft Visual C++ runtime the game does not start with Starfield2VR. You can install ' +
          'it later from ' + VcRedistUrl + '.' + #13#10#13#10 + 'Continue without it?', mbConfirmation,
          MB_YESNO or MB_DEFBUTTON2) <> IDYES then
          Result := False;
      if Result then
        Running := RunningPrograms
      else
        Running := '';
      while (Running <> '') and Result do begin
        if MsgBox('Please close these first:' + #13#10 + Running + #13#10 + 'Then click Retry.', mbError,
          MB_RETRYCANCEL) <> IDRETRY then
          Result := False
        else
          Running := RunningPrograms;
      end;
    end;
  end else if CurPageID = wpReady then begin
    // Downloaded once: going back and on again (e.g. after a failed install step) doesn't download it again.
    if VcNeeded and VcRedistBox.Checked and not VcDownloaded and not VcRan then begin
      VcDownloaded := DownloadVcRedist;
      if (not VcDownloaded) and not WizardSilent then
        Result := MsgBox('The Microsoft Visual C++ runtime could not be downloaded (' + VcError + ').' + #13#10#13#10 +
          'The game does not start with Starfield2VR without it; you can install it later from ' + VcRedistUrl +
          '.' + #13#10#13#10 + 'Install Starfield2VR anyway?', mbConfirmation, MB_YESNO) = IDYES;
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S, On, Off: String;
  I: Integer;
begin
  if AType = 'genesis' then begin
    S := 'Star Wars Genesis (Mod Organizer 2):' + NewLine + Space + ATarget + NewLine + NewLine;
    S := S + 'Will write:' + NewLine + Space + AddBackslash(AModsDir) + ModFolderName + '\Root\dxgi.dll' + NewLine +
      Space + AddBackslash(AModsDir) + ModFolderName + '\Root\openxr_loader.dll' + NewLine + Space +
      AddBackslash(AModsDir) + ModFolderName + '\meta.ini' + NewLine;
    On := '';
    Off := '';
    for I := 0 to GetArrayLength(AProfiles) - 1 do
      if ProfileOff(AProfiles[I]) and not EnableAllBox.Checked then
        Off := Off + Space + AProfiles[I] + NewLine
      else if ModStateIn(AProfiles[I]) <> '+' then
        On := On + Space + AProfiles[I] + NewLine;
    if On <> '' then
      S := S + NewLine + 'Will turn the mod on in (a backup of each is kept):' + NewLine + On;
    if Off <> '' then
      S := S + NewLine + 'Left turned off (as you set it):' + NewLine + Off;
    if (GetArrayLength(AOldMods) > 0) and DisableOldBox.Checked then begin
      S := S + NewLine + 'Will turn off the other Starfield2VR install(s):' + NewLine;
      for I := 0 to GetArrayLength(AOldMods) - 1 do
        S := S + Space + AOldMods[I] + NewLine;
    end;
  end else begin
    S := 'Steam game folder:' + NewLine + Space + ATarget + NewLine + NewLine;
    S := S + 'Will write:' + NewLine + Space + AddBackslash(ATarget) + 'dxgi.dll' + NewLine + Space +
      AddBackslash(ATarget) + 'openxr_loader.dll' + NewLine;
  end;
  S := S + NewLine + 'Starfield version: ' + AGameVersion;
  if not AVersionOk then
    S := S + ' (not ' + ExpectedGameVersion + ': the mod''s game features will stay off)';
  S := S + NewLine;
  if GetArrayLength(AMoves) > 0 then begin
    S := S + NewLine + 'Will move aside (into the installer''s backup folder):' + NewLine;
    for I := 0 to GetArrayLength(AMoves) - 1 do
      S := S + Space + Field(AMoves[I], 0) + ' (' + Field(AMoves[I], 3) + ')' + NewLine;
  end;
  if VcNeeded then begin
    if VcRedistBox.Checked then
      S := S + NewLine + 'Will download and install the Microsoft Visual C++ runtime (x64).' + NewLine
    else
      S := S + NewLine + 'The Microsoft Visual C++ runtime is NOT installed (you chose not to).' + NewLine;
  end;
  S := S + NewLine + 'Uninstaller, license texts and backups:' + NewLine + Space + ExpandConstant('{app}') + NewLine;
  Result := S;
end;

// Runs Microsoft's installer for the Visual C++ runtime (it asks for administrator permission itself), once. Returns
// '' if it worked, else why not.
function RunVcRedist: String;
var
  RC: Integer;
  F, Params: String;
begin
  Result := '';
  VcRan := True;
  F := ExpandConstant('{tmp}\') + VcRedistFile;
  // Unattended installs: no window from it either.
  if WizardSilent then
    Params := '/install /quiet /norestart'
  else
    Params := '/install /passive /norestart';
  // Checked again just before it runs: it must still be Microsoft's signed file.
  if not SignedByMicrosoft(F) then begin
    Result := 'Microsoft''s Visual C++ runtime installer was not run: it no longer carries a valid signature from ' +
      'Microsoft.';
    Exit;
  end;
#ifdef TestRoot
  // The test build never runs it: /FAKEVCEXIT= is the exit code to act on.
  Log('Test build: not running ' + F + ' ' + Params);
  RC := StrToIntDef(ParamValue('FAKEVCEXIT'), 0);
#else
  if not Exec(F, Params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, RC) then begin
    // Should Windows want it elevated up front, start it that way (ShellExec then gives its exit code too).
    if not ShellExec('runas', F, Params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, RC) then begin
      Result := 'Microsoft''s Visual C++ runtime installer could not be started (' + SysErrorMessage(RC) + ').';
      Exit;
    end;
  end;
#endif
  Log('Visual C++ runtime installer exit code: ' + IntToStr(RC));
  case RC of
    0, 1638: begin
        VcRunOk := True;
        Log('Visual C++ runtime installed (or a newer one already was).');
      end;
    3010, 1641: begin
        VcRestart := True;
        Log('Visual C++ runtime installed; Windows needs a restart.');
      end;
  else
    Result := 'Microsoft''s Visual C++ runtime installer ended with code ' + IntToStr(RC) + '.';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Running, W: String;
begin
  Result := '';
  // Checked again here: on a silent install this is the only check, and things may have changed since the pages.
  if not Analyze(SelectedType, FolderPage.Values[0], True) then begin
    Result := AError;
    Exit;
  end;
  Running := RunningPrograms;
  if Running <> '' then begin
    Result := 'Please close these first, then run the installer again:' + #13#10 + Running;
    Exit;
  end;
  if (not AVersionOk) and not InstallAnywayBox.Checked then begin
    Result := 'This build of Starfield2VR is made for Starfield ' + ExpectedGameVersion + ', but the game is ' +
      AGameVersion + '. Nothing was installed. (Unattended installs: add /INSTALLANYWAY to install all the same.)';
    Exit;
  end;
  if ANeedsConsent and not MoveForeignBox.Checked then begin
    Result := 'Another mod''s file is where Starfield2VR''s file must go. Nothing was installed. ' +
      '(Unattended installs: add /MOVEFOREIGN to move it aside.)';
    Exit;
  end;
  // The runtime first: it is a part of Windows, so it stays installed even if the mod's install fails. Run once only,
  // even if this page is reached again.
  if VcDownloaded and not VcRan then begin
    W := RunVcRedist;
    if W <> '' then begin
      Log(W);
      if not WizardSilent then
        MsgBox(W + #13#10#13#10 + 'Starfield2VR is installed all the same, but the game does not start with it until ' +
          'the runtime is installed: ' + VcRedistUrl, mbInformation, MB_OK);
    end;
    // It took a while: Mod Organizer 2 or the game may have been started meanwhile.
    Running := RunningPrograms;
    if Running <> '' then begin
      Result := 'Please close these first, then run the installer again:' + #13#10 + Running;
      Exit;
    end;
  end else if VcNeeded and not VcRan then
    Log('The Visual C++ runtime is missing or too old and is not being installed.');
  Result := DoInstall;
  if Result <> '' then
    Log('Install failed: ' + Result);
end;

// A restart is only asked for when Microsoft's runtime installer said so. (With /VERYSILENT and no /NORESTART, Setup
// restarts Windows without asking; the test build never asks for one.)
function NeedRestart: Boolean;
begin
#ifdef TestRoot
  if VcRestart then
    Log('Test build: a restart would be asked for now.');
  Result := False;
#else
  Result := VcRestart;
#endif
end;

// Exit code 10 (instead of 0) when the mod is installed but the Visual C++ runtime it needs is still missing or too old.
function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if VcStillMissing then begin
    Log('Exit code 10: the Visual C++ runtime is still missing or too old.');
    Result := 10;
  end;
end;
procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'InstallType', AType);
  if AType = 'genesis' then begin
    SetPreviousData(PreviousDataKey, 'GenesisDir', ATarget);
    SetPreviousData(PreviousDataKey, 'SteamDir', GetPreviousData('SteamDir', ''));
  end else begin
    SetPreviousData(PreviousDataKey, 'SteamDir', ATarget);
    SetPreviousData(PreviousDataKey, 'GenesisDir', GetPreviousData('GenesisDir', ''));
  end;
end;

{ ---------- uninstalling ---------- }

var
  UninstallLeft: TArrayOfString;
  UninstallKeepBackup: Boolean;

// Deletes a file only if it is recognisably Starfield2VR's.
procedure RemoveOurFile(const Path: String; const State: TArrayOfString);
var
  K: Integer;
begin
  if not FileExists(Path) then
    Exit;
  K := ClassifyFile(Path, CompareText(ExtractFileName(Path), 'dxgi.dll') = 0, State);
  if (K = fcSame) or (K = fcOurs) then begin
    if DeleteFile(Path) then
      Log('Deleted ' + Path)
    else
      AddStr(UninstallLeft, Path + ' (could not be deleted)');
  end else
    AddStr(UninstallLeft, Path + ' (left alone: it is not Starfield2VR''s file now)');
end;

procedure RemoveModLines(const ModList: String);
var
  Lines: TRawLines;
  Bom, Eol, NewRaw: AnsiString;
  I: Integer;
  Changed: Boolean;
begin
  if not LoadModList(ModList, Bom, Lines, Eol) then
    Exit;
  NewRaw := Bom;
  Changed := False;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if IsOurModLine(Lines[I]) then
      Changed := True
    else
      NewRaw := NewRaw + Lines[I];
  if Changed then begin
    if SaveStringToFile(ModList, NewRaw, False) then
      Log('Removed the mod from ' + ModList)
    else
      AddStr(UninstallLeft, ModList + ' (could not be changed)');
  end;
end;

procedure RemoveFromProfiles(const ProfilesDir: String);
var
  FR: TFindRec;
begin
  if FindFirst(AddBackslash(ProfilesDir) + '*', FR) then begin
    try
      repeat
        if (FR.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (FR.Name <> '.') and (FR.Name <> '..') then
          RemoveModLines(AddBackslash(ProfilesDir) + FR.Name + '\modlist.txt');
      until not FindNext(FR);
    finally
      FindClose(FR);
    end;
  end;
end;

// Where a file that cannot be put back is kept (the test build keeps it inside its test folder).
function KeptFilesDir: String;
begin
#ifdef TestRoot
  Result := '{#TestRoot}\kept files\';
#else
  Result := ExpandConstant('{userdocs}\Starfield2VR kept files\');
#endif
end;

// A file name that isn't taken yet: Base, else Base-2, Base-3...
function FreeName(const Base: String): String;
var
  N: Integer;
begin
  Result := Base;
  N := 2;
  while FileExists(Result) or DirExists(Result) do begin
    Result := Base + '-' + IntToStr(N);
    N := N + 1;
  end;
end;

// Puts a moved-aside file back. The backup is only deleted once a checked copy of it is somewhere the uninstaller
// doesn't delete; the backup folder itself is deleted at the end (usPostUninstall), unless something is still in it.
procedure RestoreBackup(const Backup, Original: String);
var
  Dir, Saved, Kept: String;
begin
  if not FileExists(Backup) then
    Exit;
  // Its folder is made again only if the folder above it is still there (e.g. overwrite\Root after Wabbajack cleared
  // overwrite), never a whole deleted install.
  Dir := ExtractFileDir(Original);
  if not DirExists(Dir) and DirExists(ExtractFileDir(Dir)) then
    ForceDirectories(Dir);
  if DirExists(Dir) then begin
    if not FileExists(Original) then begin
      if CopyFile(Backup, Original, True) and (FileHash(Original) = FileHash(Backup)) then begin
        DeleteFile(Backup);
        Log('Put back ' + Original);
        Exit;
      end;
    end else begin
      // Something new is in its place: keep both.
      Saved := FreeName(Original + '.before-starfield2vr');
      if CopyFile(Backup, Saved, True) and (FileHash(Saved) = FileHash(Backup)) then begin
        DeleteFile(Backup);
        AddStr(UninstallLeft, Original + ' was not put back because another file is there now; the old one is ' + Saved);
        Exit;
      end;
    end;
  end;
  // It can't go back (its folder is gone or can't be written): keep it in Documents.
  Kept := FreeName(KeptFilesDir + GetDateTimeString('yyyymmdd-hhnnss', #0, #0));
  if ForceDirectories(Kept) and CopyFile(Backup, Kept + '\' + ExtractFileName(Original), True) then begin
    DeleteFile(Backup);
    SaveStringToFile(Kept + '\where it was.txt', Original + #13#10, False);
    AddStr(UninstallLeft, Original + ' could not be put back; it is kept in ' + Kept);
  end else begin
    UninstallKeepBackup := True;
    AddStr(UninstallLeft, Original + ' could not be put back; it is kept in ' + Backup);
  end;
end;

// Turns an install of Starfield2VR that the installer turned off back on ("-Name" -> "+Name"), if it is still off.
procedure TurnBackOn(const ModList, Name: String);
var
  Lines: TRawLines;
  Bom, Eol, NewRaw, L, Plus: AnsiString;
  I: Integer;
  Mark: String;
  Changed: Boolean;
begin
  if not LoadModList(ModList, Bom, Lines, Eol) then
    Exit;
  Plus := '+';
  NewRaw := Bom;
  Changed := False;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    L := Lines[I];
    if (CompareText(ModLineName(L, Mark), Name) = 0) and (Mark = '-') then begin
      L := Plus + Copy(L, 2, MaxInt);
      Changed := True;
    end;
    NewRaw := NewRaw + L;
  end;
  if Changed then begin
    if SaveStringToFile(ModList, NewRaw, False) then
      Log('Turned "' + Name + '" back on in ' + ModList)
    else
      AddStr(UninstallLeft, ModList + ' (could not turn "' + Name + '" back on)');
  end;
end;

// True if a folder holds nothing but a meta.ini.
function OnlyMetaIniIn(const Dir: String): Boolean;
var
  FR: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Dir) + '*', FR) then begin
    try
      repeat
        if (FR.Name <> '.') and (FR.Name <> '..') and (CompareText(FR.Name, 'meta.ini') <> 0) then
          Result := False;
      until not FindNext(FR);
    finally
      FindClose(FR);
    end;
  end;
end;

function InitializeUninstall: Boolean;
var
  Running: String;
begin
  Result := True;
  Running := RunningPrograms;
  while (Running <> '') and Result do begin
    if UninstallSilent then begin
      Log('Uninstall stopped: close these first: ' + Running);
      Result := False;
    end else if MsgBox('Please close these first:' + #13#10 + Running + #13#10 + 'Then click Retry.', mbError,
      MB_RETRYCANCEL) <> IDRETRY then
      Result := False
    else
      Running := RunningPrograms;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  State, SettingsDirs: TArrayOfString;
  I: Integer;
  L, Kind, Msg: String;
  RemoveSettings: Boolean;
begin
  if CurUninstallStep = usPostUninstall then begin
    if not UninstallKeepBackup then begin
      DelTree(ExpandConstant('{app}\backup'), True, True, True);
      RemoveDir(ExpandConstant('{app}'));
    end;
    Exit;
  end;
  if CurUninstallStep <> usUninstall then
    Exit;
  SetArrayLength(UninstallLeft, 0);
  LoadState(State);

  // The mod's settings and logs (its sfvr folder) are only deleted if the user says so.
  SetArrayLength(SettingsDirs, 0);
  for I := 0 to GetArrayLength(State) - 1 do begin
    L := State[I];
    Kind := Field(L, 0);
    if (Kind = 'steam') and DirExists(AddBackslash(Field(L, 1)) + 'sfvr') then
      AddStr(SettingsDirs, AddBackslash(Field(L, 1)) + 'sfvr');
    if (Kind = 'overwrite') and DirExists(AddBackslash(Field(L, 2)) + 'Root\sfvr') then
      AddStr(SettingsDirs, AddBackslash(Field(L, 2)) + 'Root\sfvr');
    if (Kind = 'moddir') and DirExists(AddBackslash(Field(L, 2)) + 'Root\sfvr') then
      AddStr(SettingsDirs, AddBackslash(Field(L, 2)) + 'Root\sfvr');
  end;
  RemoveSettings := False;
  if GetArrayLength(SettingsDirs) > 0 then begin
    if UninstallSilent then
      RemoveSettings := HasParam('REMOVESETTINGS')
    else
      RemoveSettings := MsgBox('Also delete Starfield2VR''s settings and logs?' + #13#10#13#10 +
        JoinLines(SettingsDirs) + #13#10 + 'Click No to keep them (for example to install the mod again later).',
        mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  end;

  // The installed files (only if they are still Starfield2VR's).
  for I := 0 to GetArrayLength(State) - 1 do
    if Field(State[I], 0) = 'file' then
      RemoveOurFile(Field(State[I], 1), State);
  // The files moved aside go back, newest first.
  for I := GetArrayLength(State) - 1 downto 0 do
    if Field(State[I], 0) = 'restore' then
      RestoreBackup(Field(State[I], 1), Field(State[I], 2));
  // Mod Organizer 2: the mod's lines in every profile, other Starfield2VR installs it turned off back on, then the
  // mod folder (only what the installer put there).
  for I := 0 to GetArrayLength(State) - 1 do begin
    L := State[I];
    Kind := Field(L, 0);
    if Kind = 'profile' then
      RemoveModLines(Field(L, 2))
    else if Kind = 'profilesdir' then
      RemoveFromProfiles(Field(L, 2))
    else if Kind = 'turnedoff' then
      TurnBackOn(Field(L, 1), Field(L, 2));
  end;
  if RemoveSettings then
    for I := 0 to GetArrayLength(SettingsDirs) - 1 do
      if DelTree(SettingsDirs[I], True, True, True) then
        Log('Deleted ' + SettingsDirs[I])
      else
        AddStr(UninstallLeft, SettingsDirs[I] + ' (could not be deleted)');
  for I := 0 to GetArrayLength(State) - 1 do
    if Field(State[I], 0) = 'moddir' then begin
      L := Field(State[I], 2);
      RemoveDir(AddBackslash(L) + 'Root');
      if DirExists(L) then begin
        // meta.ini goes only with the folder: if something else is still in it, Mod Organizer keeps its notes.
        if OnlyMetaIniIn(L) then
          DeleteFile(AddBackslash(L) + 'meta.ini');
        if not RemoveDir(L) then
          AddStr(UninstallLeft, L + ' (not empty, so it was left)');
      end;
    end;

  if GetArrayLength(UninstallLeft) > 0 then begin
    Msg := 'Starfield2VR is uninstalled, but these were left:' + #13#10#13#10 + JoinLines(UninstallLeft);
    Log(Msg);
    if not UninstallSilent then
      MsgBox(Msg, mbInformation, MB_OK);
  end;
end;
