#pragma once

#include <cstdint>
#include <string>

// (fix171) The favorites wheel's picture (FavoritesWheel): made on the processor on a thread of its own, only when what it shows changes,
// and handed to the headset's side (OpenXR::set_wheel_picture), which copies it into the wheel's own images. Nothing is drawn on the
// graphics card (the gun sight's picture is made the same way): a dark disc, 12 slices around a middle circle (each favorite's name, its
// count or ammo, a small drawn symbol for its kind, marks for what each hand holds and the power in use), the slice under each hand lit in
// that hand's colour and drawn out further, and the hovered one's name large in the middle. Text with Windows' own font drawing (GDI, Segoe
// UI, smoothed), into the picture with premultiplied colour in sRGB bytes.
namespace WheelPicture
{
    // The picture: kPixels square, kSizeM wide in the room. The middle circle (picks nothing) and the slices' outer edge, in metres from the
    // middle, for the hands' choosing (FavoritesWheel) and the drawing alike.
    constexpr int   kPixels  = 1024;
    constexpr float kSizeM   = 0.37f;
    constexpr float kInnerM  = 0.045f;
    constexpr float kOuterM  = 0.17f;
    // The slot shown in each slice: slice k is centred at 30k degrees (0 to the right, counterclockwise as seen). The game's cross bent into a
    // ring: its left arm (slots 0, 1, 2) on the left, its up arm (6, 7, 8) on top, its right arm (3, 4, 5) on the right, its down arm (9, 10,
    // 11) below, each arm's three in slot order clockwise.
    constexpr int kSliceSlot[12] = { 4, 3, 8, 7, 6, 2, 1, 0, 11, 10, 9, 5 };

    enum Kind : int
    {
        kEmpty = 0,
        kMelee,
        kGun,
        kThrown,  // grenades, mines (a weapon with no ammo of its own)
        kAid,     // ALCH: medpacks, food, drinks, chems
        kApparel, // ARMO
        kPower,   // SPEL
        kOther,
    };

    struct Card
    {
        int         kind{ kEmpty };
        std::string name{};   // (UTF-8, as the game has it)
        std::string detail{}; // its count, a gun's ammo
        std::string more{};   // shown in the middle only, under the detail (a gun's ammo's name)
        bool        right{ false };   // your right hand holds it (the game's equipped weapon)
        bool        left{ false };    // the left hand's weapon (LeftWeapon)
        bool        in_use{ false };  // the power in use
        bool        worn{ false };    // apparel worn
        bool        left_ok{ false }; // the left hand can take it (a melee weapon)
        bool        none{ false };    // nothing of it carried now

        bool operator==(const Card&) const = default;
    };

    struct Content
    {
        Card     cards[12]{};
        int      hover[2]{ -1, -1 }; // the slot under each hand (0 left, 1 right; -1 none)
        int      focus{ -1 };        // the slot shown large in the middle (the last one a hand came to)
        int      focus_hand{ -1 };
        bool     reading{ false };   // the slots not read yet (shown as such)
        uint32_t open{ 0 };          // which opening it is drawn for

        bool operator==(const Content&) const = default;
    };

    // Asks for a picture of this (any thread; the newest asked for is drawn, older ones skipped). The thread is started the first time.
    void Draw(const Content& content);
    // For the log: how many pictures were drawn, and the last one's time (ms).
    std::string Status();
}
