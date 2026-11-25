#include "FavoritesWheel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include <mods/VR.hpp>

#include "CreationEngineSingletonManager.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "LadderClimb.h"
#include "LeftWeapon.h"
#include "MeleeThrow.h"
#include "Physical.h"
#include "TempleGestures.h"
#include "WheelPicture.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace FavoritesWheel
{
    namespace
    {
        // ---- The game's favorites (Starfield 1.16.236; a helper's research of the exe, each checked at Install) ----
        // The favorites manager (the game's FavoritesManager) at [0x1461F6330], set by its constructor (0x141EC0B40, the store at
        // 0x141EC0B7A; its type table 0x144B07DE8 written first, at 0x141EC0B5C). Its 12 slots: a small array of forms, its size at +0x430,
        // +0x438 its capacity (below 0: kept inline), +0x440 the forms (inline) or a pointer to them. The menu's choice calls UseQuickkey with
        // it (its event sink, 0x141EC4F10).
        // (fix173) The first table written (0x144B07DE8) is its base class's (IDataModel); the constructor then writes FavoritesManager's own
        // (0x144D0D398, its RTTI ".?AVFavoritesManager@@", at 0x141EC0BEA: lea rax,[table]; mov [rsi],rax), which the living manager carries.
        // fix171 compared the manager to the first: it never matched, and the wheel was always shown empty (the log: "the favorites
        // manager not found" on every open of fix172's test).
        constexpr uintptr_t kManagerRva          = 0x61F6330;
        constexpr uintptr_t kBaseTableRva        = 0x4B07DE8;
        constexpr uintptr_t kManagerTableRva     = 0x4D0D398;
        constexpr uintptr_t kCtorTableStoreRva   = 0x1EC0B5C; // lea rax,[base type table]
        constexpr uintptr_t kCtorOwnTableRva     = 0x1EC0BEA; // lea rax,[its own type table]; mov [rsi],rax
        constexpr uintptr_t kCtorManagerStoreRva = 0x1EC0B7A; // mov [manager],rdx
        constexpr uint8_t   kCtorLea[]           = { 0x48, 0x8D, 0x05 }; // (fix190) lea rax,[...] (both type tables' stores)
        constexpr uint8_t   kCtorOwnStore[]      = { 0x48, 0x89, 0x06 }; // (fix190) mov [rsi],rax (after its own table's lea)
        constexpr uint8_t   kCtorManagerStore[]  = { 0x48, 0x89, 0x15 }; // (fix190) mov [manager],rdx
        constexpr uintptr_t kSinkUseRva          = 0x1EC4F10; // mov rcx,[manager]; movzx edx,bl; call UseQuickkey
        constexpr uint8_t   kSinkUse[]           = { 0x48, 0x8B, 0x0D };
        constexpr uint8_t   kSinkUseTail[]       = { 0x0F, 0xB6, 0xD3, 0xE8 };
        constexpr uintptr_t kSlotsSize           = 0x430;
        constexpr uintptr_t kSlotsCapacity       = 0x438;
        constexpr uintptr_t kSlotsData           = 0x440;
        constexpr int       kSlots               = 12;
        // FavoritesManager::UseQuickkey 0x141EC2040(manager, slot): the keyboard's hotkeys' and the menu's own use of a slot: an item equipped
        // or used by the inventory's own equip (a weapon drawn, a medpack used), a power made the one in use. The same bytes LeftWeapon checks
        // (its hook in it, at 0x141EC20B5, gives the left hand's choice to the left hand). (fix191) In Starfield 1.16.244 the same function
        // at 0x141EBF380 (the menu's sink calls it there), the slot read at the same distance: only a thread counter it keeps moved (+0x278
        // is +0x27C there); matched by hand for the port table (docs/PORTING.md).
        constexpr uintptr_t kUseQuickkeyRva      = 0x1EC2040;
        constexpr uint8_t   kUseQuickkeyStart[]  = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
                                                     0x24, 0xE1, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xFA, 0x4C, 0x8B, 0xE9 };
        constexpr uintptr_t kUseSlotReadRva      = 0x1EC209D; // (its read of the slot: lea rbx,[r13+440h]; cmp [r13+438h],0; jl; mov rbx,[rbx]; ...)
        constexpr uint8_t   kUseSlotRead[]       = { 0x49, 0x8D, 0x9D, 0x40, 0x04, 0x00, 0x00, 0x41, 0x83, 0xBD, 0x38, 0x04, 0x00, 0x00, 0x00, 0x7C, 0x03,
                                                     0x48, 0x8B, 0x1B, 0x48, 0x8B, 0x1C, 0xFB };
        // Your inventory (the player's +0xA0 list: +0x28 its count, +0x30 its entries of 0x28 bytes), read under its own lock (+0xA8) as the
        // game's favorites do (0x141EC6280: an entry's +0x24 is its favorites slot, a signed byte, -1 none). An entry: +0x00 the item, +0x08
        // its instance data, +0x10 the number of its stacks and +0x18 them (0x10 bytes each, +0x08 their count), +0x20 its flags (& 7:
        // equipped, as the menu's bIsEquipped).
        constexpr uintptr_t kActorInventory     = 0xA0;
        constexpr uintptr_t kActorInventoryLock = 0xA8;
        constexpr uintptr_t kListCount          = 0x28;
        constexpr uintptr_t kListEntries        = 0x30;
        constexpr size_t    kEntrySize          = 0x28;
        constexpr uintptr_t kEntryInstance      = 0x08;
        constexpr uintptr_t kEntryStackCount    = 0x10;
        constexpr uintptr_t kEntryStacks        = 0x18;
        constexpr uintptr_t kEntryFlags         = 0x20;
        constexpr uintptr_t kEntrySlot          = 0x24;
        // The read lock let go as the game lets it go (0x14063EEF8, as LeftWeapon checks it): one reader less; the last with writers waiting
        // wakes them (the import at [0x143A442C8]).
        constexpr uintptr_t kUnlockRva          = 0x63EEF8;
        constexpr uint8_t   kUnlock[]           = { 0x48, 0x8D, 0x8B, 0xAC, 0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xC1, 0x11, 0x8B,
                                                    0xC2, 0x25, 0xFF, 0x0F, 0xC0, 0xFF, 0x83, 0xF8, 0x01, 0x75, 0x0E, 0xF7, 0xC2, 0x00, 0xF0, 0x3F, 0x00,
                                                    0x74, 0x06, 0xFF, 0x15 };
        constexpr uintptr_t kWakeImportRva      = 0x3A442C8;
        // What the game's menu shows for a slot (its builders 0x141EBF620 and 0x141EBF360): an item's name 0x140641B30(entry) (its stack's
        // own name, else its form's), a form's name 0x1405E49E0(form), a gun's ammo from its instance data ([[instance+0x20]+0x18], at
        // 0x141EBF844) and the ammo's name 0x14064B3D0(ammo+0x218) (at 0x141EBF864), a power in use 0x1419A6490(_, player, spell) (at
        // 0x141EBF53A).
        constexpr uintptr_t kEntryNameRva       = 0x641B30;
        constexpr uint8_t   kEntryNameStart[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x41,
                                                    0x10, 0x48, 0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x65 };
        constexpr uintptr_t kEntryNameFormCall  = 0x641BB1;
        constexpr uintptr_t kFormNameRva        = 0x5E49E0;
        constexpr uint8_t   kFormNameStart[]    = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0xD9, 0x33, 0xFF };
        constexpr uintptr_t kBuilderAmmoRva     = 0x1EBF844;
        constexpr uint8_t   kBuilderAmmo[]      = { 0x48, 0x8B, 0x40, 0x20, 0x48, 0x8B, 0x78, 0x18 };
        constexpr uintptr_t kBuilderAmmoNameRva = 0x1EBF864;
        constexpr uint8_t   kBuilderAmmoName[]  = { 0x48, 0x8D, 0x8F, 0x18, 0x02, 0x00, 0x00, 0xE8 };
        constexpr uintptr_t kAmmoNameRva        = 0x64B3D0;
        constexpr uint8_t   kAmmoNameStart[]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19, 0x48, 0x85, 0xDB, 0x74, 0x4D };
        constexpr uintptr_t kInstanceAmmoData   = 0x20;
        constexpr uintptr_t kAmmoDataAmmo       = 0x18;
        constexpr uintptr_t kAmmoName           = 0x218;
        constexpr uintptr_t kBuilderPowerRva    = 0x1EBF53A;
        constexpr uintptr_t kPowerInUseRva      = 0x19A6490;
        constexpr uint8_t   kPowerInUseStart[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89,
                                                    0x4C, 0x24, 0x08, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        constexpr uintptr_t kEquipManagerRva    = 0x61EA358; // (ActorEquipManager, as UseQuickkey passes it to its power calls)
        // Form types (+0x2E) and ids (+0x28); a weapon's type (its instance data +0x248, that one's +0x50, +0x38: 1-4 one-handed melee, 5-6
        // two-handed, as LeftWeapon reads it).
        constexpr uintptr_t kFormType           = 0x2E;
        constexpr uintptr_t kFormId             = 0x28;
        constexpr uint8_t   kTypeSpell          = 0x1C;
        constexpr uint8_t   kTypeArmor          = 0x22;
        constexpr uint8_t   kTypeWeapon         = 0x30;
        constexpr uint8_t   kTypeAlchemy        = 0x36;
        // The time slow of the game's own favorites (FavoritesMenu::ProcessMessage 0x141EC53D0, its opening at 0x141EC54CA): the time
        // manager [0x1461E5258] keeps a short list of time multipliers (+0x10 their number, +0x18 them: 12 bytes each, +0 how it counts (1:
        // multiplied in), +4 and +8 the world's and the player's); its second (+0x0C) is the favorites': 1, fFavoritesTimeMult (its value
        // at [0x145F5E9D8]; 1 when not above 0), twice. Then 0x140900D20(manager) multiplies them out (into +0x20 and +0x38) and
        // 0x1422CF380(_, [manager+0x20], 0) hands that to the game's timer. Closing writes 1, 1.0, 1.0 the same way (0x141EC5405).
        constexpr uintptr_t kMenuSlowRva        = 0x1EC54CA;
        constexpr uint8_t   kMenuSlowLoad[]     = { 0x4C, 0x8B, 0x1D };             // mov r11,[time manager]
        constexpr uint8_t   kMenuSlowSetting[]  = { 0xC5, 0xFA, 0x10, 0x1D };       // vmovss xmm3,[fFavoritesTimeMult] (at +7)
        constexpr uint8_t   kMenuSlowList[]     = { 0x49, 0x8B, 0x43, 0x18 };       // mov rax,[r11+18h] (at +0x0F)
        constexpr uintptr_t kMenuSlowWriteRva   = 0x1EC54F0;
        constexpr uint8_t   kMenuSlowWrite[]    = { 0xC6, 0x40, 0x0C, 0x01, 0xC5, 0xFA, 0x11, 0x58, 0x10, 0xC5, 0xFA, 0x11, 0x58, 0x14, 0x49, 0x8B, 0xCB, 0xE8 };
        constexpr uintptr_t kMenuSlowApplyRva   = 0x1EC5506;
        constexpr uint8_t   kMenuSlowApply[]    = { 0x45, 0x33, 0xC0, 0xC4, 0xC1, 0x7A, 0x10, 0x4B, 0x20, 0xE8 };
        constexpr uintptr_t kTimeManagerRva     = 0x61E5258;
        constexpr uintptr_t kTimeMultRva        = 0x5F5E9D8;
        constexpr uintptr_t kRecomputeRva       = 0x900D20;
        constexpr uint8_t   kRecomputeStart[]   = { 0xC5, 0xFA, 0x10, 0x05, 0x50, 0x78, 0x60, 0x04, 0x4C, 0x8B, 0xC9, 0x8B, 0x49, 0x10, 0xC5, 0xF8, 0x28, 0xC8,
                                                    0x85, 0xC9, 0x74, 0x3C, 0x49, 0x8B, 0x41, 0x18 };
        constexpr uintptr_t kApplyRva           = 0x22CF380;
        constexpr uint8_t   kApplyStart[]       = { 0x80, 0x3D, 0xE1, 0x83, 0xCF, 0x03, 0x00, 0xC5, 0xFA, 0x11, 0x0D, 0x75, 0xE5, 0x60, 0x03, 0xC5, 0xF8, 0x28,
                                                    0xE1, 0x74, 0x34, 0x45, 0x84, 0xC0 };
        constexpr uintptr_t kTimeCount          = 0x10;
        constexpr uintptr_t kTimeList           = 0x18;
        constexpr uintptr_t kTimeWorld          = 0x20;
        constexpr uintptr_t kFavoritesEntry     = 0x0C; // (the list's second entry)

        // Placing it and choosing: at least this far from your eyes; leaned back at least this much; each hand's slice from the middle
        // circle out to this far (wider than the drawn wheel: easy to hit seated), within this much in front of or behind it; a slice kept
        // until the hand is this far past its edge.
        constexpr float   kMinDistanceM  = 0.35f;
        constexpr float   kLeanDeg       = 10.0f;
        constexpr float   kReachM        = 0.30f;
        constexpr float   kDepthM        = 0.22f;
        constexpr float   kKeepDeg       = 4.0f;
        constexpr float   kKeepPastM     = 0.08f; // (fix174) a highlighted slice kept this far past the wheel's edge or depth
        constexpr float   kStickCentred  = 0.35f;
        constexpr int64_t kReadEveryMs   = 500;
        constexpr int64_t kShowWithinMs  = 1500;
        constexpr int     kFailsToStop   = 3;
        constexpr int64_t kLongestOpenMs = 120000;

        using UseFn       = bool (*)(void*, uint8_t);
        using NameFn      = const char* (*)(uintptr_t);
        using PowerFn     = bool (*)(void*, void*, void*);
        using RecomputeFn = void (*)(void*);
        using ApplyFn     = void (*)(void*, float, bool);
        using LockFn      = void (*)(void*);

        uintptr_t g_base{ 0 };
        bool      g_ready{ false };    // (the slots, their use, the inventory's read: the wheel itself)
        bool      g_names_ok{ false }; // (the names: else the form ids are shown)
        bool      g_ammo_ok{ false };
        bool      g_power_ok{ false };
        bool      g_time_ok{ false };
        std::string g_install_status{ "not checked" };

        int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        template <class T>
        T Read(uintptr_t at, T fallback = T{})
        {
            T v{};
            return SafeRead(&v, at, sizeof(T)) ? v : fallback;
        }

        uintptr_t CallTarget(uintptr_t at)
        {
            uint8_t op = 0;
            int32_t rel = 0;
            return SafeRead(&op, at, 1) && op == 0xE8 && SafeRead(&rel, at + 1, 4) ? at + 5 + static_cast<intptr_t>(rel) : 0;
        }

        // A rip-relative operand (the displacement in the instruction's last 4 bytes): where it points.
        uintptr_t RipOperand(uintptr_t at, size_t length)
        {
            int32_t rel = 0;
            return SafeRead(&rel, at + length - 4, sizeof(rel)) ? at + length + static_cast<intptr_t>(rel) : 0;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // ---- Your inventory's read lock, as LeftWeapon takes it (never waits: the frame's own read; a writer now: read a frame later) ----
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

        // ---- The game's own functions, each under a guard (a bad pointer: nothing, not a crash) ----
        // A name the game gives (a pointer into its string pool) copied out.
        bool CopyName(const char* p, char* out, size_t cap)
        {
            __try {
                size_t i = 0;
                for (; p != nullptr && i + 1 < cap && p[i] != 0; ++i) {
                    out[i] = p[i];
                }
                out[i] = 0;
                return i > 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out[0] = 0;
                return false;
            }
        }

        bool CallName(uintptr_t fn, uintptr_t arg, char* out, size_t cap)
        {
            out[0] = 0;
            if (fn == 0 || arg == 0) {
                return false;
            }
            const char* p = nullptr;
            __try {
                p = reinterpret_cast<NameFn>(fn)(arg);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            return CopyName(p, out, cap);
        }

        int CallPowerInUse(uintptr_t player, uintptr_t spell)
        {
            uintptr_t manager = 0;
            SafeRead(&manager, At(kEquipManagerRva), sizeof(manager));
            __try {
                return reinterpret_cast<PowerFn>(At(kPowerInUseRva))(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(player),
                           reinterpret_cast<void*>(spell))
                           ? 1
                           : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        int CallUseQuickkey(uintptr_t manager, int slot)
        {
            __try {
                return reinterpret_cast<UseFn>(At(kUseQuickkeyRva))(reinterpret_cast<void*>(manager), static_cast<uint8_t>(slot)) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        bool WriteTime(uintptr_t manager, uintptr_t list, float mult)
        {
            __try {
                auto* entry = reinterpret_cast<uint8_t*>(list + kFavoritesEntry);
                entry[0]    = 1;
                *reinterpret_cast<float*>(entry + 4) = mult;
                *reinterpret_cast<float*>(entry + 8) = mult;
                reinterpret_cast<RecomputeFn>(At(kRecomputeRva))(reinterpret_cast<void*>(manager));
                const float world = *reinterpret_cast<const float*>(manager + kTimeWorld);
                reinterpret_cast<ApplyFn>(At(kApplyRva))(nullptr, world, false);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        uintptr_t Manager()
        {
            const uintptr_t manager = Read<uintptr_t>(At(kManagerRva));
            return manager != 0 && Read<uintptr_t>(manager) == At(kManagerTableRva) ? manager : 0;
        }

        HWND GameWindow() { return g_framework != nullptr ? g_framework->get_window() : nullptr; }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // Where a hand points from on the wheel: the middle of the controller (between its grip and its front), as you point with it. The
        // same point places the wheel as it opens and picks the slices (the review: the grip alone placed it, the hand then started a few cm off
        // the middle).
        glm::vec3 HandPoint(bool left)
        {
            const glm::vec3 grip{ tracking::GripPose(left)[3] };
            const glm::vec3 aim{ tracking::AimPose(left)[3] };
            return Finite(aim) && Finite(grip) ? (grip + aim) * 0.5f : grip;
        }

        bool InFront()
        {
            const HWND window = GameWindow();
            const HWND front  = GetForegroundWindow();
            return window != nullptr && front != nullptr && (front == window || GetAncestor(front, GA_ROOT) == GetAncestor(window, GA_ROOT));
        }

        // ---- What each slot holds ----
        struct Slot
        {
            uintptr_t form{ 0 };
            uint32_t  id{ 0 };
            int       count{ 0 };
            WheelPicture::Card card{};
        };

        // A weapon form's melee kind (1 one-handed, 2 two-handed; 0 not melee), as LeftWeapon and the game's melee read it.
        int MeleeKind(uintptr_t form)
        {
            uintptr_t data = 0, info = 0;
            uint32_t  type = 0;
            if (form == 0 || Read<uint8_t>(form + kFormType) != kTypeWeapon || !SafeRead(&data, form + 0x248, 8) || data == 0 ||
                !SafeRead(&info, data + 0x50, 8) || info == 0 || !SafeRead(&type, info + 0x38, 4)) {
                return 0;
            }
            return type >= 1 && type <= 4 ? 1 : (type == 5 || type == 6 ? 2 : 0);
        }

        int StackCount(const uint8_t* entry)
        {
            uint32_t  number = 0;
            uintptr_t stacks = 0;
            std::memcpy(&number, entry + kEntryStackCount, sizeof(number));
            std::memcpy(&stacks, entry + kEntryStacks, sizeof(stacks));
            int n = 0;
            for (uint32_t k = 0; stacks != 0 && k < std::min<uint32_t>(number, 256); ++k) {
                uint32_t c = 0;
                if (SafeRead(&c, stacks + k * 0x10ull + 0x08, sizeof(c)) && c < 1000000) {
                    n += static_cast<int>(c);
                }
            }
            return n;
        }

        std::string Name(const char* text, uint32_t id)
        {
            return text[0] != 0 ? std::string{ text } : std::format("{:08X}", id);
        }

        enum class ReadResult
        {
            Ok,
            Busy,  // (your inventory being changed now: read again next frame)
            Failed
        };

        // The 12 slots as the game's menu shows them, read now (main thread).
        ReadResult ReadSlots(Slot out[kSlots], std::string& what)
        {
            for (int i = 0; i < kSlots; ++i) {
                out[i] = Slot{};
            }
            const uintptr_t manager = Manager();
            if (manager == 0) {
                // (fix173) Which way it failed: not made yet, or another type table than its own.
                const uintptr_t at = Read<uintptr_t>(At(kManagerRva));
                what = at == 0 ? std::string{ "the favorites manager not made yet" }
                               : std::format("the favorites manager's type table is rva {:x}, not its own {:x}", Read<uintptr_t>(at) - g_base, kManagerTableRva);
                return ReadResult::Failed;
            }
            const uint32_t size     = Read<uint32_t>(manager + kSlotsSize);
            const int32_t  capacity = Read<int32_t>(manager + kSlotsCapacity);
            const uintptr_t data    = capacity < 0 ? manager + kSlotsData : Read<uintptr_t>(manager + kSlotsData);
            uintptr_t      forms[kSlots]{};
            const int      slots = static_cast<int>(std::min<uint32_t>(size, kSlots));
            if (data == 0 || slots <= 0 || !SafeRead(forms, data, sizeof(uintptr_t) * static_cast<size_t>(slots))) {
                what = std::format("its slots not readable (size {}, capacity {})", size, capacity);
                return ReadResult::Failed;
            }
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (player == 0) {
                what = "no player";
                return ReadResult::Failed;
            }
            // Your inventory's entries, under its read lock: each favorite item's entry (+0x24 its slot), its name, count, flags, ammo.
            uintptr_t ammo[kSlots]{};
            {
                const uintptr_t lock = player + kActorInventoryLock;
                if (!TryLockRead(lock)) {
                    return ReadResult::Busy;
                }
                struct Unlock
                {
                    uintptr_t at;
                    ~Unlock() { UnlockRead(at); }
                } unlock{ lock };
                const uintptr_t list    = Read<uintptr_t>(player + kActorInventory);
                const uint32_t  count   = list != 0 ? Read<uint32_t>(list + kListCount) : 0;
                const uintptr_t entries = list != 0 ? Read<uintptr_t>(list + kListEntries) : 0;
                std::vector<uint8_t> raw;
                if (count > 0 && count <= 20000 && entries != 0) {
                    raw.resize(static_cast<size_t>(count) * kEntrySize);
                    if (!SafeRead(raw.data(), entries, raw.size())) {
                        raw.clear();
                    }
                }
                const size_t n = raw.size() / kEntrySize;
                int          entry_of[kSlots];
                std::fill(std::begin(entry_of), std::end(entry_of), -1);
                for (size_t i = 0; i < n; ++i) {
                    const int slot = static_cast<int8_t>(raw[i * kEntrySize + kEntrySlot]);
                    if (slot >= 0 && slot < kSlots && entry_of[slot] < 0) {
                        entry_of[slot] = static_cast<int>(i);
                    }
                }
                for (int s = 0; s < slots; ++s) {
                    if (entry_of[s] < 0) {
                        continue;
                    }
                    const uint8_t* e    = raw.data() + static_cast<size_t>(entry_of[s]) * kEntrySize;
                    uintptr_t      form = 0, instance = 0;
                    uint32_t       flags = 0;
                    std::memcpy(&form, e, sizeof(form));
                    std::memcpy(&instance, e + kEntryInstance, sizeof(instance));
                    std::memcpy(&flags, e + kEntryFlags, sizeof(flags));
                    if (form == 0) {
                        continue;
                    }
                    Slot& o     = out[s];
                    o.form      = form;
                    o.id        = Read<uint32_t>(form + kFormId);
                    o.count     = StackCount(e);
                    const auto type = Read<uint8_t>(form + kFormType);
                    char text[256]{};
                    if (g_names_ok) {
                        CallName(At(kEntryNameRva), entries + static_cast<size_t>(entry_of[s]) * kEntrySize, text, sizeof(text));
                    }
                    o.card.name     = Name(text, o.id);
                    const bool worn = (flags & 7) != 0;
                    if (type == kTypeWeapon) {
                        const int melee = MeleeKind(form);
                        o.card.left_ok  = melee != 0;
                        o.card.right    = worn;
                        if (melee != 0) {
                            o.card.kind = WheelPicture::kMelee;
                        } else {
                            const uintptr_t ammo_data = g_ammo_ok && instance != 0 ? Read<uintptr_t>(instance + kInstanceAmmoData) : 0;
                            ammo[s]                   = ammo_data != 0 ? Read<uintptr_t>(ammo_data + kAmmoDataAmmo) : 0;
                            o.card.kind               = ammo[s] != 0 || instance == 0 || !g_ammo_ok ? WheelPicture::kGun : WheelPicture::kThrown;
                        }
                    } else if (type == kTypeArmor) {
                        o.card.kind = WheelPicture::kApparel;
                        o.card.worn = worn;
                    } else if (type == kTypeAlchemy) {
                        o.card.kind = WheelPicture::kAid;
                    } else {
                        o.card.kind = WheelPicture::kOther;
                    }
                    o.card.none = o.count <= 0;
                }
                // Each gun's ammo carried (every entry of it).
                for (int s = 0; s < slots; ++s) {
                    if (ammo[s] == 0) {
                        continue;
                    }
                    int carried = 0;
                    for (size_t i = 0; i < n; ++i) {
                        uintptr_t form = 0;
                        std::memcpy(&form, raw.data() + i * kEntrySize, sizeof(form));
                        if (form == ammo[s]) {
                            carried += StackCount(raw.data() + i * kEntrySize);
                        }
                    }
                    out[s].card.detail = std::format("ammo {}", carried); // (the ammo's name added below, outside the lock)
                }
            }
            // Powers (a spell in the slot, no item) and forms no longer carried: their own names (outside the inventory's lock).
            const uint32_t left_id = LeftWeapon::FormId();
            for (int s = 0; s < slots; ++s) {
                Slot& o = out[s];
                if (o.form == 0 && forms[s] != 0) {
                    o.form          = forms[s];
                    o.id            = Read<uint32_t>(forms[s] + kFormId);
                    const auto type = Read<uint8_t>(forms[s] + kFormType);
                    char       text[256]{};
                    if (g_names_ok) {
                        CallName(At(kFormNameRva), forms[s], text, sizeof(text));
                    }
                    o.card.name = Name(text, o.id);
                    if (type == kTypeSpell) {
                        o.card.kind   = WheelPicture::kPower;
                        o.count       = 1;
                        o.card.in_use = g_power_ok && CallPowerInUse(player, forms[s]) == 1;
                        o.card.detail = "power";
                    } else {
                        // (An item the slot still names but you no longer carry: shown dim, nothing to use.)
                        o.card.kind   = type == kTypeWeapon ? (MeleeKind(forms[s]) != 0 ? WheelPicture::kMelee : WheelPicture::kGun)
                                        : type == kTypeArmor ? WheelPicture::kApparel
                                        : type == kTypeAlchemy ? WheelPicture::kAid
                                                               : WheelPicture::kOther;
                        o.card.none   = true;
                        o.card.detail = "none left";
                    }
                    continue;
                }
                if (o.form == 0) {
                    continue;
                }
                o.card.left = o.card.kind == WheelPicture::kMelee && left_id != 0 && o.id == left_id;
                if (ammo[s] != 0) {
                    char text[128]{};
                    if (CallName(At(kAmmoNameRva), ammo[s] + kAmmoName, text, sizeof(text))) {
                        o.card.more = text; // (its ammo's name, in the middle)
                    }
                } else if (o.card.kind == WheelPicture::kAid || o.card.kind == WheelPicture::kThrown ||
                           (o.card.kind == WheelPicture::kOther && o.count > 1)) {
                    o.card.detail = std::format("\xC3\x97{}", o.count); // (U+00D7, the multiplication sign)
                }
            }
            return ReadResult::Ok;
        }

        // ---- The wheel's state (main thread: the input's poll and the frame step are both on the game's main thread) ----
        struct State
        {
            bool      opening{ false }; // (asked for by the poll; placed by the next frame step)
            bool      open{ false };
            uint32_t  open_id{ 0 };
            int64_t   asked_ms{ 0 };
            int64_t   open_ms{ 0 };
            // (fix174) The wheel at each hand: [0] at the left hand, [1] the same wheel at the right (after fix174's first build:
            // one wheel shown at both hands with the left grip + trigger); each hand picks on its own one.
            struct Place
            {
                glm::vec3 centre{ 0.0f };
                glm::vec3 right{ 1.0f, 0.0f, 0.0f };
                glm::vec3 up{ 0.0f, 1.0f, 0.0f };
                glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
                glm::quat turn{ 1.0f, 0.0f, 0.0f, 0.0f };
            };
            Place place[2]{};
            bool  second{ false }; // (the right hand's own shown: off when the right hand has no pose)
            int       hover[2]{ -1, -1 };
            int       focus{ -1 };
            int       focus_hand{ -1 };
            int       right_used{ -1 }; // (the slice the right trigger just used: not chosen again for the right hand at the let-go)
            bool      read_ok{ false };
            int64_t   read_ms{ 0 };
            int64_t   reread_at{ 0 };
            int       read_tries{ 0 };
            bool      front_at_open{ false };
            bool      shown{ false };   // (its picture shown by the headset in this opening: only then does a let-go or the right trigger choose)
            int       hovers{ 0 };
            int       uses{ 0 };
            Slot      slots[kSlots]{};
            WheelPicture::Content drawn{};
            bool      drawn_valid{ false };
        };
        State g{};
        // (The poll's own.)
        bool g_left_was{ false }, g_right_was{ false };
        bool g_left_owned{ false }, g_right_owned{ false }, g_lb_held{ false }, g_rb_held{ false };
        bool g_choose_asked{ false }, g_use_asked{ false };
        std::atomic<int64_t> g_poll_ms{ 0 };
        std::atomic<int64_t> g_update_ms{ 0 };
        std::atomic<bool>    g_is_open{ false };
        std::atomic<int64_t> g_closed_ms{ 0 }; // (fix175) when it last closed
        // (Time slowed: the main thread's, or the watchdog's on the present thread, under g_time_mtx.)
        std::mutex           g_time_mtx;
        std::atomic<bool>    g_slowed{ false };
        float                g_slow_value{ 1.0f };
        // (Session.)
        int  g_show_failures{ 0 };
        bool g_off_session{ false };
        // (fix189) Off for the session because the game's own use faulted (that stays off until the game restarts: the game may hold a lock
        // of its own); and the setting as the frame step last saw it: turned off and on again, a wheel off only because its picture was not
        // shown (a hitch or a Virtual Desktop stall as likely as a headset that refuses it) is tried again, its count from 0.
        bool g_off_fault{ false };
        bool g_setting_was{ true };
        // (The poll and the frame step share the wheel's state without locks: both must be on the game's main thread (the log of fix165's
        // test: the gamepad's spoofing and LeftWeapon::Update both on one thread). Checked once each: else the wheel stays off.)
        std::atomic<DWORD> g_poll_thread{ 0 }, g_update_thread{ 0 };
        std::atomic<bool>  g_threads_differ{ false };

        void NoteThread(bool poll)
        {
            auto&       mine   = poll ? g_poll_thread : g_update_thread;
            const auto& theirs = poll ? g_update_thread : g_poll_thread;
            if (mine.load() != 0) {
                return;
            }
            mine.store(GetCurrentThreadId());
            const DWORD other = theirs.load();
            spdlog::info("[Wheel] (fix171) the {} runs on thread {}", poll ? "controller's poll" : "frame step", mine.load());
            if (other != 0 && other != mine.load()) {
                g_threads_differ = true;
                spdlog::warn("[Wheel] (fix171) the controller's poll ({}) and the frame step ({}) run on different threads: the wheel stays off (the game's "
                             "own favorites)",
                    g_poll_thread.load(), g_update_thread.load());
            }
        }
        int  g_open_logs{ 0 }, g_refusal_logs{ 0 }, g_choose_logs{ 0 }, g_read_logs{ 0 }, g_time_logs{ 0 };
        std::mutex  g_status_mtx;
        std::string g_status{ "Favorites wheel: not used yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        std::shared_ptr<runtimes::OpenXR> Xr()
        {
            static auto vr = VR::get();
            return vr->m_openxr; // (a copy: it may be made anew on another thread)
        }

        // The wheel can be used at all now (the setting, the game's code as expected, the headset with its controllers, not refused).
        const char* Unusable()
        {
            static auto vr = VR::get();
            const auto& s  = GameFlow::gStore.internalSettings;
            if (!s.favoritesWheel) {
                return "the setting is off";
            }
            if (!g_ready) {
                return "the game's favorites are not as in Starfield 1.16.236";
            }
            if (g_off_session) {
                return g_off_fault ? "the game's use of a favorite faulted (off for this session)"
                                   : "its picture was not shown three times (off for this session; the setting off and on again tries it again)";
            }
            if (s.alternativeJoyLayout) {
                return "the alternative controller layout (its left grip is the D-pad)";
            }
            if (g_threads_differ) {
                return "the controller's poll and the frame step run on different threads";
            }
            if (vr == nullptr || !vr->is_hmd_active() || !vr->is_using_controllers() || !vr->get_runtime()->is_openxr()) {
                return "not in the headset with its controllers (OpenXR)";
            }
            // (Swapped controllers: the gesture comes from one hand, the poses and buzzes of the other; not mapped: the game's own favorites.
            // The review.)
            if (vr->get_left_controller_index() != 1) {
                return "the controllers are swapped (Swap Controller Inputs)";
            }
            const auto xr = Xr();
            if (xr == nullptr) {
                return "no OpenXR";
            }
            if (xr->wheel_refused.load()) {
                return "the headset refused its layer";
            }
            return nullptr;
        }

        // Where you are allows it (quiet: a place where the gesture means something else, a menu's D-pad: not logged).
        const char* PlaceRefusal(bool& quiet)
        {
            quiet = true;
            if (GameFlow::isShowingMenu()) {
                return "a menu is open";
            }
            // (fix175) Build mode (no menu by the count since fix175): the left grip + left trigger + stick is the D-pad there (its list and
            // variants), not the wheel; not logged, and the D-pad not taken (Poll).
            if (GameFlow::isWorkshopShown()) {
                return "build mode";
            }
            if (GameFlow::isFavoritesShown()) {
                return "the game's favorites menu is open";
            }
            if (GameFlow::isInDialogue()) {
                return "a conversation";
            }
            if (GameFlow::isPilotingShip()) {
                return "the pilot's seat";
            }
            if (GameFlow::isLoadingScreenShown() || !GameFlow::worldBehindMenu()) {
                return "not in the game (a loading screen or the main menu)";
            }
            if (g_framework != nullptr && g_framework->is_drawing_ui()) {
                return "the mod's own menu is open";
            }
            if (const auto xr = Xr(); xr != nullptr && xr->panel_mode) {
                return "the menu panel is up";
            }
            quiet = false;
            if (!GameFlow::isInFirstPerson()) {
                return "not in first person";
            }
            if (GameFlow::isOnLadder() || LadderClimb::ViewAtHead()) {
                return "on a ladder";
            }
            if (GameFlow::isScannerOpen()) {
                return "the hand scanner is open";
            }
            if (ModSettings::showFlatScreenDisplay() || ModSettings::g_internalSettings.showQuadDisplay) {
                return "the flat screen is shown";
            }
            if (GameFlow::isImmovable() || GameFlow::isControlledByAI()) {
                return "the game is holding you still";
            }
            return nullptr;
        }

        // Your left hand is free for it.
        const char* HandRefusal()
        {
            if (HandGrab::HoldingWith(true) || Physical::HoldingPerson(true)) {
                return "your left hand holds something";
            }
            if (MeleeThrow::Away(true)) {
                return "your left hand's weapon is thrown";
            }
            if (HandGrab::GloveMark mark{}; HandGrab::AimMark(mark) && mark.hand == 0 && (mark.kind == 1 || mark.kind == 2)) {
                return "the gravity gloves have something in sight";
            }
            if (TempleGestures::HoldsTrigger(true)) {
                return "the left trigger is at your temple (the flashlight)";
            }
            const glm::vec3 hand = HandPoint(true);
            const glm::vec3 eyes{ tracking::HeadPose()[3] };
            if (!Finite(hand) || !Finite(eyes)) {
                return "no pose for your left hand";
            }
            if (glm::length(hand - eyes) < 0.17f) {
                return "your left hand is at your head";
            }
            return nullptr;
        }

        void Publish(int64_t now)
        {
            const auto xr = Xr();
            if (xr == nullptr) {
                return;
            }
            runtimes::OpenXR::WheelLayer layer{};
            if (g.open) {
                const auto& a          = g.place[0];
                const auto& b          = g.place[1];
                layer.pose.orientation = { a.turn.x, a.turn.y, a.turn.z, a.turn.w };
                layer.pose.position    = { a.centre.x, a.centre.y, a.centre.z };
                layer.pose2.orientation = { b.turn.x, b.turn.y, b.turn.z, b.turn.w };
                layer.pose2.position    = { b.centre.x, b.centre.y, b.centre.z };
                layer.second            = false; // (fix176: the right hand's wheel works unseen)
                layer.size             = WheelPicture::kSizeM;
                layer.open             = g.open_id;
                layer.ms               = now;
            }
            xr->set_wheel(layer);
        }

        // The picture asked for again when what it shows changed.
        void Redraw()
        {
            WheelPicture::Content c{};
            for (int i = 0; i < kSlots; ++i) {
                c.cards[i] = g.slots[i].card;
            }
            c.hover[0]   = g.hover[0];
            c.hover[1]   = g.hover[1];
            c.focus      = g.focus;
            c.focus_hand = g.focus_hand;
            c.reading    = !g.read_ok;
            c.open       = g.open_id;
            if (!g.drawn_valid || !(c == g.drawn)) {
                g.drawn       = c;
                g.drawn_valid = true;
                WheelPicture::Draw(c);
            }
        }

        // ---- Time ----
        void SlowTime()
        {
            std::scoped_lock _{ g_time_mtx };
            if (g_slowed.load() || !g_time_ok) {
                return;
            }
            float mult = Read<float>(At(kTimeMultRva), 1.0f);
            if (!(mult > 0.0f)) {
                mult = 1.0f; // (as the game: not above 0, no slow)
            }
            mult = std::clamp(mult, 0.05f, 1.0f);
            if (mult >= 0.999f) {
                if (g_time_logs++ < 5) {
                    spdlog::info("[Wheel] (fix171) time not slowed: the game's fFavoritesTimeMult is 1");
                }
                return;
            }
            const uintptr_t manager = Read<uintptr_t>(At(kTimeManagerRva));
            const uint32_t  count   = manager != 0 ? Read<uint32_t>(manager + kTimeCount) : 0;
            const uintptr_t list    = manager != 0 ? Read<uintptr_t>(manager + kTimeList) : 0;
            if (manager == 0 || list == 0 || count < 2 || count > 64) {
                if (g_time_logs++ < 5) {
                    spdlog::warn("[Wheel] (fix171) time not slowed: the game's time multipliers are not as expected (manager {:#x}, {} of them)", manager,
                        count);
                }
                return;
            }
            if (!WriteTime(manager, list, mult)) {
                if (g_time_logs++ < 5) {
                    spdlog::warn("[Wheel] (fix171) time not slowed: writing the favorites' multiplier failed");
                }
                return;
            }
            g_slowed     = true;
            g_slow_value = mult;
            if (g_time_logs++ < 40) {
                spdlog::info("[Wheel] (fix171) time slowed to {:.2f} (the game's favorites' own, fFavoritesTimeMult)", mult);
            }
        }

        // (Main thread only: the frame step and the input's poll, both on the game's main thread; never from the present thread, where the
        // game may be rebuilding its time list during a loading screen: the review.)
        void RestoreTime(const char* why)
        {
            std::scoped_lock _{ g_time_mtx };
            if (!g_slowed.load()) {
                return;
            }
            // (The game's own favorites menu open now: the multiplier is its own (the same value): left to it, which puts it back as it closes.)
            if (GameFlow::isFavoritesShown()) {
                g_slowed = false;
                if (g_time_logs++ < 40) {
                    spdlog::info("[Wheel] (fix171) time: the game's own favorites menu is open: its slow left to it ({})", why);
                }
                return;
            }
            const uintptr_t manager = Read<uintptr_t>(At(kTimeManagerRva));
            const uintptr_t list    = manager != 0 ? Read<uintptr_t>(manager + kTimeList) : 0;
            const float     world   = list != 0 ? Read<float>(list + kFavoritesEntry + 4, -1.0f) : -1.0f;
            const float     player  = list != 0 ? Read<float>(list + kFavoritesEntry + 8, -1.0f) : -1.0f;
            // (Only ours put back: one the game set meanwhile (its own favorites) is the game's to put back.)
            if (manager != 0 && list != 0 && world == g_slow_value && player == g_slow_value) {
                const bool ok = WriteTime(manager, list, 1.0f);
                // (Writing failed: still slowed, tried again at the next frame step; the review.)
                g_slowed = !ok;
                if (g_time_logs++ < 40) {
                    spdlog::info("[Wheel] (fix171) time {} ({})", ok ? "back to normal" : "could not be put back yet (writing failed; again next frame)", why);
                }
                return;
            }
            g_slowed = false;
            if (g_time_logs++ < 40) {
                spdlog::info("[Wheel] (fix171) time: the favorites' multiplier is {:.2f}, {:.2f} now (not the wheel's {:.2f}): left as the game has it ({})",
                    world, player, g_slow_value, why);
            }
        }

        // ---- Opening, choosing, closing ----
        void Close(const char* why)
        {
            const bool was = g.open || g.opening;
            if (was) {
                g_closed_ms = NowMs(); // (fix175: the palm powers wait a moment after it)
            }
            g.open         = false;
            g.opening      = false;
            g_is_open      = false;
            g_choose_asked = false;
            g_use_asked    = false;
            RestoreTime(why);
            Publish(NowMs());
            if (was && g_open_logs < 60) {
                ++g_open_logs;
                spdlog::info("[Wheel] (fix171) closed ({}; open {:.1f} s, {} new slices under your hands, {} used with the right trigger)", why,
                    g.open_ms != 0 ? (NowMs() - g.open_ms) / 1000.0 : 0.0, g.hovers, g.uses);
            }
            g.hover[0] = g.hover[1] = -1;
            g.focus                 = -1;
            g.drawn_valid           = false;
            SetStatus(std::format("Favorites wheel: closed ({})", why));
        }

        void ReadNow(int64_t now)
        {
            std::string      what;
            Slot             slots[kSlots]{};
            const ReadResult r = ReadSlots(slots, what);
            if (r == ReadResult::Busy) {
                ++g.read_tries;
                return;
            }
            g.read_ms    = now;
            g.read_tries = 0;
            if (r == ReadResult::Failed) {
                if (g_read_logs++ < 20) {
                    spdlog::warn("[Wheel] (fix171) the favorites could not be read: {} (shown empty)", what);
                }
                g.read_ok = true; // (shown empty, not "reading" for ever)
                for (auto& s : g.slots) {
                    s = Slot{};
                }
                return;
            }
            bool changed = !g.read_ok;
            for (int i = 0; i < kSlots; ++i) {
                changed    = changed || !(slots[i].card == g.slots[i].card);
                g.slots[i] = slots[i];
            }
            g.read_ok = true;
            if (changed && g_read_logs < 30) {
                ++g_read_logs;
                std::string list;
                int         used = 0;
                for (int i = 0; i < kSlots; ++i) {
                    const auto& c = g.slots[i].card;
                    if (c.kind == WheelPicture::kEmpty) {
                        continue;
                    }
                    ++used;
                    list += std::format("{}{}: {} {:08X}{}{}{}{}{}{}", list.empty() ? "" : "; ", i, c.name, g.slots[i].id,
                        c.detail.empty() ? std::string{} : " " + c.detail, c.right ? " [R]" : "", c.left ? " [L]" : "", c.in_use ? " [in use]" : "",
                        c.worn ? " [worn]" : "", c.none ? " [none left]" : "");
                }
                spdlog::info("[Wheel] (fix171) the favorites read: {} of 12 used{}{}", used, used > 0 ? ": " : "", list);
            }
        }

        // A wheel centred at `centre`, facing your eyes, upright, leaned back at least kLeanDeg (Spell Wheel VR's tilt; more when it is below
        // your eyes, as it then faces them). Returns the lean (radians).
        float PlaceWheel(State::Place& out, const glm::vec3& centre, const glm::vec3& eyes, const glm::vec3& forward)
        {
            glm::vec3 to_eyes = eyes - centre;
            to_eyes           = glm::length(to_eyes) > 1e-3f ? glm::normalize(to_eyes) : -forward;
            glm::vec3 flat{ to_eyes.x, 0.0f, to_eyes.z };
            flat               = glm::length(flat) > 1e-3f ? glm::normalize(flat) : glm::vec3{ -forward.x, 0.0f, -forward.z };
            flat               = glm::length(flat) > 1e-3f ? glm::normalize(flat) : glm::vec3{ 0.0f, 0.0f, 1.0f };
            const float face   = glm::degrees(std::asin(std::clamp(to_eyes.y, -1.0f, 1.0f)));
            const float lean   = glm::radians(std::clamp(std::max(face, kLeanDeg), -60.0f, 70.0f));
            const glm::vec3 z  = glm::normalize(flat * std::cos(lean) + glm::vec3{ 0.0f, 1.0f, 0.0f } * std::sin(lean));
            glm::vec3       x  = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, z);
            x                  = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 y  = glm::normalize(glm::cross(z, x));
            out.centre         = centre;
            out.right          = x;
            out.up             = y;
            out.normal         = z;
            out.turn           = glm::normalize(glm::quat_cast(glm::mat3{ x, y, z }));
            return lean;
        }

        // Where a hand is, pushed out to at least kMinDistanceM from your eyes.
        glm::vec3 WheelCentre(const glm::vec3& hand, const glm::vec3& eyes, const glm::vec3& forward)
        {
            const float d = glm::length(hand - eyes);
            if (d < 0.05f) {
                glm::vec3 flat{ forward.x, 0.0f, forward.z };
                flat = glm::length(flat) > 1e-3f ? glm::normalize(flat) : glm::vec3{ 0.0f, 0.0f, -1.0f };
                return eyes + flat * 0.4f;
            }
            return d < kMinDistanceM ? eyes + (hand - eyes) / d * kMinDistanceM : hand;
        }

        bool Open(int64_t now)
        {
            const glm::mat4 head = tracking::HeadPose();
            const glm::vec3 eyes{ head[3] };
            glm::vec3       forward = -glm::vec3{ head[2] };
            const glm::vec3 hand = HandPoint(true);
            if (!Finite(eyes) || !Finite(hand) || !Finite(forward) || glm::length(forward) < 1e-3f) {
                return false;
            }
            forward = glm::normalize(forward);
            const float d      = glm::length(hand - eyes);
            glm::vec3   centre = WheelCentre(hand, eyes, forward);
            // (fix174) The same wheel at your right hand too, each hand picking on its own. (fix176) Not shown there (after fix175:
            // only the left one is seen, the right hand working as if it were still there): the right hand
            // still picks on an unseen wheel centred where it was as the wheel opened (its highlight shown on the left one), so the two are
            // no longer pushed apart.
            const glm::vec3 right_hand = HandPoint(false);
            g.second                   = Finite(right_hand) && glm::length(right_hand - eyes) >= 0.17f;
            const glm::vec3 centre2    = g.second ? WheelCentre(right_hand, eyes, forward) : centre;
            const float lean = PlaceWheel(g.place[0], centre, eyes, forward);
            PlaceWheel(g.place[1], centre2, eyes, forward);
            g.open_id          = g.open_id + 1 == 0 ? 1 : g.open_id + 1;
            g.open             = true;
            g.opening          = false;
            g_is_open          = true;
            g.open_ms          = now;
            g.hover[0] = g.hover[1] = -1;
            g.focus                 = -1;
            g.focus_hand            = -1;
            g.right_used            = -1;
            g.read_ok               = false;
            g.read_tries            = 0;
            g.reread_at             = 0;
            g.hovers                = 0;
            g.uses                  = 0;
            g.drawn_valid           = false;
            g.front_at_open         = InFront();
            g.shown                 = false;
            ReadNow(now);
            SlowTime();
            haptics::MeleePulse(true, 0.3f, 0.06f); // (it opened: felt in the hand that opened it)
            if (g_open_logs < 60) {
                ++g_open_logs;
                spdlog::info("[Wheel] (fix171) opened {:.2f} m from your eyes ({}), leaned back {:.0f} degrees, {:.2f} m wide{}; the favorites {}; time {}",
                    glm::length(centre - eyes), d < kMinDistanceM ? std::format("your left hand at {:.2f} m: pushed out", d) : std::string{ "at your left hand" },
                    glm::degrees(lean), WheelPicture::kSizeM,
                    g.second ? std::format("; (fix176) the same wheel at your right hand, unseen, {:.2f} m from the left one", glm::length(centre2 - centre))
                             : std::string{ "; (fix174) only at your left hand (no pose for your right)" },
                    g.read_ok ? "read" : "being changed by the game (read next frame)",
                    g_slowed.load() ? std::format("slowed to {:.2f}", g_slow_value) : std::string{ "not slowed" });
            }
            SetStatus(std::format("Favorites wheel: open (opening {})", g.open_id));
            return true;
        }

        // The slot under a hand (-1 none), kept while the hand stays near it.
        int HoverOf(bool left, int current)
        {
            // (The right hand holding something with the hand grab picks nothing: its trigger equips what it holds, HandGrab; the review.)
            if (!left && (HandGrab::HoldingWith(false) || Physical::HoldingPerson(false))) {
                return -1;
            }
            const glm::vec3 point = HandPoint(left);
            if (!Finite(point)) {
                return current;
            }
            // (fix174: each hand on its own wheel; the right hand on the left hand's when its own is not shown.)
            const auto&     w  = g.place[left || !g.second ? 0 : 1];
            const glm::vec3 d  = point - w.centre;
            const float     px = glm::dot(d, w.right), py = glm::dot(d, w.up), pz = glm::dot(d, w.normal);
            const float     r  = std::sqrt(px * px + py * py);
            const float     inner = current >= 0 ? WheelPicture::kInnerM - 0.006f : WheelPicture::kInnerM + 0.004f;
            // (fix174) A slice highlighted stays the hand's until it moves onto another one or back into the middle circle (the middle
            // chooses nothing): past the wheel's edge or out of its depth it is kept (after fix173: items chosen by
            // highlighting and closing; fix173's log: a wheel open 11 s with 4 slices visited closed with "no hand on a slice", the hands
            // gone off the wheel as the trigger was let go). As Spell Wheel VR, letting go chooses what is highlighted.
            if (r < inner) {
                return -1;
            }
            if (std::abs(pz) > kDepthM || r > kReachM) {
                // (Kept only just past it, 8 cm: the review: a hand resting farther, e.g. the right one beside the wheel, does not keep a
                // slice it only brushed.)
                return std::abs(pz) <= kDepthM + kKeepPastM && r <= kReachM + kKeepPastM ? current : -1;
            }
            float angle = glm::degrees(std::atan2(py, px));
            if (angle < 0.0f) {
                angle += 360.0f;
            }
            if (current >= 0) {
                for (int k = 0; k < kSlots; ++k) {
                    if (WheelPicture::kSliceSlot[k] == current) {
                        const float delta = std::remainder(angle - 30.0f * static_cast<float>(k), 360.0f);
                        if (std::abs(delta) < 15.0f + kKeepDeg) {
                            return current;
                        }
                        break;
                    }
                }
            }
            const int k = static_cast<int>(std::floor((angle + 15.0f) / 30.0f)) % kSlots;
            return WheelPicture::kSliceSlot[k];
        }

        std::string Describe(int slot)
        {
            const auto& s = g.slots[slot];
            return std::format("slot {} ({} {:08X})", slot, s.card.name, s.id);
        }

        // A slot used for a hand: the right hand's the game's own use; the left hand's taken by LeftWeapon's hook (a melee weapon its weapon).
        // Returns: the right: 1 used, 0 not, -1 failed; the left: 1 taken, 2 nothing for it, 0 the hook did not run, -1 failed.
        int UseSlot(int slot, bool left)
        {
            const uintptr_t manager = Manager();
            if (manager == 0) {
                return -1;
            }
            LeftWeapon::BeginWheelUse(left);
            const int used = CallUseQuickkey(manager, slot);
            const int took = LeftWeapon::EndWheelUse();
            if (used < 0) {
                g_off_fault = true; // (fix189: not tried again when the setting is turned off and on)
            }
            if (used < 0 && !g_off_session) {
                // (A fault inside the game's own use: it may have left a lock of its own held; never called again this session. The review.)
                g_off_session = true;
                spdlog::error("[Wheel] (fix171) the game's UseQuickkey faulted on slot {} ({} hand): the wheel is off for this session (the game's own "
                              "favorites)",
                    slot, left ? "left" : "right");
            }
            return used < 0 ? -1 : (left ? took : used);
        }

        void Choose()
        {
            int         L = g.hover[0], R = g.hover[1];
            std::string what;
            if (R >= 0 && R == g.right_used) {
                what += std::format("right hand on {}: just used with the right trigger, not again; ", Describe(R));
                R = -1;
            }
            if (R >= 0 && (g.slots[R].card.kind == WheelPicture::kEmpty || g.slots[R].card.none)) {
                R = -1;
            }
            if (L >= 0 && (g.slots[L].card.kind == WheelPicture::kEmpty || g.slots[L].card.none)) {
                L = -1;
            }
            // (fix174) The left hand's slice when it is not a melee weapon (or dual wielding is off): used the game's own way, as the right
            // hand's (a power made the one in use, food eaten, a gun drawn); fix173's test: every left-hand choice (food, a gun, powers) was
            // dropped as "nothing for the left hand", and nothing could be chosen. Used before the right hand's, so the right
            // hand's own choice is the one that stays (two weapons: the right hand's drawn last). The same slot in both hands: used once.
            bool left_game_use = false;
            if (L >= 0 && (!g.slots[L].card.left_ok || !LeftWeapon::WheelReady())) {
                left_game_use = true;
            }
            // (The review: against the right hand's slice as it was, and the one the right trigger just used: never used twice.)
            if (L >= 0 && (L == g.hover[1] || L == g.right_used) && (left_game_use || g.slots[L].count < 2)) {
                what += std::format("both hands on {}{}: the right hand's; ", Describe(L), left_game_use ? "" : " with one carried");
                L = -1;
            }
            if (L >= 0 && left_game_use) {
                const int l = UseSlot(L, false);
                what += std::format("left hand: {} {}; ", Describe(L), l > 0 ? "used by the game (UseQuickkey; not a melee weapon for the left hand)"
                                                                      : l == 0 ? "the game did not use it" : "failed");
                if (l > 0) {
                    haptics::MeleePulse(true, 0.4f, 0.06f);
                }
            }
            if (g_off_session) {
                R = -1; // (the review: the game's use faulted just now: never called again)
                L = left_game_use ? L : -1;
            }
            if (R >= 0) {
                const int r = UseSlot(R, false);
                what += std::format("right hand: {} {}; ", Describe(R), r > 0 ? "used by the game (UseQuickkey)" : r == 0 ? "the game did not use it" : "failed");
                if (r > 0) {
                    haptics::MeleePulse(false, 0.4f, 0.06f);
                }
            }
            if (L >= 0 && !left_game_use) {
                const int l = UseSlot(L, true);
                what += std::format("left hand: {} {}; ", Describe(L),
                    l == 1 ? "taken as the left hand's weapon" : l == 2 ? "nothing (not taken)" : l == 0 ? "the left weapon's hook did not run" : "failed");
                if (l == 1) {
                    haptics::MeleePulse(true, 0.4f, 0.06f);
                }
            }
            if (what.empty()) {
                what = "nothing chosen (no hand on a slice)";
            }
            if (g_choose_logs < 60) {
                ++g_choose_logs;
                spdlog::info("[Wheel] (fix171) the left trigger let go: {}", what);
            }
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (g_base == 0) {
            g_install_status = "the game's module not found";
            spdlog::warn("[Wheel] (fix171) the favorites wheel is off: {}", g_install_status);
            return;
        }
        // (fix190) Each place through GameCode (GameCode.h). A call or reference found at a place not checked itself, compared with a place
        // both of which could be 0 in a game version they are not known in: that place known first; the equip manager (read, not checked):
        // known, or the power in use not shown.
        // The manager: its constructor's stores, the menu's use of it.
        const bool manager = SameCode(kCtorTableStoreRva, kCtorLea, sizeof(kCtorLea)) && RipOperand(At(kCtorTableStoreRva), 7) == At(kBaseTableRva) &&
                             SameCode(kCtorOwnTableRva, kCtorLea, sizeof(kCtorLea)) && RipOperand(At(kCtorOwnTableRva), 7) == At(kManagerTableRva) &&
                             SameCode(kCtorOwnTableRva + 7, kCtorOwnStore, sizeof(kCtorOwnStore)) &&
                             SameCode(kCtorManagerStoreRva, kCtorManagerStore, sizeof(kCtorManagerStore)) && RipOperand(At(kCtorManagerStoreRva), 7) == At(kManagerRva) &&
                             SameCode(kSinkUseRva, kSinkUse, sizeof(kSinkUse)) && RipOperand(At(kSinkUseRva), 7) == At(kManagerRva) &&
                             SameCode(kSinkUseRva + 7, kSinkUseTail, sizeof(kSinkUseTail)) && GameCode::Known({ kSinkUseRva + 10 }) &&
                             CallTarget(At(kSinkUseRva + 10)) == At(kUseQuickkeyRva);
        const bool use     = SameCode(kUseQuickkeyRva, kUseQuickkeyStart, sizeof(kUseQuickkeyStart)) && SameCode(kUseSlotReadRva, kUseSlotRead, sizeof(kUseSlotRead));
        const bool unlock  = SameCode(kUnlockRva, kUnlock, sizeof(kUnlock)) && GameCode::Known({ kUnlockRva + sizeof(kUnlock) - 2 }) &&
                            RipOperand(At(kUnlockRva + sizeof(kUnlock) - 2), 6) == At(kWakeImportRva);
        g_names_ok         = SameCode(kEntryNameRva, kEntryNameStart, sizeof(kEntryNameStart)) && CallTarget(At(kEntryNameFormCall)) == At(kFormNameRva) &&
                     SameCode(kFormNameRva, kFormNameStart, sizeof(kFormNameStart));
        g_ammo_ok  = SameCode(kBuilderAmmoRva, kBuilderAmmo, sizeof(kBuilderAmmo)) && SameCode(kBuilderAmmoNameRva, kBuilderAmmoName, sizeof(kBuilderAmmoName)) &&
                    CallTarget(At(kBuilderAmmoNameRva + 7)) == At(kAmmoNameRva) && SameCode(kAmmoNameRva, kAmmoNameStart, sizeof(kAmmoNameStart));
        g_power_ok = CallTarget(At(kBuilderPowerRva)) == At(kPowerInUseRva) && SameCode(kPowerInUseRva, kPowerInUseStart, sizeof(kPowerInUseStart)) &&
                     GameCode::Known({ kEquipManagerRva });
        g_time_ok  = SameCode(kMenuSlowRva, kMenuSlowLoad, sizeof(kMenuSlowLoad)) && RipOperand(At(kMenuSlowRva), 7) == At(kTimeManagerRva) &&
                    SameCode(kMenuSlowRva + 7, kMenuSlowSetting, sizeof(kMenuSlowSetting)) && RipOperand(At(kMenuSlowRva + 7), 8) == At(kTimeMultRva) &&
                    SameCode(kMenuSlowRva + 0x0F, kMenuSlowList, sizeof(kMenuSlowList)) && SameCode(kMenuSlowWriteRva, kMenuSlowWrite, sizeof(kMenuSlowWrite)) &&
                    CallTarget(At(kMenuSlowWriteRva + sizeof(kMenuSlowWrite) - 1)) == At(kRecomputeRva) &&
                    SameCode(kMenuSlowApplyRva, kMenuSlowApply, sizeof(kMenuSlowApply)) &&
                    CallTarget(At(kMenuSlowApplyRva + sizeof(kMenuSlowApply) - 1)) == At(kApplyRva) &&
                    SameCode(kRecomputeRva, kRecomputeStart, sizeof(kRecomputeStart)) && SameCode(kApplyRva, kApplyStart, sizeof(kApplyStart));
        g_ready          = manager && use && unlock;
        g_install_status = std::format("the favorites' slots {}, their use (UseQuickkey) {}, your inventory's read {}; names {}, a gun's ammo {}, the "
                                       "power in use {}, the favorites' time slow {}",
            manager ? "ok" : "differ", use ? "ok" : "differs", unlock ? "ok" : "differs", g_names_ok ? "ok" : "differ (form ids shown)",
            g_ammo_ok ? "ok" : "differs (not shown)", g_power_ok ? "ok" : "differs (not shown)", g_time_ok ? "ok" : "differs (time not slowed)");
        if (g_ready) {
            spdlog::info("[Wheel] (fix171) the favorites wheel: ready ({}): hold the left grip, then squeeze the left trigger (the left stick in the "
                         "middle); setting FavoritesWheel",
                g_install_status);
        } else {
            spdlog::warn("[Wheel] (fix171) the favorites wheel is off ({}): the game's own favorites as before", g_install_status);
        }
    }

    Taken Poll(const Input& in)
    {
        const int64_t now = NowMs();
        g_poll_ms.store(now);
        NoteThread(true);
        // (The frame step stopped running while it is open: closed here, so the controller is not kept from the game; the review.)
        if ((g.open || g.opening) && now - g_update_ms.load() > 600 && !g_threads_differ.load()) {
            Close("the frame step stopped running");
        }
        const bool left_fresh    = in.left_trigger && !g_left_was;
        const bool left_released = !in.left_trigger && g_left_was;
        const bool right_fresh   = in.right_trigger && !g_right_was;
        g_left_was               = in.left_trigger;
        g_right_was              = in.right_trigger;
        const char* unusable     = Unusable();
        // The opening: a fresh pull of the left trigger with the left grip held (in play, the left hand free, the stick in the middle).
        if (!g.open && !g.opening && left_fresh && in.left_grip && unusable == nullptr) {
            bool        quiet = false;
            const char* why   = PlaceRefusal(quiet);
            if (why == nullptr) {
                quiet = false;
                why   = HandRefusal();
            }
            if (why == nullptr && (std::abs(in.stick_x) > kStickCentred || std::abs(in.stick_y) > kStickCentred)) {
                why = "the left stick is pushed";
            }
            if (why == nullptr) {
                g.opening      = true;
                g.asked_ms     = now;
                g.open_ms      = 0;
                g_left_owned   = true;
                g_choose_asked = false;
                g_use_asked    = false;
                g_is_open      = true;
            } else if (!quiet && g_refusal_logs < 30) {
                ++g_refusal_logs;
                spdlog::info("[Wheel] (fix171) not opened: {}", why);
            }
        }
        const bool open = g.open || g.opening;
        if (open) {
            if (left_released) {
                g_choose_asked = true;
            }
            if (right_fresh) {
                g_use_asked = true;
            }
            // (Any trigger held while it is open stays the wheel's until let go: a pull just before it closes never reaches the game as a
            // fresh one: no shot, no left weapon drawn.)
            g_left_owned  = g_left_owned || in.left_trigger;
            g_right_owned = g_right_owned || in.right_trigger;
            g_lb_held     = g_lb_held || in.left_thumbrest;
            g_rb_held     = g_rb_held || in.right_thumbrest;
        }
        if (!in.left_trigger) {
            g_left_owned = false;
        }
        if (!in.right_trigger) {
            g_right_owned = false;
        }
        if (!in.left_thumbrest) {
            g_lb_held = false;
        }
        if (!in.right_thumbrest) {
            g_rb_held = false;
        }
        Taken t{};
        t.open           = open;
        t.left_trigger   = open || g_left_owned;
        t.right_trigger  = open || g_right_owned;
        t.left_shoulder  = open || g_lb_held;
        t.right_shoulder = open || g_rb_held;
        // (The D-pad: never while open; in play while the wheel can be used, none at all (the game's favorites menu would open). Where the
        // wheel cannot open (third person, the scanner, ...), the D-pad as before: the game's own favorites there.)
        bool quiet = false;
        t.dpad     = open || (unusable == nullptr && PlaceRefusal(quiet) == nullptr);
        return t;
    }

    void Update()
    {
        const int64_t now      = NowMs();
        const int64_t previous = g_update_ms.exchange(now);
        NoteThread(false);
        // (fix189) The setting turned off and on again: off for the session only because its picture was not shown, the wheel tried again.
        if (const bool on = GameFlow::gStore.internalSettings.favoritesWheel; on != g_setting_was) {
            g_setting_was = on;
            if (on) {
                if (g_off_session && !g_off_fault) {
                    spdlog::info("[Wheel] (fix189) the setting turned on again: the wheel tried again (it was off for this session: its picture not shown)");
                }
                g_show_failures = 0;
                g_off_session   = g_off_fault;
            }
        }
        if (!g.open && !g.opening) {
            if (g_slowed.load()) {
                RestoreTime("the wheel is closed");
            }
            return;
        }
        // Closed at once whenever it cannot stay open.
        {
            const char* why   = Unusable();
            bool        quiet = false;
            if (why == nullptr) {
                why = PlaceRefusal(quiet);
            }
            if (why == nullptr && now - g_poll_ms.load() > 300) {
                why = "the game stopped asking for the controller";
            }
            if (why == nullptr && previous != 0 && now - previous > 600) {
                why = "the frame stalled for over 0.6 s (a loading or a hitch)";
            }
            if (why == nullptr && g.open && g.front_at_open && !InFront()) {
                why = "the game's window went behind another";
            }
            if (why == nullptr && HandGrab::HoldingWith(true)) {
                why = "your left hand took hold of something";
            }
            if (why == nullptr && g.open && now - g.open_ms > kLongestOpenMs) {
                why = "open for 2 minutes";
            }
            if (why == nullptr && g.open) {
                // (Its picture never shown by the headset: closed; three openings in a row so, off for the session, the game's own favorites
                // back. Shown: the count starts again.)
                const auto xr = Xr();
                if (xr != nullptr && !g.shown && xr->wheel_shown_ms.load() >= g.open_ms) {
                    g.shown         = true;
                    g_show_failures = 0;
                }
                if (xr != nullptr && !g.shown && now - g.open_ms > kShowWithinMs) {
                    why = "its picture was not shown by the headset";
                    if (++g_show_failures >= kFailsToStop && !g_off_session) {
                        g_off_session = true;
                        spdlog::warn("[Wheel] (fix171) its picture was not shown {} times: the wheel is off for this session (the game's own favorites)",
                            g_show_failures);
                    }
                }
            }
            if (why != nullptr) {
                Close(why);
                return;
            }
        }
        if (g.opening && !Open(now)) {
            Close("no hand pose to place it at");
            return;
        }
        // Its grips and both hands are the wheel's while it is open: no grab, gloves' aim, throw or melee swing with them.
        HandGrab::LeaveGrip(true);
        HandGrab::LeaveGrip(false);
        HandGrab::MarkBusy(true);
        HandGrab::MarkBusy(false);
        MeleeThrow::HoldOffGrip(true);
        MeleeThrow::HoldOffGrip(false);
        // The favorites: read as it opens (a frame later while the game changes your inventory), every half second, and after a use.
        if (!g.read_ok || now - g.read_ms > kReadEveryMs || (g.reread_at != 0 && now >= g.reread_at)) {
            if (g.reread_at != 0 && now >= g.reread_at) {
                g.reread_at = 0;
            }
            ReadNow(now);
            if (!g.read_ok && g.read_tries > 60) {
                g.read_ok = true; // (your inventory busy for 60 frames: shown as it was; tried again in half a second)
                g.read_ms = now;
            }
        }
        // Each hand's slice: a new one buzzes in that hand and shows in the middle.
        for (int h = 0; h < 2; ++h) {
            const int was = g.hover[h];
            const int now_slot = HoverOf(h == 0, was);
            if (now_slot == was) {
                continue;
            }
            g.hover[h] = now_slot;
            if (h == 1 && now_slot != g.right_used) {
                g.right_used = -1;
            }
            if (now_slot >= 0) {
                ++g.hovers;
                g.focus      = now_slot;
                g.focus_hand = h;
                if (g.slots[now_slot].card.kind != WheelPicture::kEmpty) {
                    haptics::MeleePulse(h == 0, 0.3f, 0.05f);
                }
            } else if (g.focus_hand == h) {
                // (The hand left the slices: the other hand's slice, if any, in the middle.)
                g.focus      = g.hover[1 - h];
                g.focus_hand = g.focus >= 0 ? 1 - h : -1;
            }
        }
        // (Nothing is chosen or used before its picture has been shown: a quick grip-and-trigger tap would otherwise use whatever the right
        // hand happened to be near, with no wheel ever seen. The review.)
        if (!g.shown && (g_use_asked || g_choose_asked)) {
            if (g_choose_logs < 60) {
                ++g_choose_logs;
                spdlog::info("[Wheel] (fix171) {} before its picture was shown: nothing chosen", g_choose_asked ? "the left trigger let go" : "the right trigger");
            }
            g_use_asked = false;
            if (g_choose_asked) {
                g_choose_asked = false;
                Close("let go before it was shown");
                return;
            }
        }
        // The right trigger on a slice: used now, the wheel stays open.
        if (g_use_asked) {
            g_use_asked = false;
            const int R = g.hover[1];
            if (R >= 0 && g.slots[R].card.kind != WheelPicture::kEmpty && !g.slots[R].card.none) {
                const int r = UseSlot(R, false);
                ++g.uses;
                g.right_used = R;
                g.reread_at  = now + 300;
                if (r > 0) {
                    haptics::MeleePulse(false, 0.4f, 0.06f);
                }
                if (g_choose_logs < 60) {
                    ++g_choose_logs;
                    spdlog::info("[Wheel] (fix171) the right trigger on {}: {}", Describe(R),
                        r > 0 ? "used by the game (UseQuickkey); the wheel stays open" : r == 0 ? "the game did not use it" : "failed");
                }
            }
        }
        // The left trigger let go: each hand's slice to that hand, and closed.
        if (g_choose_asked) {
            g_choose_asked = false;
            Choose();
            Close("the left trigger let go");
            return;
        }
        Redraw();
        Publish(now);
    }

    bool IsOpen() { return g_is_open.load(); }

    bool OpenOrClosedWithin(int64_t ms) { return g_is_open.load() || NowMs() - g_closed_ms.load() < ms; }

    std::string Status()
    {
        std::string status;
        {
            std::scoped_lock _{ g_status_mtx };
            status = g_status;
        }
        const char* unusable = nullptr;
        if (!GameFlow::gStore.internalSettings.favoritesWheel) {
            unusable = "the setting is off (the game's own favorites)";
        } else if (!g_ready) {
            unusable = "off: the game's favorites are not as in Starfield 1.16.236 (the game's own favorites)";
        } else if (g_off_session) {
            unusable = g_off_fault ? "off for this session: the game's use of a favorite faulted (the game's own favorites)"
                                   : "off for this session: its picture was not shown (the game's own favorites; the setting off and on again tries it again)";
        }
        return std::format("{}{}; {}; {}", status, unusable != nullptr ? std::format(" ({})", unusable) : std::string{}, g_install_status,
            WheelPicture::Status());
    }
}
