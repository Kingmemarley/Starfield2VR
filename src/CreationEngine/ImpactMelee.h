#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

// Impact melee: with the weapon out, a hard swing of a hand (or the gun or blade in it) makes the game's
// melee attack, and the attack hits whatever the fist, blade or gun swept through during the swing.
//
// How the game does melee (found with the melee probe test build, Starfield 1.16.236):
// - Pressing melee (the right stick click) plays the attack; at the attack's hit moment the game runs
//   its melee hit (function rva 198c9e0, the attacker in rsi). That asks the attacker where the attack
//   starts and which way it goes (your view: camera position and look direction), then sweeps a sphere
//   from there along that direction for the weapon's reach (rva d4fe30). Whatever the sphere touches
//   first is hit; a person counts when they are within reach of the start.
// What this changes, for your attacks only:
// - Instant hits (default): while a swing faster than the "swing speed" setting moves, the game's melee
//   hit sweeps each new stretch of the swing's path; the first stretch that strikes someone applies the
//   hit right then, with the game's own function for applying a melee hit. Every swing counts; there is
//   no attack animation and no oxygen cost.
// - With instant hits off, a swing presses melee for a moment (the game still decides whether an attack
//   can start, plays it, uses oxygen and sets the damage), and the attack's sweep is moved as below.
// - At the hit moment the sweep is moved from your view onto the path your striking hand's fist, blade
//   or gun front took during that swing, and its sphere is made hand-sized. If the swing went through
//   nothing, the attack misses. A right stick click without a swing jabs at where your right hand is.
namespace ImpactMelee
{
    // Finds and hooks the game's melee hit (by signature; stays off with a log line if not found).
    void Install();
    // Once per engine frame, right after the tracked poses are sampled (tracking::Snapshot).
    void Update();
    // On every controller poll: true while a swing's melee press is held.
    bool WantsPress();
    // That hand's swing so far strikes nothing more, and a pressed attack still to come does not sweep along it (main thread,
    // where Update runs): its weapon was just thrown.
    void ForgetSwings(bool left);
    // A thrown weapon (MeleeThrow), main thread (where Update runs): the game's melee hit can sweep a stretch of the world and
    // its hit can be applied (the hooks, the melee hit and the function that applies its hit were all found).
    bool CanSweepWorld();
    // Sweeps a sphere of `radius` (metres) from `from` to `to` (game world) with the game's melee hit, for your attack ((fix166) the
    // left hand's weapon's own when `left`: LeftWeapon's).
    // 2: it struck a person (`target`, and where in `point`); 1: something else (where in `point`); 0: nothing; -1: unavailable
    // or the game's code failed.
    int SweepWorld(bool left, const glm::vec3& from, const glm::vec3& to, float radius, uintptr_t& target, glm::vec3& point);
    // Applies your melee hit to that person (the game's own function, as its attack step does: your weapon's damage; (fix166) the left
    // hand's weapon's own when `left`).
    bool ApplyHitTo(bool left, uintptr_t target);
    // One line for the settings panel.
    std::string Status();
}
