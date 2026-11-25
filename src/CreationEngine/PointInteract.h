#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// Hand interaction: the game picks what the "Open / Take / Talk" prompt is for (its ViewCaster)
// with a shape cast from the rendered camera, which stays on your head in VR. While the weapon is put
// away that cast is moved to the right hand and made short and a little wider, so the prompt is for
// whatever is near the hand. (While a hand aims with the gravity gloves, its cast reaches far and can be held still.)
namespace PointInteract
{
    // Hooks the game's interaction pick (Starfield 1.16.236: rva 12e2ed0, found by its signature).
    void Install();
    // Both hooks are in (the pick can be moved, or made to find nothing while a grenade's copy is in your hand: GrenadeHand).
    bool PicksHooked();
    // Once per frame: a log line every 10 s while the hand picks.
    void Report();
    // Which hand the interaction pick follows (the right one unless set; hand grab switches it to the hand
    // that reaches for something).
    void SetPointingHand(bool left);
    bool PointingHandIsLeft();
    // While a hand aims to pull something from afar (gravity gloves): the hand's cast reaches this far with this
    // radius (metres, before the height scale). A reach of 0 goes back to the hand reach setting.
    void SetFarCast(float reach, float radius);
    // Keep casting along the hand's ray the latest cast took (true; a far cast still starts along it as its radius
    // needs) instead of following the hand, so a target that was found stays the target while the hand flicks; false
    // follows the hand again.
    void HoldCast(bool hold);
    // The latest cast made at the hand: its radius (metres, before the height scale), how many such casts there have been
    // (a new count: a new result in the pick), and (steer) the tag of the steer it took (0: it was not steered), read together.
    void LastCast(float& radius, int& count, uint32_t* steer = nullptr);
    // Gravity gloves picking as HIGGS does (the object nearest the pointing line, not the first thing a sphere touches): the
    // far cast goes from the hand straight at this point (game world), reaching a little past it, so the floor or a shelf
    // under it cannot get in first, with this radius (metres, before the height scale; the steered casts' own, whatever
    // SetFarCast says). `tag` names what it is aimed at (LastCast says which cast took it). on false: the hand's own ray again.
    void SetSteer(bool on, const glm::vec3& target, uint32_t tag, float radius = 0.0f);
}
