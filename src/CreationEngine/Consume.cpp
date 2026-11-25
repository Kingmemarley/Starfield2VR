#include "Consume.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "CreationEngineSingletonManager.h"
#include "GrenadeHand.h"
#include "memory/GameCode.h"

namespace Consume
{
    namespace
    {
        // The game's activate, TESObjectREFR::ActivateRef(object, block) (Starfield 1.16.236): its first bytes, and the call to it
        // in ObjectReference.Activate's code (rva 206a8c0, at 206a97d), checked before anything is hooked.
        constexpr uintptr_t kActivateRva = 0xb48040;
        constexpr uint8_t   kActivateStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
              0x48, 0x8D, 0xAC, 0x24, 0x80, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x80, 0x02, 0x00, 0x00 };
        constexpr uintptr_t kNativeCallRva = 0x206a97d;

        // Forms (CommonLibSF layouts, as the hand grab reads them): a reference's base form at +0x98; a form's id at +0x28 and its
        // type at +0x2e; TESForm's Activate is type table slot 0x54 (+0x2a0), LoopingActivate 0x53 (+0x298).
        constexpr uintptr_t kRefBase     = 0x98;
        constexpr uintptr_t kFormId      = 0x28;
        constexpr uintptr_t kFormType    = 0x2e;
        constexpr uint8_t   kAlchemy     = 0x36; // ALCH: food, drinks, aid, chems
        constexpr uintptr_t kActivateFn  = 0x2a0;
        constexpr uintptr_t kLoopingFn   = 0x298;

