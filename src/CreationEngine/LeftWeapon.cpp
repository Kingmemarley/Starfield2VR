#include "LeftWeapon.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include <mods/VR.hpp>

#include "CreationEngineSingletonManager.h"
#include "Consume.h"
#include "GrenadeHand.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "LadderClimb.h"
#include "MeleeThrow.h"
#include "Physical.h"
#include "MenuPointer.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"

namespace LeftWeapon
{
    namespace
    {
        // Starfield 1.16.236: a form by its id as Game.GetForm finds it (0x142017960: jmp 0x1405DEF30, TESForm::LookupByID).
        constexpr uintptr_t kLookupRva    = 0x5DEF30;
        constexpr uintptr_t kGetFormRva   = 0x2017960;
        constexpr uint8_t   kLookupStart[] = { 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB };
        // A form's type (+0x2E: a weapon 0x30, as HandActions reads launch data) and a weapon's type: its instance data (+0x248), that
        // one's general data (+0x50), the type there (+0x38), as the game's melee reads it (0x14198CC3B; ImpactMelee::WeaponKind):
        // 1-4 one-handed melee (sword, dagger, axe, mace), 5-6 two-handed melee; 0 hand to hand, 7 on bows, guns, grenades.
        constexpr uintptr_t kFormType    = 0x2E;
        constexpr uint8_t   kTypeWeapon  = 0x30;
        constexpr uintptr_t kFormId      = 0x28;
        // Your inventory, as Consume reads it (the game's equip reads it so, 0x14196E8A0): the actor's +0xa0 list, its +0x28 count
        // and +0x30 entries of 0x28 bytes, the item first.
        constexpr uintptr_t kActorInventory = 0xa0;
        constexpr uintptr_t kListCount      = 0x28;
        constexpr uintptr_t kListEntries    = 0x30;
        constexpr size_t    kEntrySize      = 0x28;
        // How long the left weapon may be missing from your inventory before it leaves the left hand (a menu's moment, a transfer).
        constexpr int64_t kMissingMs = 1500;
        // A press of the left trigger in the inventory this recent is the menu's equip asked for with it.
        constexpr int64_t kPressRecentMs = 1500;

        using LookupFn = void* (*)(uint32_t);

