#include "ImpactMelee.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <glm/glm.hpp>
#include <mods/VR.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include <CreationEngine/memory/ScanHelper.h>
#include <ModSettings.h>

#include "CreationEngineSingletonManager.h"
#include "HandGrab.h"
#include "GrenadeHand.h"
#include "body/BodyIK.h"
#include "LeftWeapon.h"
#include "Haptics.h"
#include "MeleeThrow.h"
#include "Physical.h"
#include "RE/N/NiCamera.h"
#include "body/SafeScan.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace ImpactMelee
{
    namespace
    {
        using steady = std::chrono::steady_clock;

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(steady::now().time_since_epoch()).count();
        }

        // Reads of game objects that are not ours go through ReadProcessMemory on our own process: a bad
        // address makes the call fail instead of crashing the game.
        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        // ---- The game's melee hit (Starfield 1.16.236) ----
        // In the melee hit (rva 198c9e0, attacker in rsi for the whole function):
        //   198ccef  call [vtable+0xaa8](attacker, &origin [rsp+0xd0], &direction [rsp+0xa0], 0)
        //   198ce8c  mov [rsp+0x20],ecx ; vmovaps xmm3,xmm7 (length = reach) ; lea r8,&direction ;
        //            lea rdx,&origin ; mov rcx,r15 ; call sweep (rva d4fe30) ; test al,al ; je miss
        //   198ceb4  (something was hit) ... an actor hit is kept when it is closer to the origin than
        //            xmm8 (198d02f: vmulss xmm1,xmm8,xmm8 ; vcomiss ; jb).
        // The hook before the sweep (198ce94) finds the origin and direction written and the length in
        // xmm3, so all three are replaced there.
        constexpr const char* kCastSitePattern =
            "89 4C 24 20 C5 F8 28 DF 4C 8D 84 24 A0 00 00 00 48 8D 94 24 D0 00 00 00 49 8B CF E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 48 8D 05";
        constexpr uintptr_t kCastSiteRva   = 0x198ce8c;
        constexpr uintptr_t kPreCastOffset = 0x08;
        constexpr uintptr_t kCallOffset    = 0x1b;
        constexpr uintptr_t kHitOffset     = 0x28;
        // The melee hit's first instruction (rva 198c9e0) and its prologue.
        constexpr uintptr_t kHitFunctionBack = 0x4ac;
        constexpr uint8_t   kHitPrologue[]   = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
              0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x0C, 0x00, 0x00 };

        // The attack step (rva 1980990): (attacker, number, flag). The game queues it as a small job when
        // an attack reaches its hit moment. It charges the attack's oxygen cost, runs the melee hit above,
        // then applies the hit to whoever it returned (damage), adds perk hits on people nearby, or plays
        // the miss. Found by its call of the melee hit:
        //   1980cda  mov edx,edi ; mov rcx,r14 ; call melee hit ; mov r15,rax ; cmp r14,[player]
        // The attack step applies the hit to the person the melee hit returned with
        //   1980f1e  xor r9d,r9d ; mov r8d,edi ; mov rdx,r15 ; call apply hit (rva 19813d0: attacker, target, number, 0)
        constexpr uintptr_t kApplyCallInStep = 0x58e;
        constexpr uint8_t   kApplyCall[]     = { 0x45, 0x33, 0xC9, 0x44, 0x8B, 0xC7, 0x49, 0x8B, 0xD7, 0xE8 };
        constexpr const char* kAttackCallPattern = "8B D7 49 8B CE E8 ? ? ? ? 4C 8B F8 4C 3B 35";
        constexpr uintptr_t   kAttackCallRva     = 0x1980cda;
        constexpr uintptr_t   kAttackCallInStep  = 0x34a;
        constexpr uint8_t     kAttackPrologue[]  = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x38, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00 };

        // Checked before hooking, so the stack slots and registers mean what they meant in 1.16.236.
        constexpr uint8_t kOriginFill[] = { 0x48, 0x8B, 0x06, 0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8D,
            0x94, 0x24, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCE, 0xFF, 0x90, 0xA8, 0x0A, 0x00, 0x00 };
        constexpr uintptr_t kOriginFillBack = 0x19d;
        constexpr uint8_t   kWeaponRead[]   = { 0x48, 0x8B, 0x8C, 0x24, 0x28, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x38, 0x48,
              0x8B, 0x81, 0x48, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x48, 0x50, 0x8B, 0x51, 0x38 };
        constexpr uintptr_t kWeaponReadBack = 0x251;
        constexpr uint8_t   kReachCheck[]   = { 0xC4, 0xC1, 0x3A, 0x59, 0xC8, 0xC5, 0xF8, 0x2F, 0xE9, 0x0F, 0x82 };
        constexpr uintptr_t kReachCheckAhead = 0x1a3;
        constexpr uint8_t   kPlayerCompare[] = { 0x48, 0x3B, 0x35 }; // cmp rsi,[global] at 198cd2c
        constexpr uintptr_t kPlayerCompareInFill = 0x3d;

        // The sweep (rva d4fe30) makes its sphere from a game setting: vmovss xmm2,[setting] at +0x75,
        // then the sphere is made at +0x7d with xmm2 as its radius. Its prologue sets rbp = entry rsp -
        // 0x148, so its return address is at [rbp+0x148] there.
        constexpr uint8_t kSweepPrologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18,
            0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xB8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x20, 0x02, 0x00,
            0x00 };
        constexpr uintptr_t kSweepRadiusLoad  = 0x75;
        constexpr uint8_t   kRadiusLoad[]     = { 0xC5, 0xFA, 0x10, 0x15 };
        constexpr uintptr_t kSweepSphere      = 0x7d;
        constexpr uint8_t   kSphereArgs[]     = { 0x48, 0x8D, 0x55, 0x10, 0x48, 0x8D, 0x4D, 0x20, 0xE8 };
        constexpr uintptr_t kSweepReturnAtRbp = 0x148;

        // Stack slots of the melee hit at the sweep.
        constexpr uintptr_t kOriginSlot    = 0xd0;
        constexpr uintptr_t kDirectionSlot = 0xa0;
        constexpr uintptr_t kWeaponSlot    = 0x628;
        constexpr uintptr_t kHitPointSlot  = 0x320;

        // What strikes: how far past the controller's grip it is along the pointing direction, and the
        // sphere swept along the swing (metres, before the body-height scale).
        struct Strike
        {
            float       reach;
            float       radius;
            const char* name;
        };
        enum Kind : int { kFist = 0, kOneHanded = 1, kTwoHanded = 2 };
        constexpr Strike kStrike[] = {
            { 0.06f, 0.10f, "fist" },
            { 0.25f, 0.12f, "one-handed weapon or gun" },
            { 0.45f, 0.15f, "two-handed weapon" },
        };
        constexpr float kMaxRadius = 0.20f; // a wide arc may widen the sphere up to this
        // (fix154) The left hand strikes with its weapon when it holds one (dual wielding: LeftWeapon), else with the fist. (Its hit's
        // damage is the game's for what the right hand holds.)
        int LeftKind() { return std::clamp(LeftWeapon::Kind(), 0, 2); }

        // Swing detection.
        constexpr float   kSlowFraction  = 0.4f;      // below this share of the swing speed a hand is at rest again
        constexpr int64_t kLeadUs        = 250'000;   // a swing's path starts at most this long before it got fast
        constexpr int64_t kMaxSwingUs    = 500'000;   // and ends at most this long after
        constexpr int64_t kCooldownUs    = 350'000;   // between two attacks started by swings
        constexpr int64_t kPressUs       = 90'000;    // the melee button is held this long (a few game polls)
        constexpr int64_t kSwingValidUs  = 1'000'000; // an attack this long after its swing jabs at the hand instead
        constexpr int64_t kStaleUs       = 250'000;   // hands not tracked for this long: the game's own sweep is kept
        constexpr float   kBackwardDot   = -0.6f;     // moving back toward you (pulling a punch back) is not a swing
        constexpr float   kMinPath       = 0.12f;     // a shorter path becomes a jab
        constexpr float   kJabBack       = 0.10f;
        constexpr float   kJabForward    = 0.15f;
        constexpr float   kBehindStart   = 0.05f;     // the sweep starts a little before the path, for what it already touched

        // Instant hits: while a swing moves, the game's melee hit sweeps each new stretch of the strike
        // point's path (one per tracked pose); the first stretch that strikes something is the hit, applied
        // right then. No button, no attack animation, so every swing counts.
        constexpr float   kInstantSlowFraction = 0.5f;    // a swing ends (and that hand can swing again) below half the swing speed
        constexpr float   kReverseDot          = -0.2f;   // or as soon as the hand turns back against the swing
        constexpr int64_t kInstantCooldownUs   = 150'000; // between two swings of the same hand
        constexpr int64_t kInstantMaxUs        = 700'000; // a swing sweeps for at most this long
        constexpr float   kMinStretch          = 0.03f;   // a shorter stretch is swept this far forward from the strike point

        // Tracked hand poses (stage space), stored when they change (the runtime updates them once per
        // stereo pair). Mapped into the world when an attack happens, so the path is where the hand went
        // relative to you, wherever you have walked or turned since.
        struct Sample
        {
            int64_t   t_us{ 0 };
            bool      valid[2]{};
            glm::vec3 grip[2]{};
            glm::vec3 forward[2]{};
        };
        constexpr int kSamples    = 256;
        constexpr int kPathPoints = 64;

        struct Swing
        {
            int     hand{ -1 }; // 0 left, 1 right
            int64_t start_us{ 0 };
            int64_t trigger_us{ 0 };
            int64_t end_us{ 0 };
            float   peak{ 0.0f };
            bool    used{ false };
        };
        constexpr int kSwings = 4;

        std::mutex                   g_mutex;
        std::array<Sample, kSamples> g_samples{};
        int                          g_next{ 0 };
        int                          g_count{ 0 };
        int64_t                      g_last_sample_us{ 0 };
        std::array<Swing, kSwings>   g_swing_queue{};
        int                          g_swing_next{ 0 };
        int                          g_current_swing{ -1 };

        // Update() only (one thread).
        struct HandTrack
        {
            bool      have{ false };
            glm::vec3 grip{ 0.0f };
            glm::vec3 forward{ 0.0f };
            int64_t   t_us{ 0 };
            glm::vec3 velocity{ 0.0f }; // from positions, for runtimes that give no velocity
        };
        HandTrack g_track[2]{};
        bool      g_armed[2]{ true, true };
        int64_t   g_slow_us[2]{ 0, 0 };
        int64_t   g_last_trigger_us{ 0 };

        std::atomic<int64_t> g_press_until_us{ 0 };
        std::atomic<int>     g_kind{ kOneHanded }; // the weapon the game's melee last used
        std::atomic<bool>    g_ready{ false };
        std::atomic<bool>    g_adjust_reach_check{ false };
        const char*          g_off_reason{ "not installed yet" };
        uintptr_t            g_game_radius_setting{ 0 };
        uintptr_t            g_player_global{ 0 };
        uintptr_t            g_sweep_return{ 0 };
        uintptr_t            g_hit_function{ 0 };
        uintptr_t            g_attack_function{ 0 };
        uintptr_t            g_apply_function{ 0 };
        safetyhook::MidHook  g_attack_entry_hook{};
        // What the game passes to the attack step for your attacks (instant hits pass the same; 0 until seen).
        std::atomic<bool>     g_have_attack_args{ false };
        std::atomic<uint32_t> g_attack_number{ 0 };
        std::atomic<uint32_t> g_attack_flag{ 1 };
        std::atomic<int>      g_attack_steps{ 0 };

        // Instant hits, per hand (Update only).
        struct InstantSwing
        {
            bool      active{ false };
            bool      done{ false }; // struck something: no more sweeps for this swing
            int64_t   trigger_us{ 0 };
            glm::vec3 last_grip{ 0.0f };    // stage-space pose the next stretch starts from
            glm::vec3 last_forward{ 0.0f };
            glm::vec3 direction{ 0.0f };    // which way the strike point was moving when the swing started
            float     peak{ 0.0f };
            int       sweeps{ 0 };
            int       result{ 0 };          // Outcome
            int64_t   result_us{ 0 };
            int       number{ 0 };          // for the log
        };
        enum Outcome : int { kMissed = 0, kSurface = 1, kPerson = 2, kFailed = 3 };
        InstantSwing     g_instant[2]{};
        bool             g_instant_armed[2]{ true, true };
        int64_t          g_instant_rest_us[2]{ 0, 0 };
        int64_t          g_instant_last_trigger_us[2]{ 0, 0 };
        std::atomic<bool> g_instant_failed{ false }; // an exception in the game's code: swings press melee again
        std::atomic<int> g_instant_swings{ 0 };
        std::atomic<int> g_instant_people{ 0 };
        std::atomic<int> g_instant_surfaces{ 0 };
        std::atomic<int> g_instant_misses{ 0 };
        std::atomic<int> g_instant_sweeps{ 0 };

        safetyhook::MidHook g_precast_hook{};
        safetyhook::MidHook g_hit_hook{};
        safetyhook::MidHook g_sphere_hook{};

        std::atomic<int>   g_swings{ 0 };
        std::atomic<int>   g_attacks{ 0 };
        std::atomic<int>   g_swept{ 0 };
        std::atomic<int>   g_strikes{ 0 };
        std::atomic<int>   g_other_attackers{ 0 };
        std::atomic<float> g_game_radius{ 0.0f };

        // ---- (fix166) The left hand's hits with its own weapon ----
        // (The left saber's hits did the right weapon's damage. A helper's research, checked against 1.16.236.) The melee hit
        // (rva 198c9e0, at 198ca84) and applying it (rva 19813d0, at 1981560) take the attacker's weapon from 0x141987B20(actor, &out,
        // equip index): out {form, instance data}, a reference added for the caller (who lets it go), from the actor's equipped item (the
        // right hand's: Starfield has one weapon slot). Hooked: while the mod's own left-hand hit runs on this thread (instant hits' left
        // stretches, the left weapon's throws), it gives your left weapon instead (its form and its own inventory item's instance data,
        // LeftWeapon::AttackInstance), for you and that hit's index only; everything else goes to the game's function as it is (one
        // thread-local test: it runs about 125 times a frame, on many threads). Left as it is: 0x141988200 (called in the hit's fill at
        // 0x141B2E5B9), the hit's item health, read from the equipped (right) item's own inventory entry: the right weapon's condition
        // (the default for an item without one).
        constexpr uintptr_t kEquippedWeaponRva     = 0x1987b20;
        constexpr uint8_t   kEquippedWeaponStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x41, 0x8B,
                                                       0xC0, 0x48, 0x8B, 0xDA };
        constexpr uintptr_t kHitRva                = 0x198c9e0;
        constexpr uintptr_t kHitWeaponCallRva      = 0x198ca84;
        constexpr uintptr_t kApplyRva              = 0x19813d0;
        constexpr uintptr_t kApplyWeaponCallRva    = 0x1981560;
        // And a test log of the first hits the mod applies (each hand's): the hit just after its fill (0x141B2E170, called at 0x1419819EB;
        // the hit at [rbp+0x130], 0x141981944, rbp the apply's frame, 0x1419813F3): its weapon at +0x58, its damage at +0x90.
        constexpr uintptr_t kAppliedRva            = 0x19819f0;
        constexpr uint8_t   kApplied[]             = { 0x8B, 0x8D, 0x08, 0x02, 0x00, 0x00, 0x8B, 0xC1, 0xC1, 0xE8, 0x15 };
        constexpr uintptr_t kAppliedFillCallRva    = 0x19819eb;
        constexpr uintptr_t kHitFillRva            = 0x1b2e170;
        constexpr uintptr_t kAppliedHitAtRva       = 0x1981944;
        constexpr uint8_t   kAppliedHitAt[]        = { 0x48, 0x8D, 0x8D, 0x30, 0x01, 0x00, 0x00 };
        constexpr uintptr_t kApplyFrameRva         = 0x19813f3;
        constexpr uint8_t   kApplyFrame[]          = { 0x48, 0x8D, 0xA8, 0x68, 0xFD, 0xFF, 0xFF };
        constexpr uintptr_t kHitOnFrame = 0x130, kHitWeapon = 0x58, kHitDamage = 0x90;
        safetyhook::InlineHook g_weapon_hook{};
        safetyhook::MidHook    g_applied_hook{};
        std::atomic<bool>   g_left_weapon_ready{ false };
        std::atomic<int>    g_left_given{ 0 };
        std::atomic<int>    g_applied_told[2]{};
        // (This thread) the left weapon given to the game's hit now, and the hand whose hit the mod applies now (-1: none).
        thread_local bool      t_left_on{ false };
        thread_local uintptr_t t_left_actor{ 0 };
        thread_local uint32_t  t_left_index{ 0 };
        thread_local uintptr_t t_left_form{ 0 };
        thread_local uintptr_t t_left_instance{ 0 };
        thread_local int       t_applying{ -1 };

        void* OnEquippedWeapon(void* actor, uintptr_t* out, uint32_t index)
        {
            if (!t_left_on) {
                return g_weapon_hook.unsafe_call<void*>(actor, out, index);
            }
            if (reinterpret_cast<uintptr_t>(actor) != t_left_actor || index != t_left_index || out == nullptr || t_left_instance == 0) {
                return g_weapon_hook.unsafe_call<void*>(actor, out, index);
            }
            // (As the game's function hands it out: the form, the instance data with a reference for the caller.)
            out[0] = t_left_form;
            out[1] = t_left_instance;
            _InterlockedIncrement(reinterpret_cast<volatile long*>(t_left_instance + 8));
            if (g_left_given.fetch_add(1) < 6) {
                uint32_t id = 0;
                SafeRead(&id, t_left_form + 0x28, sizeof(id));
                spdlog::info("[Melee] (fix166) a left hand's hit given its own weapon {:08X} (instance data {:#x}) instead of your equipped one", id, t_left_instance);
            }
            return out;
        }

        // The left weapon given to the game's hit while this lives (this thread; only when the hit is the left hand's and LeftWeapon has
        // its item: else the game's own weapon, as before).
        struct LeftWeaponScope
        {
            LeftWeaponScope(bool left, uint32_t index)
            {
                uintptr_t form = 0, instance = 0;
                auto*     player = CreationEngineSingletonManager::GetPlayerRef();
                if (!left || !g_left_weapon_ready.load(std::memory_order_relaxed) || player == nullptr || !LeftWeapon::AttackInstance(form, instance)) {
                    return;
                }
                t_left_actor    = reinterpret_cast<uintptr_t>(player);
                t_left_index    = index;
                t_left_form     = form;
                t_left_instance = instance;
                t_left_on       = true;
            }
            ~LeftWeaponScope() { t_left_on = false; }
            LeftWeaponScope(const LeftWeaponScope&)            = delete;
            LeftWeaponScope& operator=(const LeftWeaponScope&) = delete;
        };

        // The hand whose hit the mod applies, for the test log (this thread).
        struct ApplyingScope
        {
            explicit ApplyingScope(int hand) { t_applying = hand; }
            ~ApplyingScope() { t_applying = -1; }
            ApplyingScope(const ApplyingScope&)            = delete;
            ApplyingScope& operator=(const ApplyingScope&) = delete;
        };

        void OnHitApplied(safetyhook::Context& ctx)
        {
            const int hand = t_applying;
            if (hand < 0 || hand > 1 || g_applied_told[hand].fetch_add(1) >= 8) {
                return;
            }
            uintptr_t weapon = 0;
            uint32_t  id     = 0;
            float     damage = -1.0f;
            if (SafeRead(&weapon, ctx.rbp + kHitOnFrame + kHitWeapon, sizeof(weapon)) && weapon != 0) {
                SafeRead(&id, weapon + 0x28, sizeof(id));
            }
            SafeRead(&damage, ctx.rbp + kHitOnFrame + kHitDamage, sizeof(damage));
            spdlog::info("[Melee] (fix166) the {} hand's hit as the game applies it: weapon {:08X}, damage {:.1f}", hand == 0 ? "left" : "right", id, damage);
        }

        // The two parts of the attack step that instant hits run themselves, called the way the step calls
        // them. An exception inside either is caught so it does not take the game down with it.
        // The melee hit: (attacker, number) -> the person struck, or null.
        bool CallHit(uintptr_t function, void* actor, uint32_t number, uintptr_t& target)
        {
            using HitFn = uintptr_t (*)(void*, uint32_t);
            __try {
                target = reinterpret_cast<HitFn>(function)(actor, number);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        // Applying the hit (damage) to that person: (attacker, target, number, 0).
        bool CallApply(uintptr_t function, void* actor, uintptr_t target, uint32_t number)
        {
            using ApplyFn = void (*)(void*, uintptr_t, uint32_t, uint64_t);
            __try {
                reinterpret_cast<ApplyFn>(function)(actor, target, number, 0);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Start of the attack step: what the game passes for your attacks.
        void OnAttackEntry(safetyhook::Context& ctx)
        {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr || ctx.rcx != reinterpret_cast<uintptr_t>(player)) {
                return;
            }
            const auto number = static_cast<uint32_t>(ctx.rdx);
            const auto flag   = static_cast<uint32_t>(ctx.r8 & 0xFF);
            g_attack_number   = number;
            g_attack_flag     = flag;
            g_have_attack_args = true;
            const int steps   = g_attack_steps.fetch_add(1) + 1;
            if (steps <= 3 || steps % 50 == 0) {
                spdlog::info("[Melee] the game's attack step #{} for you: number {}, flag {}", steps, number, flag);
            }
        }

        // The player's attack being swept on this thread (set before the sweep, read in and after it).
        thread_local bool      t_ours{ false };
        thread_local float     t_radius{ 0.0f };
        thread_local int       t_hand{ 1 };
        thread_local glm::vec3 t_start{};
        thread_local glm::vec3 t_axis{};
        thread_local float     t_length{ 0.0f };

        // An instant hit's stretch of path (stage space), for the next melee hit on this thread. Or, for a thrown weapon
        // (SweepWorld), a stretch of the game world (a to b) and the sphere's radius.
        struct Stretch
        {
            bool      set{ false };
            int       hand{ 1 };
            glm::vec3 grip0{ 0.0f }, forward0{ 0.0f }, grip1{ 0.0f }, forward1{ 0.0f };
            bool      world{ false };
            glm::vec3 a{ 0.0f }, b{ 0.0f };
            float     radius{ 0.0f };
        };
        thread_local Stretch t_stretch{};
        // A world stretch's sweep (SweepWorld): set before the sweep, and whether and where it struck something.
        thread_local bool      t_world{ false };
        thread_local bool      t_world_hit{ false };
        thread_local glm::vec3 t_world_point{ 0.0f };
        // Set when the melee hit took the stretch (its sweep went along it). If not, whatever it returns
        // came from the game's own sweep and is ignored.
        thread_local bool t_stretch_taken{ false };

        bool ShouldLog(int n) { return n <= 20 || n % 10 == 0; }

        // What the game's melee is using: no weapon or hand-to-hand (fists), two-handed melee, or anything
        // else (one-handed melee weapons and guns). The game reads the same fields at 198cc3b.
        int WeaponKind(uintptr_t rsp, int& type)
        {
            type             = -1;
            uintptr_t weapon = 0;
            if (!SafeRead(&weapon, rsp + kWeaponSlot, sizeof(weapon)) || weapon == 0) {
                return kFist;
            }
            uintptr_t data = 0, info = 0;
            uint32_t  value = 0;
            if (SafeRead(&data, weapon + 0x248, sizeof(data)) && SafeRead(&info, data + 0x50, sizeof(info)) && SafeRead(&value, info + 0x38, sizeof(value))) {
                type = static_cast<int>(value);
            }
            if (type == 0) {
                return kFist;
            }
            if (type == 5 || type == 6) {
                return kTwoHanded;
            }
            return kOneHanded;
        }

        struct Path
        {
            glm::vec3 start{};
            glm::vec3 end{};
            float     scale{ 1.0f };  // game units per tracked metre
            float     widest{ 0.0f }; // how far the swing curved away from a straight line (world)
            int       hand{ 1 };
            bool      swing{ false };
            bool      jab{ false };
            int       points{ 0 };
            int64_t   since_trigger_us{ 0 };
            int64_t   swept_us{ 0 };
            float     peak{ 0.0f };
        };

        // The strike point's path for an attack happening now: the oldest swing no attack has used yet,
        // or a jab at the right hand.
        bool BuildPath(int kind, Path& out)
        {
            std::array<glm::vec3, kPathPoints> grips;
            std::array<glm::vec3, kPathPoints> forwards;
            int       n = 0;
            glm::vec3 last_grip{ 0.0f }, last_forward{ 0.0f };
            bool      have_last = false;
            Swing     swing{};
            bool      use_swing = false;
            int64_t   first_t = 0, last_t = 0;
            const int64_t t = NowUs();
            {
                std::scoped_lock _{ g_mutex };
                if (g_count == 0 || t - g_last_sample_us > kStaleUs) {
                    return false;
                }
                int pick = -1;
                for (int i = 0; i < kSwings; ++i) {
                    const auto& s = g_swing_queue[i];
                    if (s.hand >= 0 && !s.used && t - s.trigger_us < kSwingValidUs && (pick < 0 || s.trigger_us < g_swing_queue[pick].trigger_us)) {
                        pick = i;
                    }
                }
                if (pick >= 0) {
                    g_swing_queue[pick].used = true;
                    swing                    = g_swing_queue[pick];
                    use_swing                = true;
                }
                const int     hand = use_swing ? swing.hand : 1;
                const int64_t from = swing.start_us;
                const int64_t to   = swing.end_us != 0 ? swing.end_us : t;
                for (int i = 0; i < g_count; ++i) {
                    const Sample& sample = g_samples[(g_next - g_count + i + kSamples * 2) % kSamples];
                    if (!sample.valid[hand]) {
                        continue;
                    }
                    last_grip    = sample.grip[hand];
                    last_forward = sample.forward[hand];
                    have_last    = true;
                    if (use_swing && sample.t_us >= from && sample.t_us <= to && n < kPathPoints) {
                        if (n == 0) {
                            first_t = sample.t_us;
                        }
                        last_t      = sample.t_us;
                        grips[n]    = sample.grip[hand];
                        forwards[n] = sample.forward[hand];
                        ++n;
                    }
                }
            }
            if (!have_last) {
                return false;
            }
            out                  = Path{};
            out.hand             = use_swing ? swing.hand : 1;
            out.swing            = use_swing;
            out.peak             = swing.peak;
            out.since_trigger_us = use_swing ? t - swing.trigger_us : 0;
            out.points           = n;
            // A left hand strikes with the fist (the weapon is in the right hand), or (fix154) with its own weapon (dual wielding).
            const float reach = out.hand == 0 ? kStrike[LeftKind()].reach : kStrike[kind].reach;

            // Into the world as you stand now.
            std::array<glm::vec3, kPathPoints> points;
            int valid_points = 0;
            for (int i = 0; i < n; ++i) {
                glm::vec3 point{}, unused{};
                if (tracking::StageToWorld(grips[i] + forwards[i] * reach, forwards[i], point, unused)) {
                    points[valid_points++] = point;
                }
            }
            glm::vec3 tip{}, tip_forward{};
            if (!tracking::StageToWorld(last_grip + last_forward * reach, last_forward, tip, tip_forward, &out.scale)) {
                return false;
            }

            bool path_ok = false;
            if (valid_points >= 2) {
                const glm::vec3 start = points[0];
                const glm::vec3 end   = points[valid_points - 1];
                const float     chord = glm::length(end - start);
                if (chord >= kMinPath * out.scale) {
                    // A curved swing: move the straight sweep halfway toward the curve's farthest point, so
                    // the sphere covers the arc rather than cutting inside it.
                    const glm::vec3 axis = (end - start) / chord;
                    glm::vec3       bulge{ 0.0f };
                    float           widest = 0.0f;
                    for (int i = 1; i < valid_points - 1; ++i) {
                        const glm::vec3 d    = points[i] - start;
                        const glm::vec3 side = d - axis * glm::dot(d, axis);
                        const float     w    = glm::length(side);
                        if (w > widest) {
                            widest = w;
                            bulge  = side;
                        }
                    }
                    out.start    = start + bulge * 0.5f;
                    out.end      = end + bulge * 0.5f;
                    out.widest   = widest;
                    out.swept_us = last_t - first_t;
                    path_ok      = true;
                }
            }
            if (!path_ok) {
                // A jab at where the strike point is now: a right stick click without a swing, or a swing
                // too short to sweep.
                out.start = tip - tip_forward * (kJabBack * out.scale);
                out.end   = tip + tip_forward * (kJabForward * out.scale);
                out.jab   = true;
            }
            return std::isfinite(out.start.x) && std::isfinite(out.start.y) && std::isfinite(out.start.z) && std::isfinite(out.end.x) &&
                   std::isfinite(out.end.y) && std::isfinite(out.end.z) && glm::length(out.end - out.start) > 1e-3f;
        }

        // An instant hit's stretch: from the strike point at the last pose to the strike point now.
        bool StretchPath(int kind, const Stretch& stretch, Path& out)
        {
            const float reach = stretch.hand == 0 ? kStrike[LeftKind()].reach : kStrike[kind].reach;
            glm::vec3   a{}, b{}, forward_a{}, forward_b{};
            float       scale = 1.0f;
            if (!tracking::StageToWorld(stretch.grip0 + stretch.forward0 * reach, stretch.forward0, a, forward_a) ||
                !tracking::StageToWorld(stretch.grip1 + stretch.forward1 * reach, stretch.forward1, b, forward_b, &scale)) {
                return false;
            }
            out       = Path{};
            out.hand  = stretch.hand;
            out.scale = scale;
            out.swing = true;
            out.start = a;
            out.end   = glm::length(b - a) >= kMinStretch * scale ? b : a + forward_b * (kMinStretch * scale);
            return std::isfinite(out.start.x) && std::isfinite(out.start.y) && std::isfinite(out.start.z) && std::isfinite(out.end.x) &&
                   std::isfinite(out.end.y) && std::isfinite(out.end.z) && glm::length(out.end - out.start) > 1e-4f;
        }

        // Before the sweep: the attacker is in rsi, origin and direction are written, the length is in xmm3.
        void OnPreCast(safetyhook::Context& ctx)
        {
            t_ours      = false;
            t_radius    = 0.0f;
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr || ctx.rsi != reinterpret_cast<uintptr_t>(player)) {
                const int others = g_other_attackers.fetch_add(1) + 1;
                if (others <= 3) {
                    uintptr_t game_player = 0;
                    SafeRead(&game_player, g_player_global, sizeof(game_player));
                    spdlog::info("[Melee] someone else's melee hit (attacker {:x}, you {:x}, the game's own player check {:x}); left as it is",
                        ctx.rsi, reinterpret_cast<uintptr_t>(player), game_player);
                }
                // (fix172) Blocking: an NPC's hit at you whose path your blade is in now misses (its sweep made to reach nothing, as a thrown
                // weapon's pressed attack below); the attacker recoils (Physical, on the main thread).
                // (Only with the reach check found: shortening the sweep alone leaves someone at its very start struck; the review.)
                if (player != nullptr && g_adjust_reach_check.load()) {
                    const auto* origin    = reinterpret_cast<const float*>(ctx.rsp + kOriginSlot);
                    const auto* direction = reinterpret_cast<const float*>(ctx.rsp + kDirectionSlot);
                    if (Physical::BlockAttack(ctx.rsi, glm::vec3{ origin[0], origin[1], origin[2] }, glm::vec3{ direction[0], direction[1], direction[2] },
                            ctx.xmm3.f32[0], g_adjust_reach_check.load() ? ctx.xmm8.f32[0] : 0.0f)) {
                        ctx.xmm3.f32[0] = 1e-4f;
                        if (g_adjust_reach_check.load()) {
                            ctx.xmm8.f32[0] = 0.0f;
                        }
                    }
                }
                return;
            }
            const Stretch stretch_data = t_stretch; // an instant hit's stretch, if this call is one
            const bool    stretch      = stretch_data.set;
            t_stretch.set              = false;
            t_world                    = false;
            if (stretch && stretch_data.world) {
                // A thrown weapon's stretch of flight (game world): swept as it is, with its own sphere.
                glm::vec3   axis   = stretch_data.b - stretch_data.a;
                const float length = glm::length(axis);
                if (!(length > 1e-4f) || !std::isfinite(length) || !(stretch_data.radius > 0.0f)) {
                    return;
                }
                axis /= length;
                auto* origin    = reinterpret_cast<float*>(ctx.rsp + kOriginSlot);
                auto* direction = reinterpret_cast<float*>(ctx.rsp + kDirectionSlot);
                origin[0]       = stretch_data.a.x;
                origin[1]       = stretch_data.a.y;
                origin[2]       = stretch_data.a.z;
                direction[0]    = axis.x;
                direction[1]    = axis.y;
                direction[2]    = axis.z;
                ctx.xmm3.f32[0] = length;
                if (g_adjust_reach_check.load() && ctx.xmm8.f32[0] < length + stretch_data.radius + 0.25f) {
                    ctx.xmm8.f32[0] = length + stretch_data.radius + 0.25f;
                }
                t_ours          = true;
                t_radius        = stretch_data.radius;
                t_hand          = 1;
                t_start         = stretch_data.a;
                t_axis          = axis;
                t_length        = length;
                t_world         = true;
                t_stretch_taken = true;
                return;
            }
            const int  attack  = stretch ? 0 : g_attacks.fetch_add(1) + 1;
            static auto vr = VR::get();
            // Only while your hands are the character's hands: headset and controllers in use, first
            // person, not on the flat screen.
            if (!GameFlow::gStore.internalSettings.impactMelee || !vr->is_hmd_active() || !vr->is_using_controllers() || !GameFlow::isInFirstPerson() ||
                ModSettings::showFlatScreenDisplay()) {
                if (!stretch && ShouldLog(attack)) {
                    spdlog::info("[Melee] attack {}: the game's own sweep from your view is kept (impact melee off, or not playing with controllers in first person)",
                        attack);
                }
                return;
            }
            if (!stretch && MeleeThrow::HandWithoutWeapon(false)) {
                // Your melee weapon was just thrown (an attack pressed before it left your hand): this one strikes nothing.
                ctx.xmm3.f32[0] = 1e-4f;
                if (g_adjust_reach_check.load()) {
                    ctx.xmm8.f32[0] = 0.0f;
                }
                if (ShouldLog(attack)) {
                    spdlog::info("[Melee] attack {}: your weapon was thrown, so this attack strikes nothing", attack);
                }
                return;
            }
            int        type = -1;
            const int  kind = WeaponKind(ctx.rsp, type);
            if (!(stretch && stretch_data.hand == 0)) {
                g_kind = kind; // (fix166: a left stretch's hit has the left weapon: not the right hand's strike)
            }
            Path path{};
            if (stretch) {
                if (!StretchPath(kind, stretch_data, path)) {
                    return;
                }
            } else if (!BuildPath(kind, path)) {
                if (ShouldLog(attack)) {
                    spdlog::warn("[Melee] attack {}: no recent hand position, the game's own sweep from your view is kept", attack);
                }
                return;
            }
            auto*           origin    = reinterpret_cast<float*>(ctx.rsp + kOriginSlot);
            auto*           direction = reinterpret_cast<float*>(ctx.rsp + kDirectionSlot);
            const glm::vec3 game_origin{ origin[0], origin[1], origin[2] };
            const float     game_length = ctx.xmm3.f32[0];
            const float     game_check  = ctx.xmm8.f32[0];

            glm::vec3 axis   = path.end - path.start;
            float     length = glm::length(axis);
            axis /= length;
            const glm::vec3 start = path.start - axis * (kBehindStart * path.scale);
            length += kBehindStart * path.scale;
            const Strike& strike = kStrike[path.hand == 0 ? LeftKind() : kind];
            // Hand-sized, a little wider when the swing curved more than the straight sweep can follow.
            const float radius = std::clamp(path.widest * 0.5f, strike.radius * path.scale, std::max(strike.radius, kMaxRadius) * path.scale);

            origin[0]       = start.x;
            origin[1]       = start.y;
            origin[2]       = start.z;
            direction[0]    = axis.x;
            direction[1]    = axis.y;
            direction[2]    = axis.z;
            ctx.xmm3.f32[0] = length;
            // A person counts as hit when they are within xmm8 of the start; the path is usually shorter
            // than the game's reach, but a long swing must not lose its hit to that check.
            if (g_adjust_reach_check.load() && game_check < length + radius + 0.25f) {
                ctx.xmm8.f32[0] = length + radius + 0.25f;
            }
            t_ours   = true;
            t_radius = radius;
            t_hand   = path.hand;
            t_start  = start;
            t_axis   = axis;
            t_length = length;
            if (stretch) {
                t_stretch_taken = true;
                return; // instant hits are logged per swing
            }
            g_swept.fetch_add(1);

            if (ShouldLog(attack)) {
                float camera_distance = -1.0f;
                if (auto root = CreationEngineSingletonManager::GetSceneGraphRoot(); root && root->worldCamera) {
                    const auto& c   = root->worldCamera->world.translate;
                    camera_distance = glm::length(game_origin - glm::vec3{ c.x, c.y, c.z });
                }
                if (path.jab) {
                    spdlog::info("[Melee] attack {}: {} ({}, weapon type {}), {} hand: jab at the hand ({}), sweep {:.2f} m, sphere {:.2f} | game's own: "
                                 "from {:.2f} m off the camera, {:.2f} m long, hit range {:.2f}, sphere {:.2f}",
                        attack, strike.name, kStrike[kind].name, type, path.hand == 0 ? "left" : "right",
                        path.swing ? "swing too short" : "no unused swing in the last second", length, radius, camera_distance, game_length,
                        game_check, g_game_radius.load());
                } else {
                    spdlog::info("[Melee] attack {}: {} ({}, weapon type {}), {} hand: swing (peak {:.1f} m/s) swept {:.2f} m over {} ms ({} samples, "
                                 "curve {:.2f} m), the hit came {} ms after the swing pressed melee, sphere {:.2f} | game's own: from {:.2f} m off the "
                                 "camera, {:.2f} m long, hit range {:.2f}, sphere {:.2f}, its start {:.2f} m from the swing's",
                        attack, strike.name, kStrike[kind].name, type, path.hand == 0 ? "left" : "right", path.peak, length,
                        path.swept_us / 1000, path.points, path.widest, path.since_trigger_us / 1000, radius, camera_distance, game_length,
                        game_check, g_game_radius.load(), glm::length(game_origin - start));
                }
            }
        }

        // In the sweep, just before its sphere is made (radius in xmm2): hand-sized for the player's swing.
        // Only when the sweep was called from the melee hit (its return address) on the swing's thread.
        void OnSweepSphere(safetyhook::Context& ctx)
        {
            if (!t_ours || t_radius <= 0.0f) {
                return;
            }
            if (*reinterpret_cast<const uintptr_t*>(ctx.rbp + kSweepReturnAtRbp) != g_sweep_return) {
                return;
            }
            g_game_radius   = ctx.xmm2.f32[0];
            ctx.xmm2.f32[0] = t_radius;
            t_radius        = 0.0f; // this sweep only
        }

        // After the sweep, only when it hit something.
        void OnHit(safetyhook::Context& ctx)
        {
            if (!t_ours) {
                return;
            }
            t_ours      = false;
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr || ctx.rsi != reinterpret_cast<uintptr_t>(player)) {
                t_world = false;
                return;
            }
            if (t_world) {
                // A thrown weapon's sweep struck something: where (SweepWorld reads it).
                t_world     = false;
                t_world_hit = true;
                float hit[4]{};
                if (SafeRead(hit, ctx.rsp + kHitPointSlot, sizeof(hit)) && std::isfinite(hit[0]) && std::isfinite(hit[1]) && std::isfinite(hit[2])) {
                    t_world_point = glm::vec3{ hit[0], hit[1], hit[2] };
                } else {
                    t_world_point = t_start + t_axis * t_length;
                }
                return;
            }
            const int strikes = g_strikes.fetch_add(1) + 1;
            haptics::MeleePulse(t_hand == 0, 1.0f, 0.09f);
            if (ShouldLog(strikes)) {
                float hit[4]{};
                float along = -1.0f;
                if (SafeRead(hit, ctx.rsp + kHitPointSlot, sizeof(hit))) {
                    along = glm::dot(glm::vec3{ hit[0], hit[1], hit[2] } - t_start, t_axis);
                }
                spdlog::info("[Melee] strike {}: the {} hand's swing struck something {:.2f} m along its {:.2f} m path", strikes,
                    t_hand == 0 ? "left" : "right", along, t_length);
            }
        }

        bool Same(uintptr_t at, const uint8_t* bytes, size_t n)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            if (at < base || at + n > base + MemoryScan::mod_size) {
                return false;
            }
            return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }
    }

    void Install()
    {
        const auto site = body::ScanUnique("melee hit sweep (impact melee)", kCastSitePattern, kCastSiteRva, "Starfield 1.16.236 offset");
        if (!site) {
            g_off_reason = "off: the game's melee hit was not found (another game version?)";
            spdlog::error("[Melee] impact melee is off: the game's melee hit was not found");
            return;
        }
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        const auto fill = site - kOriginFillBack;
        if (!Same(fill, kOriginFill, sizeof(kOriginFill)) || !Same(site - kWeaponReadBack, kWeaponRead, sizeof(kWeaponRead))) {
            g_off_reason = "off: the game's melee hit is not the code this was made for";
            spdlog::error("[Melee] impact melee is off: the melee hit at {:x} is not laid out like Starfield 1.16.236's", site - base);
            return;
        }
        g_adjust_reach_check = Same(site + kReachCheckAhead, kReachCheck, sizeof(kReachCheck));
        if (Same(fill + kPlayerCompareInFill, kPlayerCompare, sizeof(kPlayerCompare))) {
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(fill + kPlayerCompareInFill + 3), sizeof(rel));
            g_player_global = fill + kPlayerCompareInFill + 7 + rel;
        }

        // The sweep the melee hit calls, and the place in it where the sphere is made.
        int32_t call_rel = 0;
        std::memcpy(&call_rel, reinterpret_cast<const void*>(site + kCallOffset + 1), sizeof(call_rel));
        g_sweep_return           = site + kCallOffset + 5;
        const uintptr_t sweep    = g_sweep_return + call_rel;
        const bool      sweep_ok = Same(sweep, kSweepPrologue, sizeof(kSweepPrologue)) && Same(sweep + kSweepRadiusLoad, kRadiusLoad, sizeof(kRadiusLoad)) &&
                              Same(sweep + kSweepSphere, kSphereArgs, sizeof(kSphereArgs));
        if (sweep_ok) {
            int32_t radius_rel = 0;
            std::memcpy(&radius_rel, reinterpret_cast<const void*>(sweep + kSweepRadiusLoad + 4), sizeof(radius_rel));
            g_game_radius_setting = sweep + kSweepSphere + radius_rel;
            g_sphere_hook         = safetyhook::create_mid(reinterpret_cast<void*>(sweep + kSweepSphere), &OnSweepSphere);
        }

        g_hit_hook     = safetyhook::create_mid(reinterpret_cast<void*>(site + kHitOffset), &OnHit);
        g_precast_hook = safetyhook::create_mid(reinterpret_cast<void*>(site + kPreCastOffset), &OnPreCast);
        if (!g_precast_hook) {
            g_off_reason = "off: could not hook the game's melee hit";
            spdlog::error("[Melee] impact melee is off: could not hook the melee hit's sweep");
            return;
        }
        // Instant hits: the melee hit's start, and the attack step that calls it (for the function that applies the hit).
        if (Same(site - kHitFunctionBack, kHitPrologue, sizeof(kHitPrologue))) {
            g_hit_function = site - kHitFunctionBack;
        }
        if (g_hit_function != 0) {
            if (const auto call = body::ScanUnique("attack step (impact melee test)", kAttackCallPattern, kAttackCallRva, "Starfield 1.16.236 offset")) {
                int32_t rel = 0;
                std::memcpy(&rel, reinterpret_cast<const void*>(call + 6), sizeof(rel));
                const uintptr_t target = call + 10 + rel;
                const uintptr_t step   = call - kAttackCallInStep;
                if (target == g_hit_function && Same(step, kAttackPrologue, sizeof(kAttackPrologue)) &&
                    Same(step + kApplyCallInStep, kApplyCall, sizeof(kApplyCall))) {
                    int32_t apply_rel = 0;
                    std::memcpy(&apply_rel, reinterpret_cast<const void*>(step + kApplyCallInStep + sizeof(kApplyCall)), sizeof(apply_rel));
                    g_apply_function    = step + kApplyCallInStep + sizeof(kApplyCall) + 4 + apply_rel;
                    g_attack_function   = step;
                    g_attack_entry_hook = safetyhook::create_mid(reinterpret_cast<void*>(step), &OnAttackEntry);
                }
            }
        }
        if (g_attack_function == 0 || !g_attack_entry_hook) {
            g_attack_function = 0;
            spdlog::warn("[Melee] the game's attack step was not found where it was in 1.16.236; instant hits are unavailable (swings press melee)");
        } else {
            spdlog::info("[Melee] instant hits: the game's attack step at {:x} (melee hit at {:x}, applying the hit at {:x})",
                g_attack_function - base, g_hit_function - base, g_apply_function - base);
        }
        // (fix166) The left hand's hits with its own weapon: the weapon the game's hit takes, as checked against 1.16.236.
        {
            static constexpr uint8_t kCallByte[] = { 0xE8 };
            const auto call_target = [&](uintptr_t at) -> uintptr_t {
                if (!Same(at, kCallByte, sizeof(kCallByte))) {
                    return 0;
                }
                int32_t rel = 0;
                std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof(rel));
                return at + 5 + rel;
            };
            // (fix190) (the hit and its applying compared with the functions found: known in this game version, or the hook stays off)
            const uintptr_t weapon_fn = At(kEquippedWeaponRva);
            const bool      code      = GameCode::Known({ kHitRva, kApplyRva }) && g_hit_function == At(kHitRva) && g_apply_function == At(kApplyRva) &&
                              SameCode(kEquippedWeaponRva, kEquippedWeaponStart, sizeof(kEquippedWeaponStart)) && call_target(At(kHitWeaponCallRva)) == weapon_fn &&
                              call_target(At(kApplyWeaponCallRva)) == weapon_fn;
            if (code) {
                // (Made disabled and turned on once stored here: the function runs on several threads all the time, and a call between the
                // hook going in and its being stored would find no original to call.)
                g_weapon_hook = safetyhook::create_inline(reinterpret_cast<void*>(weapon_fn), reinterpret_cast<void*>(&OnEquippedWeapon),
                    safetyhook::InlineHook::StartDisabled);
                g_left_weapon_ready = static_cast<bool>(g_weapon_hook) && g_weapon_hook.enable().has_value();
            }
            // (fix190) (the hit's fill compared with the call found there: known in this game version, or the test log's hook stays off)
            if (g_left_weapon_ready.load() && SameCode(kAppliedRva, kApplied, sizeof(kApplied)) && GameCode::Known({ kHitFillRva }) &&
                call_target(At(kAppliedFillCallRva)) == At(kHitFillRva) && SameCode(kAppliedHitAtRva, kAppliedHitAt, sizeof(kAppliedHitAt)) &&
                SameCode(kApplyFrameRva, kApplyFrame, sizeof(kApplyFrame))) {
                g_applied_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kAppliedRva)), &OnHitApplied);
            }
            if (g_left_weapon_ready.load()) {
                spdlog::info("[Melee] (fix166) the left hand's hits use its own weapon (the game's equipped weapon lookup hooked at rva {:x}){}", kEquippedWeaponRva,
                    g_applied_hook ? "; the first hits the mod applies are logged with their damage" : "");
            } else {
                spdlog::warn("[Melee] (fix166) the game's equipped weapon lookup is not as in Starfield 1.16.236 (or could not be hooked): the left hand's hits "
                             "keep the right weapon's damage");
            }
        }
        g_ready = true;
        spdlog::info("[Melee] impact melee hooks: sweep at {:x} {}, strike feedback {}, hand-sized sphere {} (sweep at {:x}, the game's sphere setting at {:x}), "
                     "hit range check {}, player check at {:x}",
            site - base + kPreCastOffset, static_cast<bool>(g_precast_hook), static_cast<bool>(g_hit_hook), static_cast<bool>(g_sphere_hook),
            sweep - base, g_game_radius_setting ? g_game_radius_setting - base : 0, g_adjust_reach_check.load() ? "found" : "not found (left alone)",
            g_player_global ? g_player_global - base : 0);
    }

    void Update()
    {
        static auto vr = VR::get();
        const auto& s  = GameFlow::gStore.internalSettings;
        const int64_t t = NowUs();
        if (!g_ready.load() || !vr->is_hmd_active() || !vr->is_using_controllers()) {
            g_track[0].have = g_track[1].have = false;
            g_instant[0].active = g_instant[1].active = false;
            return;
        }
        // (fix154) The left weapon shown or gone: the left hand's strike line turns at once (the fist's way to the blade's): its swing
        // followed afresh, not taken for a fast one (the review).
        static bool left_held_was = false;
        if (const bool held = LeftWeapon::Held(); held != left_held_was) {
            left_held_was       = held;
            g_track[0].have     = false;
            g_instant[0].active = false;
        }
        // (fix158) The right hand's one-handed melee weapon held by its own model or not: its strike line turns at once too.
        static bool right_blade_was = false;
        if (glm::vec3 unused{}; (body::RightBladeLocal(unused) && HandGrab::OneHandedMeleeOut()) != right_blade_was) {
            right_blade_was     = !right_blade_was;
            g_track[1].have     = false;
            g_instant[1].active = false;
        }

        // Facing (stage space, level): a hand moving back toward you is pulling back, not swinging.
        const glm::mat4 head   = vr->get_transform(0);
        glm::vec3       facing = -glm::vec3{ head[2] };
        facing.y               = 0.0f;
        const float facing_len = glm::length(facing);
        facing                 = facing_len > 1e-3f ? facing / facing_len : glm::vec3{ 0.0f };

        Sample    sample{};
        sample.t_us = t;
        bool      fresh = false;
        bool      fresh_hand[2]{ false, false };
        float     speed[2]{ 0.0f, 0.0f };
        glm::vec3 velocity_of[2]{ glm::vec3{ 0.0f }, glm::vec3{ 0.0f } };
        bool      outward[2]{ false, false };
        for (int hand = 0; hand < 2; ++hand) {
            const bool      left    = hand == 0;
            const auto      index   = static_cast<uint32_t>(left ? vr->get_left_controller_index() : vr->get_right_controller_index());
            const glm::mat4 grip    = tracking::GripPose(left);
            const glm::mat4 aim     = tracking::AimPose(left);
            const glm::vec3 grip_at{ grip[3] };
            glm::vec3       forward = -glm::vec3{ aim[2] };
            const float     flen    = glm::length(forward);
            auto&           track   = g_track[hand];
            if (!(flen > 1e-4f) || !std::isfinite(grip_at.x) || !std::isfinite(grip_at.y) || !std::isfinite(grip_at.z)) {
                track.have = false;
                continue;
            }
            forward /= flen;
            // (fix154) The left hand's own weapon: along its blade as drawn (laid across the palm, not along the pointing line; the review).
            // (Its way taken anew only with a new pose: the poses come once per stereo pair, and a new way alone would count as one.)
            // (fix158) The right hand's one-handed melee weapon too, held by its own model the same way (body::RightBladeLocal).
            static glm::vec3 blade_local[2]{ glm::vec3{ 0.0f }, glm::vec3{ 0.0f } };
            glm::vec3        blade_now{};
            const bool       blade_known = left ? GrenadeHand::LeftBladeLocal(blade_now) : body::RightBladeLocal(blade_now);
            if (blade_known && (!track.have || grip_at != track.grip)) {
                blade_local[hand] = blade_now;
            }
            const bool held_blade = left ? LeftWeapon::Held() : blade_known && HandGrab::OneHandedMeleeOut();
            if (const glm::vec3 local = blade_local[hand]; held_blade && glm::length(local) > 0.5f) {
                glm::mat3 axes{ 1.0f };
                for (int k = 0; k < 3; ++k) {
                    axes[k] = glm::normalize(glm::vec3{ aim[k] });
                }
                const glm::vec3 along = axes * local;
                if (const float len = glm::length(along); len > 0.5f && std::isfinite(len)) {
                    forward = along / len;
                }
            }
            // The strike point: the fist for the left hand (or its own weapon's, fix154), the weapon's striking part for the right.
            const float     reach = left ? kStrike[LeftKind()].reach : kStrike[g_kind.load()].reach;
            const glm::vec3 tip   = grip_at + forward * reach;

            // A new pose (the runtime updates them once per stereo pair, not every engine frame).
            if (!track.have || grip_at != track.grip || forward != track.forward) {
                if (track.have && t > track.t_us && t - track.t_us < 200'000) {
                    const glm::vec3 tip_then = track.grip + track.forward * reach;
                    track.velocity           = (tip - tip_then) / (static_cast<float>(t - track.t_us) * 1e-6f);
                } else {
                    track.velocity = glm::vec3{ 0.0f };
                }
                track.have       = true;
                track.grip       = grip_at;
                track.forward    = forward;
                track.t_us       = t;
                fresh            = true;
                fresh_hand[hand] = true;
            }

            const glm::vec3 linear{ vr->get_velocity(index) };
            const glm::vec3 angular{ vr->get_angular_velocity(index) };
            glm::vec3       velocity = linear + glm::cross(angular, tip - grip_at);
            if (glm::length(linear) < 1e-5f && glm::length(angular) < 1e-5f) {
                velocity = track.velocity; // no velocity from the runtime: from the last two poses
            }
            speed[hand] = glm::length(velocity);
            if (!std::isfinite(speed[hand])) {
                speed[hand] = 0.0f;
                continue;
            }
            velocity_of[hand]    = velocity;
            outward[hand]        = speed[hand] < 1e-4f || glm::dot(velocity / speed[hand], facing) > kBackwardDot;
            sample.valid[hand]   = true;
            sample.grip[hand]    = grip_at;
            sample.forward[hand] = forward;
        }

        const float threshold = std::clamp(s.meleeSwingSpeed, 0.5f, 10.0f);
        const float slow      = threshold * kSlowFraction;
        // (fix175: not in build mode, no menu by the count: a swing's press is the right stick's click there, precise placement.)
        // (fix189: nor sitting or lying, a chair, a bed or the pilot's seat without its HUD: the left weapon is hidden there, LeftWeapon.)
        const bool  playing   = s.impactMelee && GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown() &&
                             !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && !GameFlow::isAimingDownSights() && !ModSettings::showFlatScreenDisplay() &&
                             !LeftWeapon::PlayerSeated();
        const bool  allowed   = playing && GameFlow::isWeaponDrawn();
        const bool  instant   = s.meleeInstantHits && g_hit_function != 0 && g_apply_function != 0 && !g_instant_failed.load();
        // (fix166) Each hand's own: the right hand strikes with your weapon (or fists) drawn; the left also with its own weapon out on its
        // own (your weapon put away, LeftWeapon's drawn alone) when its instant hits can be its own weapon's (a pressed attack is the game's
        // own and needs your weapon out).
        uintptr_t   left_form = 0, left_instance = 0;
        const bool  left_own  = instant && g_left_weapon_ready.load() && LeftWeapon::Held() && LeftWeapon::AttackInstance(left_form, left_instance);
        const bool  allowed_hand[2]{ allowed || (playing && left_own), allowed };
        // A hand holding something, aiming the gravity gloves or just done with that (fists out): its swing is a throw or a pull.
        // (The weapon hand with its weapon thrown away: nothing to strike with.)
        const bool  busy[2]{ HandGrab::HandBusy(true) || MeleeThrow::HandWithoutWeapon(true), HandGrab::HandBusy(false) || MeleeThrow::HandWithoutWeapon(false) };
        int   started       = -1;
        float started_speed = 0.0f;
        {
            std::scoped_lock _{ g_mutex };
            if (fresh && (sample.valid[0] || sample.valid[1])) {
                g_samples[g_next] = sample;
                g_next            = (g_next + 1) % kSamples;
                g_count           = std::min(g_count + 1, kSamples);
                g_last_sample_us  = t;
            }

            for (int hand = 0; hand < 2; ++hand) {
                if (speed[hand] < slow) {
                    g_armed[hand]   = true;
                    g_slow_us[hand] = t;
                }
            }
            // The current swing ends when its hand slows down.
            if (g_current_swing >= 0) {
                auto& swing = g_swing_queue[g_current_swing];
                swing.peak  = std::max(swing.peak, speed[swing.hand]);
                if (speed[swing.hand] < slow || t - swing.trigger_us > kMaxSwingUs) {
                    swing.end_us    = t;
                    g_current_swing = -1;
                }
            }
            // Swings press the game's melee button (when instant hits are off).
            if (!instant && allowed && t - g_last_trigger_us >= kCooldownUs) {
                for (int hand = 0; hand < 2; ++hand) {
                    if (g_armed[hand] && outward[hand] && speed[hand] >= threshold && !busy[hand] && (started < 0 || speed[hand] > speed[started])) {
                        started = hand;
                    }
                }
                if (started >= 0) {
                    if (g_current_swing >= 0) {
                        g_swing_queue[g_current_swing].end_us = t; // a new swing ends the one before
                    }
                    g_armed[started]  = false;
                    g_last_trigger_us = t;
                    g_current_swing   = g_swing_next;
                    g_swing_next      = (g_swing_next + 1) % kSwings;
                    // The path starts where the hand was last at rest (one pose earlier, to be sure it is included).
                    g_swing_queue[g_current_swing] = Swing{ started, std::max(g_slow_us[started] - 60'000, t - kLeadUs), t, 0, speed[started], false };
                    g_press_until_us = t + kPressUs;
                    started_speed    = speed[started];
                }
            }
        }
        if (started >= 0) {
            // A light tick: the swing started an attack.
            haptics::MeleePulse(started == 0, 0.25f, 0.03f);
            const int swings = g_swings.fetch_add(1) + 1;
            if (ShouldLog(swings)) {
                spdlog::info("[Melee] swing {}: {} hand at {:.1f} m/s (needs {:.1f}), melee pressed", swings, started == 0 ? "left" : "right",
                    started_speed, threshold);
            }
        }

        // Instant hits: each hand's swing sweeps every new stretch of its path until it strikes something.
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        for (int hand = 0; hand < 2; ++hand) {
            auto&       swing        = g_instant[hand];
            const float instant_slow = threshold * kInstantSlowFraction;
            if (speed[hand] < instant_slow) {
                g_instant_armed[hand]   = true;
                g_instant_rest_us[hand] = t;
            }
            if (swing.active) {
                swing.peak          = std::max(swing.peak, speed[hand]);
                const bool reversed = speed[hand] >= instant_slow && glm::dot(velocity_of[hand], swing.direction) < kReverseDot * speed[hand];
                if (speed[hand] < instant_slow || reversed || t - swing.trigger_us > kInstantMaxUs || !allowed_hand[hand] || !instant) {
                    swing.active = false;
                    if (reversed) {
                        g_instant_armed[hand] = true; // a slash back the other way is the next swing
                    }
                    if (swing.result == kMissed) {
                        g_instant_misses.fetch_add(1);
                    }
                    const int n = swing.number;
                    if (ShouldLog(n)) {
                        const char* what = swing.result == kPerson ? "hit a person" : swing.result == kSurface ? "struck a surface or object" : "missed";
                        spdlog::info("[Melee] instant swing {}: {} hand, peak {:.1f} m/s: {}{} ({} sweeps, swing {} ms, ended by {})", n,
                            hand == 0 ? "left" : "right", swing.peak, what,
                            swing.result != kMissed ? fmt::format(" {} ms after it got fast", (swing.result_us - swing.trigger_us) / 1000) : std::string{},
                            swing.sweeps, (t - swing.trigger_us) / 1000,
                            reversed ? "turning back" : speed[hand] < instant_slow ? "slowing down" : !allowed_hand[hand] || !instant ? "leaving melee" : "time");
                    }
                }
            }
            if (!swing.active && instant && allowed_hand[hand] && player != nullptr && sample.valid[hand] && g_instant_armed[hand] && outward[hand] && !busy[hand] &&
                speed[hand] >= threshold && t - g_instant_last_trigger_us[hand] >= kInstantCooldownUs) {
                // A new swing. Its first stretch runs from where the hand was last at rest.
                g_instant_armed[hand]           = false;
                g_instant_last_trigger_us[hand] = t;
                swing                           = InstantSwing{};
                swing.active                    = true;
                swing.trigger_us                = t;
                swing.direction                 = velocity_of[hand] / speed[hand];
                swing.peak                      = speed[hand];
                swing.last_grip                 = sample.grip[hand];
                swing.last_forward              = sample.forward[hand];
                {
                    const int64_t rest = std::max(g_instant_rest_us[hand], t - kLeadUs);
                    std::scoped_lock _{ g_mutex };
                    for (int i = g_count - 1; i >= 0; --i) {
                        const Sample& old = g_samples[(g_next - g_count + i + kSamples * 2) % kSamples];
                        if (old.valid[hand] && old.t_us <= rest) {
                            swing.last_grip    = old.grip[hand];
                            swing.last_forward = old.forward[hand];
                            break;
                        }
                    }
                }
                swing.number = g_instant_swings.fetch_add(1) + 1;
                haptics::MeleePulse(hand == 0, 0.15f, 0.02f);
            }
            if (swing.active && !swing.done && player != nullptr && sample.valid[hand] && (fresh_hand[hand] || swing.sweeps == 0)) {
                // Sweep the stretch from the last pose to this one with the game's melee hit.
                const uint32_t number          = g_attack_number.load();
                const int      strikes_before  = g_strikes.load();
                t_stretch                      = Stretch{ true, hand, swing.last_grip, swing.last_forward, sample.grip[hand], sample.forward[hand] };
                t_stretch_taken                = false;
                uintptr_t target               = 0;
                // (fix166) The left hand's hit with its own weapon (held, its item known), for the sweep and for applying it.
                const bool own_left            = hand == 0 && LeftWeapon::Held();
                bool       ok                  = false;
                {
                    const LeftWeaponScope scope(own_left, number);
                    ok = CallHit(g_hit_function, player, number, target);
                }
                t_stretch.set                  = false;
                if (!t_stretch_taken) {
                    target = 0; // the sweep did not go along the hand: nothing it found counts
                }
                swing.last_grip                = sample.grip[hand];
                swing.last_forward             = sample.forward[hand];
                ++swing.sweeps;
                g_instant_sweeps.fetch_add(1);
                int outcome = kMissed;
                if (ok && target != 0 && target != reinterpret_cast<uintptr_t>(player)) {
                    // Someone was struck: the hit is applied now, the way the game's attack step does. (fix172) Not on a follower (the
                    // setting); a hard punch also knocks them down (Physical).
                    if (!Physical::SpareTarget(target)) {
                        {
                            const LeftWeaponScope scope(own_left, number);
                            const ApplyingScope   applying(hand);
                            ok = CallApply(g_apply_function, player, target, number);
                        }
                        if (ok) {
                            Physical::OnHandHit(hand, target, swing.peak, swing.direction);
                        }
                    }
                    outcome = kPerson;
                } else if (ok && t_stretch_taken && g_strikes.load() > strikes_before) {
                    outcome = kSurface;
                }
                if (!ok) {
                    g_instant_failed = true;
                    swing.done       = true;
                    spdlog::error("[Melee] the game's melee code failed with an exception during an instant hit; swings press melee again from now on");
                } else if (outcome != kMissed) {
                    swing.done      = true;
                    swing.result    = outcome;
                    swing.result_us = t;
                    (outcome == kPerson ? g_instant_people : g_instant_surfaces).fetch_add(1);
                }
            }
        }
    }

    void ForgetSwings(bool left)
    {
        const int hand = left ? 0 : 1;
        g_instant[hand].done = true; // (an instant swing going on: no more sweeps)
        std::scoped_lock _{ g_mutex };
        for (auto& swing : g_swing_queue) {
            if (swing.hand == hand) {
                swing.used = true; // (a pressed attack still to come does not sweep along this swing)
            }
        }
    }

    bool CanSweepWorld()
    {
        return g_ready.load() && g_hit_function != 0 && g_apply_function != 0 && !g_instant_failed.load();
    }

    int SweepWorld(bool left, const glm::vec3& from, const glm::vec3& to, float radius, uintptr_t& target, glm::vec3& point)
    {
        target    = 0;
        point     = to;
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        if (!CanSweepWorld() || player == nullptr) {
            return -1;
        }
        t_stretch        = Stretch{};
        t_stretch.set    = true;
        t_stretch.world  = true;
        t_stretch.a      = from;
        t_stretch.b      = to;
        t_stretch.radius = radius;
        t_stretch_taken  = false;
        t_world_hit      = false;
        uintptr_t      found  = 0;
        const uint32_t number = g_attack_number.load();
        bool           ok     = false;
        {
            const LeftWeaponScope scope(left, number); // (fix166: the left weapon's flight, with its own weapon)
            ok = CallHit(g_hit_function, player, number, found);
        }
        t_stretch.set    = false;
        t_world          = false;
        t_ours           = false;
        if (!ok) {
            g_instant_failed = true;
            spdlog::error("[Melee] the game's melee code failed with an exception during a thrown weapon's sweep; instant hits and throws are off from now on");
            return -1;
        }
        if (!t_stretch_taken) {
            return 0; // the sweep did not go along the stretch: nothing it found counts
        }
        if (t_world_hit) {
            point = t_world_point;
        }
        if (found != 0 && found != reinterpret_cast<uintptr_t>(player)) {
            target = found;
            return 2;
        }
        return t_world_hit ? 1 : 0;
    }

    bool ApplyHitTo(bool left, uintptr_t target)
    {
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        if (!CanSweepWorld() || player == nullptr || target == 0 || target == reinterpret_cast<uintptr_t>(player)) {
            return false;
        }
        if (Physical::SpareTarget(target)) {
            return false; // (fix172: a thrown weapon does not hurt a follower either; the setting)
        }
        bool applied = false;
        {
            const uint32_t        number = g_attack_number.load();
            const LeftWeaponScope scope(left, number); // (fix166: the left weapon's throw: its own damage)
            const ApplyingScope   applying(left ? 0 : 1);
            applied = CallApply(g_apply_function, player, target, number);
        }
        if (!applied) {
            g_instant_failed = true;
            spdlog::error("[Melee] the game's melee code failed with an exception applying a thrown weapon's hit; instant hits and throws are off from now on");
            return false;
        }
        return true;
    }

    bool WantsPress()
    {
        return g_ready.load(std::memory_order_relaxed) && NowUs() < g_press_until_us.load(std::memory_order_relaxed);
    }

    std::string Status()
    {
        if (!g_ready.load()) {
            return std::string{ "Impact melee is " } + g_off_reason + ".";
        }
        if (GameFlow::gStore.internalSettings.meleeInstantHits) {
            if (g_instant_failed.load()) {
                return "Instant hits stopped after the game's code failed; swings press melee instead (see the log).";
            }
            if (g_hit_function == 0 || g_apply_function == 0) {
                return "Instant hits are unavailable on this game version; swings press melee instead.";
            }
            return fmt::format("Ready (instant hits). This session: {} swings: {} hit a person, {} struck a surface, {} missed.", g_instant_swings.load(),
                g_instant_people.load(), g_instant_surfaces.load(), g_instant_misses.load());
        }
        return fmt::format("Ready (swings press melee). This session: {} swings started an attack; {} of your {} attacks swept along your hand, {} struck "
                           "something.",
            g_swings.load(), g_swept.load(), g_attacks.load(), g_strikes.load());
    }
}
