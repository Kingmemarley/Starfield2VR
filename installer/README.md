# Starfield2VR installer

`Starfield2VR.iss` builds `Starfield2VR-Setup-<version>.exe`, a single installer with two install types:

- **Star Wars Genesis (Mod Organizer 2)**: adds the mod to a portable Mod Organizer 2 instance as
  `mods\[NoDelete] Starfield2VR\Root\` (`dxgi.dll`, `openxr_loader.dll`) plus a `meta.ini`, and turns it on in the
  profiles' `modlist.txt` (as the first line after the header: the highest priority). A profile where the player turned
  the mod off is left off, unless they tick "Turn Starfield2VR on in every profile". `modlist.txt` is edited as bytes:
  only the mod's line changes (other lines, their encoding, the line breaks and a BOM are kept). Root Builder copies the
  Root folder next to `Starfield.exe` while the game runs. The `[NoDelete]` prefix keeps a Wabbajack update of the
  modlist from deleting the mod (an update turns it off; run the installer again, or turn it on in Mod Organizer 2).
- **Steam game folder**: copies `dxgi.dll` and `openxr_loader.dll` next to `Starfield.exe`.

The installer finds Star Wars Genesis (`<drive>:\Star Wars Genesis\Game`, its setup's uninstall entry, Mod Organizer's
nexus link handler) and Steam's Starfield (Steam's uninstall entry, `libraryfolders.vdf`, `appmanifest_1716740.acf`), and
lets the player browse. It checks the folder, shows the Starfield version, and asks for an explicit "install anyway" if the
game is not the version the bundled `dxgi.dll` is made for. Mod Organizer 2 and the game must be closed. It needs no
administrator rights (only Microsoft's Visual C++ runtime installer asks for them, if that is installed).
A profile that only lists an earlier Starfield2VR mod, turned off, counts as turned off by the player too.

A file already where Starfield2VR's goes is handled like this:
- Starfield2VR's own (by its version resource's ProductName `Starfield2VR`, or a known hash, including the v1.0.0
  release): replaced, or moved out of Mod Organizer's `overwrite\Root` (which would beat the mod).
- Another OpenXR loader, or the stock loader next to another mod's `dxgi.dll`: moved aside, put back on uninstall.
- Anything else (ReShade, an upscaler...): only moved aside if the player agrees; put back on uninstall.

An earlier Starfield2VR installed by hand as its own Mod Organizer mod (a mod whose `Root\dxgi.dll` is Starfield2VR's)
is recognised, and turned off in the profiles where the new mod is on if the player agrees (turned back on on uninstall).

The mod's `dxgi.dll` needs the Microsoft Visual C++ v14 runtime (x64), 14.51 or newer (it is built with the 14.51 toolset).
If `msvcp140.dll` is older or one of the runtime's DLLs is missing, the Checks page offers to download Microsoft's
installer (`https://aka.ms/vc14/vc_redist.x64.exe`, about 19 MB). The download must carry a valid Authenticode signature
from Microsoft Corporation (checked with Windows' `Get-AuthenticodeSignature` after the download and again just before it
runs); then it runs once with `/install /passive /norestart` (`/quiet` on a silent install). Exit code 3010 asks for a
restart at the end. Declining is allowed, with a warning.

The installer's own folder (`%LOCALAPPDATA%\Programs\Starfield2VR`) holds the uninstaller, the license texts, the backups
and `install-state.txt`, the record of everything done. A failed or cancelled install puts back everything it changed; if something can't be put back (a file held open by
another program), nothing is lost: the file stays in the backup folder, it is recorded, the message says what was left,
and running the installer again and uninstalling puts it right. Another Starfield2VR build that was already there (in
`overwrite\Root`, the game folder or the mod folder, e.g. one for another game version) is kept and put back on
uninstall; if it is made for the game's actual version and the bundled build isn't, the Checks page says so plainly.
The uninstaller removes only files it recognises as Starfield2VR's, puts the moved files back (a file whose folder is gone
is kept in `Documents\Starfield2VR kept files`, never deleted), takes the mod out of every profile, and asks before
deleting the mod's settings and logs (the `sfvr` folder).