        // Taking and using, as the game itself does (Starfield 1.16.236), every piece checked against the game's own code at start:
        // - Take: the player's activate (rva 1a392f0) does, at 1a39a1c: count = the reference's count (its extra data at +0xc8,
        //   rva 50b8d0, called at 1a39a2e), then ActivateRef(reference, { you, 0, 0, count, 0 }) (called at 1a39a51), the object in a
        //   game global ("being taken") during the call. For food, weapons and other carried things that picks it up. The game runs
        //   your Take on its worker threads (the test log: three Takes, three different threads; the activate has its own lock).
        // - Equip: script Actor.EquipItem's code (rva 1fc8df0) finds the actor's handle (its id at +0x24 looked up with a small
        //   function object, rva 28d0ff0) and hands (handle, item, abPreventRemoval, abSilent) to the game's queue (rva 18cb1a0),
        //   which runs the equip (rva 19667d0, then ActorEquipManager::EquipObject) at once on the game's own thread for it, or
        //   queues it to run there. Equipping food or a drink uses it (HIGGS eats this way in Skyrim VR: take it, then EquipItem);
        //   a weapon goes into your hands.
        constexpr uintptr_t kTakeRva = 0x1a39a1c;
        constexpr uint8_t   kTakeCode[] = { 0x49, 0x8B, 0x1C, 0x24, 0x48, 0x89, 0x1D, 0xF9, 0x39, 0x58, 0x04, 0x48, 0x8B, 0x8B, 0xC8, 0x00, 0x00, 0x00,
              0xE8, 0x9D, 0x1E, 0xAD, 0xFE, 0x4C, 0x89, 0x75, 0xF7, 0xC5, 0xF9, 0xEF, 0xC0, 0xC5, 0xFA, 0x7F, 0x45, 0xFF, 0x89, 0x45, 0x0F, 0xC7, 0x45,
              0x13, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x55, 0xF7, 0x48, 0x8B, 0xCB, 0xE8, 0xEA, 0xE5, 0x10, 0xFF };
        constexpr size_t    kTakeCountCall    = 0x12; // the call of the count function in it
        constexpr size_t    kTakeActivateCall = 0x35; // the call of ActivateRef in it
        constexpr size_t    kTakeCurrentStore = 0x04; // mov [rip+x],rbx: the object being taken, in a game global while it is taken
        // ...and the Take's checks just before (rva 1a399c1), each repeated: a game state byte ([[61ea1f0]+0xb2] set: nothing is
        // taken), the object's extra data of type 0x62 with +0x18 set, or its extra flag 2 (activation blocked, as by a script).
        constexpr uintptr_t kTakeChecksRva = 0x1a399c1;
        constexpr uint8_t   kTakeChecks[]  = { 0x48, 0x8B, 0x05, 0x28, 0x08, 0x7B, 0x04, 0x80, 0xB8, 0xB2, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x79, 0x01, 0x00,
              0x00, 0x49, 0x8B, 0x0C, 0x24, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x67, 0x01, 0x00, 0x00, 0xB2, 0x62, 0x48, 0x8B, 0x89, 0xC8, 0x00, 0x00, 0x00, 0xE8,
              0xF0, 0xDC, 0x90, 0xFE, 0x48, 0x85, 0xC0, 0x74, 0x0A, 0x83, 0x78, 0x18, 0x00, 0x0F, 0x85, 0x3E, 0x01, 0x00, 0x00, 0x49, 0x8B, 0x04, 0x24, 0xBA,
              0x02, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xC8, 0x00, 0x00, 0x00, 0xE8, 0x4C, 0x8C, 0xAF, 0xFE, 0x84, 0xC0, 0x0F, 0x85, 0x21, 0x01, 0x00, 0x00 };
        constexpr size_t    kChecksStateLoad  = 0x00; // mov rax,[rip+x]: the state's address
        constexpr size_t    kChecksByTypeCall = 0x2a; // the extra data lookup by type (rcx extra data, dl type)
        constexpr size_t    kChecksFlagCall   = 0x4e; // the extra flag test (rcx extra data, edx flag)
        constexpr uint8_t   kExtraBlocking    = 0x62;
        constexpr uint32_t  kFlagBlocked      = 2;
        constexpr uintptr_t kStateBlocked     = 0xb2;
        // Script Actor.EquipItem's code from the handle to the queue (rva 1fc8e8f..1fc8f08, 121 bytes).
        constexpr uintptr_t kEquipNativeRva   = 0x1fc8e8f;
        constexpr uint8_t   kEquipNative[]    = { 0x8B, 0x15, 0x87, 0x9E, 0x23, 0x04, 0x48, 0x8B, 0x1D, 0x4C, 0x06, 0xEB, 0x03, 0x89, 0x54, 0x24, 0x78, 0x48,
                 0x85, 0xF6, 0x74, 0x43, 0x8B, 0x46, 0x24, 0x85, 0xC0, 0x74, 0x3C, 0x48, 0x8D, 0x4C, 0x24, 0x78, 0x48, 0x89, 0x74, 0x24, 0x38, 0x48, 0x89, 0x4C,
                 0x24, 0x30, 0x4C, 0x8D, 0x44, 0x24, 0x40, 0x48, 0x8D, 0x0D, 0x49, 0x64, 0xB3, 0x02, 0x8B, 0xD0, 0x48, 0x89, 0x4C, 0x24, 0x40, 0x48, 0x8D, 0x4C,
                 0x24, 0x30, 0x48, 0x89, 0x4C, 0x24, 0x48, 0x48, 0x8B, 0x0D, 0x61, 0xF2, 0xE9, 0x03, 0xE8, 0x0C, 0x81, 0x90, 0x00, 0x8B, 0x54, 0x24, 0x78, 0x0F,
                 0xB6, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC7, 0x44, 0x0F, 0xB6, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0x88, 0x44,
                 0x24, 0x20, 0xE8, 0x98, 0x22, 0x90, 0xFF };
        constexpr size_t    kNativeNullHandle = 0x00; // mov edx,[rip+x]: the null handle
        constexpr size_t    kNativeService    = 0x06; // mov rbx,[rip+x]: the queue's owner
        constexpr size_t    kNativeFunctor    = 0x31; // lea rcx,[rip+x]: the function object's type table
        constexpr size_t    kNativeManager    = 0x49; // mov rcx,[rip+x]: the handle lookup's owner
        constexpr size_t    kNativeLookup     = 0x50; // call: the handle lookup
        constexpr size_t    kNativeQueue      = 0x74; // call: the queue
        constexpr uintptr_t kActorHandleId    = 0x24; // (mov eax,[rsi+0x24]: what the lookup is given)
        // Unequipping: script Actor.UnequipItem's code (rva 1fce980; (script engine, stack id, actor, item, abPreventEquip, abSilent)).
        // Given a base form it locks the actor's inventory (actor+0xa8), finds every stack of it, and for each equipped copy calls
        // the game's ActorEquipManager::UnequipObject (rva 19a5070) queued (as the game's own unequips are), then (abSilent false)
        // shows the "unequipped" message; the game then gives you your fists. Only its three checks at the start report to the
        // script engine (no item; the actor has no inventory, rva b25380; the item is not a bound object, type table +0x1e8),
        // so those are checked first and it is called without a script engine. Its start up to those checks:
        constexpr uintptr_t kUnequipNativeRva = 0x1fce980;
        constexpr uint8_t   kUnequipNative[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55,
              0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0xDA, 0x48, 0x8B,
              0xF1, 0x33, 0xFF, 0x89, 0x7D, 0xD7, 0x4D, 0x85, 0xC9, 0x0F, 0x84, 0xE3, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xC8, 0xE8, 0xC3, 0x69, 0xB5,
              0xFE, 0x84, 0xC0, 0x0F, 0x84, 0xB3, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x4D, 0x6F, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xE8, 0x01, 0x00, 0x00,
              0x84, 0xC0, 0x0F, 0x84, 0x7E, 0x01, 0x00, 0x00 };
        constexpr size_t    kUnequipInventoryCall = 0x38;  // call: the actor has an inventory (rva b25380)
        constexpr uintptr_t kBoundObjectSlot      = 0x1e8; // the form's type table: IsBoundObject
        // The game's thread for these (the queue runs the work at once on it, otherwise queues it): only logged.
        constexpr uintptr_t kMainThreadRva  = 0x18cb1df;
        constexpr uint8_t   kMainThreadCode[] = { 0xFF, 0x15, 0x33, 0x82, 0x17, 0x02, 0x39, 0x05, 0x7D, 0x09, 0x68, 0x04, 0x0F, 0x84, 0xCD, 0x00, 0x00, 0x00 };
        constexpr size_t    kMainThreadCmp  = 0x06; // cmp [rip+x],eax
        constexpr uintptr_t kRefLoaded        = 0xb8; // TESObjectREFR::loadedData (0 once its 3D is gone)
        constexpr uintptr_t kRefExtra         = 0xc8; // its extra data (the count)
        constexpr int       kSettleFrames     = 2;    // frames after your hand let go (the grab's springs gone) before taking it...
        constexpr int       kEquipFrames      = 2;    // ...and after taking it before using it
        constexpr int64_t   kTimeoutUs        = 2'000'000;

