#pragma once

#include <string>

#include <glm/glm.hpp>

namespace RE
{
    class NiAVObject;
}

// (fix179) Climbing with your hands, as Skyrim VR's "VR Climbing" (Bellbound, Nexus 168553: its ClimbManager): put a hand on a wall,
// a rock face or the ground's edge, squeeze its grip and pull yourself along it. Stage 1 of three (the aim: the features
// of Skyrim VR's climbing mod; launches, catching yourself and getting up onto ledges come later).
//
// What it does:
// - A grip squeezed with that hand against something climbable (Starfield.esm's collision layers: static, moving-static, trees, props,
//   terrain, ground) holds you there. The hand grab goes first: the squeeze is the climb's only when the hand grab took nothing with it (no
//   object or person next to the hand, none on the way to it) once it has decided (HandGrab::Deciding: its pick following the hand, the left
//   grip's 0.2 s wait for the wheel's gesture; 0.6 s at most). Then the hand grab is told to leave that grip (its gravity gloves' aim
//   ends), and a short buzz says you hold on. The rays at the hand reach 10 cm from the palm; one from your eyes through the palm counts
//   only a surface within those 10 cm of it (a hand already inside a collider).
// - While a hand holds on, your body moves by the opposite of that hand's motion in the room (1:1, both hands averaged; times the pull
//   setting, for reaching further seated), eased in at 13 a second as VR Climbing does, so the hand stays where it took hold (the play
//   area follows your character). Each move is first cast along its way from your feet, waist and head with the game's own ray pick, and
//   cut short before anything solid (walls, rocks, the invisible walls at the world's edges); a move down stops at the ground.
// - Your character is moved with the game's own queued move (the one Actor MoveTo uses, run where the game moves people; it also zeroes the
//   controller's speed and its fall height each time), its heading kept as it is when it runs. The controller's gravity factor is set to 0
//   while you hold on (the game's own zero-gravity case: it then keeps the fall height at where you are), and put back as you let go.
// - Letting go of every grip is a plain fall: the game's own gravity and fall damage, counted from where you let go.
// - While a hand holds you on, the right trigger and A (jump, the boost pack) are held back from the game until let go (ModConfig).
// - Off (and you let go) in menus, conversations, build mode, the scanner and the star map, the favorites, on a ladder, in a seat or the
//   pilot's seat, in third person, on the flat screen, with a weapon in your hands, while something or someone is held, dead, or after a
//   jump of over 2 m (a load, a teleport).
// What it needs from the game (Starfield 1.16.236, each checked at Install against the game's bytes; any difference and it stays off):
// - The ray pick: the pick data's constructor (rva 4951d0: filter info at +0x14 and +0xFC, the ray from/to), the cell's pick (rva b05b80:
//   its world, then the bhk pick rva 2358360, which copies the first hit to +0x70 place, +0x80 normal, +0x90 fraction, +0xB8 body, +0xCC
//   the hit body's filter info, and keeps a reference on its world at +0xE0, let go of after each pick with rva 2e934a0 as the game's
//   callers do). The rays are cast as the line-of-sight layer (41) and the camera's layer (39) in your own collision group (your character
//   controller's, +0x3C of its proxy), so they never hit you.
// - The queued move (rva 18c5440, its run 18cfd40 calling the actor's SetPosition, slot 0x9b8 = Actor::SetPosition 193dcb0 for the player)
//   and a hook in its run (18cfd69) that keeps your heading for its own moves (else a snap turn made meanwhile would be undone).
// - Your character controller ([[player+0x228]+8]+0x458): its gravity factor at +0x350 (written by the game at 1947fb9, used at 23f4307;
//   0 is the game's own case at 23f45f6) and its fall height at +0x348 (Actor::SetPosition zeroes it at 193de22; the fall damage reads it
//   at 1a058fd), written only while its state (+0x2E8, written at 23afe00) is in the air (2).
namespace Climbing
{
    // Checks the game's code and hooks the queued move's heading (the mod's start).
    void Install();
    // Once a frame (tracking::Snapshot, after the hand grab and Physical have read the grips): holding on, pulling, letting go.
    void Update();
    // That grip holds you on a surface now (any thread): it is no aim, favorites-wheel gesture or D-pad combo for the game.
    bool HoldsGrip(bool left);
    // A hand holds you on a surface now (any thread): the left stick does not walk meanwhile.
    bool Active();
    // (fix180) Where a hand holds the wall, for the shown body to draw that hand on it (any thread):
    // `point` the surface where it took hold (game world: it stays put while you pull, the body moving instead), `normal` the surface's
    // outward way there (the pick's own hit normal), `weight` 0..1, eased in over 0.15 s as the hand takes hold and out over 0.15 s after it
    // lets go. False (out untouched) while the weight is 0.
    struct WallGrip {
        glm::vec3 point{};
        glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
        float     weight{ 0.0f };
    };
    bool GripSurface(bool left, WallGrip& out);
    // (fix185) The view tied to your character while you climb (the camera jittered a lot while climbing at times; chosen: "Tie
    // the view to you"). The game hangs its camera root on the first-person skeleton's camera node (FirstPersonState's place, its slot 14
    // 0x141E878D0: [state+0xA0]'s world place), which while climbing does not follow your character: the log of fix184's test, 4-7 cm off
    // on average with moves of 6 cm and more against your climb in every 10 s, worse with your controller in its in-air state. So while a
    // hand holds you on (eased in over 0.15 s) and for 0.3 s after letting go (eased out), the camera root is put at your character's place
    // as asked this frame plus where the game had your eyes from your feet, measured standing still before the climb (kept turned with
    // your heading). Ladders put that root themselves (LadderClimb): none of this then.
    // ViewTie (any thread): how much the root is tied now (0: not at all, 1: wholly) and where it is tied to (game world; untouched at 0).
    float ViewTie(glm::vec3& place);
    // (For what is placed at a frame's start, tracking::LeadNow) While tied: how much further the root will be put this frame than
    // ViewTie says now (the frame before's step until this frame's move is asked, then nothing; game world).
    glm::vec3 ViewTieLead();
    // (The game's thread, every node's world update, before the game's) The camera root: put where ViewTie says (blended with the game's
    // own place by its weight) just before its world place is worked out; the world camera, its child, follows.
    void BeforeNodeUpdate(RE::NiAVObject* obj);
    // One line for the settings panel.
    std::string Status();
}
