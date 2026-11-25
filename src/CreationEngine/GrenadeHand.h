#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

#include "HandGrab.h"

// A grenade in your hand: while you hold a grenade taken from behind your shoulder (HandActions), a copy of it sits in your right
// hand, held the way the hand grab holds things (against the drawn hand's palm, the fingers closed around it). The game itself
// shows nothing in the hand while its grenade button is held (two test builds: no model on either rig's hand anchors).
//
// The copy is a world object made and removed the way Papyrus scripts do it (the classic "prop in the hand": PlaceAtMe, then
// SetMotionType keyframed and a move every frame, then Delete), with the game's own functions, each checked against Starfield
// 1.16.236's code before it is used (the physics test builds' dumps):
// - Made: PlaceAtMe's own helper (rva 207bcc0; script ObjectReference.PlaceAtMe calls it at 207d33f), at your hand, one of the
//   grenade equipped in its slot, not persistent, delete-when-able (the script defaults). The helper reports to a script engine
//   only for a cell it cannot place in (the game's "no cell" value, read from its own compare), which is checked first.
// - Kept by its handle (the reference's +0x24, looked up with the game's handle table as every script job does, rva 28d0ff0 with
//   the function object script Delete uses), never by a pointer kept across frames.
// - Once its 3D is in (the check script SetMotionType makes, rva 3537c0): put in your collision group (as the hand grab does with
//   a held object: it no longer touches you or your character controller) and made keyframed (the function script SetMotionType
//   calls, rva 235a310: no gravity, moved only where it is put).
// - Every frame: moved to your hand with the function script SetPosition's job calls (rva 20660f0: the reference, no target, its
//   cell and worldspace, the place, the angles). The angles are worked out as Creation Engine references store them (the stored
//   rows rebuilt as Rz * Ry * Rx, as ROCK found in Fallout 4 VR); the log compares where the 3D ended up with where it was asked
//   to go.
// - Removed with the function script Delete's job calls (rva 187e370: the handle, not "delete when able"): it takes the 3D out at
//   once and queues the reference for deletion. That happens the moment the grenade leaves your hand (thrown or put back), a menu
//   opens, the setting is turned off, or anything else goes wrong (its 3D not in after 2 s, the hand not found).
// - It can never be taken: while it is in your hand the game's interaction pick finds nothing (PointInteract: no prompt, nothing
//   to activate or grab), and the game's activate refuses it outright (Consume's hook on ActivateRef returns false for it, as if
//   its activation were blocked).
namespace GrenadeHand
{
    // Checks the game's code (see above); stays off with a log line if anything differs.
    void Install();
    // Once per engine frame (main thread, tracking::Snapshot), after HandActions::Update (whose grenade state it follows).
    void Update();
    // At the start of the game's activate (Consume's hook, any thread): `object` is the copy, so the activate must return false at
    // once: the address of a `ret` of the game's to resume at (checked at Install). 0: not the copy (or nothing checked).
    uintptr_t RefuseActivation(uintptr_t object);
    // The interaction pick should find nothing now: a copy is in your hand (any thread; PointInteract).
    bool PicksOff();
    // For the shown body's fingers: that hand holds the copy, and its box (as the hand grab's HeldBox gives a held object's).
    bool InHand(bool left);
    bool HeldShape(bool left, HandGrab::HeldShape& out);
    // (fix154) The left hand's melee weapon (dual wielding, LeftWeapon): a copy of `form` (a weapon form; 0: none) held by its grip in
    // your left hand, made, kept and removed as the grenade's copy is (asked each frame, main thread).
    void WantLeftWeapon(uintptr_t form);
    // The left weapon's copy is in your hand now (any thread).
    bool LeftWeaponShown();
    // (fix156) ...in your left hand or flying where its throw goes (MeleeThrow). Any thread.
    bool LeftWeaponOut();
    // Its reference's handle while it is held (0: none; main thread): the throw takes it.
    uint32_t LeftWeaponHandle();
    // Its blade where it was last put (game world: its grip, its way, its length; main thread): false without it.
    bool LeftBlade(glm::vec3& grip, glm::vec3& way, float& length);
    // (fix154) The left weapon's blade's way in the left controller's aim frame (its tracked axes: aim * way is the blade in the room), as last
    // drawn: the left hand's hits go along it. False: no left weapon held. Any thread.
    bool LeftBladeLocal(glm::vec3& way);
    // (fix154) The game starts saving (any thread): the copies in your hands now (or gone a moment ago) are noted, to be deleted if a load
    // brings one back (a save would keep it: after the load it would lie in the world, a weapon for free). what: for the log.
    void NoteSaving(const char* what);
    // (fix172) The game is about to save (the save manager's tasks ask for one). Main thread.
    bool SavePending();
    // (fix154) Those noted copies, kept in the settings: given at the config load; taken when changed, to be saved ("ref:base,...", hex).
    void LoadStrays(const std::string& text);
    bool TakeStraysChanged(std::string& text);
    // One line for the settings panel.
    std::string Status();
    // The 3D of the reference with this handle, held while use runs (the gravity gloves' highlight; the game's thread): false if it
    // cannot be found by its handle or has no 3D.
    bool WithRoot(uint32_t handle, const std::function<void(uintptr_t root)>& use);
    // (fix141) The reference with this handle itself and its 3D (0 if none), held while use runs (the game's thread): false if it cannot
    // be found by its handle. (The saber throw's targets: MeleeThrow.)
    bool WithReference(uint32_t handle, const std::function<void(uintptr_t ref, uintptr_t root)>& use);
}
