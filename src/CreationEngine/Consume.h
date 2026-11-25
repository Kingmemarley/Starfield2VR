#pragma once

#include <cstdint>
#include <string>

// Taking and using things with your hands, as HIGGS does it in Skyrim VR (and as the game itself takes and equips):
// - Eating and drinking: hold food or a drink (an AlchemyItem: Starfield's food, drinks, aid and chems; form type 0x36) at your
//   mouth (HandGrab: the palm within 17 cm of a point 13 cm in front of and 12 cm below your eyes, the hand slower than 2 m/s;
//   HIGGS: 10 Skyrim units forward, 9 down, radius 11, 2 m/s) and let go there: it is taken and used.
// - The bag: let go of something you can carry (apparel, books, ingredients, misc items and resources, weapons, ammo, keys, aid,
//   slates) behind either shoulder or at either hip: it is taken into your inventory (HIGGS's shoulder stash).
// - Picked-up weapons: pull the trigger with a weapon in your hand: it is taken and equipped
//   (HandGrab remembers your weapon before and brings it back when this one is dropped or put in the bag). Only what you carry is
//   ever equipped (the game's equip would add a missing item: a free copy).
// How, each as the game itself does it (Starfield 1.16.236, every piece checked against the game's code at start; anything
// different and these stay off, the log says why):
// - Taking: your own Take (the player's activate, rva 1a392f0) calls TESObjectREFR::ActivateRef (rva b48040) with you, the
//   object's count and 0 flags, after its checks (activation blocked, the game's take state); this does the same, a moment after
//   the hand grab has let go of the object (its springs gone).
// - Equipping: script Actor.EquipItem's code (rva 1fc8df0) finds your handle and hands (handle, item) to the game's queue (rva
//   18cb1a0), which runs the equip (rva 19667d0) on the game's own thread for it. Equipping food or a drink uses it.
// Your activations are logged ("[Eat] activation").
namespace Consume
{
    enum class Action
    {
        Eat,    // taken, then used (food, drinks)
        Bag,    // taken
        Weapon, // taken, then equipped
    };

    // Checks the game's code, and logs your activations.
    void Install();
    // Taking and equipping are available.
    bool Ready();
    // The game's activate is hooked (your activations are seen; a grenade's copy in your hand is refused there: GrenadeHand).
    bool ActivateHooked();
    // A reference's base form: its form type (0: unreadable), and its address and id.
    uint8_t BaseOf(uintptr_t ref, uintptr_t* base = nullptr, uint32_t* base_id = nullptr);
    // The object (a reference) is food or a drink (its base form an AlchemyItem). Any thread; reads only.
    bool IsFood(uintptr_t ref, uint32_t* base_id = nullptr);
    // Things of this form type go into your inventory.
    bool IsCarried(uint8_t type);
    // Your hand let go of `ref` for this (main thread): it is taken once the hand grab has let go of it, then used or equipped.
    // False: not done (off, busy with another, not that kind of thing).
    bool AfterRelease(Action action, uintptr_t ref, int hand);
    // Equip an item you carry (main thread), as script EquipItem does: handed to the game's queue. False: not handed over.
    bool QueueEquip(uintptr_t item, const char* why);
    // Unequip something you carry and have equipped (main thread), as script UnequipItem does (silently): every equipped copy of
    // `item` (a base form) is unequipped by the game's equip manager, queued; with a weapon the game then gives you your fists.
    // False: unavailable on this game version, or not something you carry (nothing done).
    bool Unequip(uintptr_t item, const char* why);
    // The last weapon taken to be equipped: 0 still going (or none), 1 its equip handed to the game, -1 it failed.
    int WeaponResult(uintptr_t& base);
    // Once per frame (main thread). `grabbing`: you still hold something with the game's grab; `menu`: a menu is open (it waits).
    void Update(bool grabbing, bool menu);
}
