#pragma once

#include <cstdint>
#include <string>

// (fix193) The VR controllers as an Xbox pad, with no driver.
//
// Starfield reads an Xbox pad only through Windows' gamepad API (Windows.Gaming.Input; never XInput). Its input handler asks
// Windows once for the list of connected pads (the "Gamepad statics": Gamepads, GamepadAdded, GamepadRemoved), keeps its own copy
// of that list, makes its own gamepad device for the first pad in it, and then, every frame, asks that pad for its current state
// (GetCurrentReading) and sends the game's rumble to it (put_Vibration). Before this, the mod plugged in a virtual Xbox pad
// through the ViGEmBus driver so that Windows would list it.
//
// Now the mod watches the call the game asks Windows with (combase's RoGetActivationFactory). When the game asks for the Gamepad
// statics, it gets a stand-in that forwards the plug-in and plug-out events to Windows' own object, but answers the list of pads
// itself: (fix195) the virtual pad alone, since the game makes no gamepad device at all when two or more pads are listed. The game
// then treats the virtual pad as an Xbox pad that was connected when it started (its own device, Xbox button prompts, its own
// switch to gamepad mode). Each time the game reads the virtual pad, the VR controllers are read (the same path the ViGEm pad was
// fed by: CreationEngineInputManager::ReadVrPad); the game's rumble goes to the controllers. Only the game's own calls get the
// stand-in (fix195: also when one other module's hook sits in front of the watch, the game's code then being a few stack frames
// below that module's; if the game then never reads the pad, the mod falls back to the ViGEm pad, see Abandon); any other module
// asking Windows gets Windows' list unchanged.
//
// What follows from it: a real Xbox pad (or another program's virtual pad), if one is connected, is hidden from the game while the
// mod's pad is given: the VR controllers are the pad; a DualSense the game finds through Sony's library still comes first, as it did
// with the ViGEm pad; and the game gets the VR input even while its window is in the background.
//
// Not done (possible later): the pad answers only the interfaces the game asks for (IGamepad; the game's code holds no other
// gamepad interface id), not IGameController or IGamepad2, which a module wrapping Windows' gamepad list in between might ask for;
// and the VR controllers are read only when the game reads the pad, not on every run of the game's gamepad poll.
namespace VirtualGamepad
{
    // Puts the watch on Windows' call. Run once when the dll starts, before the game sets up its input (the log is not open yet:
    // nothing is logged here; Describe says what happened). True when the watch is in place.
    bool InstallEarly();

    // The watch is in place.
    bool HookInstalled();

    // The game has been given the stand-in list (the virtual pad is the game's pad from now on).
    bool Wrapped();

    // The mod chose the ViGEmBus pad instead: from now on the game's requests are passed on unchanged. False when that is too
    // late (the game already has the stand-in, so the virtual pad is in use and no second pad must be plugged in).
    bool GiveUp();

    // The mod is ready to read the VR controllers (its VR part is set up and the log is open): until then the virtual pad reads
    // as an idle pad.
    void Activate();

    // (fix195) Real pads in Windows' list at the game's last look, all hidden from the game (-1: the game has not looked yet).
    int HiddenPads();

    // (fix195) The game's reads of the virtual pad since the game started.
    std::uint64_t Reads();

    // (fix195) The game's request reached the watch through another module's hook in front of it (the stand-in was still given),
    // and no request came from the game's code directly.
    bool ThroughHook();

    // (fix195) Windows took every GamepadAdded/Removed registration (the game then hears of a pad plugged in, and reads its list
    // again; without that, giving the virtual pad up would leave the game with no pad it knows of).
    bool EventsWork();

    // (fix195) Gives the virtual pad up for the ViGEm pad, only after it was given (Wrapped): from then on it reads as an idle pad
    // (the VR controllers are not read for it, its rumble is not passed on) and the game's list is Windows' own, so the two pads
    // never both feed the game. False when it was not given. Resume undoes it (the ViGEm pad could not be plugged in).
    bool Abandon();
    void Resume();

    // What the watch did so far, for the log.
    std::string Describe();
}
