#pragma once

#include <cstdint>
#include <string>

// (fix171) A favorites wheel as Spell Wheel VR's (Shizof's Skyrim VR mod) in place of Starfield's own favorites menu (a
// Spell Wheel VR-style wheel, time slowed while it is open as the game's own favorites do).
//
// Hold the left grip, then squeeze the left trigger with the left stick in the middle (the gesture that opened the game's favorites with the
// stick before): the wheel opens where your left hand is (at least 0.35 m from your eyes), facing you, leaned back a little, and stays where
// it is in the room while it is open. Around its middle are your 12 favorites in the game's own slots (the game's cross bent into a ring: the
// left arm's three on the left, the up arm's on top, the right arm's on the right, the down arm's below), each with its name, its count or a
// gun's ammo, and marks for what each hand holds and the power in use. Each hand picks by where it is over the wheel (12 wide slices around
// the middle; the middle circle picks nothing): the slice under it grows, its name and count show large in the middle, and that hand feels a
// short buzz at each new one. Let go of the left trigger to choose: the slice under the left hand goes to the left hand (melee weapons only,
// as the left trigger gives the left hand: LeftWeapon), the one under the right hand to the right hand (the game's own use of a favorite,
// UseQuickkey: a weapon drawn, a medpack used, a power made the one in use); both at once with both hands on slices; neither: closed, nothing
// chosen. The right trigger on a slice uses it at once and the wheel stays open (several medpacks in a row). While it is open no trigger,
// grip, shoulder button or D-pad reaches the game, and time runs as slowly as in the game's own favorites (fFavoritesTimeMult, 0.23), always
// put back as it closes, as a menu or a conversation opens, a loading screen comes, in the pilot's seat, with the setting turned off or the
// game's window behind another.
//
// The game's own favorites menu no longer opens from this gesture in play (the mod makes every D-pad press: none is sent in play while the
// wheel is on); the inventory's own "favorite" still opens it to assign a slot. Off (the setting FavoritesWheel), or anything it needs not as
// in Starfield 1.16.236 (logged once), the game's own favorites as before.
//
// Drawn by the mod on a quad of its own (the headset's layer, as the conversation's panel): its picture made on the processor on a thread of
// its own (WheelPicture) only when what it shows changes, copied into its own images by the graphics side; nothing drawn while it is closed.
namespace FavoritesWheel
{
    // Finds and checks the game's favorites (the slots, their use, the time slow; the mod's start).
    void Install();

    // (The game's input, main thread: each gamepad poll, ModConfig.) The buttons as the player's hands hold them (left: the left
    // controller's, as ModConfig takes it), the left stick. Returns what the wheel takes from the game this poll.
    struct Input
    {
        bool  left_grip{ false };
        bool  left_trigger{ false };
        bool  right_grip{ false };
        bool  right_trigger{ false };
        float stick_x{ 0.0f };
        float stick_y{ 0.0f };
        bool  left_thumbrest{ false };
        bool  right_thumbrest{ false };
    };
    struct Taken
    {
        bool open{ false };          // the wheel is open (or opening this poll): no D-pad, grips, shoulder buttons for the game
        bool left_trigger{ false };  // the left trigger is the wheel's (from its opening pull until it is let go)
        bool right_trigger{ false }; // the right trigger is the wheel's (while open; one held as it closes, until let go)
        bool left_shoulder{ false };  // LB held back (the left thumbrest touched while open, until the thumb leaves it)
        bool right_shoulder{ false }; // RB likewise (the right thumbrest)
        bool dpad{ false };          // the D-pad not sent in play (the game's favorites menu does not open from the gesture any more)
    };
    Taken Poll(const Input& in);

    // (The game's thread, each frame: tracking::Snapshot, before the hand grab, melee and the left weapon.) Opens, places, reads the slots,
    // follows the hands, chooses, slows and puts back time, closes.
    void Update();
    // The wheel is open now (any thread).
    bool IsOpen();
    // (fix175) Open now, or closed within the last `ms` milliseconds (any thread): the palm powers wait (a grip still held as it closes).
    bool OpenOrClosedWithin(int64_t ms);
    // One line for the settings panel.
    std::string Status();
}
