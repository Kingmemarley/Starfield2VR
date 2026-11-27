#pragma once

#include <cstdint>
#include <string>

// (fix154) Dual wielding melee weapons (a melee weapon chosen from a menu with the left trigger for the left hand and the right
// trigger for the right hand, and thrown from either hand; with a gun or
// fists in the right hand too; the same weapon in both hands only with two of it carried; no flicker as it is chosen).
//
// Starfield has one weapon slot. The right hand keeps the game's own equipped weapon; the left hand's is the mod's: a weapon form it
// remembers (saved with the mod's settings), shown as a world copy held by its grip in your left hand (GrenadeHand's second copy: made,
// moved and deleted as the game's scripts do, never takeable), struck with as the left hand already strikes (ImpactMelee's sweep, the
// weapon's reach; (fix166) the damage its own item's: the game's hit given it). (fix166) The copy carries the chosen item's own mods (a
// saber's colour crystal): the item is remembered with them. Chosen in the inventory with the left trigger: the game's
// equip of it is not made at all (no equip, no sound, no flicker), the weapon becoming the left hand's instead; the left trigger on the
// left hand's weapon again takes it out of the left hand. Shown while it is out (fix169: its own state, as a weapon's in the right
// hand: out when you choose it, away and out again only by you), in first person, outside menus, conversations, the pilot's seat and
// ladders, and while you carry it (two of it when the right hand holds the same; fix168: one taken from the right hand).
namespace LeftWeapon
{
    void Install();
    // (The game's main thread, each frame: tracking::Snapshot, before GrenadeHand::Update.) Whether the left weapon is shown, and its
    // copy asked for or let go (GrenadeHand::WantLeftWeapon).
    void Update();
    // The left hand's weapon as the strike knows kinds (ImpactMelee: 0 fist, 1 one-handed, 2 two-handed); 0 while none is shown.
    int Kind();
    // A left weapon is shown in your left hand now (any thread).
    bool Held();
    // (fix189) You are in furniture: sitting, lying, getting in or out (a chair, a bed, the pilot's seat also when the ship's HUD is not
    // drawn): the player's sit state, (actor+0xFC >> 11) & 3 as Physical reads it, not 0. No player or not readable: false. Any thread.
    // The left weapon is hidden then (not on foot), and the instant hits (ImpactMelee) and the throw (MeleeThrow) wait.
    bool PlayerSeated();
    // The left hand's weapon's form id (0: none), and setting it (main thread; saved with the settings by CreationEngineEntry). A new
    // form id forgets the mods chosen with the one before.
    uint32_t FormId();
    void SetFormId(uint32_t id, const char* why);
    // (fix166) The mods of the left hand's weapon as it was chosen (its inventory item's own: a saber's colour crystal, ...): the mods'
    // form ids, sorted, hex, comma separated ("": none, or not known). Saved as LeftWeaponMods; set as the settings are loaded.
    std::string Mods();
    void SetMods(const std::string& mods);
    // (CreationEngineEntry, each frame) The form id and mods to save, when they changed since the last call: true, `id` and `mods`.
    bool TakeChanged(uint32_t& id, std::string& mods);
    // (fix166) Each choice of a left weapon (or of the same one with other mods) counts one more: its copy is made anew (GrenadeHand).
    uint32_t ChoiceSerial();
    // (fix154) The game's equip of `form` (its inventory item's handle `handle`) asked for by the inventory menu (any thread: the hook's):
    // true when the game's equip is not made: a melee weapon chosen with the left trigger becomes the left hand's weapon, and (fix166)
    // anything else chosen with the left trigger is left alone (nothing done).
    bool TakeMenuEquip(uintptr_t form, uint32_t handle);
    // (fix166) The game's own copy of the left weapon's mods (its inventory item's object-instance extra data, as chosen: same form and
    // mods, else the first of that form), for the placement of its copy (GrenadeHand::Make; main thread). 0: none (the copy gets the
    // weapon's default parts). The caller owns it until the game takes it; FreeModsExtra frees one the game did not take. `ids` says
    // which mods it carries.
    // (fix166, the review) `busy`: your inventory was being changed (its lock taken): nothing read, asked again a frame later.
    uintptr_t CloneModsExtra(uintptr_t form, std::string& ids, bool& busy, bool may_wait);
    void      FreeModsExtra(uintptr_t extra);
    // (fix166) Whether a reference's own extra data carries this object-instance extra (the first of its kind), and the mods it carries.
    bool ReferenceHasExtra(uintptr_t ref, uintptr_t extra, std::string& ids);
    // (fix166) The left weapon as the game's hit knows a weapon (main thread, where ImpactMelee and MeleeThrow run): its form and its
    // inventory item's instance data (its damage, its mods), kept with a reference by LeftWeapon (refreshed each second; the caller
    // adds its own when it hands it on). False when there is none.
    bool AttackInstance(uintptr_t& form, uintptr_t& instance);
    // A form by its id (the game's TESForm::LookupByID, checked at Install; 0: none, or not checked). Main thread.
    uintptr_t LookupForm(uint32_t id);
    // "Y" below is the reload button, the one that sends the game's X (fix194: Touch X with the face buttons as printed, Touch Y in the
    // old layout); the names are the old layout's.
    // (fix157) Two quick presses of Y (ModConfig): the left hand's weapon put away, or drawn again (any thread). Your weapons put away by
    // the game (Y held) draw both again.
    void ToggleSheathed();
    // (fix158) The left trigger pulled in play (ModConfig: not with the grip, at the temple, or on a menu): the left weapon drawn when put
    // away (on its own when nothing else is out). And Y's state each poll: your weapons drawn by the game with Y held draw both.
    void LeftTriggerPulled();
    void NoteYHeld(bool down);
    // (fix179) The left weapon out with your own put away (any thread), and Y held then: the left one put away alone (taken by Update).
    bool OnlyLeftOut();
    void PutAwayAlone();
    // (fix157) The favorites menu: a trigger chose the selected favorite (ModConfig sends A for it): which (any thread).
    void NoteFavoritesPress(bool left);
    // (fix157) The favorites' equip of `form` (its item's handle `handle`; the hook's thread): true when the game's equip is not made (a
    // melee weapon chosen there with the left trigger, taken as the left hand's; (fix166) anything else chosen with the left trigger is
    // left alone).
    bool TakeFavoriteEquip(uintptr_t form, uint32_t handle);
    // (fix171) The favorites wheel (FavoritesWheel) uses a favorite itself, with the game's own UseQuickkey, on the main thread. Around that
    // call (BeginWheelUse .. EndWheelUse, the same thread) the favorites' hook knows the choice is the wheel's and for which hand: the left
    // hand's takes a melee weapon as the left hand's weapon (as the left trigger in the favorites does; again on the left hand's own: taken
    // out) and does nothing with anything else (the game's equip never made); the right hand's is the game's own use, as always.
    // WheelReady: the hook is there and dual wielding is on (else the wheel gives the left hand nothing). EndWheelUse: 1 the left hand took
    // it, 2 it took nothing (not a melee weapon), 0 the hook did not run.
    bool WheelReady();
    void BeginWheelUse(bool left);
    int  EndWheelUse();
    std::string Status();
}
