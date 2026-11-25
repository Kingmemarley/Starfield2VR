#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <glm/glm.hpp>

// Throwing your melee weapon, as Weapon Throw VR's "Thor's Hammer" mode does in Skyrim VR: hold a button, swing and let go, the
// weapon flies and hits for its own damage, and comes back when you call it (or by itself after a while); it never leaves your
// inventory.
// - With a melee weapon out (the shown body holds it), squeeze the grip, swing and let go of the grip during the swing (the hand
//   moving away from you, any way, sideways as a frisbee too, at least at the throw speed: the swing speed setting, at least 2.5
//   m/s; its fastest move away from you in the last moments, not the wind-up: fix141). Squeezing the grip still blocks.
// - (fix141) While the grip is squeezed, the living person (friend or foe, never an object) nearest your hand's pointing line, within
//   15 degrees, 25 m and your view, is locked on with the gravity gloves' buzz (kept through the wind-up); the throw then flies straight at them and
//   homes on them (no falling meanwhile), for up to 2 s.
// - (fix143, as Jedi Academy's saber throw) No one locked on: the person nearest where your hand points as you let
//   go (within 20 degrees), else nearest the throw's direction, is flown at and homed on; else it goes straight where it was thrown
//   (a quarter toward where you look when that is within 35 degrees).
// - The weapon stays equipped and drawn. The shown body's hand opens and its weapon (the body's weapon bone) flies from where the
//   hand held it, at 20 m/s, spinning flat (the blade laid level, turning about the vertical), never falling. (fix144) While
//   you hold the right trigger after the throw, your throwing hand guides it: it turns fast toward the point your hand points at, at
//   its distance, and stays out up to 30 m or 6 s; let go of the trigger and it comes back. Not held, it keeps its course.
// - Each frame the game's own melee hit sweeps the stretch it flew (ImpactMelee::SweepWorld, a sphere about the weapon's size).
//   A person struck takes your melee hit, applied with the game's own function (your weapon's damage, as instant hits apply
//   it), each once on the way out and once on the way back; it cuts through them and flies on. Anything else (a wall, the ground)
//   sends it back.
// - 20 m out, or 2 s (not guided), it comes back to your hand by itself, striking people on the way. Squeeze the grip again and
//   it comes back at once. It also comes back by itself 10 s after the throw (a failsafe), and at once if the weapon
//   is put away or changed, a menu opens, a loading screen comes, or the body is no longer shown.
// - While it is away your empty hand does not attack (impact melee) and the right trigger does not reach the game.
// - (fix156) The left hand's weapon (dual wielding, LeftWeapon) is thrown the same way with the left grip, guided with the left trigger,
//   called back with the left grip; its copy (GrenadeHand) is moved where it flies.
namespace MeleeThrow
{
    // Logs whether throwing is available (it needs impact melee's hooks: the game's melee hit and the function applying its hit).
    void Install();
    // Once per engine frame on the main thread (tracking::Snapshot), after HandGrab::Update (its grips).
    void Update();
    // The shown body placed your weapon in your hand this frame (BodyIK, any thread): its game-world transform (columns of the
    // rotation, position, scale), and the weapon node as last drawn (its world transform and bounding sphere, game world).
    // (fix156) left: the left hand's weapon (GrenadeHand's copy, placed in the left hand this frame); else the right hand's (BodyIK).
    void OnWeaponInHand(bool left, const glm::mat3& r, const glm::vec3& t, float s, const glm::mat3& drawn_r, const glm::vec3& drawn_t, float drawn_s,
        const glm::vec3& bound_centre, float bound_radius);
    // Your weapon is away from your hand (flying, resting where it struck, or coming back). Any thread.
    bool Away(bool left);
    // (fix172) The weapon in that hand as last placed there (within 0.2 s): its hilt (the weapon bone) and the far end of its model along
    // the hand's line through its middle (game world), for blocking (Physical). False: none placed, or its size not read. Any thread.
    bool BladeSegment(bool left, glm::vec3& hilt, glm::vec3& tip);
    // A melee weapon is out and its grip throws it (or it is away). As of the last Update. Any thread.
    bool Available(bool left);
    // Where the shown body puts the weapon bone while it is away (game world). False: not away. Any thread.
    bool FlightPose(bool left, glm::mat3& r, glm::vec3& t, float& s);
    // The weapon hand has no weapon to strike with (away, or caught a moment ago): its swings do not attack. Any thread.
    bool HandWithoutWeapon(bool left);
    // The right trigger is held back from the game (it guides the weapon instead). Any thread.
    bool BlocksRightTrigger();
    // (fix171) Another action has that hand's grip (the favorites wheel, each frame it is open; main thread, before Update): a weapon in that
    // hand is not readied to throw with it, until the grip is let go.
    void HoldOffGrip(bool left);
    // One line for the settings panel.
    // (fix141) Throwing locks onto the person nearest your hand's pointing line while the grip is squeezed, and flies at them.
    void SetAimAtPeople(bool on);
    // (fix145) The living people near you (the game's high-process actors, as the throw looks through them): see(their handle, their
    // reference, their middle, game world). Returns how many were listed (-1: the list could not be read). The main thread.
    int PeopleNearby(const std::function<void(uint32_t handle, uintptr_t ref, const glm::vec3& middle)>& see);
    std::string Status();
}
