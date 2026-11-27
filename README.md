# Starfield2VR

A VR mod for **Starfield** on PC. You play the whole game in a PC VR headset with motion controls, in the style of
Skyrim VR and Fallout 4 VR mods: you see your own body, hold your gun in your hand, grab and throw things, climb with
your hands, and use the game's menus on a panel in front of you.

The mod is built on [Mutar's starfield2vr](https://github.com/mutars/starfield2vr). It runs on an open VR layer
(`extern/vrcore`), which is based on praydog's [REFramework](https://github.com/praydog/REFramework). Everything is MIT
licensed.

**Download:** [Releases](https://github.com/Kingmemarley/Starfield2VR/releases). The release is for **Starfield 1.16.244
(Steam)** only, and holds two files: `dxgi.dll` and `openxr_loader.dll`. For Starfield 1.16.236, build the mod from source
(see [Building from source](#building-from-source)).

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [First start](#first-start)
- [Controls](#controls)
- [Settings](#settings)
- [Compatibility](#compatibility)
- [Known limitations](#known-limitations)
- [Uninstall](#uninstall)
- [Reporting bugs](#reporting-bugs)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [License](#license)

---

## Features

**Seeing**
- Full 6DoF head tracking, in your headset's own resolution.
- Three eye modes: **full stereo** (both eyes drawn every frame: real depth), **mono** (one picture for both eyes:
  fastest, but flat) and **AER** (one eye drawn each frame). Choose one in the F11 menu; a button combo switches between
  stereo and mono in game.
- Snap or smooth turning. Optional level view (look up and down with your head only).
- Menus on a panel that stays in the room. Many menus are see-through, with the live world behind them.
- Conversations on their own panel, in front of the person who is talking.
- The game's messages (notifications, mission updates, level-ups) gathered just below the middle of your view.
- Build mode laid out as a console in front of you.

**Your body and hands**
- A visible body. Your arms follow the controllers, your legs step as you walk, and the spine leans toward your head.
- Fingers follow the trigger, grip and thumb. Quest body tracking and hand tracking are used when the runtime gives them.
- Guns are held in your hand and shots leave from the muzzle. For two-handed aim, squeeze the left grip with your left hand at the gun's front (a left hand only resting near the gun no longer steers it).
- A red dot or laser sight on your gun.
- Controller vibration for shots, hits and reloads, and the game's own rumble passed on.

**Hands-on play**
- **Grab** loose objects with a grip, turn them in your hand, throw them. Hold one object with both hands, or one in each hand.
- **Gravity gloves**: point at something far away, flick your hand back, and it flies to you.
- **Bag and food**: let go of an item behind your shoulder or at your hip to put it in your inventory. Let go of food at your
  mouth to eat it.
- **Pick up weapons**: hold a weapon and pull the trigger to equip it.
- **Hand interaction**: with your weapon put away, the "Take / Open / Talk" prompt is for whatever is near your hand.
- **Melee by swinging**: swing your fist, blade or gun hard to hit what it passes through.
- **Throw your melee weapon** (lightsaber style) and call it back. Guide it in the air with the trigger.
- **Dual wielding**: a melee weapon in your left hand as well.
- **Powers**: hold a grip and push your open palm out toward the target.
- **Grenades**: reach behind your right shoulder, grab a grenade, and throw it with your arm.
- **Temple gestures**: trigger at your left temple for the flashlight (no helmet needed), at your right temple for the scanner.
- **Climbing**: put a hand on a wall, rock or ledge, squeeze the grip and pull yourself along.
- **Ladders** in first person, climbed with your hands.
- **People**: grab and drag bodies, hold people by the arm, shove them, knock them down with a hard punch, and block
  melee attacks with your blade.
- **Favorites wheel**: a wheel at your left hand (as in Spell Wheel VR) instead of the game's favorites menu.

**Menus**
- Point at a menu with either controller and pull the trigger to click. The thumbstick scrolls.
- The left thumbstick can also move through menus the way a gamepad does.
- A **VR tab** in the game's own Settings menu with the main VR settings: eye mode, sharpness, recentring, body,
  turning, left-handed, face buttons, vibration, HUD size and climbing.
- The mod's own settings window (**F11**): in the headset on a panel of its own, used with a controller's pointer; on the
  monitor with the mouse: everything else, by feature.

---

## Requirements

| What | Notes |
| - | - |
| **Starfield 1.16.244, Steam** | Check it: right-click `Starfield.exe` > Properties > Details. It should say `1.16.244.0`. On any other version the mod's own features switch themselves off, and the log and the F11 window say so. The Game Pass / Microsoft Store version is not supported by this release. |
| **A PC VR headset with an OpenXR runtime** | Developed and tested with a Meta Quest 3 over Virtual Desktop (VDXR). Other OpenXR runtimes (Meta Quest Link, SteamVR's OpenXR runtime, Windows Mixed Reality) should work, but have not been tested yet. Your runtime must be set as the active OpenXR runtime. |
| **Motion controllers** | Made for Meta Touch controllers (A/B/X/Y, grips, triggers, thumbsticks, **thumbrests**). The code also has bindings for Valve Index, HTC Vive and Windows Mixed Reality controllers. Those have not been tested yet. They have no thumbrest sensor, so you need the Alternative Joy Layout (see [CONTROLS.md](CONTROLS.md)). |
| **Microsoft Visual C++ runtime (x64), 14.51 or newer** | The installer checks it and offers Microsoft's download. Installing by hand: get the [latest x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe) from Microsoft. |
| **No ViGEmBus driver needed** | The mod gives the game its own virtual Xbox controller through Windows' gamepad list. If that ever fails, it falls back to the [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0) driver when it is installed (the log says which way it used). |
| **Windows 10 or 11, 64-bit, a CPU with AVX2** | The release is built with AVX2. A CPU that runs Starfield should have it. |
| **A strong graphics card** | The code has no minimum, but VR is heavy. An NVIDIA RTX card is recommended: each eye keeps its own DLSS history, and the laser sight's dot lands on what you point at only with DLSS. Without DLSS (FSR, TAA) the game still runs, but the dot sits at a fixed distance. Foveated shading needs a card with variable rate shading tier 2. |

SFSE and the Address Library are **not** needed.

---

## Installation

### 1. The installer (recommended)

Download `Starfield2VR-Setup-<version>.exe` from the [release](https://github.com/Kingmemarley/Starfield2VR/releases) and run it with the game and Mod Organizer 2 closed. It offers two install types:
- **Star Wars Genesis (Mod Organizer 2)**: adds the mod to the modlist as its own mod with a `Root` folder (for Root Builder) and turns it on in the profiles.
- **Steam game folder**: copies the files next to `Starfield.exe`.

It finds the folders itself (or lets you browse), checks the game version, moves another mod's `dxgi.dll` aside only if you agree (and puts it back on uninstall), and offers Microsoft's Visual C++ runtime if yours is too old. Uninstall it from Windows Settings > Apps. Then set your OpenXR runtime (step 2) and check the game settings (step 4).

To install by hand instead, follow steps 2 to 4.

### 2. Set your OpenXR runtime

Make your headset's software the active OpenXR runtime:
- **Virtual Desktop**: in the streamer's settings, choose VDXR as the OpenXR runtime.
- **Meta Quest Link**: Meta Quest Link app > Settings > General > OpenXR Runtime > set as active.
- **SteamVR**: SteamVR Settings > OpenXR > set SteamVR as the OpenXR runtime.

### 3. Copy the two files

From the [release](https://github.com/Kingmemarley/Starfield2VR/releases), copy **`dxgi.dll`** and **`openxr_loader.dll`**
into the folder that holds `Starfield.exe`. On Steam that is usually:

```
<your Steam library>\steamapps\common\Starfield\
```

### Mod Organizer 2

Both files must end up next to `Starfield.exe`, not in `Data`.
- **With Root Builder**: make a mod with a `Root` folder, put the two files in `Root`, and enable the mod.
  Files the mod writes while the game runs (its `sfvr` folder) then end up in MO2's `overwrite\Root\sfvr`.
- **Without Root Builder**: copy the two files into the real game folder by hand.

### Vortex

Not tested yet. The two files must end up next to `Starfield.exe`, not in `Data`. If you are not sure Vortex puts them
there, copy them into the game folder by hand.

### 4. Game settings

- **Frame Generation: off.** It is not supported.
- DLSS is recommended (see Requirements).
- Using the gun sight? Turn the game's own crosshair off (Settings > Interface).
- Keep the game's controller vibration on if you want to feel hits and explosions.
- The original starfield2vr also suggests: display mode windowed, V-Sync off, motion blur off, depth of field off, and
  dynamic resolution off. These have not been re-tested with this mod.

---

## First start

1. Start your VR runtime (for example Virtual Desktop) first, then start Starfield the way you usually do (Steam, or MO2).
2. At the main menu the game window fits your monitor, and the menu is on a panel in the headset. Point a controller at it
   and pull the trigger to click.
3. **Recenter.** The mod recenters your view after every loading screen (Settings > VR > "Recenter after loading screens", on
   by default). To recenter at any time: Settings > VR > tick **Recenter now** (it unticks itself). Sit or stand the way you
   will play, and look straight ahead.
4. **Seated or standing** is worked out by itself after each recenter, from how high your eyes are above the floor:
   - below 1.3 m you count as **seated**, and your movement is used 1:1;
   - standing, your head and hand movement are scaled so your eye height matches your character's ("Match your height to the
     character's", on by default);
   - "Real crouch" (bend the body when you crouch in real life) is off by default. It is meant for standing play.
5. **The VR tab: the main VR settings.** Pause > Settings > **VR** (also from the main menu's Settings), used with the
   controllers like the rest of the game's Settings:
   - **Recentring**: Recenter now, Recenter after loading screens.
   - **View**: Eye mode, Sharpness (resolution).
   - **Body**: Body and motion controls; Seat: view at the body's eyes (sitting in a chair or the pilot's seat of a landed
     ship, your view starts at your seated body's eyes; leaning more than 15 cm from where you sat is held back softly, never
     past 30 cm).
   - **Turning**: Turn mode, Snap angle, Smooth turn speed.
   - **Controllers**: Left-handed, Face buttons as printed, Controller vibration, Vibration strength.
   - **HUD**: HUD size. **Climbing**: Climb with your hands, Climbing pull.

   The game's Defaults button resets every row of that tab to the mod's defaults, the eye mode (Full stereo) and the
   sharpness (1.0) too. They are saved in the mod's own settings file, with the F11 menu's settings. (If the VR tab cannot
   be added, for example on another game version, they are on F11's Main tab instead. If the tab is not shown, for example
   because another mod replaces the Settings menu, turn on F11 > Advanced > "Show the main VR settings here too".)
6. **The mod's menu (F11).** Press **F11** on the keyboard (only F11 opens it). In the headset it is
   on a panel of its own, put in front of you each time it opens: point a controller at it and pull the trigger to click,
   push that controller's stick up or down to scroll. On the monitor, use the mouse. The game gets no input while it is open.
   It has three tabs:
   - **Main**: this menu's own size and distance in the headset, and Save settings (and a line saying the main VR settings
     are in the game's Settings > VR tab).
   - **Advanced**: everything else, in groups by feature (body details, hands and gravity gloves, melee, people, aiming,
     climbing, the HUD, eye-mode options, DLSS, drawing for performance, menus and conversations, controllers). At its top,
     "Show the main VR settings here too" (off by default) shows the VR tab's settings in this menu as well.
   - **Debug**: tests, diagnostics and troubleshooting. Leave these as they are unless you are testing something.
7. **Eye modes and their cost.** Choose one in Settings > VR > **Eye mode**. In game, hold the **left grip** and click the
   **right thumbstick** to switch between stereo and mono: a short buzz means mono, a long buzz back to stereo. With
   F11 > Advanced > "Include AER in the eye-mode button" on, each click moves on stereo > mono > AER > stereo instead (a
   short buzz means mono, a long buzz AER, a medium buzz stereo). AER chosen in the menu goes to stereo at the next click.

   | Mode | What you see | Cost |
   | - | - | - |
   | **Full stereo** (default) | Both eyes drawn every frame: real depth | The heaviest: the world is drawn twice each frame |
   | **Mono** | One picture for both eyes, a new one every frame: no depth | The lightest |
   | **AER** | One eye drawn each frame, the other eye's last picture shown with it | One view a frame, like mono, so it should run close to mono's frame rate (not measured yet). Each eye gets a new picture every other frame |

   - The eye mode "Stereo, eyes take turns" is cheaper than full stereo, but each eye gets a new picture only every other
     frame.
   - Menus are shown in mono by default (F11 > Advanced > "Mono while a game menu is shown"), and the mode goes back when
     the menu closes.
   - When video memory is full, a switch to a heavier mode (from mono to stereo or AER, or to full stereo) is refused and you
     feel **two short buzzes**. Try again a few seconds later. A switch to a lighter mode is always made.
   - Mono and AER need OpenXR.
   - The mod also lowers some costs by default:
     - foveated shading (the lens edges shaded coarsely);
     - the right eye reusing the left eye's sun shadows;
     - cheaper clouds and grass shadows. These are changed in memory only, never in your INI files.
   - **Sharpness** (Settings > VR; the OpenXR resolution scale) is 1.0 by default: the size your runtime asks for. A change
     is applied when you close the menu (changed at the main menu, you see it once a save is loaded), so the headset's images
     are remade once, not at each step. Higher is sharper but costs frame rate and video memory; lower gives more fps but a
     blurrier picture (1.0 has about 1.56 times the pixels of 0.8, the default before this version; a config that still had 0.8
     starts at 1.0 once, and 0.8 chosen again after that stays).

---

## Controls

The full guide is in **[CONTROLS.md](CONTROLS.md)**. The basics (default layout):

| Input | What it does |
| - | - |
| Left thumbstick | Move. Click: sprint |
| Right thumbstick | Turn (snap 45 degrees by default). Click: melee |
| Right trigger | Fire |
| Right grip | Aim (weapon out). Grab (weapon put away) |
| Left grip | Grab. Hold it for combos |
| Right **A** | Use / activate / talk. In menus: select |
| Right **B** | Sneak. In menus: back |
| Left **X** | Reload. Hold: put weapons away or draw them. In menus: drop, transfer |
| Left **Y** | Jump. In the air: boost pack. In menus: favorite |
| Left thumbrest (touch) | Hand scanner on or off |
| Right thumbrest (rest your thumb) | Grenade button. Lift your thumb to throw |
| Left menu button | Data menu. With the left grip held: switch first / third person |
| Palm pushed out, grip held | Your power |
| Right grip behind right shoulder | Take a grenade, throw it with your arm |
| Trigger at left / right temple | Flashlight / hand scanner |
| Left grip, then left trigger | Favorites wheel |
| Left grip + right stick click | Eye mode: stereo or mono (AER too, if included in F11 > Advanced) |
| Left grip + left stick click, or F12 | Flat screen on or off |
| F11 (keyboard) | The mod's menu; in the headset its close button closes it too |

The game actions above are Starfield's default gamepad actions for the buttons the mod sends. If you changed the game's
controller bindings, they change here too.

The face buttons send the game the button printed on them, so the game's button prompts match your controllers. The
layout of earlier versions (right B = jump, left X = sneak and back, left Y = reload) is Settings > VR > "Face buttons as
printed", turned off.

---

## Settings

Everything is saved in the **`sfvr`** folder next to `Starfield.exe` (with MO2 and Root Builder: `overwrite\Root\sfvr`):

| File | What it is |
| - | - |
| `config.txt` | All the mod's settings (plain text). Delete it to go back to the defaults. |
| `sfvr_log.txt` | This session's log. |
| `sfvr_log_previous.txt` | The previous session's log. |
| `imgui.ini` | The F11 window's layout. |
| `stereo_*_left.jpg` / `_right.jpg` | Stereo screenshots (F11 > Debug > Stereo (diagnostics) > Save Stereo Screenshot). |

Settings are saved when the F11 menu opens or closes, when you change a setting on the VR tab, and when the game exits.
There is also a "Save settings" button (F11 > Main). The settings on the game's Settings > VR tab are the same ones.

Some defaults worth knowing:

| Setting | Default | Where |
| - | - | - |
| Turning (right stick) | Snap, 45 degrees (smooth speed 1.0) | Settings > VR |
| Level view | On | F11 > Advanced > View on foot |
| Body and motion controls | On | Settings > VR |
| Seat: view at the body's eyes | On | Settings > VR (also F11 > Advanced > Body) |
| Real crouch | Off | F11 > Advanced > Body |
| Face buttons as printed | On (off: the old layout) | Settings > VR |
| Alternative joy layout | Off | F11 > Advanced > Controller layout and vibration |
| Left-handed | Off | Settings > VR |
| Eye mode | Full stereo | Settings > VR |
| Include AER in the eye-mode button | Off | F11 > Advanced > Eye modes and recentring |
| Mono while a game menu is shown | On | F11 > Advanced > Eye modes and recentring |
| Sharpness (resolution scale) | 1.0 | Settings > VR |
| Menu size / distance in the headset | 1.1 m tall / 1.75 m | F11 > Main |
| Favorites wheel | On | F11 > Advanced > Aiming, powers and grenades |
| Climbing | On (pull 1.0x) | Settings > VR (also F11 > Advanced > Climbing, ladders and workbenches) |
| Gravity gloves | On (reach 5 m) | F11 > Advanced > Hands |
| Impact melee | On (2.2 m/s, instant hits) | F11 > Advanced > Melee and throwing |
| Powers by pushing your palm | On (1.5 m/s) | F11 > Advanced > Aiming, powers and grenades |
| Grenades from your shoulder | On | F11 > Advanced > Aiming, powers and grenades |
| Gun sight, with its laser | On | F11 > Advanced > Aiming, powers and grenades |
| The grips as LB and RB in the scanner (also the star map, build mode and character creation) | On | F11 > Advanced > Conversations, the scanner and menus |
| Recenter after loading screens | On | Settings > VR |
| HUD size | 40% | Settings > VR |

---

## Compatibility

- The game loads the mod as **`dxgi.dll`**. Only one file can have that name, so another mod that also needs its own
  `dxgi.dll` in the game folder cannot be installed at the same time. ReShade is often installed that way. Running ReShade
  under another file name alongside this mod has not been tested yet.
- The mod needs no SFSE, no Address Library and no plugin (`.esm` / `.esp`). It should work alongside SFSE, its plugins and
  normal data mods, but that has not been tested yet.
- Each feature checks the game's code before it uses it. If the game is another version, or another mod changed the same
  code, that feature stays off and the log says why. Some of these log lines name 1.16.236 even in the 1.16.244 build.
- If Steam updates Starfield past 1.16.244, the mod's features switch off until there is a new build. To stay on 1.16.244,
  set Steam's update option for Starfield to "Only update this game when I launch it", or play from a copy of the game folder.

### VectorXR

[VectorXR](https://github.com/DienerTech/vectorxr) is an OpenXR layer (a separate program) whose Frame Generation makes
extra headset frames from the game's. It has had a first test with this mod (Quest 3, Virtual Desktop). If you use it:

- Use VectorXR **0.20.2 or newer** (it fixed a crash when a game restarts VR on Virtual Desktop, which this mod can do).
- Use Frame Generation only with the eye mode **mono** or **full stereo**, never **AER**: it makes new frames from the
  first picture of both eyes, and AER's frames carry one new eye and one older one. The mod warns in its log, and on the
  F11 Main tab, when it sees Frame Generation running with AER.
- Follow VectorXR's own setup: **Virtual Desktop's SSW set to Disabled** (not Automatic), the headset at its full refresh
  rate, the game reaching at least half the refresh rate on its own first, and a restart of the game after turning Frame
  Generation on. With SSW on, Frame Generation keeps switching itself on and off; if it still does, try VectorXR's frame
  rate **Auto** instead of Dynamic.
- While Frame Generation may run, menus over the world as it was (the opaque ones) show the soft light or darkness behind
  their panel instead of the world, and see-through menus keep the live world behind them: the mod keeps the frame laid out
  as in play, because Frame Generation hung the graphics card with the layout used behind menus before. With Frame
  Generation off for Starfield in VectorXR, menus look as without VectorXR.
- Leave VectorXR's **Pivot**, **Depth** and **Turbo** off for Starfield: the mod does its own head tracking, eye placement
  and frame pacing. VectorXR's on-screen display is fine.
- The mod's log lists the OpenXR layers it sees at start ("OpenXR API layers: ..."), and, with VectorXR's layer present,
  Frame Generation's state as it changes ("VectorXR: ...").

---

## Known limitations

- **The 1.16.244 build has had little testing.**
  - It is the same code as the 1.16.236 build. Every place the mod uses in the game is looked up in a table made from both
    versions' `Starfield.exe`.
  - In a short test on 1.16.244 every feature started. Most play-testing was done on 1.16.236.
  - Please report anything that behaves differently.
- **Full stereo is heavy**: it draws the world twice per frame. Use mono or AER if your frame rate is too low.
- **Frame Generation**: the game's own (DLSS Frame Generation) does not work with the mod. VectorXR's, an OpenXR layer,
  has had a first test: see [VectorXR](#vectorxr) below.
- **SteamVR's own API (OpenVR)** is not part of this release. Use an OpenXR runtime (SteamVR has one too). Mono, AER, full
  stereo, the menu pointer and the favorites wheel need OpenXR.
- The **favorites wheel** is off with "Left-handed" (Swap Controller Inputs) or the alternative joy layout. The game's own favorites
  menu is used instead.
- **Left-handed play** (Settings > VR > "Left-handed") and **controllers other than Meta Touch** have not been tested yet.
- **Build mode** is only partly tested:
  - full stereo stands down while you build (the eyes take turns);
  - outdoor outpost building and some build menus (such as the fabricator) have not been tried.
- The **star map** (and a planet's view) shows its own 3D scene, not the live world behind it.
- **Climbing on moving lifts** does not move with the lift.
- The **gravity gloves** do not pull bodies.
- The **F11 menu** is used with a controller's pointer only on its own panel in the headset (OpenXR, an 8-bit game picture).
  Otherwise (OpenVR, or HDR on) it is drawn into the game's picture and needs the mouse.
- In busy cities the "people near your hands" checks cost a little frame time.
- The laser sight's dot lands on surfaces only with DLSS.

---

## Uninstall

1. Installed with the installer: uninstall Starfield2VR in Windows Settings > Apps (it puts back what it moved aside). By hand: delete `dxgi.dll` and `openxr_loader.dll` from the game folder (or disable the mod in MO2).
2. Delete the `sfvr` folder next to `Starfield.exe` (with MO2: `overwrite\Root\sfvr`).
3. If you installed ViGEmBus for an earlier version of the mod, you can uninstall it (Windows Settings > Apps) if nothing else uses it.

Possible leftovers (not tested yet):
- **Field of view.** While it runs, the mod sets the game's field of view and aspect limits to the headset's. It does not
  write them to your INI files, but the game might save them when you change a setting. If your field of view looks
  wrong after uninstalling, set it again in Settings > Display.
- **Weapon and grenade copies.** The left-hand weapon and a grenade in your hand are shown as copies in the world. The mod
  removes them when a save brings one back. Without the mod, a save made while one was shown might have a loose copy
  lying in the world. Put the left weapon away and save again before you uninstall.

---

## Reporting bugs

Open an [issue](https://github.com/Kingmemarley/Starfield2VR/issues) and attach **`sfvr\sfvr_log.txt`**. If you
restarted the game after the problem, attach `sfvr_log_previous.txt` too. Please say:

- what you did and what you saw: what looked wrong, or what did not work;
- roughly when it happened, so it can be found in the log;
- your headset and runtime (for example Quest 3 + Virtual Desktop), and the eye mode (stereo, mono or AER);
- your game version, and whether you use MO2 or Vortex.

The log starts with the game version the mod found and which features started.

---

## Building from source

You need Visual Studio 2022 or newer with "Desktop development with C++", Git, and internet access for the first
configure. From an **x64 Native Tools Command Prompt**:

```
:: Starfield 1.16.244 (the release build)
cmake --preset build-release-no-debug-msvc-1.16.244
cmake --build --preset release-no-debug-msvc-1.16.244

:: Starfield 1.16.236
cmake --preset build-release-no-debug-msvc
cmake --build --preset release-no-debug-msvc
```

- The DLL is `build/<preset folder>/dxgi.dll`. `openxr_loader.dll` is in `shipping/`.
- Details: [BUILDING.md](BUILDING.md).
- How the 1.16.244 port table is made from the two versions' `Starfield.exe`, and how to port to another version:
  [docs/PORTING.md](docs/PORTING.md).

---

## Credits

- **Mutar**, [starfield2vr](https://github.com/mutars/starfield2vr) (MIT): the VR mod this project is built on. The
  1.16.244 offsets of its core come from pull request #9 on that repository.
- **AetherianArtificer**, [starfield2vr fork](https://github.com/AetherianArtificer/starfield2vr) (MIT): the visible body and
  its IK (`src/CreationEngine/body`), the tracking-space mapping (`src/CreationEngine/vr/TrackingSpace`) and the muzzle
  code (`src/CreationEngine/MuzzleFire`) were ported from it.
- **Homard**, StarfieldVR3 (another starfield2vr fork; its source used with permission): the model for the face buttons as
  printed, the F11 menu as a panel in the room, the body's place behind the eyes, the weapon steadiness work (the grip lock, the
  virtual stock) and the mining cutter's beam along the barrel, and a check of the virtual controller against its own.
- **praydog**, [REFramework](https://github.com/praydog/REFramework) (MIT): the VR layer in `extern/vrcore` (the D3D12
  hooks, the OpenXR runtime code, eye submission) is based on it.
- **Nefarius**, the [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver, the fallback way to give the game a controller.
- Several features are modelled on Skyrim VR and Fallout 4 VR mods, as the code's comments note:
  - HIGGS and PLANCK: grabbing, the gravity gloves, physical people;
  - ROCK: hands in Fallout 4 VR;
  - Spell Wheel VR: the favorites wheel;
  - Weapon Throw VR: melee throws;
  - VR Climbing: climbing;
  - Resident Evil 4 VR's laser sight.

Libraries built into `dxgi.dll`, and the OpenXR loader shipped beside it. The full texts are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

| Library | License |
| - | - |
| [bddisasm](https://github.com/bitdefender/bddisasm) | Apache 2.0 |
| [DirectXTK12](https://github.com/microsoft/DirectXTK12) | MIT |
| [GLM](https://github.com/g-truc/glm) | Happy Bunny or MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) (with parts of stb) | MIT (stb: MIT or public domain) |
| [kananlib](https://github.com/cursey/kananlib) | Boost Software License 1.0 |
| [OpenVR](https://github.com/ValveSoftware/openvr) (headers and import library) | BSD 3-Clause |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) and its loader, `openxr_loader.dll` (with JsonCpp) | Apache 2.0 (JsonCpp: MIT or public domain) |
| [safetyhook](https://github.com/cursey/safetyhook) | Boost Software License 1.0 |
| [spdlog](https://github.com/gabime/spdlog) (with fmt) | MIT |
| [ViGEmClient](https://github.com/nefarius/ViGEmClient) | MIT |
| [Zydis](https://github.com/zyantific/zydis) (with Zycore) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) (`extern/vrcore/third_party`) | MIT |

---

## License

MIT. See [LICENSE](LICENSE), [extern/vrcore/LICENSE](extern/vrcore/LICENSE),
[extern/vrcore/LICENSE.REFramework](extern/vrcore/LICENSE.REFramework) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
The game-structure headers in `sdk-lite/` very likely derive from CommonLibSF and are under GPL-3.0-or-later with CommonLibSF's
modding and linking exceptions (see [sdk-lite/LICENSE.md](sdk-lite/LICENSE.md)). The compiled mod includes them, and its full
source is in this repository.

Starfield is a trademark of Bethesda Softworks / ZeniMax Media. This is a fan-made mod, not affiliated with or endorsed by
them.
