#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

// Hand actions with the grips, alongside the hand grab:
// - Powers: hold a grip in a hand with nothing in it (the gun hand does not count), turn the palm out, away from you (the
//   wrist bent back, as in a Force push), and push it out briskly: your equipped power is used (the game's LB + RB), aimed
//   where the palm faces. Reaching, pointing the gravity gloves and punching keep the palm sideways or down: never a push.
// - Grenades: reach behind your right shoulder with your right hand and squeeze the grip: you take a grenade (a buzz; one
//   must be equipped). Bring the hand forward and the game readies it (the grenade button held, RB); let go of the grip as
//   you throw and it is thrown, along your hand's throw. Let go before your hand leaves the shoulder and it goes back.
// The shoulder works as in HIGGS (Skyrim VR): a zone behind the shoulder, relative to the headset, taken with a slow hand.
// - (fix173) In the hand scanner and the star map the grips are LB (left) and RB (right): a squeeze taps that button (ModConfig) and the grip
//   is that button's until let go (no grab, throw, power or grenade with it). No power push while the scanner is open. (fix175) Build mode
//   likewise (no power push or grenade there at all).
namespace HandActions
{
    // Once per engine frame (main thread, tracking::Snapshot), after the poses are stored and before melee and the hand grab
    // (a grip taken here is then neither a grab nor a melee swing).
    void Update();
    // A power was just pushed out: the aim (stage space, as tracking's aim rotation) follows that push for a moment.
    bool AimOverride(glm::mat4& rotation);
    // That moment (any thread): a projectile the player launches now is the power's, not the gun's (MuzzleFire leaves it
    // the game's way, along the aim).
    bool PowerAimActive();

    // For the controller mapping (any thread):
    // the right grip has a grenade (or is about to take one): it does not aim (left trigger);
    bool RightGripTaken();
    // a grenade taken from behind the shoulder is in your right hand now (taken or readied, not yet thrown or put back);
    bool GrenadeInHand();
    // hold the grenade button (RB);
    bool PressGrenade();
    // the grenade button is this one's for now (from taking a grenade until the game has thrown it): nothing else presses it
    // (the right thumbrest would ready or hold back the grenade, or ready another);
    bool OwnsGrenadeButton();
    // press the power buttons (LB + RB together).
    bool PressPower();
    // (fix173) the grips are LB and RB now (the hand scanner drawn this moment with no menu over it and no LB sent into it yet, or the star map
    // shown; setting ScannerGrips, not in the alternative layout): a fresh squeeze taps its button (ModConfig);
    // (fix175: and build mode, with no menu over it)
    bool GripsAreShoulders();
    // (fix175) where that is, for the log: "star map", "build mode" or "hand scanner";
    const char* GripsPlace();
    // (fix175) the left grip's LB waits a moment there (the star map, build mode: not the scanner), for a combo joining it (ModConfig);
    bool LeftGripWaits();
    // that grip was squeezed there and is still held: it is its button's, not the hand grab's, the throw's, a power's or the aim's (LT).
    bool GripIsShoulder(bool left);
    // (fix173, ModConfig) an LB press went to the game (closing the scanner, if it is open); a grip's tap was made.
    void NoteScannerLb();
    void NoteGripTap(bool left);

    // From the projectile launch hook (MuzzleFire), for each projectile the player launches: a grenade thrown from the
    // shoulder is waiting for the game to throw it.
    bool ThrowPending();
    // That projectile was just launched (`before`: the launch data's first kLaunchBytes before the game filled it in): if it
    // is the grenade, it goes along the hand's throw.
    constexpr std::size_t kLaunchBytes = 0x100;
    void OnPlayerLaunch(std::uint8_t* launch_data, const std::uint8_t* before);
    // Launch data (its first kLaunchBytes) for a grenade or mine.
    bool IsThrownLaunch(const std::uint8_t* bytes);

    // One line for the settings panel.
    std::string Status();
}
