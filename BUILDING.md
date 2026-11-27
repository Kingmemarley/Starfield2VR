# Building Starfield2VR

The mod builds against `extern/vrcore`, the open VR layer in this repository. Every other dependency is downloaded by CMake
the first time you configure.

## Requirements

- Visual Studio 2022 or newer, with the **Desktop development with C++** workload. It includes MSVC, the Windows SDK, CMake
  (3.27 or newer) and Ninja.
- Git (CMake uses it to download the dependencies).
- An internet connection for the first configure.
- Only for making a port table for another game version: Python 3.9+ with `pip install capstone numpy` (see
  [docs/PORTING.md](docs/PORTING.md)).

## Build

Open an **x64 Native Tools Command Prompt** for your Visual Studio version, go to this folder, and configure and build the
preset for your game version:

```
:: Starfield 1.16.244 (Steam's latest; the release build)
cmake --preset build-release-no-debug-msvc-1.16.244
cmake --build --preset release-no-debug-msvc-1.16.244

:: Starfield 1.16.236
cmake --preset build-release-no-debug-msvc
cmake --build --preset release-no-debug-msvc
```

| Game version | The DLL |
| - | - |
| 1.16.244 | `build/build-release-no-debug-msvc-1.16.244/dxgi.dll` (also copied to `artifact/ModRelease-1.16.244/sfvr/`) |
| 1.16.236 | `build/build-release-no-debug-msvc/dxgi.dll` (also copied to `artifact/ModRelease/sfvr/`) |

- Each DLL reads the running game's version from `Starfield.exe` and logs it. On another version, the mod's own features
  stay off.
- **Copying into the game.** The 1.16.236 build copies `dxgi.dll` into `STARFIELD_GAME_DIR` if that folder exists. The
  default is `C:/Program Files (x86)/Steam/steamapps/common/Starfield`.
  - For another folder, add `-DSTARFIELD_GAME_DIR="D:/SteamLibrary/steamapps/common/Starfield"` (your path) when you
    configure.
  - With Mod Organizer 2, give it a folder that does not exist, and copy the DLL into your mod by hand.
  - The 1.16.244 build never copies itself anywhere.
- How the 1.16.244 build finds the game's code, and how to port to another version: [docs/PORTING.md](docs/PORTING.md).

## What goes in a release

- `dxgi.dll` from the 1.16.244 build.
- `shipping/openxr_loader.dll` (the Khronos OpenXR loader).
- `LICENSE` and `THIRD_PARTY_NOTICES.md`. The licenses of the libraries inside the two DLLs ask for their notices to go
  with the files.

`shipping/openvr_api.dll` is not part of a release. The mod uses OpenXR. Its OpenVR path is untested, and it has no default
SteamVR bindings.

Players do not need the ViGEmBus driver: the mod gives the game its own virtual controller and uses ViGEmBus only as a fallback, when it is installed (see [README.md](README.md)). It is not bundled.

## When something goes wrong

- **Build errors:** the first error lines of the build output say what failed.
- **In the game:** the log is `sfvr/sfvr_log.txt` next to `Starfield.exe` (with MO2 and Root Builder:
  `overwrite\Root\sfvr`). F11 opens the mod's menu on the monitor and in the headset.