        // The 0x20 bytes ActivateRef reads: who activates it, two pointers (0 from your Take), the count, flags (0: your Take).
        struct ActivateBlock {
            uintptr_t who{ 0 };
            uintptr_t unk08{ 0 };
            uintptr_t unk10{ 0 };
            int32_t   count{ 1 };
            uint32_t  flags{ 0 };
        };
        static_assert(sizeof(ActivateBlock) == 0x20);
        // The handle lookup's function object, as EquipItem's code builds it on its stack: its type table and its captures (where
        // the handle goes, and the actor it is for; the object's call writes the handle when the actor matches).
        struct HandleCaptures {
            uint32_t* out{ nullptr };
            uintptr_t actor{ 0 };
        };
        struct HandleFunctor {
            uintptr_t       table{ 0 };
            HandleCaptures* captures{ nullptr };
        };
        using CountFn    = int32_t (*)(void*);
        using ActivateFn = bool (*)(void*, ActivateBlock*);
        using ByTypeFn   = void* (*)(void*, uint8_t);
        using FlagFn     = bool (*)(void*, uint32_t);
        using LookupFn   = void (*)(void*, uint32_t, HandleFunctor*);
        using QueueFn    = void (*)(void*, uint32_t, void*, bool, bool);
        using UnequipFn  = void (*)(void*, uint32_t, void*, void*, bool, bool);
        using TestFn     = bool (*)(void*);

        constexpr int kFoodLogs  = 30; // activations of food or drinks logged
        constexpr int kOtherLogs = 8;  // ...and of anything else (doors, containers, people)

        uintptr_t           g_base{ 0 };
        uintptr_t           g_text_begin{ 0 }, g_text_end{ 0 };
        safetyhook::MidHook g_activate_hook{};
        std::atomic<int>    g_food_logged{ 0 };
        std::atomic<int>    g_other_logged{ 0 };
        std::atomic<int>    g_refused{ 0 };
        std::atomic<int>    g_activations{ 0 };
        std::atomic<int64_t>   g_released_us{ 0 };  // the last object let go for one of these
        std::atomic<uintptr_t> g_released_ref{ 0 };
        uintptr_t              g_count_fn{ 0 }, g_activate_fn{ 0 }, g_by_type_fn{ 0 }, g_flag_fn{ 0 }, g_lookup_fn{ 0 }, g_queue_fn{ 0 };
        uintptr_t              g_taking_global{ 0 };  // the game's "object being taken" global
        uintptr_t              g_state_global{ 0 };   // the game state the Take checks
        uintptr_t              g_null_handle{ 0 };    // the game's null handle (a global)
        uintptr_t              g_service_global{ 0 }; // the queue's owner (a global holding it)
        uintptr_t              g_manager_global{ 0 }; // the handle lookup's owner (a global holding it)
        uintptr_t              g_functor_table{ 0 };  // the lookup's function object type
        uintptr_t              g_main_thread{ 0 };    // the game's thread id for its commands (a global)
        uintptr_t              g_unequip_fn{ 0 };     // script UnequipItem's code (0: unavailable)
        uintptr_t              g_inventory_fn{ 0 };   // its check that the actor has an inventory
        std::atomic<bool>      g_ready{ false };
        std::string            g_off{ "not set up yet" }; // why taking and equipping are off (for the log)

        // An object let go for an action (main thread): 1 waiting for your hand to let go of it, 2 taken (equipped next).
        struct Pending {
            int       stage{ 0 };
            Action    action{ Action::Eat };
            uintptr_t ref{ 0 };
            uintptr_t base{ 0 };
            uint32_t  ref_id{ 0 };
            uint32_t  base_id{ 0 };
            int       hand{ 1 };
            int64_t   since_us{ 0 };
            int       frames{ 0 };
            int64_t   last_us{ 0 }; // (the time a menu is open does not count toward the timeout)
        };
        Pending g_pending{};
        // The last weapon taken to be equipped: its base and how it went (0 pending, 1 equip queued, -1 failed).
        uintptr_t g_weapon_base{ 0 };
        int       g_weapon_result{ 0 };

        // (The game's functions are called without catching faults: the activate takes a game-wide lock it releases only at its
        // end, so a caught fault would leave it held and the game frozen; a crash says more. The object is checked before.)

        const char* ActionName(Action a) { return a == Action::Eat ? "eat" : (a == Action::Bag ? "put in your bag" : "equip"); }

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        bool InText(uintptr_t a, size_t n = 1) { return a >= g_text_begin && a < g_text_end && n <= g_text_end - a; }

        std::string Rva(uintptr_t a)
        {
            return InText(a) ? fmt::format("{:x}", a - g_base) : fmt::format("{:x} (not the game's code)", a);
        }

