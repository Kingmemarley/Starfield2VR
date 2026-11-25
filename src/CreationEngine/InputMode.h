#pragma once

#include <cstdint>
#include <string>

// Starfield's input mode: it runs either with the mouse and keyboard or with a gamepad, and switches the whole input system
// between them. With a gamepad it hides its menu cursor and does not move it with the mouse; moving a gamepad stick (or a
// button) switches to the gamepad, and only a keyboard key or a mouse button switches back (SimultaneousInput, an open-source
// Starfield plugin, patches the same switch: github.com/Platin117/Starfield-SimultaneousInput). The VR controllers reach the game
// as a gamepad (a virtual Xbox pad), so the game is usually in gamepad mode, and the menu pointer has to switch it.
//
// The mode is a byte of the game's input device manager (BSInputDeviceManager, whose pointer the game keeps in a global; 1.16.236:
// rva 5fe1a50): +0x60, more than 1 with the mouse and keyboard (0xFF), 0 or 1 (a gamepad's number) with a gamepad. The game's own
// code tests it so (rva 189f914: the cursor is kept in the window only when it is more than 1; the window procedure, 1883450, the
// same). Found by that code (a pattern), checked against the fixed place and by the object's type (RTTI).
namespace InputMode
{
    // Looks for the manager (a scan of the game's code: called on a thread of its own, CreationEngineEntry; again every few
    // seconds while the game has not made it yet). True when found and checked; until then the mode is not known.
    bool Find();
    // The mode byte now: -1 not known, else its value.
    int Raw();
    // The game is in mouse-and-keyboard mode (false when not known).
    bool IsMouse();
    // The game is in gamepad mode (false when not known).
    bool IsGamepad();
    // Puts the game in mouse-and-keyboard mode by writing the byte (what SimultaneousInput does for looking around); the menu
    // pointer first presses a key instead, the way the game expects (MenuPointer).
    bool ForceMouse();
    // What was found, for the log.
    std::string Describe();
}
