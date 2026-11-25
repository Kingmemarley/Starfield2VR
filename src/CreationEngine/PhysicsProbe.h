#pragma once

// Test build only: finds what hand and arm collision needs in Starfield's physics (Havok "hknp", Starfield 1.16.236), so
// the next build can give the hands and forearms bodies that push things (as ROCK does in Fallout 4 VR, also hknp, and
// HIGGS in Skyrim VR), and keep a held object from colliding with you. Nothing in the game is changed: no hooks, and every
// read of game memory goes through ReadProcessMemory, only of committed memory that is not a guard page (an unreadable
// address fails instead of faulting).
// - First (v3, for the next builds' bag, weapon auto-equip and weapon throw): the game's script functions by name (Actor's
//   EquipItem, UnequipItem, GetEquippedWeapon; ObjectReference's DropObject, RemoveItem, AddItem, Activate, PlaceAtMe,
//   ApplyHavokImpulse, SetMotionType): where the game registers each (its name next to its script type's name) and its code,
//   two levels down (the game's own equip, drop, activate... functions). And the code that makes hit collectors for ray and
//   shape casts (closest hit, all hits, line of sight), with what it calls: the cast a gun sight on what the gun points at
//   would use.
// - Then (v4, for eating and drinking what you hold at your mouth): the game's activate (TESObjectREFR::ActivateRef, rva b48040),
//   what it calls and every function that calls it; Actor.EquipItem's code (rva 1fc8df0) and the EquipItem console command's
//   (rva c3bb30), two levels down, with the type tables they load (a task they queue); AlchemyItem's type table and its
//   activate functions (slots 0x53, 0x54), two levels down. (Consume.cpp logs your own activations as they happen.)
// - (v7, fix102, first: for the menus' backgrounds) the engine code that reads the menus' full-screen 3D views (the names the menu
//   files send it: "uColor", the fill colour; "UpdateSceneRectEvent"; the view's corners...; every string with "SceneRect"), two
//   levels down, and its callers; and the code using the render graph name "FullscreenMenu".
// - (v6, fix101: for the menu pointer) where the game calls Windows' mouse and cursor functions (GetCursorPos,
//   SetCursorPos, raw input, ...: each place using its import slot, the functions those are in and their callers), the types with
//   "Mouse" or "Cursor" in their name with their functions, and Scaleform's movie type table (its mouse state functions).
// - (v5) Only the code part below runs now: the in-game scans (your character controller, held objects, the bodies near you)
//   are off. Their search was running during every freeze of the sessions that had them, and their data is in the dumps.
// - A few seconds after start it finds the game's physics types by their RTTI names (the world, collision objects,
//   physics systems, character controllers, collision filters, shapes, actions, step listeners, and every command of the
//   physics command queue: set velocity, set transform, set motion type, set filter info, apply hard keyframe, add
//   bodies...), logs their type tables and saves the code of their functions, the functions those call (two levels for the
//   commands), the functions that use the type tables (constructors, the code that queues each command) and their callers
//   to sfvr/physics_dump.bin.
//   Then, deeper: the functions the first test build pointed at for what comes next (the model update the shown body
//   hooks and who calls it, the collision object's functions that may write physics poses into the 3D, the code that adds
//   bodies to the world, the world's set-motion-type, set-velocity, apply-hard-keyframe and set-transform helpers and who
//   calls them, the collision filter's pair test), and what draws held objects: the scene writer (rva 237cb20, whole, and who
//   calls it), the node world update that hands a node to its collision object (rva 2bd9330), how that one gets the body's
//   pose (the world's predicted pose and the check before it) and its other way, a node moving its body (rva 237a890).
// - Once you are in the game: your character controller (its type, what it points to, its memory) and its collision
//   filter info.
// - Up to three times while you hold an object (the hand grab; each a second or more into a hold and 15 s or more after the
//   one before): the held 3D's collision object, physics system and world (found the way the game's own functions find
//   them), the world's type and what it points to, the held body's record (Starfield's layout: the world's bodies 0xb0
//   bytes each, a body's transform at +0 with its place at +0x30; its place against where the hand grab has the held 3D),
//   where motions are kept (a field of the body that indexes records starting at the body's place), the bodies within 3 m
//   of you (their filter info and flags, what they point to, their records), the held body's motion a few times as it
//   moves, and the collision filter's layer masks.
namespace PhysicsProbe
{
    // Starts the scan 5 s from now (a thread of its own). (fix189) Called only with the developer setting PhysicsCodeScan on, once a
    // session (CreationEngineEntry); it no longer runs at every start.
    void Install();
}