## Building

1. Install Inno Setup 7 (`winget install --id JRSoftware.InnoSetup.7 -e`).
2. Build the 1.16.244 DLL: `cmake --preset build-release-no-debug-msvc-1.16.244`, then
   `cmake --build --preset release-no-debug-msvc-1.16.244`.
3. Compile:

   ```
   "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" installer\Starfield2VR.iss
   ```

   The output is `build\installer\Starfield2VR-Setup-1.1.1.exe`.

Options (`/D` on the ISCC command line):

| Define | Default | |
| - | - | - |
| `AppVersion` | `1.1.0` | Version shown and in the file name. |
| `DxgiSource` | `build\build-release-no-debug-msvc-1.16.244\dxgi.dll` | The `dxgi.dll` to bundle. |
| `OpenXrSource` | `shipping\openxr_loader.dll` | |
| `ExpectedGameVersion` | `1.16.244` | The Starfield version the bundled `dxgi.dll` is made for. |
| `ExtraKnownHashes` | the v1.0.0 release's hash | SHA-256 hashes (space-separated) of earlier released `dxgi.dll` builds, so they count as Starfield2VR's. |
| `TestRoot` | not set | A test build: its own AppId and installer folder, output in `<TestRoot>\out`; it refuses every folder outside `<TestRoot>`, never runs the Visual C++ installer or asks for a restart, and has test switches (`/FAKEVCVERSION=`, `/FAKEVCEXIT=`, `/VCURL=`, `/TESTFAIL`, `/TESTABORT`). |

The `dxgi.dll`'s hash is built into the installer, so compile again after rebuilding the DLL.

## Command line

```
Starfield2VR-Setup-1.1.1.exe /VERYSILENT /NORESTART /INSTALLTYPE=genesis /TARGET="C:\Star Wars Genesis\Game" /VCREDIST
Starfield2VR-Setup-1.1.1.exe /VERYSILENT /NORESTART /INSTALLTYPE=steam /TARGET="C:\Program Files (x86)\Steam\steamapps\common\Starfield"
```

| Parameter | |
| - | - |
| `/INSTALLTYPE=genesis` or `/INSTALLTYPE=steam` | Install type (also preselects it in the wizard). `/TYPE` is Inno Setup's own parameter, so it isn't used. |
| `/TARGET=<folder>` | The Mod Organizer 2 folder (with `ModOrganizer.exe`) or the game folder (with `Starfield.exe`); a relative path is taken from the current folder. |
| `/INSTALLANYWAY` | Install on a Starfield version other than the expected one. |
| `/MOVEFOREIGN` | Move another mod's `dxgi.dll` / `openxr_loader.dll` aside. |
| `/ENABLEALL` | Turn the mod on in every profile, also where the player turned it off. |
| `/DISABLEOLD` | Turn off an earlier Starfield2VR installed as its own mod (ticked by default in the wizard). |
| `/VCREDIST` | Download and install the Visual C++ runtime if it is needed (ticked by default in the wizard). |
| `/NORESTART` | Inno Setup's own: never restart Windows. Use it with `/VCREDIST`: if Microsoft's runtime installer asks for a restart, a `/VERYSILENT` install otherwise restarts Windows without asking. |
| `/RESTARTEXITCODE=<n>` | Inno Setup's own: the exit code to return when a restart is needed (with `/NORESTART`). |
| `/DIR=<folder>` | The installer's own folder (default `%LOCALAPPDATA%\Programs\Starfield2VR`). |
| `/LOG=<file>` | Inno Setup's log; the checks and every action are written to it. |

Exit code 0: installed. Exit code 10: installed, but the Visual C++ runtime the mod needs is still missing or too old.
Exit code 7: a check failed and nothing was installed, or the install failed and its changes were undone (the reason, and
anything that could not be put back, is in the log).

Uninstall: `unins000.exe /VERYSILENT` in the installer's folder; add `/REMOVESETTINGS` to also delete the `sfvr` folders.
