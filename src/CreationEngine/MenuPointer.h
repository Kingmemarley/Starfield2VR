#pragma once

#include <string>

// The controllers as the mouse on a menu's panel (vrcore: game menus on a panel that stays in the room, as in Skyrim VR). Where
// the pointing hand's beam meets the panel is a place on the game's picture, so in the menu (the menu's own pixels: the area it
// draws in, the game's centred 16:9 box).
//
// Starfield shows and moves its menu cursor only in its mouse-and-keyboard mode (InputMode); the controllers are a gamepad to it,
// and a gamepad's stick or button switches it to gamepad mode, where the cursor is hidden. So while you point at a menu, the
// pointer switches it back the way the game expects, with a keyboard press (F13, a key nothing uses; the mode byte is written only
// if three presses did not do it), and keeps the sticks, triggers and shoulder buttons (which a resting thumb or finger can press)
// from reaching the game; a stick pushed up or down scrolls instead (the mouse wheel). The face buttons still reach it (B goes
// back); the mouse mode comes back once they are let go.
//
// In mouse mode the game puts its cursor where its mouse-move messages say the mouse is (fix104's test: it went exactly where
// Windows' cursor was kept). So the place in those messages is made the dot's (vrcore's window hook: mouse_override; it may be
// below the screen's edge, where Windows' own cursor cannot go, as the game's window is taller than the screen), and a move of one
// count makes the game take it. The cursor is read back (UiProbe: the menu movie's own mouse state); in a menu where it does not
// go where it is put, it is steered as a mouse moves instead (relative moves, read back, corrected). The pointing hand's trigger
// presses the left button once the cursor is under the dot (or after a moment), and lets it go with the trigger; that trigger is
// then not the gamepad's. Nothing is sent unless the game's window is in front and a panel is showing that very moment. The log
// (prefix [MenuPointer]) tells where the cursor is against the aim and how placing went.
struct _XINPUT_GAMEPAD;

namespace MenuPointer
{
    // Each gamepad poll (VR::on_xinput_get_state). The triggers pulled (left, right); `allowed`: the mod's own menu is closed.
    // Returns the triggers that are the pointer's now (bit 0 left, bit 1 right): they must not reach the game as its triggers.
    int Update(bool left_trigger, bool right_trigger, bool allowed);
    // The same poll, once the gamepad's report is made (its end): while the pointer is in use, takes out of it what would switch
    // the game to gamepad mode (the sticks, the triggers, the shoulder buttons) and scrolls with a stick's up and down (the
    // controllers' sticks, -1..1).
    void FilterGamepad(_XINPUT_GAMEPAD* pad, float left_y, float right_y);
    // (fix147, fix169) After FilterGamepad: in the menus that move sideways only on the D-pad or LB and RB (missions, ship crew, skills,
    // research, the almanac, the ship menu; trading and containers with setting TradeStickTabs) the left stick's left and right make those
    // moves (per menu: GameFlow::stickSidewaysMenu), and are not the stick's left and right for the game while they do.
    void StickSideways(_XINPUT_GAMEPAD* pad, float left_x);
    // (fix175, after StickSideways) The star map (drawn this moment): the right stick's up and down zoom whether you point or not: pointing
    // at it, the mouse wheel (FilterGamepad); otherwise its push sent as RT (up, in) or LT (down, out), as far as it is pushed. Logged with
    // the counts as the star map closes.
    // (fix203) Character creation too, while you do not point (pointing, the stick's up and down are its lists' wheel): RT (up) brings the
    // character nearer, LT (down) farther, as its own trigger zoom does.
    void StarMapZoom(_XINPUT_GAMEPAD* pad);
    // Each frame (the mod's frame): lets go of the button (and gives the cursor back) when the gamepad polls have stopped.
    void Watchdog();
    // (fix173, before FilterGamepad) A gamepad button is added to this poll's report after the filter (the grips' LB and RB taps in the star
    // map, fix175: build mode): no mode key in this poll, nor until it has been let go a while, as for the face buttons.
    void HoldModeKey();
    // (fix154) The last button press the pointer sent a menu: the hand whose trigger made it (0 left, 1 right) and when (steady clock
    // milliseconds, its own stamp); false: none yet (any thread).
    bool LastPress(int& hand, int64_t& at_ms);
    // One line for the settings panel.
    std::string Status();
}