        bool FindText()
        {
            g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (g_base == 0) {
                return false;
            }
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                return false;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) {
                return false;
            }
            const auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
                if (std::strncmp(reinterpret_cast<const char*>(section->Name), ".text", 8) == 0) {
                    g_text_begin = g_base + section->VirtualAddress;
                    g_text_end   = g_text_begin + section->Misc.VirtualSize;
                    return g_text_end > g_text_begin;
                }
            }
            return false;
        }

        // A rel32 call at `at`: where it goes (0 if it is not one, or not into the game's code).
        uintptr_t CallTarget(uintptr_t at)
        {
            if (!InText(at, 5) || *reinterpret_cast<const uint8_t*>(at) != 0xE8) {
                return 0;
            }
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof(rel));
            const uintptr_t target = at + 5 + static_cast<intptr_t>(rel);
            return InText(target) ? target : 0;
        }

        // A rip-relative operand of the instruction at `at` (`length` bytes, the 32-bit displacement in its last 4): its address in
        // the game's image (0 if outside it).
        uintptr_t RipTarget(uintptr_t at, size_t length)
        {
            if (!InText(at, length)) {
                return 0;
            }
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + length - 4), sizeof(rel));
            const uintptr_t target = at + length + static_cast<intptr_t>(rel);
            const auto*     dos    = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto*     nt     = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            return target > g_base && target < g_base + nt->OptionalHeader.SizeOfImage ? target : 0;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return InText(GameCode::At(rva), n) && GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // Finds what taking and equipping call, checked against the game's own code (see above); false with the reason in g_off.
        bool FindActions(uintptr_t activate)
        {
            if (!SameCode(kTakeRva, kTakeCode, sizeof(kTakeCode))) {
                g_off = fmt::format("the game's Take (rva {:x}) is not as in Starfield 1.16.236", kTakeRva);
                return false;
            }
            if (CallTarget(At(kTakeRva + kTakeActivateCall)) != activate) {
                g_off = "the game's Take does not call its activate as expected";
                return false;
            }
            g_count_fn      = CallTarget(At(kTakeRva + kTakeCountCall));
            g_taking_global = RipTarget(At(kTakeRva + kTakeCurrentStore), 7);
            if (!SameCode(kTakeChecksRva, kTakeChecks, sizeof(kTakeChecks))) {
                g_off = fmt::format("the game's checks before its Take (rva {:x}) are not as in Starfield 1.16.236", kTakeChecksRva);
                return false;
            }
            g_state_global = RipTarget(At(kTakeChecksRva + kChecksStateLoad), 7);
            g_by_type_fn   = CallTarget(At(kTakeChecksRva + kChecksByTypeCall));
            g_flag_fn      = CallTarget(At(kTakeChecksRva + kChecksFlagCall));
            if (!SameCode(kEquipNativeRva, kEquipNative, sizeof(kEquipNative))) {
                g_off = fmt::format("script EquipItem's code (rva {:x}) is not as in Starfield 1.16.236", kEquipNativeRva);
                return false;
            }
            g_null_handle    = RipTarget(At(kEquipNativeRva + kNativeNullHandle), 6);
            g_service_global = RipTarget(At(kEquipNativeRva + kNativeService), 7);
            g_functor_table  = RipTarget(At(kEquipNativeRva + kNativeFunctor), 7);
            g_manager_global = RipTarget(At(kEquipNativeRva + kNativeManager), 7);
            g_lookup_fn      = CallTarget(At(kEquipNativeRva + kNativeLookup));
            g_queue_fn       = CallTarget(At(kEquipNativeRva + kNativeQueue));
            if (SameCode(kMainThreadRva, kMainThreadCode, sizeof(kMainThreadCode))) {
                g_main_thread = RipTarget(At(kMainThreadRva + kMainThreadCmp), 6);
            }
            // (Unequipping is optional: without it a picked-up weapon is only put away.)
            if (SameCode(kUnequipNativeRva, kUnequipNative, sizeof(kUnequipNative))) {
                g_inventory_fn = CallTarget(At(kUnequipNativeRva + kUnequipInventoryCall));
                g_unequip_fn   = g_inventory_fn != 0 ? At(kUnequipNativeRva) : 0;
            }
            spdlog::info("[Eat] unequipping as script UnequipItem does (rva {:x}): {}", kUnequipNativeRva,
                g_unequip_fn != 0 ? "ready" : "unavailable (its code is not as in Starfield 1.16.236; a weapon is put away instead)");
            g_activate_fn = activate;
            if (g_count_fn == 0 || g_by_type_fn == 0 || g_flag_fn == 0 || g_lookup_fn == 0 || g_queue_fn == 0) {
                g_off = "a function the Take or EquipItem calls is not in the game's code";
                return false;
            }
            if (g_taking_global == 0 || g_state_global == 0 || g_null_handle == 0 || g_service_global == 0 || g_functor_table == 0 ||
                g_manager_global == 0) {
                g_off = "a game global the Take or EquipItem uses is not where expected";
                return false;
            }
            return true;
        }

        // A return address: the bytes before it are a call (direct, through memory, or a register).
        bool AfterCall(uintptr_t at)
        {
            if (!InText(at - 7, 7)) {
                return false;
            }
            uint8_t b[7]{};
            std::memcpy(b, reinterpret_cast<const void*>(at - 7), sizeof(b)); // b[6] is the byte just before `at`
            return b[2] == 0xE8                                                // call rel32
                   || (b[1] == 0xFF && b[2] == 0x15)                          // call [rip+x]
                   || (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0)                 // call reg
                   || (b[4] == 0x41 && b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) // call r8..r15
                   || (b[4] == 0xFF && (b[5] & 0xF8) == 0x50)                 // call [reg+d8]
                   || (b[3] == 0x41 && b[4] == 0xFF && (b[5] & 0xF8) == 0x50)
                   || (b[1] == 0xFF && (b[2] & 0xF8) == 0x90)                 // call [reg+d32]
                   || (b[0] == 0x41 && b[1] == 0xFF && (b[2] & 0xF8) == 0x90);
        }

        // The code that led here: return addresses on the stack above `from` (a guess: values that point just after a call). The
        // stack is copied with a checked read (up to the thread's stack base, at most 6 KB).
        std::string Chain(uintptr_t from)
        {
            const auto*     tib  = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const uintptr_t high = reinterpret_cast<uintptr_t>(tib->StackBase);
            size_t          size = 0x1800;
            if (high > from && high - from < size) {
                size = static_cast<size_t>(high - from);
            }
            std::vector<uintptr_t> values(size / sizeof(uintptr_t));
            while (!values.empty() && !SafeRead(values.data(), from, values.size() * sizeof(uintptr_t))) {
                values.resize(values.size() / 4); // (beyond the stack's end: a shorter stretch)
            }
            std::string out;
            int         found = 0;
            for (const uintptr_t value : values) {
                if (found < 12 && AfterCall(value)) {
                    out += fmt::format(" {:x}", value - g_base);
                    ++found;
                }
            }
            return out.empty() ? std::string{ " none found" } : out;
        }

        std::string Hex(const uint8_t* p, size_t n)
        {
            std::string out;
            for (size_t i = 0; i < n; ++i) {
                out += fmt::format("{}{:02x}", i == 0 ? "" : (i % 8 == 0 ? " | " : " "), p[i]);
            }
            return out;
        }

        // At the start of the game's activate (any thread): when you are the one activating, what and how.
        void OnActivate(safetyhook::Context& ctx)
        {
            const uintptr_t object = ctx.rcx;
            // A grenade's copy in your hand (GrenadeHand) is never activated (taken, picked up) by anyone: the activate returns
            // false at once, as for an object whose activation is blocked (straight to a `ret` of the game's: the caller's return
            // address is on top of the stack at this first instruction).
            if (const uintptr_t ret = GrenadeHand::RefuseActivation(object); ret != 0) {
                ctx.rax = 0;
                ctx.rip = ret;
                const int n = g_refused.fetch_add(1) + 1;
                if (n <= 10) {
                    spdlog::info("[Eat] the grenade's copy in your hand was not activated (refused {} time(s); thread {})", n, GetCurrentThreadId());
                }
                return;
            }
            uint8_t         block[0x20]{};
            if (!SafeRead(block, ctx.rdx, sizeof(block))) {
                return;
            }
            uintptr_t who = 0;
            std::memcpy(&who, block, sizeof(who));
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (player == 0 || who != player) {
                return;
            }
            const int n = g_activations.fetch_add(1) + 1;
            uintptr_t base = 0, table = 0, activate_fn = 0, looping_fn = 0;
            uint32_t  object_id = 0, base_id = 0;
            uint8_t   type = 0;
            SafeRead(&object_id, object + kFormId, sizeof(object_id));
            const bool have_base = SafeRead(&base, object + kRefBase, sizeof(base)) && base != 0 && SafeRead(&base_id, base + kFormId, sizeof(base_id)) &&
                                   SafeRead(&type, base + kFormType, sizeof(type));
            if (have_base && SafeRead(&table, base, sizeof(table))) {
                SafeRead(&activate_fn, table + kActivateFn, sizeof(activate_fn));
                SafeRead(&looping_fn, table + kLoopingFn, sizeof(looping_fn));
            }
            const bool carried = have_base && IsCarried(type);
            auto&      logged  = carried ? g_food_logged : g_other_logged;
            if (logged.fetch_add(1) >= (carried ? kFoodLogs : kOtherLogs)) {
                return;
            }
            uintptr_t ret = 0;
            SafeRead(&ret, ctx.rsp, sizeof(ret));
            uint8_t site[8]{};
            if (InText(ret - sizeof(site), sizeof(site))) {
                std::memcpy(site, reinterpret_cast<const void*>(ret - sizeof(site)), sizeof(site));
            }
            int32_t count = 0;
            std::memcpy(&count, block + 0x18, sizeof(count));
            const int64_t since = NowUs() - g_released_us.load();
            spdlog::info("[Eat] activation {} by you (thread {}): object {:08X} (base {:08X}, form type {:x}{}), count {}, bytes +1c..+1f {} {} {} {}, "
                         "block {}; called from {} (the call: {}); its Activate {}, LoopingActivate {}; the code before: {}{}",
                n, GetCurrentThreadId(), object_id, base_id, type, carried ? ": something you can carry" : "", count, block[0x1c], block[0x1d],
                block[0x1e], block[0x1f], Hex(block, sizeof(block)), Rva(ret), Hex(site, sizeof(site)), Rva(activate_fn), Rva(looping_fn),
                Chain(ctx.rsp + 8), g_released_ref.load() == object && since < 3'000'000 ? " (the object you just let go of for it)" : "");
        }

        // Your inventory (as the game's equip reads it, rva 196e8a0): the actor's +0xa0 holds its list, whose +0x28 is the number of
        // entries and +0x30 the entries (0x28 bytes each, the item first). Read without its lock (a check only).
        constexpr uintptr_t kActorInventory = 0xa0;
        constexpr uintptr_t kListCount      = 0x28;
        constexpr uintptr_t kListEntries    = 0x30;
        constexpr size_t    kEntrySize      = 0x28;
        constexpr size_t    kEntryEquipped  = 0x20; // its equipped flags (bits 0-2)

        bool Carries(uintptr_t actor, uintptr_t item)
        {
            uintptr_t list = 0, entries = 0;
            uint32_t  count = 0;
            if (!SafeRead(&list, actor + kActorInventory, sizeof(list)) || list == 0 || !SafeRead(&count, list + kListCount, sizeof(count)) || count == 0 ||
                count > 20000 || !SafeRead(&entries, list + kListEntries, sizeof(entries)) || entries == 0) {
                return false;
            }
            std::vector<uint8_t> raw(static_cast<size_t>(count) * kEntrySize);
            if (!SafeRead(raw.data(), entries, raw.size())) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uintptr_t object = 0;
                std::memcpy(&object, raw.data() + static_cast<size_t>(i) * kEntrySize, sizeof(object));
                if (object == item) {
                    return true;
                }
            }
            return false;
        }

        // The actor has a copy of `item` equipped: an inventory entry of it with any of its equipped flags (+0x20, bits 0-2, as script
        // UnequipItem's code tests them, rva 1fe6920) set. Read without the inventory's lock (a check only).
        bool HasEquipped(uintptr_t actor, uintptr_t item)
        {
            uintptr_t list = 0, entries = 0;
            uint32_t  count = 0;
            if (!SafeRead(&list, actor + kActorInventory, sizeof(list)) || list == 0 || !SafeRead(&count, list + kListCount, sizeof(count)) || count == 0 ||
                count > 20000 || !SafeRead(&entries, list + kListEntries, sizeof(entries)) || entries == 0) {
                return false;
            }
            std::vector<uint8_t> raw(static_cast<size_t>(count) * kEntrySize);
            if (!SafeRead(raw.data(), entries, raw.size())) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uintptr_t object = 0;
                uint32_t  flags  = 0;
                std::memcpy(&object, raw.data() + static_cast<size_t>(i) * kEntrySize, sizeof(object));
                std::memcpy(&flags, raw.data() + static_cast<size_t>(i) * kEntrySize + kEntryEquipped, sizeof(flags));
                if (object == item && (flags & 7) != 0) {
                    return true;
                }
            }
            return false;
        }

        // Your handle (as EquipItem's code finds an actor's): the null handle if there is none.
        uint32_t ActorHandle(uintptr_t actor, uint32_t& null_handle)
        {
            null_handle      = *reinterpret_cast<const volatile uint32_t*>(g_null_handle);
            uint32_t handle  = null_handle;
            uint32_t id      = 0;
            uintptr_t manager = 0;
            if (!SafeRead(&id, actor + kActorHandleId, sizeof(id)) || id == 0 || !SafeRead(&manager, g_manager_global, sizeof(manager)) || manager == 0) {
                return handle;
            }
            HandleCaptures captures{ &handle, actor };
            HandleFunctor  functor{ g_functor_table, &captures };
            reinterpret_cast<LookupFn>(g_lookup_fn)(reinterpret_cast<void*>(manager), id, &functor);
            return handle;
        }

        // Takes the object exactly as your Take does, with its checks (main thread). True when the game took it.
        bool Take(Pending& e, uintptr_t player)
        {
            uintptr_t base = 0, loaded = 0, extra = 0, state = 0;
            uint32_t  id   = 0;
            if (!SafeRead(&base, e.ref + kRefBase, sizeof(base)) || base != e.base || !SafeRead(&id, e.ref + kFormId, sizeof(id)) || id != e.ref_id ||
                !SafeRead(&loaded, e.ref + kRefLoaded, sizeof(loaded)) || loaded == 0) {
                spdlog::info("[Eat] {:08X} is no longer in the world as it was; not taken", e.base_id);
                return false;
            }
            uint8_t blocked_state = 0;
            if (SafeRead(&state, g_state_global, sizeof(state)) && state != 0 && SafeRead(&blocked_state, state + kStateBlocked, sizeof(blocked_state)) &&
                blocked_state != 0) {
                spdlog::info("[Eat] the game is not letting you take things right now; {:08X} not taken", e.base_id);
                return false;
            }
            SafeRead(&extra, e.ref + kRefExtra, sizeof(extra));
            int32_t count = 1;
            if (extra != 0) {
                const auto* blocking = reinterpret_cast<const uint8_t*>(reinterpret_cast<ByTypeFn>(g_by_type_fn)(reinterpret_cast<void*>(extra), kExtraBlocking));
                uint32_t    flag     = 0;
                if ((blocking != nullptr && SafeRead(&flag, reinterpret_cast<uintptr_t>(blocking) + 0x18, sizeof(flag)) && flag != 0) ||
                    reinterpret_cast<FlagFn>(g_flag_fn)(reinterpret_cast<void*>(extra), kFlagBlocked)) {
                    spdlog::info("[Eat] {:08X} can't be taken (its activation is blocked, as by a quest script)", e.base_id);
                    return false;
                }
                count = reinterpret_cast<CountFn>(g_count_fn)(reinterpret_cast<void*>(extra));
                if (count < 1 || count > 100000) {
                    count = 1;
                }
            }
            ActivateBlock block{};
            block.who   = player;
            block.count = count;
            *reinterpret_cast<volatile uintptr_t*>(g_taking_global) = e.ref;
            const bool taken = reinterpret_cast<ActivateFn>(g_activate_fn)(reinterpret_cast<void*>(e.ref), &block);
            *reinterpret_cast<volatile uintptr_t*>(g_taking_global) = 0;
            if (!taken) {
                spdlog::info("[Eat] the game did not let you take {:08X} (count {})", e.base_id, count);
                return false;
            }
            spdlog::info("[Eat] took {:08X} (base {:08X}, count {}) as the game's Take does, to {}", e.ref_id, e.base_id, count, ActionName(e.action));
            return true;
        }
    }

    uint8_t BaseOf(uintptr_t ref, uintptr_t* base_out, uint32_t* base_id)
    {
        uintptr_t base = 0;
        uint8_t   type = 0;
        if (ref == 0 || !SafeRead(&base, ref + kRefBase, sizeof(base)) || base == 0 || !SafeRead(&type, base + kFormType, sizeof(type))) {
            return 0;
        }
        if (base_out != nullptr) {
            *base_out = base;
        }
        if (base_id != nullptr) {
            *base_id = 0;
            SafeRead(base_id, base + kFormId, sizeof(*base_id));
        }
        return type;
    }

    bool IsFood(uintptr_t ref, uint32_t* base_id) { return BaseOf(ref, nullptr, base_id) == kAlchemy; }

    bool IsCarried(uint8_t type)
    {
        switch (type) {
        case 0x22: // ARMO: apparel, spacesuits, packs, helmets
        case 0x23: // BOOK
        case 0x26: // INGR
        case 0x28: // MISC: resources, junk
        case 0x30: // WEAP
        case 0x31: // AMMO
        case 0x35: // KEYM: keys, keycards
        case 0x36: // ALCH: food, drinks, aid, chems
        case 0x39: // NOTE: slates
            return true;
        default:
            return false;
        }
    }

    bool Ready() { return g_ready.load(); }

    bool ActivateHooked() { return static_cast<bool>(g_activate_hook); }

    void Install()
    {
        if (!FindText()) {
            g_off = "the game's image could not be read";
            spdlog::warn("[Eat] could not read the game's image; eating, the bag and picked-up weapons stay off");
            return;
        }
        const uintptr_t activate = At(kActivateRva);
        const uintptr_t call     = At(kNativeCallRva);
        std::string     why;
        if (!SameCode(kActivateRva, kActivateStart, sizeof(kActivateStart))) {
            why = fmt::format("the game's activate is not at rva {:x} as in Starfield 1.16.236 (another version, or another mod changed it)", kActivateRva);
        } else if (CallTarget(call) != activate) {
            why = fmt::format("ObjectReference.Activate's code does not call rva {:x} as expected", kActivateRva);
        }
        if (!why.empty()) {
            g_off = why;
            spdlog::warn("[Eat] eating, the bag and picked-up weapons stay off, and your activations are not recorded: {}", why);
            return;
        }
        // (Checked before hooking: the hook changes the activate's first bytes. Calling it later goes through the hook.)
        const bool found = FindActions(activate);
        g_activate_hook  = safetyhook::create_mid(reinterpret_cast<void*>(activate), &OnActivate);
        if (!g_activate_hook) {
            spdlog::warn("[Eat] could not hook the game's activate (rva {:x}): your activations are not recorded", kActivateRva);
        }
        if (found) {
            g_off.clear();
            g_ready = true;
            spdlog::info("[Eat] taking and equipping are on (eating at your mouth, the bag, picked-up weapons): taken as the game's Take does "
                         "(activate rva {:x}, count rva {:x}), equipped as script EquipItem does (handle lookup rva {:x}, the game's queue rva {:x}){}",
                kActivateRva, g_count_fn - g_base, g_lookup_fn - g_base, g_queue_fn - g_base,
                g_main_thread != 0 ? "" : " (the game's command thread was not found: not logged)");
        } else {
            spdlog::warn("[Eat] eating, the bag and picked-up weapons stay off: {}", g_off);
        }
    }

    bool AfterRelease(Action action, uintptr_t ref, int hand)
    {
        g_released_ref = ref;
        g_released_us  = NowUs();
        uint32_t  base_id = 0, ref_id = 0;
        uintptr_t base    = 0;
        const uint8_t type = BaseOf(ref, &base, &base_id);
        SafeRead(&ref_id, ref + kFormId, sizeof(ref_id));
        const char* hand_name = hand == 0 ? "left" : "right";
        if (!g_ready.load()) {
            spdlog::info("[Eat] {} hand let go of {:08X} (base {:08X}) to {}; that is off ({})", hand_name, ref_id, base_id, ActionName(action), g_off);
            return false;
        }
        if (type == 0 || !IsCarried(type) || (action == Action::Eat && type != kAlchemy) || (action == Action::Weapon && type != 0x30)) {
            return false;
        }
        if (g_pending.stage != 0) {
            static int told = 0;
            if (++told <= 10) {
                spdlog::info("[Eat] still busy with {:08X}; {:08X} is let go as usual", g_pending.base_id, base_id);
            }
            return false;
        }
        const int64_t now = NowUs();
        g_pending         = Pending{ 1, action, ref, base, ref_id, base_id, hand, now, 0, now };
        if (action == Action::Weapon) {
            g_weapon_base   = base;
            g_weapon_result = 0;
        }
        spdlog::info("[Eat] {} hand let go of {:08X} (base {:08X}) to {}: taken once your hand has let go", hand_name, ref_id, base_id, ActionName(action));
        return true;
    }

    bool QueueEquip(uintptr_t item, const char* why)
    {
        if (!g_ready.load() || item == 0) {
            return false;
        }
        const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
        uintptr_t  service = 0;
        uint32_t   item_id = 0;
        SafeRead(&item_id, item + kFormId, sizeof(item_id));
        if (player == 0 || !SafeRead(&service, g_service_global, sizeof(service)) || service == 0) {
            spdlog::warn("[Eat] equipping {:08X} ({}): the game's queue is not there yet", item_id, why);
            return false;
        }
        // Only what you carry: the game's equip adds an item you do not have (as script EquipItem does), which would be a free copy.
        if (!Carries(player, item)) {
            spdlog::info("[Eat] equipping {:08X} ({}): it is not in your inventory, so it is not equipped", item_id, why);
            return false;
        }
        uint32_t       null_handle = 0;
        const uint32_t handle      = ActorHandle(player, null_handle);
        if (handle == null_handle) {
            spdlog::warn("[Eat] equipping {:08X} ({}): your handle was not found", item_id, why);
            return false;
        }
        reinterpret_cast<QueueFn>(g_queue_fn)(reinterpret_cast<void*>(service), handle, reinterpret_cast<void*>(item), false, false);
        static int told = 0;
        if (++told <= 40) {
            const uint32_t main_thread = g_main_thread != 0 ? *reinterpret_cast<const volatile uint32_t*>(g_main_thread) : 0;
            spdlog::info("[Eat] equip {:08X} handed to the game ({}; as script EquipItem does: handle {:x}; this thread {}, the game's command thread {}{})",
                item_id, why, handle, GetCurrentThreadId(), main_thread, main_thread == GetCurrentThreadId() ? ": done at once" : ": it runs it there");
        }
        return true;
    }

    bool Unequip(uintptr_t item, const char* why)
    {
        if (g_unequip_fn == 0 || g_inventory_fn == 0 || item == 0) {
            return false;
        }
        auto* const player  = CreationEngineSingletonManager::GetPlayerRef();
        uint32_t    item_id = 0;
        SafeRead(&item_id, item + kFormId, sizeof(item_id));
        if (player == nullptr) {
            return false;
        }
        // Its own checks first (only they would report to the script engine, which is not given): a bound object (from its type
        // table), and you have an inventory. Then only what you carry (nothing to unequip otherwise).
        uintptr_t table = 0, bound_fn = 0;
        if (!SafeRead(&table, item, sizeof(table)) || !SafeRead(&bound_fn, table + kBoundObjectSlot, sizeof(bound_fn)) || !InText(bound_fn) ||
            !reinterpret_cast<TestFn>(bound_fn)(reinterpret_cast<void*>(item))) {
            spdlog::info("[Eat] unequipping {:08X} ({}): not something that can be equipped", item_id, why);
            return false;
        }
        if (!reinterpret_cast<TestFn>(g_inventory_fn)(player) || !Carries(reinterpret_cast<uintptr_t>(player), item)) {
            spdlog::info("[Eat] unequipping {:08X} ({}): it is not in your inventory", item_id, why);
            return false;
        }
        if (!HasEquipped(reinterpret_cast<uintptr_t>(player), item)) {
            spdlog::info("[Eat] unequipping {:08X} ({}): no copy of it is equipped", item_id, why);
            return false;
        }
        reinterpret_cast<UnequipFn>(g_unequip_fn)(nullptr, 0, player, reinterpret_cast<void*>(item), false, true);
        static int told = 0;
        if (++told <= 40) {
            // (fix168: with the game's command thread, as QueueEquip tells it; the review)
            const uint32_t main_thread = g_main_thread != 0 ? *reinterpret_cast<const volatile uint32_t*>(g_main_thread) : 0;
            spdlog::info("[Eat] {:08X} unequipped ({}; as script UnequipItem does, silently: the game's equip manager, queued; this thread {}, the "
                         "game's command thread {})",
                item_id, why, GetCurrentThreadId(), main_thread);
        }
        return true;
    }

    int WeaponResult(uintptr_t& base)
    {
        base = g_weapon_base;
        return g_weapon_result;
    }

    void Update(bool grabbing, bool menu)
    {
        if (g_pending.stage == 0) {
            return;
        }
        auto&         e   = g_pending;
        const int64_t now = NowUs();
        if (menu) {
            e.since_us += now - e.last_us; // (a menu's time does not count)
        }
        e.last_us = now;
        const auto fail = [&](const char* why) {
            if (why != nullptr) {
                spdlog::info("[Eat] {:08X}: not {} ({})", e.base_id, e.action == Action::Eat ? "eaten" : (e.action == Action::Bag ? "put in your bag" : "equipped"),
                    why);
            }
            if (e.action == Action::Weapon) {
                g_weapon_result = -1;
            }
            e = Pending{};
        };
        if (now - e.since_us > kTimeoutUs) {
            fail("still held after 2 s");
            return;
        }
        if (menu) {
            return;
        }
        const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
        if (player == 0) {
            fail(nullptr);
            return;
        }
        if (e.stage == 1) {
            // The hand grab lets go of it first (its springs gone), as HIGGS drops before it takes.
            if (grabbing) {
                e.frames = 0;
                return;
            }
            if (++e.frames < kSettleFrames) {
                return;
            }
            if (!Take(e, player)) {
                fail(nullptr);
                return;
            }
            if (e.action == Action::Bag) {
                static int told = 0;
                if (++told <= 40) {
                    spdlog::info("[Eat] {:08X} is in your bag", e.base_id);
                }
                e = Pending{};
                return;
            }
            e.stage  = 2;
            e.frames = 0;
            return;
        }
        // Taken (in your inventory by now): equipped as script EquipItem does (food and drinks: used).
        if (++e.frames < kEquipFrames) {
            return;
        }
        const bool queued = QueueEquip(e.base, e.action == Action::Eat ? "eating or drinking it" : "the weapon you picked up");
        if (e.action == Action::Weapon) {
            g_weapon_result = queued ? 1 : -1;
        }
        e = Pending{};
    }
}