        // The inventory's "item selected" (a helper's research, checked against 1.16.236): the menu's InventoryMenu_SelectItem event (its
        // sink's ProcessEvent 0x1415E8FD0, direct or queued) runs the worker 0x1415E9090(&item handle), for the player only and for this
        // menu only (its only two callers). It finds the item's form (into rsi, 0x1415E90EE; none: out) and then equips or unequips it
        // (0x1419A5C20 at 0x1415E9150: EquipObject with its sound), refreshes the paper doll, and returns past 0x1415E92BC (the item
        // handle's hold let go). A hook at 0x1415E90FB (the form found; nothing done with it yet): a melee weapon chosen with the left
        // trigger skips to 0x1415E92BC: no equip, no sound, no refresh, the handle let go as always.
        // (fix191) Starfield 1.16.244 has the same worker at 0x1415E8080, each of these places at the same distance into it: the one change
        // is the thread's counter it raises after the hook's place and lowers before the skip's target (+0x278 is +0x27C there, as in all
        // of 1.16.244), so the skip still passes over both; the mod never touches it. Its places there are matched by hand for the port
        // table (docs/PORTING.md).
        constexpr uintptr_t kWorkerRva   = 0x15E9090;
        constexpr uint8_t   kWorkerStart[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC,
                                                0x48, 0x83, 0xEC, 0x58, 0x45, 0x33, 0xE4, 0x44, 0x89, 0x65, 0x58, 0x8B, 0x19, 0x89, 0x5D, 0x50 };
        constexpr uintptr_t kFoundRva    = 0x15E90EE;
        constexpr uint8_t   kFound[]     = { 0x48, 0x8B, 0x75, 0x58, 0x48, 0x85, 0xF6, 0x0F, 0x84, 0xC4, 0x01, 0x00, 0x00, 0x66, 0xC7, 0x45, 0x48,
                                             0x01, 0x00, 0x41, 0xBD, 0xBC, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kHookRva     = 0x15E90FB; // (kFound + 0x0D)
        constexpr uintptr_t kEquipCallRva = 0x15E9150;
        constexpr uintptr_t kEquipRva    = 0x19A5C20;
        constexpr uintptr_t kSkipRva     = 0x15E92BC;
        constexpr uint8_t   kSkip[]      = { 0x8B, 0x5D, 0x50, 0x48, 0x8B, 0x0D };
        safetyhook::MidHook g_select_hook{};
        std::atomic<int>    g_taken{ 0 };
        std::atomic<int64_t> g_press_used{ 0 }; // (the press already taken: one equip each)
        std::atomic<uintptr_t> g_press_form{ 0 }; // (the item that press chose)
        std::atomic<int64_t>   g_press_nothing_ms{ 0 }; // (fix166) when that press found no melee weapon (nothing done), 0 if it did

        uintptr_t             g_base{ 0 };
        bool                  g_ok{ false };        // (dual wielding: the lookup and the inventory hook)
        bool                  g_lookup_ok{ false }; // (the form lookup alone: also for the copies from a save; the review)
        std::atomic<uint32_t> g_form_id{ 0 };
        std::atomic<uint32_t> g_choice{ 0 };       // (fix166) one more each choice (ChoiceSerial)
        // (fix168) A left choice to take from the right hand if it holds it: its choice's serial (g_choice; a newer choice drops it) << 32 | its
        // form id; 0: none. One value, cleared only if unchanged: a request made meanwhile on another thread is never lost (the review).
        std::atomic<uint64_t> g_move_req{ 0 };
        uint32_t              g_moving_id{ 0 };    // (fix168, main thread) its unequip asked for: the right hand may still hold it meanwhile
        int64_t               g_moving_since{ 0 };
        int64_t               g_moving_until{ 0 };
        std::atomic<uint32_t> g_out_choice{ 0 };   // (fix169) the choice (g_choice) you made with the left trigger: out at once
        std::mutex            g_mods_mtx;
        std::string           g_mods{};            // (fix166) the chosen item's mods (Mods; under g_mods_mtx)
        std::atomic<bool>     g_changed{ false };
        std::atomic<bool>     g_held{ false };
        std::atomic<bool>     g_sheathed{ false }; // (fix157) put away (fix169: the left weapon's own state, as a weapon's in the right hand)
        std::atomic<bool>     g_toggle{ false };   // (asked for, taken by Update)
        std::atomic<bool>     g_put_alone{ false }; // (fix179) Y held with only the left one out: asked for, taken by Update
        std::atomic<bool>     g_left_pull{ false }; // (fix158) the left trigger pulled (ModConfig), taken by Update
        std::atomic<int64_t>  g_y_held_ms{ 0 };     // (fix158) Y last seen held
        std::atomic<bool>     g_y_down{ false };    // (fix158) Y down now
        std::atomic<int>      g_kind{ 0 };
        // (Main thread.)
        uint32_t              g_cached_id{ 0 };
        uintptr_t             g_cached_form{ 0 };
        int                   g_cached_kind{ 0 };
        int64_t               g_missing_since{ 0 };
        std::string           g_shown_why{};
        std::mutex            g_status_mtx;
        std::string           g_status{ "Dual wielding: no left weapon" };
        std::atomic<int>      g_logs{ 0 }; // (the hook's thread and the main thread)

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it: the helpers below then read nothing).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        uintptr_t CallTarget(uintptr_t at)
        {
            if (at == 0) {
                return 0;
            }
            __try {
                const auto* b = reinterpret_cast<const uint8_t*>(at);
                if (b[0] != 0xE8) {
                    return 0;
                }
                return at + 5 + *reinterpret_cast<const int32_t*>(at + 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        uintptr_t JumpTarget(uintptr_t at)
        {
            if (at == 0) {
                return 0;
            }
            __try {
                const auto* b = reinterpret_cast<const uint8_t*>(at);
                if (b[0] != 0xE9) {
                    return 0;
                }
                return at + 5 + *reinterpret_cast<const int32_t*>(at + 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        uintptr_t Lookup(uint32_t id)
        {
            if (!g_lookup_ok || id == 0) {
                return 0;
            }
            __try {
                return reinterpret_cast<uintptr_t>(reinterpret_cast<LookupFn>(At(kLookupRva))(id));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }


        // (fix157) The favorites menu (the quick-select wheel: the left and right triggers equip there as in the
        // inventory, the right trigger for the right weapon and the left trigger for the left weapon; a helper's research,
        // checked): its choice (FavoritesMenu_UseQuickkey) runs FavoritesManager::UseQuickkey 0x141EC2040(manager, slot) (directly from
        // its event sink, or queued; also from the keyboard's hotkeys 1-12), which finds the favorite's inventory item (0x141EC4060: its
        // handle at [rbp+0x67], held; -1 a power) and equips it with 0x1419A5C20 at 0x141EC2336, every way out through 0x141EC24C7 (the
        // handle let go, the thread's counter +0x278 raised at 0x141EC208B lowered, `movzx eax,dil` returned). Hooked at 0x141EC20B5 (the
        // handle known, nothing done with it yet): a melee weapon chosen with the left trigger (ModConfig sends A for it and notes the
        // hand) becomes the left hand's, and the call goes to that way out with dil 1 ("used": no failure sound): no equip, no sound.
        // (fix191) Starfield 1.16.244 has the same UseQuickkey at 0x141EBF380, each of these places at the same distance into it, the same
        // frame ([rbp+0x67], dil): the one change is that thread's counter (+0x27C there, as in all of 1.16.244), raised before the hook's
        // place and lowered on the way out as before. Its places there are matched by hand for the port table (docs/PORTING.md).
        constexpr uintptr_t kUseQuickkeyRva   = 0x1EC2040;
        constexpr uint8_t   kUseQuickkeyStart[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
                                                    0x24, 0xE1, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xFA, 0x4C, 0x8B, 0xE9 };
        constexpr uintptr_t kQuickkeyFindRva  = 0x1EC209D;
        constexpr uint8_t   kQuickkeyFind[]   = { 0x49, 0x8D, 0x9D, 0x40, 0x04, 0x00, 0x00, 0x41, 0x83, 0xBD, 0x38, 0x04, 0x00, 0x00, 0x00, 0x7C, 0x03, 0x48, 0x8B,
                                                  0x1B, 0x48, 0x8B, 0x1C, 0xFB, 0x40, 0x32, 0xFF, 0x83, 0x7D, 0x67, 0xFF, 0x0F, 0x84, 0xB2, 0x02, 0x00, 0x00 };
        constexpr uintptr_t kQuickkeyHookRva  = 0x1EC20B5; // (kQuickkeyFind + 0x18: xor dil,dil; cmp dword [rbp+67h],-1)
        constexpr uintptr_t kQuickkeyEquipCallRva = 0x1EC2336;
        constexpr uintptr_t kQuickkeyOutRva   = 0x1EC24C7;
        constexpr uint8_t   kQuickkeyOut[]    = { 0x48, 0x8B, 0x0D, 0xB2, 0x61, 0xFA, 0x03, 0x48, 0x85, 0xC9, 0x74, 0x10, 0x83, 0x7D, 0x67, 0xFF, 0x74, 0x0A,
                                                  0x48, 0x8D, 0x55, 0x67, 0xE8 };
        // The item's form by its handle, as the inventory's own choice finds it (0x1402C5E90(handles [0x145E68680], &handle, &&&out): the
        // inventory entry's base form; its call at 0x1415E90E9).
        constexpr uintptr_t kHandleFormRva    = 0x2C5E90;
        constexpr uint8_t   kHandleFormStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                                   0x48, 0x83, 0xEC, 0x50, 0x4D, 0x8B, 0xE0, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xE9 };
        constexpr uintptr_t kHandleFormCallRva = 0x15E90E9;
        constexpr uintptr_t kHandlesRva       = 0x5E68680;
        constexpr int64_t   kFavoritesRecentMs = 1500;
        using HandleFormFn = bool (*)(void*, const uint32_t*, void*);
        safetyhook::MidHook    g_quickkey_hook{};
        std::atomic<int>       g_fav_hand{ -1 };     // (the last favorites choice's trigger: 0 left, 1 right)
        std::atomic<int64_t>   g_fav_ms{ 0 };
        std::atomic<int64_t>   g_fav_used{ 0 };
        std::atomic<uintptr_t> g_fav_form{ 0 };
        void                   OnUseQuickkey(safetyhook::Context& ctx); // (below)
        // (fix171) The favorites wheel's own use of a slot, on this thread (BeginWheelUse .. EndWheelUse): the hand it is for (-1 none, 0 left,
        // 1 right), and what the hook did for the left hand (0 not run, 1 taken as the left hand's weapon, 2 nothing).
        thread_local int t_wheel_hand{ -1 };
        thread_local int t_wheel_result{ 0 };

        uintptr_t FormOfHandle(uint32_t handle)
        {
            uintptr_t handles = 0;
            if (!SafeRead(&handles, At(kHandlesRva), sizeof(handles)) || handles == 0) {
                return 0;
            }
            uintptr_t out = 0;
            uintptr_t* p2 = &out;
            uintptr_t** p1 = &p2;
            __try {
                const bool found = reinterpret_cast<HandleFormFn>(At(kHandleFormRva))(reinterpret_cast<void*>(handles), &handle, &p1);
                return found ? out : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // A weapon form's melee kind as the strike knows them (1 one-handed, 2 two-handed); 0 not a melee weapon.
        int MeleeKind(uintptr_t form)
        {
            uint8_t   type = 0;
            uintptr_t data = 0, info = 0;
            uint32_t  weapon_type = 0;
            if (form == 0 || !SafeRead(&type, form + kFormType, 1) || type != kTypeWeapon || !SafeRead(&data, form + 0x248, 8) || data == 0 ||
                !SafeRead(&info, data + 0x50, 8) || info == 0 || !SafeRead(&weapon_type, info + 0x38, 4)) {
                return 0;
            }
            if (weapon_type >= 1 && weapon_type <= 4) {
                return 1;
            }
            if (weapon_type == 5 || weapon_type == 6) {
                return 2;
            }
            return 0;
        }

        // How many of this form you carry.
        int CarriedCount(uintptr_t form)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  list = 0, entries = 0;
            uint32_t   count = 0;
            if (player == 0 || form == 0 || !SafeRead(&list, player + kActorInventory, 8) || list == 0 || !SafeRead(&count, list + kListCount, 4) ||
                count == 0 || count > 20000 || !SafeRead(&entries, list + kListEntries, 8) || entries == 0) {
                return -1;
            }
            std::vector<uint8_t> raw(static_cast<size_t>(count) * kEntrySize);
            if (!SafeRead(raw.data(), entries, raw.size())) {
                return -1;
            }
            // (Each entry of it: its stacks (+0x10 their number, +0x18 them, 0x10 bytes each, the count at +0x08), as the game's own count
            // reads them, 0x140B64460, script GetItemCount's; every variant of the weapon.)
            int n = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* e      = raw.data() + static_cast<size_t>(i) * kEntrySize;
                uintptr_t      object = 0, stacks = 0;
                uint32_t       number = 0;
                std::memcpy(&object, e, sizeof(object));
                if (object != form) {
                    continue;
                }
                std::memcpy(&number, e + 0x10, sizeof(number));
                std::memcpy(&stacks, e + 0x18, sizeof(stacks));
                if (number == 0 || number > 256 || stacks == 0) {
                    continue; // (no stack: none of it, as the game counts)
                }
                for (uint32_t k = 0; k < number; ++k) {
                    uint32_t c = 0;
                    if (SafeRead(&c, stacks + k * 0x10ull + 0x08, sizeof(c)) && c < 100000) {
                        n += static_cast<int>(c);
                    }
                }
            }
            return n;
        }

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        bool InventoryShown() { return GameFlow::isInventoryShown(); }

        // ---- (fix166) The chosen item's own mods and instance data ----
        // (The left saber was always red, whatever crystal the chosen one had, and its hits did the right weapon's damage. A
        // helper's research, checked against 1.16.236: the left copy was placed from the base weapon, so the game gave it the weapon's
        // default parts; and the game's hit takes its weapon from your equipped item.) Your inventory's entries (kActorInventory) are one
        // per item as it is made: +0x08 its instance data (TBO_InstanceData: its damage and the rest as its mods made them; a reference
        // count at its +0x08), +0x10 the number of its stacks and +0x18 them (0x10 bytes each: +0x00 the stack's extra data list), +0x20
        // its flags (& 7: equipped). A stack's extra data holds the item's mods as an object-instance extra (BGSObjectInstanceExtra, type
        // 0x33): +0x18 the number of mods, +0x20 them (0x10 bytes each: +0x00 the mod (an OMOD form), +0x08 its attach index, +0x09 its
        // rank). The inventory is read under its own lock (the actor's +0xA8, a read lock), as the game's own reads take and let go of it.
        constexpr uintptr_t kActorInventoryLock = 0xa8;
        constexpr uintptr_t kEntryInstance      = 0x08;
        constexpr uintptr_t kEntryStackCount    = 0x10;
        constexpr uintptr_t kEntryStacks        = 0x18;
        constexpr uintptr_t kEntryFlags         = 0x20;
        constexpr uint8_t   kInstanceExtraType  = 0x33;
        constexpr uintptr_t kModsCount          = 0x18;
        constexpr uintptr_t kModsData           = 0x20;
        constexpr uint32_t  kModsMax            = 64;
        // An inventory item by its handle, as the game's equip reads it (0x1419A5C20 at 0x1419A5C82: 0x14063ED50(the handles
        // [0x145E68680], &handle, &&&out)): it finds the item's owner and entry, takes the owner's inventory lock (0x14063EEC2: +0xA8, then
        // the list at +0xA0) and copies the entry out (0x14063E4C0, called at 0x14063EEF3): +0x00 the item, +0x08 its instance data (a
        // reference added: let go after), +0x10 its count, +0x14 its equipped flags (0x1B bytes, zeroed first). Its lock let go there
        // (0x14063EEF8) as every read lock is: one reader less; the last reader with writers waiting wakes them (the import at
        // [0x143A442C8]).
        constexpr uintptr_t kItemOfHandleRva     = 0x63ED50;
        constexpr uint8_t   kItemOfHandleStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                                     0x83, 0xEC, 0x50, 0x4D, 0x8B, 0xE0, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xE9, 0x40, 0x32, 0xF6, 0x83, 0x3A,
                                                     0xFF, 0x0F, 0x84, 0xBE, 0x01, 0x00, 0x00 };
        constexpr uintptr_t kItemLockRva         = 0x63EEC2;
        constexpr uint8_t   kItemLock[]          = { 0x48, 0x8D, 0x8B, 0xA8, 0x00, 0x00, 0x00, 0xE8 };
        constexpr uintptr_t kItemListRva         = 0x63EECE;
        constexpr uint8_t   kItemList[]          = { 0x4C, 0x8B, 0x83, 0xA0, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kItemCopyCallRva     = 0x63EEF3;
        constexpr uintptr_t kItemCopyRva         = 0x63E4C0;
        constexpr uint8_t   kItemCopyStart[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89,
                                                     0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x5A, 0x08, 0x4C, 0x8B, 0xF1, 0x48,
                                                     0x8B, 0x0A, 0x48, 0x8B, 0xF2 };
        constexpr uintptr_t kItemUnlockRva       = 0x63EEF8;
        constexpr uint8_t   kItemUnlock[]        = { 0x48, 0x8D, 0x8B, 0xAC, 0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xC1, 0x11, 0x8B,
                                                     0xC2, 0x25, 0xFF, 0x0F, 0xC0, 0xFF, 0x83, 0xF8, 0x01, 0x75, 0x0E, 0xF7, 0xC2, 0x00, 0xF0, 0x3F, 0x00,
                                                     0x74, 0x06, 0xFF, 0x15 };
        constexpr uintptr_t kWakeImportRva       = 0x3A442C8;
        constexpr uintptr_t kEquipHandlesRva     = 0x19A5C82; // (mov rcx,[the handles]; call 0x14063ED50)
        // The read lock (0x1422CE9B0: one reader more, waiting while a writer of another thread holds it).
        constexpr uintptr_t kLockReadRva         = 0x22CE9B0;
        constexpr uint8_t   kLockReadStart[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x79, 0x04,
                                                     0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xC1, 0x07, 0xA9, 0x00, 0x00, 0xC0, 0xFF };
        // An extra data list's first extra of a type (0x1403476E0(list, type): the list's own lock taken inside, at +0x21).
        constexpr uintptr_t kGetByTypeRva        = 0x3476E0;
        constexpr uint8_t   kGetByTypeStart[]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
                                                     0x83, 0xEC, 0x20, 0x48, 0x8D, 0x59, 0x20, 0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xCB, 0xE8 };
        constexpr size_t    kGetByTypeLock       = 0x21;
        // An object-instance extra copied as the game copies one (0x140402C10(unused, source): a type 0x33 only (0x140402C6F); 0x30 bytes
        // from the game's heap, built by 0x140402080 (its call at 0x140402CB7) with the type table 0x144B0E998 (0x1404020B2), its mods'
        // array copied), and freed as the game frees one (that table's first function, 0x1404015E0(extra, 1): its mods' array freed, then,
        // with 1, the extra's 0x30 bytes (0x140401634)).
        constexpr uintptr_t kCloneRva            = 0x402C10;
        constexpr uint8_t   kCloneStart[]        = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x41,
                                                     0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA };
        constexpr uintptr_t kCloneTypeRva        = 0x402C6F;
        constexpr uint8_t   kCloneType[]         = { 0x80, 0x7F, 0x12, 0x33, 0x75, 0x50 };
        constexpr uintptr_t kCloneBuildCallRva   = 0x402CB7;
        constexpr uintptr_t kBuildRva            = 0x402080;
        constexpr uintptr_t kBuildTableRva       = 0x4020B2;
        constexpr uint8_t   kBuildTable[]        = { 0xC6, 0x41, 0x12, 0x33, 0x48, 0x8D, 0x05 };
        constexpr uintptr_t kInstanceExtraTable  = 0x4B0E998;
        constexpr uintptr_t kFreeRva             = 0x4015E0;
        constexpr uint8_t   kFreeStart[]         = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA,
                                                     0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x05 };
        constexpr uintptr_t kFreeFlagRva         = 0x401634;
        constexpr uint8_t   kFreeFlag[]          = { 0x40, 0xF6, 0xC7, 0x01, 0x74, 0x0D, 0xBA, 0x30, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kRefExtraList        = 0xc8; // a reference's own extra data list
        using ItemOfHandleFn = bool (*)(void*, const uint32_t*, void*);
        using LockFn         = void (*)(void*);
        using GetByTypeFn    = void* (*)(void*, uint32_t);
        using CloneFn        = void* (*)(void*, void*);
        using DeleteFn       = void (*)(void*, uint32_t);
        bool             g_mods_ok{ false }; // (fix166: all of the above checked)
        std::atomic<int> g_mods_logs{ 0 };
        // (Main thread) the left weapon's instance data for its hits, with a reference held (AttackInstance).
        uintptr_t g_attack_form{ 0 };
        uintptr_t g_attack_instance{ 0 };
        uint32_t  g_attack_choice{ 0 };
        int64_t   g_attack_at{ 0 };

        // A rip-relative operand (the displacement in the instruction's last 4 bytes): where it points.
        uintptr_t RipOperand(uintptr_t at, size_t length)
        {
            int32_t rel = 0;
            return at != 0 && SafeRead(&rel, at + length - 4, sizeof(rel)) ? at + length + static_cast<intptr_t>(rel) : 0;
        }

        bool LockRead(uintptr_t lock)
        {
            __try {
                reinterpret_cast<LockFn>(At(kLockReadRva))(reinterpret_cast<void*>(lock));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // As the game lets go of a read lock (0x14063EEF8): one reader less; the last reader with writers waiting wakes them.
        void UnlockRead(uintptr_t lock)
        {
            auto* const state = reinterpret_cast<volatile long*>(lock + 4);
            const long  was   = _InterlockedExchangeAdd(state, -1);
            if ((was & 0xFFC00FFF) == 1 && (was & 0x3FF000) != 0) {
                uintptr_t wake = 0;
                if (SafeRead(&wake, At(kWakeImportRva), sizeof(wake)) && wake != 0) {
                    reinterpret_cast<LockFn>(wake)(const_cast<long*>(state));
                }
            }
        }

        // A read lock taken only when no writer holds it now (never waits: for the frame's own once-a-second look, which must not stall the
        // frame while the game writes, a load for one). The game's own read adds its reader first and then waits; this one lets go again.
        bool TryLockRead(uintptr_t lock)
        {
            auto* const state = reinterpret_cast<volatile long*>(lock + 4);
            const long  was   = _InterlockedExchangeAdd(state, 1);
            if ((was & 0xFFC00000) == 0) {
                return true;
            }
            UnlockRead(lock);
            return false;
        }

        uintptr_t GetByType(uintptr_t list, uint8_t type)
        {
            if (list == 0) {
                return 0;
            }
            __try {
                return reinterpret_cast<uintptr_t>(reinterpret_cast<GetByTypeFn>(At(kGetByTypeRva))(reinterpret_cast<void*>(list), type));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        uintptr_t CloneExtra(uintptr_t source)
        {
            __try {
                return reinterpret_cast<uintptr_t>(reinterpret_cast<CloneFn>(At(kCloneRva))(reinterpret_cast<void*>(source), reinterpret_cast<void*>(source)));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // Something with its type table in the game's image (an instance data, an extra): not a stray pointer.
        bool GameObject(uintptr_t p)
        {
            static const uintptr_t image_end = [] {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
                const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
                return g_base + nt->OptionalHeader.SizeOfImage;
            }();
            uintptr_t table = 0;
            return p != 0 && SafeRead(&table, p, sizeof(table)) && table > g_base && table < image_end;
        }

        // An instance data's reference: one more (the inventory's lock held, so it cannot go meanwhile), or one less as the game lets go
        // of one (0x141987B20, 0x14063E526): the last one deletes it with its type table's first function and 1.
        bool AddReference(uintptr_t instance)
        {
            if (!GameObject(instance)) {
                return false;
            }
            __try {
                _InterlockedIncrement(reinterpret_cast<volatile long*>(instance + 8));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void ReleaseReference(uintptr_t instance)
        {
            if (instance == 0) {
                return;
            }
            __try {
                if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(instance + 8), -1) == 1) {
                    const uintptr_t table = *reinterpret_cast<const uintptr_t*>(instance);
                    reinterpret_cast<DeleteFn>(*reinterpret_cast<const uintptr_t*>(table))(reinterpret_cast<void*>(instance), 1);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        // The item with this handle, as the game's equip finds it: its form and instance data (a reference added: ReleaseReference it).
        bool ItemOfHandle(uint32_t handle, uintptr_t& object, uintptr_t& instance)
        {
            object = instance = 0;
            uintptr_t handles = 0;
            if (!g_mods_ok || handle == 0xFFFFFFFF || !SafeRead(&handles, At(kHandlesRva), sizeof(handles)) || handles == 0) {
                return false;
            }
            alignas(16) uint8_t out[0x20]{};
            void*               p2    = out;
            void**              p1    = &p2;
            bool                found = false;
            __try {
                found = reinterpret_cast<ItemOfHandleFn>(At(kItemOfHandleRva))(reinterpret_cast<void*>(handles), &handle, &p1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            std::memcpy(&object, out, sizeof(object));
            std::memcpy(&instance, out + 8, sizeof(instance));
            return found && object != 0;
        }

        // An object-instance extra's mods: their form ids, sorted, "XXXXXXXX,XXXXXXXX".
        std::string ModsText(uintptr_t extra)
        {
            uint32_t  count = 0;
            uintptr_t data  = 0;
            if (extra == 0 || !SafeRead(&count, extra + kModsCount, sizeof(count)) || count == 0 || count > kModsMax ||
                !SafeRead(&data, extra + kModsData, sizeof(data)) || data == 0) {
                return {};
            }
            uint8_t raw[kModsMax * 0x10]{};
            if (!SafeRead(raw, data, count * 0x10ull)) {
                return {};
            }
            std::vector<uint32_t> ids;
            for (uint32_t i = 0; i < count; ++i) {
                uintptr_t omod = 0;
                uint32_t  id   = 0;
                std::memcpy(&omod, raw + i * 0x10ull, sizeof(omod));
                if (omod != 0 && SafeRead(&id, omod + kFormId, sizeof(id)) && id != 0) {
                    ids.push_back(id);
                }
            }
            std::sort(ids.begin(), ids.end());
            std::string text;
            for (const uint32_t id : ids) {
                text += std::format("{}{:08X}", text.empty() ? "" : ",", id);
            }
            return text;
        }

        // One of your inventory's entries of a form.
        struct Candidate
        {
            uintptr_t   instance{ 0 };
            uintptr_t   extra{ 0 }; // (its first stack's mods: their object-instance extra)
            bool        equipped{ false };
            std::string mods;
        };

        // Your inventory's entries of `form` (in its order, at most 16), handed to `use` while its read lock is held (the pointers stay
        // good meanwhile: a reference or a copy taken there outlives it). `wait`: as the game's reads (else not while a writer holds it).
        // False when it could not be read.
        template <class F>
        bool WithEntries(uintptr_t form, bool wait, F&& use)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (!g_mods_ok || player == 0 || form == 0) {
                return false;
            }
            const uintptr_t lock = player + kActorInventoryLock;
            if (!(wait ? LockRead(lock) : TryLockRead(lock))) {
                return false;
            }
            struct Unlock
            {
                uintptr_t at;
                ~Unlock() { UnlockRead(at); }
            } unlock{ lock };
            std::vector<Candidate> found;
            uintptr_t              list = 0, entries = 0;
            uint32_t               count = 0;
            if (SafeRead(&list, player + kActorInventory, sizeof(list)) && list != 0 && SafeRead(&count, list + kListCount, sizeof(count)) && count > 0 &&
                count <= 20000 && SafeRead(&entries, list + kListEntries, sizeof(entries)) && entries != 0) {
                std::vector<uint8_t> raw(static_cast<size_t>(count) * kEntrySize);
                if (SafeRead(raw.data(), entries, raw.size())) {
                    for (uint32_t i = 0; i < count && found.size() < 16; ++i) {
                        const uint8_t* e      = raw.data() + static_cast<size_t>(i) * kEntrySize;
                        uintptr_t      object = 0, stacks = 0;
                        uint32_t       number = 0, flags = 0;
                        std::memcpy(&object, e, sizeof(object));
                        if (object != form) {
                            continue;
                        }
                        Candidate c;
                        std::memcpy(&c.instance, e + kEntryInstance, sizeof(c.instance));
                        std::memcpy(&number, e + kEntryStackCount, sizeof(number));
                        std::memcpy(&stacks, e + kEntryStacks, sizeof(stacks));
                        std::memcpy(&flags, e + kEntryFlags, sizeof(flags));
                        c.equipped = (flags & 7) != 0;
                        for (uint32_t k = 0; stacks != 0 && k < std::min<uint32_t>(number, 16); ++k) {
                            uintptr_t extras = 0;
                            if (SafeRead(&extras, stacks + k * 0x10ull, sizeof(extras)) && extras != 0) {
                                if (const uintptr_t x = GetByType(extras, kInstanceExtraType); x != 0) {
                                    c.extra = x;
                                    c.mods  = ModsText(x);
                                    break;
                                }
                            }
                        }
                        found.push_back(std::move(c));
                    }
                }
            }
            use(found);
            return true;
        }

        // The entry the left weapon is: the mods as chosen first (else the first of its form); then one not equipped when the right hand
        // holds the same form (two carried: the right hand's is the game's equipped one).
        const Candidate* Pick(const std::vector<Candidate>& found, const std::string& mods, bool right_same)
        {
            const Candidate* best       = nullptr;
            int              best_score = -1;
            for (const auto& c : found) {
                const int score = (c.mods == mods ? 2 : 0) + (right_same && !c.equipped ? 1 : 0);
                if (score > best_score) {
                    best       = &c;
                    best_score = score;
                }
            }
            return best;
        }

        // The mods of the item with this handle (the menu's choice): its entry in your inventory, found by its instance data.
        std::string ModsOfHandle(uint32_t handle, uintptr_t form)
        {
            uintptr_t   object = 0, instance = 0;
            std::string mods;
            bool        found_it = false;
            if (ItemOfHandle(handle, object, instance) && object == form) {
                WithEntries(form, true, [&](const std::vector<Candidate>& found) {
                    for (const auto& c : found) {
                        if (c.instance == instance) {
                            mods     = c.mods;
                            found_it = true;
                            break;
                        }
                    }
                });
            }
            ReleaseReference(instance);
            if (g_mods_logs.fetch_add(1) < 20) {
                spdlog::info("[LeftWeapon] (fix166) the chosen item (handle {:#x}): {}", handle,
                    found_it ? std::format("its mods [{}] (instance data {:#x})", mods, instance) : std::string{ "not found in your inventory (its mods not known)" });
            }
            return mods;
        }

        bool RightHoldsSame(uintptr_t form) { return form != 0 && HandGrab::WeaponInHandsForm() == form; }

        // (Main thread, each frame) The left weapon's instance data kept for its hits: found again each second, and at once after a choice.
        void KeepAttackInstance(uintptr_t form)
        {
            const int64_t  now    = NowMs();
            const uint32_t choice = g_choice.load();
            if (form == g_attack_form && choice == g_attack_choice && now - g_attack_at < 1000) {
                return;
            }
            g_attack_at     = now;
            g_attack_choice = choice;
            uintptr_t   instance = 0;
            std::string mods;
            if (form != 0) {
                const std::string want = Mods();
                const bool        same = RightHoldsSame(form);
                const bool        read = WithEntries(form, false, [&](const std::vector<Candidate>& found) {
                    if (const Candidate* c = Pick(found, want, same); c != nullptr && c->instance != 0) {
                        // (A new one: its reference taken under the inventory's lock, so it cannot go before this.)
                        if (c->instance == g_attack_instance || AddReference(c->instance)) {
                            instance = c->instance;
                            mods     = c->mods;
                        }
                    }
                });
                if (!read) {
                    // (Not read now (a writer holds your inventory): tried again next frame; another weapon's item is not kept meanwhile.)
                    g_attack_at = 0;
                    if (form != g_attack_form) {
                        ReleaseReference(g_attack_instance);
                        g_attack_instance = 0;
                        g_attack_form     = 0;
                    }
                    return;
                }
            }
            if (instance != g_attack_instance) {
                ReleaseReference(g_attack_instance);
                g_attack_instance = instance;
                if (g_mods_logs.fetch_add(1) < 20) {
                    uint32_t id = 0;
                    SafeRead(&id, form + kFormId, sizeof(id));
                    spdlog::info("[LeftWeapon] (fix166) the left weapon's hits {}", instance != 0 ? std::format("use its own item: {:08X}, instance data {:#x}, mods [{}]", id, instance, mods)
                                                                                                   : std::string{ "have no item of their own (none found)" });
                }
            }
            g_attack_form = form;
        }

        // Sets the left weapon and its mods (any thread).
        void Choose(uint32_t id, const std::string& mods, const char* why, bool yours = false)
        {
            bool     changed = false;
            uint32_t was     = 0;
            {
                std::scoped_lock _{ g_mods_mtx };
                was     = g_form_id.load();
                changed = g_mods != mods || was != id;
                g_mods  = mods;
                g_form_id.store(id);
                if (changed) {
                    // (fix169) Your own choice (the left trigger on it): out at once, as a weapon chosen for the right hand is drawn. Noted
                    // before the serial moves on, so Update never sees the new choice without it; both under the lock (SetMods' too), so
                    // another choice meanwhile cannot take its serial (the review).
                    const uint32_t serial = g_choice.load() + 1;
                    if (yours) {
                        g_out_choice.store(serial);
                    }
                    g_choice.store(serial);
                }
            }
            if (changed) {
                g_changed.store(true);
                if (g_logs.fetch_add(1) < 40) {
                    spdlog::info("[LeftWeapon] the left hand's weapon {}: {:08X}{} ({})", id != 0 ? "now" : "taken out", id != 0 ? id : was,
                        mods.empty() ? std::string{} : std::format(", its mods [{}]", mods), why);
                }
            }
        }

        // (fix168) The option chosen, "Move from right hand": a weapon chosen for the left hand that your right hand holds, carried once, is
        // taken from the right (as Skyrim's menus do with one copy; SKSE's EquipItemEx refuses one item in two hands); before, the left let
        // go of it 1.5 s later. Asked here (any thread: the menu's, the favorites' job thread), done by Update on the main thread: the
        // game's unequip takes the inventory's write lock, never from a hook (a helper's research). After Choose (its serial).
        void AskMove(uint32_t id)
        {
            g_move_req.store((static_cast<uint64_t>(g_choice.load()) << 32) | id, std::memory_order_release);
        }
    }

    namespace
    {
        // The inventory's item selected, its form found (rsi; the item's handle at [rbp+0x50]): a melee weapon chosen with the left
        // trigger becomes the left hand's, and the worker goes straight to its end (the handle's hold let go there, as always).
        void OnSelectItem(safetyhook::Context& ctx)
        {
            uint32_t handle = 0xFFFFFFFF;
            SafeRead(&handle, ctx.rbp + 0x50, sizeof(handle)); // (fix166: the item's handle, for its mods)
            if (TakeMenuEquip(ctx.rsi, handle)) {
                ctx.rip = At(kSkipRva);
            }
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        g_lookup_ok = g_base != 0 && SameCode(kLookupRva, kLookupStart, sizeof(kLookupStart)) && JumpTarget(At(kGetFormRva + 3)) == At(kLookupRva);
        g_ok        = g_lookup_ok;
        if (!g_ok) {
            spdlog::warn("[LeftWeapon] dual wielding off: the game's form lookup is not as in Starfield 1.16.236");
            return;
        }
        // The inventory's "item selected" worker, its form found, its equip call and its way out, as in 1.16.236. (fix190) The places used
        // without a check of their own code (the equip, the hook's place) known in this game version.
        const bool select_known = GameCode::Known({ kEquipRva, kHookRva });
        if (!select_known) {
            spdlog::warn("[LeftWeapon] the inventory's item selection is not known in this game version (GameCode): off");
        }
        const bool code = select_known && SameCode(kWorkerRva, kWorkerStart, sizeof(kWorkerStart)) && SameCode(kFoundRva, kFound, sizeof(kFound)) &&
                          CallTarget(At(kEquipCallRva)) == At(kEquipRva) && SameCode(kSkipRva, kSkip, sizeof(kSkip));
        if (code) {
            g_select_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kHookRva)), &OnSelectItem);
        }
        if (!g_select_hook) {
            g_ok = false;
            spdlog::warn("[LeftWeapon] dual wielding off: the inventory's item selection is not as in Starfield 1.16.236 (or could not be hooked)");
            return;
        }
        spdlog::info("[LeftWeapon] dual wielding: the inventory's item selection hooked (rva {:x}): a melee weapon chosen with the left trigger goes "
                     "to the left hand (the game's equip not made); the form lookup checked (rva 5def30)",
            kHookRva);
        // (fix157) The favorites' choice, as checked against 1.16.236. (fix190) The places used without a check of their own code (the
        // equip, the hook's place, the handles) known in this game version.
        const bool favorites_known = GameCode::Known({ kEquipRva, kQuickkeyHookRva, kHandlesRva });
        if (!favorites_known) {
            spdlog::warn("[LeftWeapon] (fix157) the favorites' choice is not known in this game version (GameCode): off");
        }
        const bool favorites = favorites_known && SameCode(kUseQuickkeyRva, kUseQuickkeyStart, sizeof(kUseQuickkeyStart)) &&
                               SameCode(kQuickkeyFindRva, kQuickkeyFind, sizeof(kQuickkeyFind)) &&
                               CallTarget(At(kQuickkeyEquipCallRva)) == At(kEquipRva) &&
                               SameCode(kQuickkeyOutRva, kQuickkeyOut, sizeof(kQuickkeyOut)) &&
                               SameCode(kHandleFormRva, kHandleFormStart, sizeof(kHandleFormStart)) &&
                               CallTarget(At(kHandleFormCallRva)) == At(kHandleFormRva);
        if (favorites) {
            g_quickkey_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kQuickkeyHookRva)), &OnUseQuickkey);
        }
        spdlog::info("[LeftWeapon] (fix157) the favorites' choice {} (rva {:x})",
            g_quickkey_hook ? "hooked: a melee weapon chosen there with the left trigger goes to the left hand; the right trigger chooses as A does"
                            : "not as in Starfield 1.16.236 (the left trigger there does as the right)",
            kQuickkeyHookRva);
        // (fix166) The chosen item's mods and instance data, as checked against 1.16.236 (see kItemOfHandleRva and below).
        {
            // (fix190) The places used without a check of their own code (the waking import, the handles, the copy's build) known in this
            // game version.
            const bool mods_known = GameCode::Known({ kWakeImportRva, kHandlesRva, kBuildRva });
            if (!mods_known) {
                spdlog::warn("[LeftWeapon] (fix166) the chosen item's own mods and instance data: not known in this game version (GameCode): off");
            }
            uintptr_t       table_first = 0;
            const bool      item = SameCode(kItemOfHandleRva, kItemOfHandleStart, sizeof(kItemOfHandleStart)) && SameCode(kItemLockRva, kItemLock, sizeof(kItemLock)) &&
                              CallTarget(At(kItemLockRva + 7)) == At(kLockReadRva) && SameCode(kItemListRva, kItemList, sizeof(kItemList)) &&
                              CallTarget(At(kItemCopyCallRva)) == At(kItemCopyRva) && SameCode(kItemCopyRva, kItemCopyStart, sizeof(kItemCopyStart)) &&
                              SameCode(kItemUnlockRva, kItemUnlock, sizeof(kItemUnlock)) &&
                              RipOperand(At(kItemUnlockRva + sizeof(kItemUnlock) - 2), 6) == At(kWakeImportRva) &&
                              RipOperand(At(kEquipHandlesRva), 7) == At(kHandlesRva) && CallTarget(At(kEquipHandlesRva + 7)) == At(kItemOfHandleRva);
            const bool lock    = SameCode(kLockReadRva, kLockReadStart, sizeof(kLockReadStart));
            const bool extras  = SameCode(kGetByTypeRva, kGetByTypeStart, sizeof(kGetByTypeStart)) && CallTarget(At(kGetByTypeRva + kGetByTypeLock)) == At(kLockReadRva);
            const bool copying = SameCode(kCloneRva, kCloneStart, sizeof(kCloneStart)) && SameCode(kCloneTypeRva, kCloneType, sizeof(kCloneType)) &&
                                 CallTarget(At(kCloneBuildCallRva)) == At(kBuildRva) && SameCode(kBuildTableRva, kBuildTable, sizeof(kBuildTable)) &&
                                 RipOperand(At(kBuildTableRva + 4), 7) == At(kInstanceExtraTable) && SafeRead(&table_first, At(kInstanceExtraTable), sizeof(table_first)) &&
                                 table_first == At(kFreeRva) && SameCode(kFreeRva, kFreeStart, sizeof(kFreeStart)) &&
                                 RipOperand(At(kFreeRva + sizeof(kFreeStart) - 3), 7) == At(kInstanceExtraTable) && SameCode(kFreeFlagRva, kFreeFlag, sizeof(kFreeFlag));
            g_mods_ok = mods_known && item && lock && extras && copying;
            if (g_mods_ok) {
                spdlog::info("[LeftWeapon] (fix166) the chosen item's own mods and instance data: ready (its handle read as the game's equip reads it, rva {:x}; "
                             "your inventory read under its own lock, rva {:x}; its mods copied as the game copies them, rva {:x}); the left copy gets them, "
                             "and the left hand's hits its own item",
                    kItemOfHandleRva, kLockReadRva, kCloneRva);
            } else {
                spdlog::warn("[LeftWeapon] (fix166) the chosen item's own mods and instance data: off (not as in Starfield 1.16.236: the item by its handle {}, "
                             "the read lock {}, the extra data lookup {}, the copy and free of the mods {}); the left copy keeps the weapon's default parts and "
                             "the left hand's hits the right weapon's damage",
                    item ? "ok" : "differs", lock ? "ok" : "differs", extras ? "ok" : "differs", copying ? "ok" : "differs");
            }
        }
    }

    void SetFormId(uint32_t id, const char* why)
    {
        // (fix166) Another form forgets the mods chosen with the one before (the settings' load sets its own after).
        if (id != g_form_id.load()) {
            Choose(id, std::string{}, why);
        }
    }

    uint32_t FormId() { return g_form_id.load(); }

    std::string Mods()
    {
        std::scoped_lock _{ g_mods_mtx };
        return g_mods;
    }

    void SetMods(const std::string& mods)
    {
        {
            std::scoped_lock _{ g_mods_mtx };
            if (g_mods == mods) {
                return;
            }
            g_mods = mods;
            g_choice.fetch_add(1); // (fix169: under the lock, as Choose's)
        }
        g_changed.store(true);
    }

    uint32_t ChoiceSerial() { return g_choice.load(); }

    uintptr_t LookupForm(uint32_t id) { return Lookup(id); }

    bool TakeChanged(uint32_t& id, std::string& mods)
    {
        if (!g_changed.exchange(false)) {
            return false;
        }
        std::scoped_lock _{ g_mods_mtx };
        id   = g_form_id.load();
        mods = g_mods;
        return true;
    }

    uintptr_t CloneModsExtra(uintptr_t form, std::string& ids, bool& busy, bool may_wait)
    {
        ids.clear();
        busy = false;
        if (!g_mods_ok || form == 0) {
            return 0;
        }
        const std::string want = Mods();
        const bool        same = RightHoldsSame(form);
        uintptr_t         made = 0;
        const bool read = WithEntries(form, may_wait, [&](const std::vector<Candidate>& found) {
            if (const Candidate* c = Pick(found, want, same); c != nullptr && c->extra != 0) {
                uintptr_t table = 0;
                if (SafeRead(&table, c->extra, sizeof(table)) && table == At(kInstanceExtraTable)) {
                    made = CloneExtra(c->extra); // (under the inventory's lock: its mods cannot change meanwhile)
                    ids  = c->mods;
                }
            }
        });
        busy = !read && !may_wait && CreationEngineSingletonManager::GetPlayerRef() != nullptr;
        return made;
    }

    void FreeModsExtra(uintptr_t extra)
    {
        uintptr_t table = 0;
        if (!g_mods_ok || extra == 0 || !SafeRead(&table, extra, sizeof(table)) || table != At(kInstanceExtraTable)) {
            return; // (not one of these: left alone)
        }
        __try {
            reinterpret_cast<DeleteFn>(At(kFreeRva))(reinterpret_cast<void*>(extra), 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    bool ReferenceHasExtra(uintptr_t ref, uintptr_t extra, std::string& ids)
    {
        ids.clear();
        uintptr_t list = 0;
        if (!g_mods_ok || ref == 0 || !SafeRead(&list, ref + kRefExtraList, sizeof(list)) || list == 0) {
            return false;
        }
        const uintptr_t on = GetByType(list, kInstanceExtraType);
        ids                = ModsText(on);
        return on != 0 && on == extra;
    }

    bool AttackInstance(uintptr_t& form, uintptr_t& instance)
    {
        form     = g_attack_form;
        instance = g_attack_instance;
        return g_mods_ok && form != 0 && instance != 0;
    }

    int Kind() { return g_held.load(std::memory_order_relaxed) ? g_kind.load(std::memory_order_relaxed) : 0; }

    bool Held() { return g_held.load(std::memory_order_relaxed); }

    bool PlayerSeated()
    {
        // (fix189) The sit state as the game's own test reads it (ActorState 0x1418EEC20, Physical's kActorState2): bits 11-12 of the
        // actor's +0xFC, 0 none, 1 getting in, 2 sitting or lying, 3 getting out.
        constexpr uintptr_t kActorState2 = 0xFC;
        const auto          player       = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
        uint32_t            state2       = 0;
        return player != 0 && SafeRead(&state2, player + kActorState2, sizeof(state2)) && ((state2 >> 11) & 3u) != 0;
    }

    bool TakeMenuEquip(uintptr_t form, uint32_t handle)
    {
        const auto& s = GameFlow::gStore.internalSettings;
        int         hand = -1;
        int64_t     at   = 0;
        static auto vr   = VR::get();
        // (The left trigger's press, recent (the menu sends its choice as the button comes up, maybe a moment later from its queue), and
        // not taken by an equip already.)
        if (!g_ok || !s.dualWield || form == 0 || vr == nullptr || !vr->is_hmd_active() || !InventoryShown() || !MenuPointer::LastPress(hand, at) ||
            hand != 0 || NowMs() - at > kPressRecentMs) {
            return false;
        }
        // (The same press's choice of the same item again (the menu's event direct and queued): kept from the game's equip too, nothing
        // changed; the review.)
        if (g_press_used.load() == at) {
            // (fix166: a press that did nothing holds back only its own event again (direct and queued, within 0.2 s): A on the same item a
            // moment later is the game's again; the review.)
            const int64_t nothing = g_press_nothing_ms.load();
            return g_press_form.load() == form && (nothing == 0 || NowMs() - nothing <= 200);
        }
        g_press_form.store(form);
        g_press_used.store(at);
        g_press_nothing_ms.store(0);
        uint32_t id = 0;
        SafeRead(&id, form + kFormId, 4);
        if (MeleeKind(form) == 0 || id == 0) {
            // (fix166) Not a melee weapon, chosen with the left trigger (the left trigger chooses only the left hand's melee
            // weapon; anything else it does nothing with): the game's equip (or use) not made, nothing changed.
            g_press_nothing_ms.store(NowMs());
            if (g_mods_logs.fetch_add(1) < 20) {
                spdlog::info("[LeftWeapon] (fix166) the left trigger chose {:08X} in the inventory: not a melee weapon, so nothing is done", id);
            }
            return true;
        }
        const std::string mods = ModsOfHandle(handle, form); // (fix166: its own mods, for its copy)
        g_taken.fetch_add(1);
        haptics::MeleePulse(true, 0.45f, 0.10f); // (the choice felt in the left hand: the menu shows nothing of it; the review)
        if (id == g_form_id.load() && mods == Mods()) {
            Choose(0, std::string{}, "the left trigger on it again in the inventory");
        } else {
            Choose(id, mods, "chosen with the left trigger in the inventory", true);
            AskMove(id);
        }
        return true; // (the game's equip not made: no flicker, no sound)
    }

    void NoteFavoritesPress(bool left)
    {
        g_fav_hand.store(left ? 0 : 1, std::memory_order_relaxed);
        g_fav_ms.store(NowMs(), std::memory_order_release);
    }

    bool TakeFavoriteEquip(uintptr_t form, uint32_t handle)
    {
        const auto& s    = GameFlow::gStore.internalSettings;
        static auto vr   = VR::get();
        const int64_t at = g_fav_ms.load(std::memory_order_acquire);
        // (The left trigger's choice in the favorites menu, recent, not taken already; anything else (A, the right trigger, a hotkey) is the
        // game's own use.)
        if (!g_ok || !s.dualWield || form == 0 || vr == nullptr || !vr->is_hmd_active() || at == 0 || g_fav_hand.load() != 0 ||
            NowMs() - at > kFavoritesRecentMs || !GameFlow::isFavoritesShown()) { // (the wheel open: not a keyboard hotkey just after; the review)
            return false;
        }
        if (g_fav_used.load() == at) {
            return g_fav_form.load() == form; // (the same choice again: kept from the game's equip too)
        }
        g_fav_form.store(form);
        g_fav_used.store(at);
        uint32_t id = 0;
        SafeRead(&id, form + kFormId, 4);
        if (MeleeKind(form) == 0 || id == 0) {
            // (fix166) Not a melee weapon, chosen with the left trigger: nothing done (as in the inventory).
            if (g_mods_logs.fetch_add(1) < 20) {
                spdlog::info("[LeftWeapon] (fix166) the left trigger chose {:08X} in the favorites: not a melee weapon, so nothing is done", id);
            }
            return true;
        }
        const std::string mods = ModsOfHandle(handle, form);
        g_taken.fetch_add(1);
        haptics::MeleePulse(true, 0.45f, 0.10f);
        if (id == g_form_id.load() && mods == Mods()) {
            Choose(0, std::string{}, "the left trigger on it again in the favorites");
        } else {
            Choose(id, mods, "chosen with the left trigger in the favorites", true);
            AskMove(id);
        }
        return true;
    }

    namespace
    {
        // (fix171) The favorites wheel chose this item for the left hand (its own UseQuickkey call, this thread): a melee weapon becomes the
        // left hand's (its own mods, as the favorites' left trigger); anything else nothing. True: the left hand took it.
        bool TakeWheelEquip(uintptr_t form, uint32_t handle)
        {
            const auto& s = GameFlow::gStore.internalSettings;
            if (!g_ok || !s.dualWield || form == 0) {
                return false;
            }
            uint32_t id = 0;
            SafeRead(&id, form + kFormId, 4);
            if (MeleeKind(form) == 0 || id == 0) {
                if (g_mods_logs.fetch_add(1) < 20) {
                    spdlog::info("[LeftWeapon] (fix171) the favorites wheel chose {:08X} for the left hand: not a melee weapon, so nothing is done", id);
                }
                return false;
            }
            const std::string mods = ModsOfHandle(handle, form);
            g_taken.fetch_add(1);
            haptics::MeleePulse(true, 0.45f, 0.10f);
            if (id == g_form_id.load() && mods == Mods()) {
                Choose(0, std::string{}, "the left hand on it again on the favorites wheel");
            } else {
                Choose(id, mods, "chosen with the left hand on the favorites wheel", true);
                AskMove(id);
            }
            return true;
        }

        // The favorites' UseQuickkey, its item's handle found ([rbp+0x67]; -1: a power): a melee weapon chosen with the left trigger becomes
        // the left hand's, and the call goes out its usual way with dil 1.
        void OnUseQuickkey(safetyhook::Context& ctx)
        {
            uint32_t handle = 0xFFFFFFFF;
            // (fix171) The favorites wheel's own use: the right hand's is the game's own (whatever the favorites' trigger noted last); the left
            // hand's never the game's equip (into your right hand): a melee weapon taken as the left hand's, anything else nothing.
            if (const int wheel = t_wheel_hand; wheel == 1) {
                return;
            } else if (wheel == 0) {
                const bool read = SafeRead(&handle, ctx.rbp + 0x67, sizeof(handle));
                t_wheel_result  = read && handle != 0xFFFFFFFF && TakeWheelEquip(FormOfHandle(handle), handle) ? 1 : 2;
                ctx.rdi         = (ctx.rdi & ~0xFFull) | 1; // (dil 1: "used")
                ctx.rip         = At(kQuickkeyOutRva);
                return;
            }
            if (!SafeRead(&handle, ctx.rbp + 0x67, sizeof(handle)) || g_fav_hand.load(std::memory_order_relaxed) != 0) {
                return;
            }
            // (fix166) A power (no item: handle -1) chosen with the left trigger: nothing (nothing for the left hand if not a melee
            // weapon), as for an item that is not a melee weapon (TakeFavoriteEquip).
            if (handle == 0xFFFFFFFF) {
                // (Only the left trigger's own choice, as TakeFavoriteEquip tells it: recent, on the open wheel, not taken already, dual
                // wielding on, in the headset; else a power (or an empty slot) is the game's as always: the last trigger noted stays the
                // left one until another is pulled, and a hotkey or A later must still use the power; the review.)
                const auto&   s  = GameFlow::gStore.internalSettings;
                static auto   vr = VR::get();
                const int64_t at = g_fav_ms.load(std::memory_order_acquire);
                if (!g_ok || !s.dualWield || vr == nullptr || !vr->is_hmd_active() || at == 0 || NowMs() - at > kFavoritesRecentMs ||
                    !GameFlow::isFavoritesShown() || g_fav_used.load() == at) {
                    return;
                }
                g_fav_used.store(at);
                g_fav_form.store(0);
                static int told = 0;
                if (told++ < 5) {
                    spdlog::info("[LeftWeapon] (fix166) a power chosen with the left trigger in the favorites: nothing done (the left hand takes melee weapons)");
                }
                ctx.rdi = (ctx.rdi & ~0xFFull) | 1; // (dil 1: "used")
                ctx.rip = At(kQuickkeyOutRva);
                return;
            }
            if (const uintptr_t form = FormOfHandle(handle); TakeFavoriteEquip(form, handle)) {
                ctx.rdi = (ctx.rdi & ~0xFFull) | 1; // (dil 1: "used")
                ctx.rip = At(kQuickkeyOutRva);
            }
        }
    }

    void Update()
    {
        static auto vr   = VR::get();
        const auto& s    = GameFlow::gStore.internalSettings;
        const uint32_t id = g_form_id.load();
        // (Looked up again while not found: at the start the game's data is not loaded yet (the saved choice is kept then; the review). A
        // form found and not a melee weapon is taken out of the left hand.)
        static int64_t looked_at = 0;
        if (id != g_cached_id || (id != 0 && g_cached_form == 0 && NowMs() - looked_at > 1000)) {
            looked_at     = NowMs();
            g_cached_id   = id;
            g_cached_form = Lookup(id);
            g_cached_kind = MeleeKind(g_cached_form);
            g_missing_since = 0;
            if (id != 0 && g_cached_form != 0 && g_cached_kind == 0) {
                spdlog::warn("[LeftWeapon] {:08X} is not a melee weapon: taken out of the left hand", id);
                SetFormId(0, "not a melee weapon");
                g_cached_id = 0;
            }
        }
        // (fix168) Moved from the right hand (AskMove): decided a frame after the choice, with the count read afresh and the same test of
        // "the right hand holds it" as the carried check below (the equipped weapon, drawn or put away). The menu may still be open (the
        // game's own menus equip while open). Waits (at most 2 s) while your weapon is being drawn or put away or is thrown.
        {
            static int64_t  asked_at  = 0;
            static uint64_t asked_req = 0; // (the request asked_at is for: a newer one starts its waits afresh; the review)
            static int      move_logs = 0;
            const auto     note = [&](const std::string& text) {
                if (move_logs++ < 40) {
                    spdlog::info("[LeftWeapon] (fix168) {}", text);
                }
            };
            const int64_t  now  = NowMs();
            uint64_t       req  = g_move_req.load(std::memory_order_acquire);
            const uint32_t want = static_cast<uint32_t>(req);
            if (req != 0) {
                if (asked_req != req) {
                    asked_req = req;
                    asked_at  = now;
                }
                const int state = GameFlow::weaponState();
                if (static_cast<uint32_t>(req >> 32) != g_choice.load() || !g_ok || !s.dualWield) {
                    g_move_req.compare_exchange_strong(req, 0);
                    asked_at = 0; // (another choice since, or off: nothing)
                } else if (want != id) {
                    // (chosen on the favorites' thread after this frame read the left weapon: next frame)
                } else if (g_cached_form == 0 && now - asked_at < 5000) {
                    // (its form not found yet)
                } else if ((state == 1 || state == 2 || state == 4 || state == 5 || MeleeThrow::Away(false)) && now - asked_at < 2000) {
                    // (your weapon being drawn or put away, or thrown: when it is in your hand again)
                } else if (!g_move_req.compare_exchange_strong(req, 0)) {
                    // (a newer request came meanwhile: next frame)
                } else {
                    asked_at                = 0;
                    const int      carried  = g_cached_form != 0 ? CarriedCount(g_cached_form) : -1;
                    const uint32_t right    = HandGrab::WeaponInHandsId();
                    if (g_cached_form == 0) {
                        note(std::format("{:08X}: its form not found: nothing moved", want));
                    } else if (right != want) {
                        note(right != 0 ? std::format("{:08X}: your right hand holds {:08X}: nothing to move", want, right)
                                        : std::format("{:08X}: your right hand holds no weapon: nothing to move", want));
                    } else if (carried >= 2) {
                        note(std::format("{:08X}: {} carried: one in each hand, nothing moved", want, carried));
                    } else if (carried == 1) {
                        if (Consume::Unequip(g_cached_form, "moved to the left hand")) {
                            g_moving_id    = want;
                            g_moving_since = now;
                            g_moving_until = now + 3000;
                            note(std::format("{:08X}: only one carried and your right hand has it ({}, state {}): taken from the right hand",
                                want, state == 3 ? "drawn" : state == 0 ? "put away" : "being drawn or put away", state));
                        } else {
                            note(std::format("{:08X}: the game's unequip could not be asked for: the left lets go of it 1.5 s later (as before)", want));
                        }
                    } else {
                        note(std::format("{:08X}: your inventory could not be counted: nothing moved", want));
                    }
                }
            }
            // While the game takes it from the right hand (queued: it may wait for the menu to close): the left holds it meanwhile.
            if (g_moving_id != 0) {
                if (GameFlow::isShowingMenu() || GameFlow::isFavoritesShown()) {
                    g_moving_until = (std::max)(g_moving_until, now + 3000);
                }
                if (g_moving_id != id) {
                    g_moving_id = 0; // (another left weapon chosen meanwhile)
                } else if (HandGrab::WeaponInHandsId() != g_moving_id) {
                    note(std::format("{:08X}: your right hand let go of it after {} ms (now {:08X}; 0: your fists or none)", g_moving_id,
                        now - g_moving_since, HandGrab::WeaponInHandsId()));
                    haptics::MeleePulse(false, 0.35f, 0.08f);
                    g_moving_id = 0;
                } else if (now > g_moving_until) {
                    note(std::format("{:08X}: the game kept it in your right hand: the left lets go of it (as before)", g_moving_id));
                    g_moving_id = 0;
                }
            }
        }
        // (fix157, fix158) Putting away and drawing (the left hand's melee weapon sheathed with two quick Y presses, both
        // sheathed with Y held; the right trigger draws the right weapon, not both, and the left trigger draws the left
        // weapon if it is equipped): your weapons put away by the game (Y held) put the left one away too; drawn again by the game, the left
        // one stays away unless Y was held for it (Y held draws both); the left trigger draws it (on its own when nothing else is out:
        // shown and thrown, though the game's own melee needs your weapon out); two quick presses of Y put it away or draw it.
        // (fix169) After fix168 the lightsaber equipped to the left hand did not stay correctly in the hand; now it works as the right
        // hand's. It was shown only while the game said your weapon was out (or with the left drawn on its own and your weapon put away), so
        // the game's own changes took it away: moved from your right hand, the game swapped that hand to fists (its weapon state out of "out"
        // for 0.15 s: the left copy deleted and made again) and put the fists away 2.3 s later (deleted again: fix168's 2 s window missed
        // it); it stayed only after a left trigger pull. Now the left weapon is out or put away by itself, as a weapon in your right hand is:
        // out as soon as you choose it (the left trigger in the inventory or the favorites; one kept from the last session comes out with
        // your weapon); put away and drawn only by you: Y held while the game puts your weapons away (both away) or draws them (both out),
        // two quick presses of Y (the left alone), the left trigger (drawn). The game's own changes (the swap to fists, its own putting away,
        // your weapon drawn by the right trigger) leave it as it is. Its own hits need no weapon of yours out (ImpactMelee).
        {
            const int       state = GameFlow::weaponState();
            static int      state_was   = -1;
            static uint32_t seen_choice = 0;
            // (A choice not yours (kept from the last session, its mods set as the settings load at the main menu): decided once you are in
            // the world, as your weapon is then: out with it if it comes out within 2 s, else put away; the review.)
            static bool     pending     = false;
            static int64_t  in_world_ms = 0;
            // (Decided only when the choice and the form read at the top agree: chosen on another thread between the two, it is decided next
            // frame, never skipped; the review.)
            if (const uint32_t choice = g_choice.load(); choice != seen_choice && g_form_id.load() == id) {
                seen_choice = choice;
                pending     = false;
                if (id != 0) {
                    if (g_out_choice.load() == choice) {
                        g_sheathed.store(false);
                        if (g_logs.fetch_add(1) < 40) {
                            spdlog::info("[LeftWeapon] (fix169) {:08X} chosen by you: out at once", id);
                        }
                    } else {
                        pending     = true; // (hidden until decided)
                        in_world_ms = 0;
                        g_sheathed.store(true);
                    }
                }
            }
            if (pending) {
                const int64_t now      = NowMs();
                const bool    in_world = state >= 0 && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown() && GameFlow::isInFirstPerson(); // (fix175: not build mode)
                if (!in_world) {
                    in_world_ms = 0;
                } else {
                    if (in_world_ms == 0) {
                        in_world_ms = now;
                    }
                    if (state == 3 || now - in_world_ms > 2000) {
                        pending = false;
                        g_sheathed.store(state != 3);
                        if (g_logs.fetch_add(1) < 40) {
                            spdlog::info("[LeftWeapon] (fix169) {:08X} kept from before: {}", id,
                                state == 3 ? "out with your weapon" : "put away, as your weapon is (the left trigger draws it)");
                        }
                    }
                }
            }
            const auto told = [&](const char* what, const char* how) {
                haptics::MeleePulse(true, 0.35f, 0.08f);
                if (g_logs.fetch_add(1) < 40) {
                    spdlog::info("[LeftWeapon] (fix169) the left hand's weapon {} ({})", what, how);
                }
            };
            // (Y held: seen held 0.3 s within the last second, as the game's own putting away and drawing need it held.)
            const bool y_held = NowMs() - g_y_held_ms.load() < 1000;
            if (state_was == 3 && state != 3 && state >= 0 && y_held && id != 0 && !g_sheathed.load()) {
                g_sheathed.store(true);
                pending = false;
                told("put away", "Y held: your weapons put away");
            }
            // (Drawn by the game: the left one with it when Y is held for it: decided as Y is held 0.3 s (both) or let go (the right only);
            // the review.)
            static bool deciding = false;
            if (state_was == 0 && state > 0) {
                deciding = true;
            }
            if (deciding) {
                if (NowMs() - g_y_held_ms.load() < 600) {
                    if (id != 0 && g_sheathed.load()) {
                        g_sheathed.store(false);
                        pending = false;
                        told("drawn", "Y held: your weapons drawn");
                    }
                    deciding = false;
                } else if (!g_y_down.load() || state == 0) {
                    deciding = false;
                }
            }
            state_was = state;
            if (g_left_pull.exchange(false) && id != 0 && g_sheathed.load()) {
                g_sheathed.store(false);
                pending = false; // (yours to say now)
                told("drawn", "the left trigger");
            }
            if (g_put_alone.exchange(false) && id != 0 && !g_sheathed.load()) {
                g_sheathed.store(true);
                pending = false;
                told("put away", "Y held with your own weapon away: the left one alone"); // (fix179)
            }
            if (g_toggle.exchange(false) && id != 0) {
                const bool away = !g_sheathed.load();
                g_sheathed.store(away);
                pending = false;
                told(away ? "put away" : "drawn again", "two quick presses of Y");
            }
        }
        const uintptr_t form = g_cached_id != 0 ? g_cached_form : 0;
        KeepAttackInstance(g_ok && s.dualWield ? form : 0); // (fix166: its own item, for its hits)
        std::string     why{};
        bool            show = false;
        if (!g_ok) {
            why = "off (the game's code is not as expected)";
        } else if (!s.dualWield) {
            why = "off (the setting)";
        } else if (form == 0) {
            why = id != 0 ? "its form not found yet (the game's data still loading)" : "no left weapon (choose one with the left trigger in the inventory)";
        } else if (vr == nullptr || !vr->is_hmd_active() || !vr->is_using_controllers()) {
            why = "not in VR with controllers";
        } else if (GameFlow::isShowingMenu() || GameFlow::isInDialogue()) {
            why = "a menu or a conversation";
        } else if (GameFlow::isWorkshopShown()) {
            why = "build mode (fix175: hidden as in menus)";
        } else if (!GameFlow::isInFirstPerson() || GameFlow::isPilotingShip() || GameFlow::isOnLadder() || LadderClimb::ViewAtHead()) {
            why = "not on foot in first person";
        } else if (PlayerSeated()) {
            why = "you are sitting or lying (fix189: hidden as in the pilot's seat)";
        } else if (g_sheathed.load()) {
            // (fix169: your weapon's own state no longer matters: the left one is out or away by itself.)
            why = "put away (two quick presses of Y or the left trigger draw it)";
        } else if (HandGrab::HoldingWith(true) || Physical::HoldingPerson(true)) {
            why = "your left hand holds something (the hand grab, or a person: fix173)";
        } else {
            // Carried (two of it when the right hand holds the same one). Missing for a moment (a transfer) is not yet gone.
            // (Counted four times a second: the inventory read each time.)
            static int     carried_then = -1;
            static int64_t counted_at   = 0;
            static uint32_t counted_id  = 0;
            if (const int64_t now = NowMs(); now - counted_at > 250 || counted_id != g_cached_id) {
                counted_at   = now;
                counted_id   = g_cached_id;
                carried_then = CarriedCount(form);
            }
            const int  carried = carried_then;
            const bool same    = HandGrab::WeaponInHandsId() == g_cached_id;
            const int  need    = same && g_moving_id != g_cached_id ? 2 : 1; // (fix168: while taken from the right, one is enough)
            if (carried >= 0 && carried < need) {
                const int64_t now = NowMs();
                if (g_missing_since == 0) {
                    g_missing_since = now;
                }
                if (now - g_missing_since > kMissingMs) {
                    SetFormId(0, same ? "only one carried, and the right hand holds it" : "no longer carried");
                }
                why = same ? "only one carried (the right hand holds it)" : "not carried";
            } else {
                g_missing_since = 0;
                show            = true;
                why             = "in your left hand";
            }
        }
        if (!show && why.find("carried") == std::string::npos) {
            g_missing_since = 0; // (not checked this frame: its grace starts afresh next time; the review)
        }
        GrenadeHand::WantLeftWeapon(show ? form : 0);
        // (fix156) Held also while it flies from a throw (its grip calls it back: still not the hand grab's); no strike reach meanwhile.
        const bool held = show && GrenadeHand::LeftWeaponOut();
        g_held.store(held, std::memory_order_relaxed);
        g_kind.store(held && !MeleeThrow::Away(true) ? g_cached_kind : 0, std::memory_order_relaxed);
        HandGrab::SetLeftGripWeapon(held); // (its grip is the weapon's: no grab, no gloves' aim with it; its d-pad stays)
        if (why != g_shown_why) {
            g_shown_why = why;
            SetStatus(std::format("Dual wielding: {}{}", id != 0 ? std::format("{:08X}, ", id) : std::string{}, why));
            if (g_logs.fetch_add(1) < 40) {
                spdlog::info("[LeftWeapon] {}", why);
            }
        }
    }

    bool WheelReady() { return g_ok && static_cast<bool>(g_quickkey_hook) && GameFlow::gStore.internalSettings.dualWield; }

    void BeginWheelUse(bool left)
    {
        t_wheel_hand   = left ? 0 : 1;
        t_wheel_result = 0;
    }

    int EndWheelUse()
    {
        t_wheel_hand = -1;
        return t_wheel_result;
    }

    void ToggleSheathed() { g_toggle.store(true); }

    // (fix179) After fix178: with the right melee weapon put away and the left one out, Y held should sheath the left one (held, the game
    // took it as "draw your weapons": the right one drawn, the left kept). With your own weapon put away (the game's weapon state 0) and the
    // left one out, Y held puts the left one away alone (ModConfig holds that press back from the game).
    bool OnlyLeftOut()
    {
        return g_ok && GameFlow::gStore.internalSettings.dualWield && g_form_id.load() != 0 && !g_sheathed.load() && GameFlow::weaponState() == 0;
    }

    void PutAwayAlone() { g_put_alone.store(true); }

    void LeftTriggerPulled() { g_left_pull.store(true); }

    void NoteYHeld(bool down)
    {
        // (Held: down for 0.3 s at least, as the game's own putting away and drawing need; not two quick presses.)
        static int64_t down_since = 0;
        const int64_t  now        = NowMs();
        g_y_down.store(down);
        if (!down) {
            down_since = 0;
            return;
        }
        if (down_since == 0) {
            down_since = now;
        }
        if (now - down_since >= 300) {
            g_y_held_ms.store(now);
        }
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
