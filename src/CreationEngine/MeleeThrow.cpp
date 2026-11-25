#include "MeleeThrow.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>

#include "CreationEngineSingletonManager.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "GrenadeHand.h"
#include "ImpactMelee.h"
#include "LeftWeapon.h"
#include "body/BodyIK.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace MeleeThrow
{
    namespace
    {
        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        // (fix190) A game place by its 1.16.236 rva, for this build's game version (GameCode.h); 0 when not known (SafeRead then fails).
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool Finite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // ---- The gesture ----
        constexpr float   kMinThrowSpeed  = 2.5f;      // m/s of the hand (stage space), or the swing speed setting if higher
        constexpr int64_t kMinGripUs      = 120'000;   // the grip squeezed at least this long before the let-go
        constexpr int64_t kPeakWindowUs   = 160'000;   // the swing's speed: its fastest in this long before the let-go...
        constexpr int64_t kSpanUs         = 45'000;    // ...measured over spans at least this long (poses change once per stereo pair)
        constexpr int64_t kSpanMaxUs      = 130'000;
        constexpr float   kBackwardDot    = -0.5f;     // moving back toward you is not a throw
        constexpr int64_t kHeldFreshUs    = 200'000;   // the body placed the weapon in your hand this recently
        // ---- The flight ----
        // (fix143) As Jedi Academy's saber throw: it flies straight out at one speed, spinning flat, with no falling; at
        // the person the throw picks (homing), else where it was thrown; your throwing hand steers it while it flies out (it turns
        // toward the point your hand points at, at its distance); it cuts through every person it passes, out and back, without
        // stopping; at its range, or striking a wall, it comes back to your hand by itself. Before: thrown at the swing's speed, falling
        // slowly, bouncing off the first person and dropping, staying where it struck a wall until the grip called it back.
        constexpr float   kAimAssistDeg   = 35.0f;     // within this of where you look...
        constexpr float   kAimAssist      = 0.25f;     // ...a quarter of the way toward it (no one picked: straight from there)
        constexpr float   kJediSpeed      = 20.0f;     // m/s, out (constant)
        constexpr float   kJediRange      = 20.0f;     // m from where it left your hand: then it comes back...
        constexpr int64_t kJediOutUs      = 2'000'000; // ...or after this long out
        constexpr float   kJediSpin       = 18.0f;     // rad/s about the vertical (about 2.9 turns a second), the blade laid flat
        // (fix144) Guided while the right trigger is held after the throw (more control while the right trigger is held
        // after the throw, to aim it more): it turns this fast toward where your hand points, wherever that is, and stays out
        // this far or long; let go of the trigger and it comes back. Without the trigger it is not steered (its course, or its homing).
        constexpr float   kGuideTurn      = 9.0f;      // rad/s (about 515 degrees a second)
        constexpr int64_t kGuideAfterUs   = 200'000;   // (not the throw's follow-through)
        constexpr float   kGuideRange     = 30.0f;     // m
        constexpr int64_t kGuideOutUs     = 6'000'000;
        constexpr float   kSpinMin        = 8.0f;      // rad/s: below this the weapon tumbles end over end instead (coming back)
        constexpr float   kTumble         = 12.0f;
        constexpr int64_t kAutoReturnUs   = 10'000'000; // comes back by itself this long after the throw
        constexpr float   kReturnTime     = 0.30f;     // s: comes back at the speed that takes this long...
        constexpr float   kReturnMin      = 15.0f;     // ...within these (m/s)
        constexpr float   kReturnMax      = 45.0f;
        constexpr int64_t kReturnMaxUs    = 2'500'000; // in your hand after this at the latest
        constexpr int64_t kNoStrikeAfterUs = 300'000;  // the hand does not strike this long after the catch
        constexpr int     kMaxStruck      = 8;
        // ---- Aiming at people (fix141): while the grip is squeezed with the weapon in hand, the person nearest the hand's pointing
        // line is locked on (the gloves' lock buzz), and a throw then flies at them, homing (Weapon Throw VR's auto-aim, the gloves' pick
        // nearest the line). Friend or foe alike; living people only, never objects. ----
        // The game's ProcessLists (Starfield 1.16.236: stored by its constructor 0x141A8B790; its type table 0x144CC41B0), the actors near
        // the player at +0x60 (count) and +0x68 (their handles), as the game's own loops read them.
        constexpr uintptr_t kProcessListsRva   = 0x5FE1908;
        constexpr uintptr_t kProcessListsTable = 0x4CC41B0;
        constexpr uintptr_t kHighCount         = 0x60;
        constexpr uintptr_t kHighHandles       = 0x68;
        constexpr uint32_t  kMaxActors         = 512;
        // A reference: its form type (+0x2E: 0x4B an actor), its flags (+0x20: deleted 1<<5, disabled 1<<11); an actor's life state
        // ((+0xF8 >> 17) & 0xF: 1, 2, 5 and 7 dead or dying, as Actor::IsDead 0x1418E6D50 reads it); its 3D's world bound (centre, radius).
        constexpr uintptr_t kRefFormType  = 0x2E;
        constexpr uint8_t   kActorType    = 0x4B;
        constexpr uintptr_t kRefFlags     = 0x20;
        constexpr uintptr_t kActorLife    = 0xF8;
        constexpr uintptr_t kNodeBound    = 0x100;
        constexpr float     kAimAcquireDeg = 15.0f;   // a person within this of the pointing line is locked on...
        constexpr float     kAimNearLine   = 0.3f;    // ...or this near it (m), close by
        constexpr float     kAimKeepDeg    = 25.0f;   // a locked one stays until further off the line than this...
        constexpr int64_t   kAimOffUs      = 700'000; // ...for this long (not counted while the hand swings: a wind-up points it away)
        constexpr float     kAimRange      = 25.0f;   // m
        constexpr float     kAimPerMetre   = 0.3f;    // a metre further counts as this many degrees further off the line (the gloves')
        constexpr float     kAimSwitchDeg  = 4.0f;    // another person takes the lock only when this much nearer the line
        constexpr float     kAimHeadDeg    = 50.0f;   // and within this of where you look
        constexpr float     kAimFreeze     = 1.5f;    // m/s: the hand moving faster (a wind-up, the throw) keeps the lock as it is...
        constexpr int64_t   kAimSettleUs   = 400'000; // ...and for this long after
        constexpr int64_t   kAimScanUs     = 40'000;  // the people near you looked through this often (25 a second: a few hundred reads each)
        constexpr float     kThrowAssistDeg = 20.0f;  // (fix142) no one locked on: the person within this of the throw's direction
        // (No line-of-sight test before the throw: the game's melee sweep, the only one at hand, strikes what it meets (pushes, sparks,
        // damages it), fix141's review. A wall in the way still stops the thrown weapon, by the flight's own sweep.)
        constexpr float     kHomingTurn    = 6.2831853f; // rad/s the flight turns toward them at most
        constexpr int64_t   kHomingMaxUs   = 2'000'000;  // homing at most this long
        constexpr int     kMaxLogLines    = 200;

        enum class Phase { InHand, Flying, Resting, Returning };
        const char* PhaseName(Phase p)
        {
            switch (p) {
            case Phase::Flying: return "flying";
            case Phase::Resting: return "resting where it struck";
            case Phase::Returning: return "coming back";
            default: return "in your hand";
            }
        }

        // What the body last placed in your hand, and recent places (game world) for the throw's speed and spin.
        struct HeldSample
        {
            int64_t   us{ 0 };
            glm::vec3 t{ 0.0f }; // the weapon bone (its grip)
            glm::mat3 r{ 1.0f };
            float     s{ 1.0f };
        };
        constexpr int kHeldSamples = 48;
        struct Held
        {
            bool      have{ false };
            int64_t   us{ 0 };
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
            glm::vec3 centre_local{ 0.0f }; // the model's middle in the weapon's space, as last drawn...
            float     radius{ 0.35f };      // ...and its size
            bool      bound_ok{ false };    // (both read sensibly)
            std::array<HeldSample, kHeldSamples> samples{};
            int       next{ 0 };
            int       count{ 0 };
        };

        struct Flight
        {
            Phase     phase{ Phase::InHand };
            uint32_t  weapon_id{ 0 };
            glm::vec3 centre{ 0.0f };
            glm::vec3 velocity{ 0.0f };
            glm::vec3 spin{ 0.0f };       // rad/s, axis * rate (game world)
            glm::mat3 r{ 1.0f };
            float     s{ 1.0f };
            glm::vec3 centre_local{ 0.0f };
            float     radius{ 0.3f };     // the sweep's sphere
            int64_t   thrown_us{ 0 };
            int64_t   phase_us{ 0 };
            int64_t   loading{ 0 };       // the last loading screen at the throw
            uintptr_t struck_out[kMaxStruck]{};
            int       n_out{ 0 };
            uintptr_t struck_back[kMaxStruck]{};
            int       n_back{ 0 };
            int       number{ 0 };
            glm::vec3 start{ 0.0f };
            uint32_t  homing{ 0 };        // (fix141) the person it flies at (their handle; 0: none), until homing_until
            int64_t   homing_until{ 0 };
            bool      steered{ false };   // (fix143) your hand has turned it (logged once a throw)
            float     steer_total{ 0.0f }; // ...by this much in all (radians)
        };

        std::mutex           g_mutex; // S.held, S.pose
        struct SharedPose
        {
            bool      away{ false };
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
        };
        std::atomic<bool>    g_ready{ false };
        // (fix171) Each hand's grip taken by the favorites wheel (main thread: HoldOffGrip, UpdateSide), until that grip is let go.
        bool                 g_hold_off[2]{ false, false };

        struct HandSample
        {
            int64_t   us{ 0 };
            glm::vec3 at{ 0.0f };
        };
        constexpr int kHandSamples = 48;
        // The weapon's middle and size, latched once per weapon from a sensible reading half a second after it is out (so every
        // place in the hand and the flight use the same middle).
        struct Model
        {
            uint32_t  id{ 0 };
            bool      ok{ false };
            bool      told{ false };
            int64_t   out_since{ 0 };
            glm::vec3 centre_local{ 0.0f };
            float     radius{ 0.35f };
        };
        constexpr int64_t kModelSettleUs = 500'000;
        constexpr int64_t kModelGiveUpUs = 3'000'000;

        // ---- Aiming at people (fix141): what is locked on, and what the aim saw ----
        struct Target
        {
            uint32_t  handle{ 0 };      // the person locked on (0: none)
            uint32_t  id{ 0 };          // (for the log)
            glm::vec3 aim{ 0.0f };      // their middle (game world), as last looked at
            int64_t   off_since{ 0 };   // further off the line than kAimKeepDeg since then (0: not)
            bool      swinging{ false }; // (fix142) locked while the hand swung (a wind-up passes over people)
        };
        struct AimSeen
        {
            int   scans{ 0 };
            int   listed{ -2 };
            int   people{ 0 };
            float nearest_deg{ 999.0f };
            float nearest_dist{ 0.0f };
            bool  nearest_in_view{ false };
            int   locks{ 0 };
            bool  unreadable{ false };
        };

        // (fix156) Each hand's throw: the right hand's weapon (the game's, the body's weapon bone) and the left hand's (LeftWeapon's,
        // GrenadeHand's copy). The same throw, aim, flight and catch for both; each hand's own grip, trigger, pose and controller.
        struct Side
        {
            bool        left{ false };
            const char* tag{ "" };              // (in the log: "[Throw left]")
            Held        held{};                 // (g_mutex)
            SharedPose  pose{};                 // (g_mutex)
            std::atomic<bool>    away{ false };
            std::atomic<int64_t> caught_us{ 0 };
            std::atomic<bool>    available{ false }; // a melee weapon is out in this hand and can be thrown (or is away)
            // Main thread (Update).
            Flight  flight{};
            int64_t last_us{ 0 };
            bool    grip_was{ false };
            int64_t grip_since_us{ 0 };
            bool    trigger_was{ false };
            bool    guided{ false };      // (fix144: the trigger held while it flew: let go, it comes back)
            bool    guide_armed{ false }; // (fix144's review: the trigger seen let go since the throw: a pull after it guides, not one held through it)
            std::array<HandSample, kHandSamples> hand{};
            int     hand_next{ 0 }, hand_count{ 0 };
            Model   model{};
            Target  target{};
            int64_t scan_us{ 0 };         // the people near you last looked through
            int64_t swung_us{ 0 };        // the hand last moved faster than kAimFreeze
            AimSeen aim_seen{};
        };
        Side g_sides[2]{ { true, " left" }, { false, "" } }; // [0] the left hand, [1] the right

        std::atomic<int> g_throws{ 0 }, g_people{ 0 }, g_surfaces{ 0 }, g_returns{ 0 };
        int g_log_lines{ 0 };

        bool LogOk()
        {
            return ++g_log_lines <= kMaxLogLines;
        }

        // The bone's transform for the model's middle at `centre`.
        void Publish(Side& S, bool away)
        {
            std::scoped_lock _{ g_mutex };
            S.pose.away = away;
            if (away) {
                S.pose.r = S.flight.r;
                S.pose.s = S.flight.s;
                S.pose.t = S.flight.centre - S.flight.r * (S.flight.centre_local * S.flight.s);
            }
            S.away = away;
        }

        // The hand's fastest speed (stage space) over spans of at least kSpanUs within kPeakWindowUs before `now`, among the spans
        // moving away from you (not back toward `facing`'s opposite: fix141, the wind-up or the arm coming back after the throw was
        // often the fastest, and the throw was taken as one back toward you; any direction away from you counts, sideways as a frisbee
        // too). `facing`: the head's forward, level (zero: every direction counts).
        bool HandPeak(Side& S, int64_t now, const glm::vec3& facing, glm::vec3& velocity)
        {
            float best = -1.0f;
            for (int i = 0; i < S.hand_count; ++i) {
                const HandSample& a = S.hand[(S.hand_next - 1 - i + kHandSamples * 2) % kHandSamples];
                if (now - a.us > kPeakWindowUs) {
                    break;
                }
                for (int j = i + 1; j < S.hand_count; ++j) {
                    const HandSample& b  = S.hand[(S.hand_next - 1 - j + kHandSamples * 2) % kHandSamples];
                    const int64_t     dt = a.us - b.us;
                    if (dt < kSpanUs) {
                        continue;
                    }
                    if (dt > kSpanMaxUs) {
                        break;
                    }
                    const glm::vec3 v     = (a.at - b.at) / (static_cast<float>(dt) * 1e-6f);
                    const float     speed = glm::length(v);
                    const bool      away  = glm::length(facing) < 0.2f || speed < 1e-3f || glm::dot(v / speed, glm::normalize(facing)) > kBackwardDot;
                    if (Finite(v) && away && speed > best) {
                        best     = speed;
                        velocity = v;
                    }
                    break; // (the nearest span that is long enough)
                }
            }
            return best >= 0.0f;
        }

        glm::vec3 Centre(const HeldSample& sample, const glm::vec3& centre_local)
        {
            return sample.t + sample.r * (centre_local * sample.s);
        }

        // The weapon's fastest move in your hand (game world: with your own movement) before the let-go, and its spin then; among the moves
        // away from you (facing: where you look, level; fix141, as HandPeak).
        bool HeldPeak(const Held& held, const glm::vec3& centre_local, int64_t now, const glm::vec3& facing, glm::vec3& velocity, glm::vec3& spin)
        {
            float best = -1.0f;
            for (int i = 0; i < held.count; ++i) {
                const HeldSample& a = held.samples[(held.next - 1 - i + kHeldSamples * 2) % kHeldSamples];
                if (now - a.us > kPeakWindowUs) {
                    break;
                }
                for (int j = i + 1; j < held.count; ++j) {
                    const HeldSample& b  = held.samples[(held.next - 1 - j + kHeldSamples * 2) % kHeldSamples];
                    const int64_t     dt = a.us - b.us;
                    if (dt < kSpanUs) {
                        continue;
                    }
                    if (dt > kSpanMaxUs) {
                        break;
                    }
                    const float     seconds = static_cast<float>(dt) * 1e-6f;
                    const glm::vec3 v       = (Centre(a, centre_local) - Centre(b, centre_local)) / seconds;
                    const float     speed   = glm::length(v);
                    const bool      away    = glm::length(facing) < 0.2f || speed < 1e-3f || glm::dot(v / speed, glm::normalize(facing)) > kBackwardDot;
                    if (Finite(v) && away && speed > best) {
                        best     = glm::length(v);
                        velocity = v;
                        const glm::quat q = glm::normalize(glm::quat_cast(a.r * glm::transpose(b.r)));
                        float           angle = glm::angle(q);
                        if (angle > glm::pi<float>()) {
                            angle -= glm::two_pi<float>();
                        }
                        spin = std::abs(angle) > 1e-4f ? glm::axis(q) * (angle / seconds) : glm::vec3{ 0.0f };
                        if (!Finite(spin)) {
                            spin = glm::vec3{ 0.0f };
                        }
                    }
                    break;
                }
            }
            return best >= 0.0f;
        }

        // Where you are (game world): your head.
        bool HeadWorld(glm::vec3& at, glm::vec3& look)
        {
            const glm::mat4 head = tracking::HeadPose();
            return tracking::StageToWorld(glm::vec3{ head[3] }, -glm::vec3{ head[2] }, at, look) && Finite(at) && Finite(look);
        }

        // Your weapon hand's grip point (game world).
        bool HandWorld(const Side& S, glm::vec3& at)
        {
            const glm::mat4 grip = tracking::GripPose(S.left);
            glm::vec3       unused{};
            return tracking::StageToWorld(glm::vec3{ grip[3] }, -glm::vec3{ grip[2] }, at, unused) && Finite(at);
        }

        uint32_t FormId(uintptr_t form)
        {
            uint32_t id = 0;
            SafeRead(&id, form + 0x28, sizeof(id));
            return id;
        }

        void BackInHand(Side& S, const char* why)
        {
            const bool was = S.flight.phase != Phase::InHand;
            if (was && LogOk()) {
                spdlog::info("[Throw{}] throw {}: your weapon is back in your hand ({}; {:.1f} s after the throw; it struck {} people)", S.tag, S.flight.number, why,
                    (NowUs() - S.flight.thrown_us) * 1e-6, S.flight.n_out + S.flight.n_back);
            }
            S.flight.phase = Phase::InHand;
            S.caught_us    = NowUs();
            S.guided       = false;
            Publish(S, false);
        }

        void StartReturn(Side& S, int64_t now, const char* why)
        {
            if (S.flight.phase == Phase::InHand || S.flight.phase == Phase::Returning) {
                return;
            }
            S.flight.phase    = Phase::Returning;
            S.flight.phase_us = now;
            g_returns.fetch_add(1);
            if (glm::length(S.flight.spin) < kSpinMin) {
                glm::vec3 to{};
                if (HandWorld(S, to)) {
                    glm::vec3 axis = glm::cross(glm::vec3{ 0.0f, 0.0f, 1.0f }, to - S.flight.centre);
                    if (glm::length(axis) > 1e-3f) {
                        S.flight.spin = glm::normalize(axis) * kTumble;
                    }
                }
            }
            haptics::MeleePulse(S.left, 0.3f, 0.05f);
            if (LogOk()) {
                spdlog::info("[Throw{}] throw {}: {}: coming back{}", S.tag, S.flight.number, why,
                    S.flight.steered ? std::format(" (your hand turned it {:.0f} degrees in all)", glm::degrees(S.flight.steer_total)) : std::string{});
            }
        }

        bool AlreadyStruck(const uintptr_t* list, int n, uintptr_t target)
        {
            for (int i = 0; i < n; ++i) {
                if (list[i] == target) {
                    return true;
                }
            }
            return false;
        }

        // A person struck: your melee hit is applied to them.
        void Strike(Side& S, uintptr_t target, const glm::vec3& point, bool returning)
        {
            uintptr_t* list = returning ? S.flight.struck_back : S.flight.struck_out;
            int&       n    = returning ? S.flight.n_back : S.flight.n_out;
            if (n < kMaxStruck) {
                list[n++] = target;
            }
            const bool applied = ImpactMelee::ApplyHitTo(S.left, target); // (fix166: the left weapon's throw hits with its own damage)
            g_people.fetch_add(1);
            haptics::MeleePulse(S.left, 0.45f, 0.07f);
            glm::vec3 me{}, unused{};
            const float away = HeadWorld(me, unused) ? glm::length(point - me) : -1.0f;
            if (LogOk()) {
                spdlog::info("[Throw{}] throw {}: {} struck a person ({:08X}) {:.1f} m from you: {}", S.tag, S.flight.number, returning ? "on its way back it" : "it",
                    FormId(target), away, applied ? "your melee hit applied (the game's own function, your weapon's damage)" : "the hit could not be applied");
            }
        }

        // ---- Aiming at people (fix141) ----
        std::atomic<bool> g_aim_on{ true }; // (the setting: GameSettingsComponent)
        uint32_t g_locks{ 0 };

        // A living person's middle (game world: their 3D's bound, a little above its centre) and size; false: not one (an object, the
        // player, dead or dying, disabled, deleted, or without 3D).
        bool PersonAim(uintptr_t ref, uintptr_t root, uintptr_t player, glm::vec3& aim, float& radius)
        {
            uint8_t  type  = 0;
            uint32_t flags = 0, life = 0;
            float    bound[4]{};
            if (ref == 0 || ref == player || root == 0 || !SafeRead(&type, ref + kRefFormType, 1) || type != kActorType ||
                !SafeRead(&flags, ref + kRefFlags, 4) || (flags & ((1u << 5) | (1u << 11))) != 0 || !SafeRead(&life, ref + kActorLife, 4) ||
                !SafeRead(bound, root + kNodeBound, sizeof(bound))) {
                return false;
            }
            const uint32_t state = (life >> 17) & 0xF;
            if (state == 1 || state == 2 || state == 5 || state == 7) {
                return false;
            }
            if (!std::isfinite(bound[0]) || !std::isfinite(bound[1]) || !std::isfinite(bound[2]) || !(bound[3] > 0.1f) || bound[3] > 5.0f) {
                return false;
            }
            aim    = glm::vec3{ bound[0], bound[1], bound[2] + 0.25f * bound[3] };
            radius = bound[3];
            return true;
        }

        void Unlock(Side& S)
        {
            S.target = Target{};
        }

        // The living people near you (the game's high-process actors, looked up by handle and held while `see` runs): see(handle, ref,
        // their middle). Returns how many actors were listed (-1: the list could not be read).
        int ForEachPerson(const std::function<void(uint32_t, uintptr_t, const glm::vec3&)>& see)
        {
            auto* player = CreationEngineSingletonManager::GetPlayerRef();
            uintptr_t lists = 0, table = 0, handles = 0;
            uint32_t  count = 0;
            if (player == nullptr || !SafeRead(&lists, At(kProcessListsRva), 8) || lists == 0 || !SafeRead(&table, lists, 8) || table == 0 ||
                table != At(kProcessListsTable) || !SafeRead(&count, lists + kHighCount, 4) || !SafeRead(&handles, lists + kHighHandles, 8) ||
                handles == 0) {
                return -1;
            }
            std::array<uint32_t, kMaxActors> copy{};
            count = std::min(count, kMaxActors);
            if (count == 0) {
                return 0;
            }
            if (!SafeRead(copy.data(), handles, count * sizeof(uint32_t))) {
                return -1;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t handle = copy[i];
                GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t root) {
                    glm::vec3 aim{};
                    float     radius = 0.5f;
                    if (PersonAim(ref, root, reinterpret_cast<uintptr_t>(player), aim, radius)) {
                        see(handle, ref, aim);
                    }
                });
            }
            return static_cast<int>(count);
        }

        // (fix142) What the aim saw while the grip was held, for the log at the let-go: scans, actors listed and living people found
        // (the most in a scan), the nearest to the hand's line (degrees, metres, whether in view), and locks made.
        uint32_t g_aim_logs{ 0 };

        // While the grip is squeezed with the weapon in hand: the people near you looked through (25 times a second) for the one nearest
        // the hand's pointing line (as the gloves pick the object nearest it), locked on with the gloves' buzz, kept while you point near
        // them (not counting the time the hand swings: a wind-up points it away). (fix142: a first lock is taken while the hand moves too:
        // the grip is often squeezed as the swing starts; only a lock already made is not taken over while it swings.)
        void AimAtPeople(Side& S, int64_t now, float hand_speed)
        {
            if (!g_aim_on.load(std::memory_order_relaxed)) {
                Unlock(S);
                return;
            }
            const bool swinging = hand_speed > kAimFreeze;
            if (swinging) {
                S.swung_us = now;
            }
            const bool settling = S.swung_us != 0 && now - S.swung_us < kAimSettleUs;
            if (now - S.scan_us < kAimScanUs) {
                return;
            }
            S.scan_us = now;
            glm::vec3 origin{}, line{}, me{}, look{};
            if (!tracking::GloveRayWorld(0, S.left, origin, line) || !Finite(origin) || !Finite(line) || glm::length(line) < 0.5f ||
                !HeadWorld(me, look) || glm::length(look) < 0.5f) {
                return;
            }
            line = glm::normalize(line);
            look = glm::normalize(look);
            struct Seen
            {
                uint32_t  handle{ 0 };
                uintptr_t ref{ 0 };
                glm::vec3 aim{ 0.0f };
                float     deg{ 180.0f };
                float     score{ 1e9f };
            };
            Seen best{}, current{};
            bool current_seen = false;
            int  people       = 0;
            const int listed  = ForEachPerson([&](uint32_t handle, uintptr_t ref, const glm::vec3& aim) {
                const glm::vec3 to    = aim - origin;
                const float     dist  = glm::length(to);
                const float     ahead = glm::dot(to, line);
                if (dist < 0.5f || dist > kAimRange) {
                    return; // (too near, too far)
                }
                ++people;
                const float deg = glm::degrees(std::acos(std::clamp(ahead / dist, -1.0f, 1.0f)));
                const Seen  s{ handle, ref, aim, deg, deg + kAimPerMetre * dist };
                // (The locked one seen wherever the hand points: a wind-up points it away, even behind it.)
                if (handle == S.target.handle) {
                    current      = s;
                    current_seen = true;
                }
                if (ahead <= 0.0f) {
                    return; // (behind the hand: not a new pick)
                }
                const float off_line = glm::length(to - line * ahead);
                const float in_view  = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(aim - me), look), -1.0f, 1.0f)));
                if (deg < S.aim_seen.nearest_deg) {
                    S.aim_seen.nearest_deg     = deg;
                    S.aim_seen.nearest_dist    = dist;
                    S.aim_seen.nearest_in_view = in_view <= kAimHeadDeg;
                }
                if ((deg <= kAimAcquireDeg || off_line <= kAimNearLine) && in_view <= kAimHeadDeg && s.score < best.score) {
                    best = s;
                }
            });
            ++S.aim_seen.scans;
            S.aim_seen.listed     = std::max(S.aim_seen.listed, listed);
            S.aim_seen.people     = std::max(S.aim_seen.people, people);
            S.aim_seen.unreadable = S.aim_seen.unreadable || listed < 0;
            // The locked one: gone, dead or too far: let go; pointed well away from (not while swinging) for a moment: let go.
            if (S.target.handle != 0) {
                if (!current_seen) {
                    Unlock(S);
                } else {
                    S.target.aim = current.aim;
                    // (Swinging or just after: the time off the line starts again, a wind-up points the hand anywhere.)
                    if (swinging || settling || current.deg <= kAimKeepDeg) {
                        S.target.off_since = 0;
                    } else if (S.target.off_since == 0) {
                        S.target.off_since = now;
                    }
                    if (S.target.off_since != 0 && now - S.target.off_since > kAimOffUs) {
                        Unlock(S);
                    }
                }
            }
            // A new lock: none yet (taken even while the hand moves), or another person much nearer the line (not while it swings).
            if (best.handle == 0 || best.handle == S.target.handle) {
                return;
            }
            if (S.target.handle != 0 && (swinging || settling || !(best.score + kAimSwitchDeg < current.score))) {
                return;
            }
            S.target.handle    = best.handle;
            S.target.id        = FormId(best.ref);
            S.target.aim       = best.aim;
            S.target.off_since = 0;
            S.target.swinging  = swinging || settling;
            ++S.aim_seen.locks;
            haptics::MeleePulse(S.left, 0.5f, 0.08f); // (the gloves' lock buzz)
            if (++g_locks <= 30 && LogOk()) {
                spdlog::info("[Throw{}] aimed at a person ({:08X}) {:.1f} m away, {:.0f} degrees off your hand's line: a throw now flies at them", S.tag,
                    S.target.id, glm::length(best.aim - origin), best.deg);
            }
        }

        // (fix142) At the let-go: what the aim saw while the grip was held (the first 40 grips held over a quarter second), for the log.
        void LogAim(Side& S, int64_t held_us, bool threw)
        {
            const AimSeen s = S.aim_seen;
            S.aim_seen      = AimSeen{};
            if (!g_aim_on.load(std::memory_order_relaxed) || held_us < 250'000 || g_aim_logs >= 40 || !LogOk()) {
                return;
            }
            ++g_aim_logs;
            spdlog::info("[Throw{}] the grip held {:.2f} s ({}): {} looks; actors near you {}{}, living people within {:.0f} m {}; nearest your hand's line {}; "
                         "locks {}", S.tag,
                held_us * 1e-6, threw ? "thrown" : "not thrown", s.scans, s.listed < 0 ? std::string{ "(the list could not be read)" } : std::to_string(s.listed),
                s.unreadable ? " (unreadable at times)" : "", kAimRange, s.people,
                s.nearest_deg < 900.0f ? std::format("{:.0f} degrees off, {:.1f} m away{}", s.nearest_deg, s.nearest_dist,
                                             s.nearest_in_view ? "" : " (out of your view)")
                                       : std::string{ "none ahead of the hand" },
                s.locks);
        }

        // (fix142) No one locked on at the throw: the person nearest the throw's direction, within kThrowAssistDeg of it, ahead and in
        // range (as Weapon Throw VR's auto-aim): 0 if none.
        uint32_t NearestToThrow(const glm::vec3& from, const glm::vec3& dir, glm::vec3& aim, uint32_t& id, float& deg_out)
        {
            uint32_t found = 0;
            float    best  = 1e9f;
            ForEachPerson([&](uint32_t handle, uintptr_t ref, const glm::vec3& at) {
                const glm::vec3 to   = at - from;
                const float     dist = glm::length(to);
                if (dist < 0.5f || dist > kAimRange) {
                    return;
                }
                const float deg = glm::degrees(std::acos(std::clamp(glm::dot(to / dist, dir), -1.0f, 1.0f)));
                if (deg <= kThrowAssistDeg && deg + kAimPerMetre * dist < best) {
                    best    = deg + kAimPerMetre * dist;
                    found   = handle;
                    aim     = at;
                    id      = FormId(ref);
                    deg_out = deg;
                }
            });
            return found;
        }

        void Throw(Side& S, int64_t now, const glm::vec3& hand_velocity, uint32_t weapon_id)
        {
            Held held;
            {
                std::scoped_lock _{ g_mutex };
                held = S.held;
            }
            if (!held.have || now - held.us > kHeldFreshUs || held.count < 3) {
                if (LogOk()) {
                    spdlog::info("[Throw{}] let go to throw, but your weapon was not placed in your hand just now{}", S.tag, S.left ? "" : " (is \"Show body\" on?)");
                }
                return;
            }
            glm::vec3 velocity{ 0.0f }, spin{ 0.0f };
            glm::vec3 me{}, look{};
            const bool looked = HeadWorld(me, look);
            const glm::vec3 level = looked ? glm::vec3{ look.x, look.y, 0.0f } : glm::vec3{ 0.0f };
            if (!HeldPeak(held, S.model.centre_local, now, level, velocity, spin) || glm::length(velocity) < 0.5f) {
                // (No weapon movement seen: the hand's own, turned into the world.)
                glm::vec3 unused{}, dir{};
                if (!tracking::StageToWorld(glm::vec3{ 0.0f }, hand_velocity, unused, dir)) {
                    return;
                }
                velocity = dir * glm::length(hand_velocity);
            }
            const HeldSample& last = held.samples[(held.next - 1 + kHeldSamples) % kHeldSamples];
            const float       hand_speed = glm::length(velocity);
            glm::vec3         dir        = velocity / hand_speed;

            bool              assisted = false;
            if (looked && glm::length(look) > 0.5f) {
                look = glm::normalize(look);
                if (glm::degrees(std::acos(std::clamp(glm::dot(dir, look), -1.0f, 1.0f))) < kAimAssistDeg) {
                    dir      = glm::normalize(dir * (1.0f - kAimAssist) + look * kAimAssist);
                    assisted = true;
                }
            }
            // (fix141) A person locked on: thrown straight at them (whichever way the swing went, away from you), homing on them.
            // (fix143) No one locked on: the person nearest where your hand points as you let go (the gloves' line), within 20 degrees
            // of it; else the person nearest the throw's direction, within 20 degrees of it; else straight where it was thrown.
            const glm::vec3 from      = Centre(last, S.model.centre_local);
            uint32_t        homing    = S.target.handle;
            uint32_t        homing_id = S.target.id;
            glm::vec3       homing_at = S.target.aim;
            const char*     picked    = homing != 0 ? "the person locked on" : "";
            float           throw_deg = 0.0f;
            // (A lock made while the hand swung, the throw going well away from them (over 45 degrees): the wind-up passed over them;
            // the person the throw goes toward, if any, instead.)
            bool wind_up_lock = false;
            if (homing != 0 && S.target.swinging) {
                const glm::vec3 to = homing_at - from;
                wind_up_lock = glm::length(to) > 0.1f && glm::dot(glm::normalize(to), dir) < std::cos(glm::radians(45.0f));
            }
            if ((homing == 0 || wind_up_lock) && g_aim_on.load(std::memory_order_relaxed)) {
                glm::vec3 at{}, origin{}, line{};
                uint32_t  id    = 0;
                uint32_t  found = 0;
                if (tracking::GloveRayWorld(0, S.left, origin, line) && Finite(origin) && Finite(line) && glm::length(line) > 0.5f) {
                    found = NearestToThrow(origin, glm::normalize(line), at, id, throw_deg);
                    picked = found != 0 ? "the person nearest where your hand points" : picked;
                }
                if (found == 0) {
                    found  = NearestToThrow(from, dir, at, id, throw_deg);
                    picked = found != 0 ? "the person nearest its direction" : picked;
                }
                if (found != 0) {
                    homing    = found;
                    homing_id = id;
                    homing_at = at;
                }
            }
            if (homing != 0) {
                const glm::vec3 to = homing_at - from;
                if (glm::length(to) > 0.1f) {
                    dir      = glm::normalize(to);
                    assisted = false;
                }
            }
            // (fix143) Spinning flat, as in Jedi Academy: the blade laid level (turned about the line across it, its middle kept), then
            // turned about the vertical, the way the swing turned (its spin about the vertical; else as seen from above, clockwise).
            float spin_sign = 1.0f;
            if (std::abs(spin.z) > 1.0f) {
                spin_sign = spin.z > 0.0f ? 1.0f : -1.0f;
            }
            glm::mat3 r = last.r;
            if (glm::length(S.model.centre_local) > 1e-3f) {
                const glm::vec3 blade = glm::normalize(last.r * S.model.centre_local);
                glm::vec3       flat{ blade.x, blade.y, 0.0f };
                if (glm::length(flat) < 0.2f) {
                    // (The blade nearly upright: laid along the throw's level direction.)
                    flat = glm::vec3{ dir.x, dir.y, 0.0f };
                }
                if (glm::length(flat) > 1e-3f) {
                    flat                 = glm::normalize(flat);
                    const glm::vec3 axis = glm::cross(blade, flat);
                    const float     turn = std::acos(std::clamp(glm::dot(blade, flat), -1.0f, 1.0f));
                    if (glm::length(axis) > 1e-4f && turn > 1e-3f) {
                        r = glm::mat3_cast(glm::angleAxis(turn, glm::normalize(axis))) * last.r;
                    }
                }
            }
            spin = glm::vec3{ 0.0f, 0.0f, kJediSpin * spin_sign };

            S.guided              = false; // (fix144: guided only by a trigger held after this throw)
            S.guide_armed         = false;
            S.flight              = Flight{};
            S.flight.phase        = Phase::Flying;
            S.flight.weapon_id    = weapon_id;
            S.flight.centre       = from;
            S.flight.start        = S.flight.centre;
            S.flight.velocity     = dir * kJediSpeed;
            S.flight.spin         = spin;
            S.flight.r            = r;
            S.flight.s            = last.s;
            S.flight.centre_local = S.model.centre_local;
            S.flight.radius       = std::clamp(S.model.radius * 0.6f, 0.15f, 0.40f);
            S.flight.thrown_us    = now;
            S.flight.phase_us     = now;
            S.flight.loading      = GameFlow::lastLoadingScreen();
            S.flight.number       = g_throws.fetch_add(1) + 1;
            S.flight.homing       = homing;
            S.flight.homing_until = homing != 0 ? now + kHomingMaxUs : 0;
            Publish(S, true);
            ImpactMelee::ForgetSwings(S.left); // (the swing that threw it does not also strike with the empty hand)
            haptics::MeleePulse(S.left, 0.35f, 0.06f);
            if (LogOk()) {
                spdlog::info("[Throw{}] throw {}: weapon {:08X} thrown at {:.0f} m/s (your swing {:.1f} m/s), {:.0f} degrees {} level, {}, spinning flat "
                             "{:.1f} turns a second, sweep sphere {:.2f} m; out to {:.0f} m or {:.1f} s, then back", S.tag,
                    S.flight.number, weapon_id, kJediSpeed, hand_speed, std::abs(glm::degrees(std::asin(std::clamp(dir.z, -1.0f, 1.0f)))),
                    dir.z >= 0.0f ? "above" : "below",
                    homing != 0 ? std::format("at {} ({:08X}{}), homing", picked, homing_id,
                                      picked[0] != '\0' && std::string_view{ picked } != "the person locked on" ? std::format(", {:.0f} degrees off", throw_deg)
                                                                                                                : std::string{})
                                : assisted ? std::string{ "straight (a quarter toward where you look)" } : std::string{ "straight where it was thrown" },
                    kJediSpin / glm::two_pi<float>(), S.flight.radius, kJediRange, kJediOutUs * 1e-6);
            }
        }

        // (fix143) Out, as Jedi Academy's saber: homing on the person the throw picked until it strikes them; (fix144) while you hold the
        // trigger, guided by your throwing hand instead (it turns toward the point your hand points at, at its distance from your hand);
        // one speed, no falling; it cuts through people (each struck once, out and back) and
        // flies on; a wall, or its range (kJediRange from where it left your hand, or kJediOutUs), and it comes back by itself.
        void Fly(Side& S, int64_t now, float dt, bool guiding)
        {
            Flight& f = S.flight;
            // (fix144: guided by the trigger: your hand, not the homing.)
            if (guiding && f.homing != 0 && now - f.thrown_us > kGuideAfterUs) {
                f.homing = 0;
            }
            const float speed = glm::length(f.velocity);
            // (fix141) Homing on the person it was thrown at: turned toward their middle, at most kHomingTurn a second, keeping its speed;
            // until they are gone or dead, struck, passed (over 90 degrees off), or kHomingMaxUs.
            if (f.homing != 0) {
                glm::vec3 aim{};
                bool      alive = false;
                auto*     player = CreationEngineSingletonManager::GetPlayerRef();
                GrenadeHand::WithReference(f.homing, [&](uintptr_t ref, uintptr_t root) {
                    float radius = 0.5f;
                    alive        = PersonAim(ref, root, reinterpret_cast<uintptr_t>(player), aim, radius);
                });
                const glm::vec3 to = aim - f.centre;
                if (!alive || now > f.homing_until || speed < 1.0f || glm::length(to) < 1e-3f) {
                    f.homing = 0;
                } else {
                    const glm::vec3 cur   = f.velocity / speed;
                    const glm::vec3 want  = glm::normalize(to);
                    const float     angle = std::acos(std::clamp(glm::dot(cur, want), -1.0f, 1.0f));
                    if (angle > glm::half_pi<float>()) {
                        f.homing = 0; // (it has passed them)
                    } else if (angle > 1e-4f) {
                        const float     turn = std::min(angle, kHomingTurn * dt);
                        const glm::vec3 axis = glm::cross(cur, want);
                        if (glm::length(axis) > 1e-6f) {
                            f.velocity = glm::angleAxis(turn, glm::normalize(axis)) * cur * speed;
                        }
                    }
                }
            }
            // (fix144) Guided (the trigger held): toward the point on your hand's pointing line as far from your hand as it is, wherever your
            // hand points; not in the throw's follow-through.
            if (guiding && f.homing == 0 && now - f.thrown_us > kGuideAfterUs && speed > 1.0f) {
                glm::vec3 origin{}, line{};
                if (tracking::GloveRayWorld(0, S.left, origin, line) && Finite(origin) && Finite(line) && glm::length(line) > 0.5f) {
                    line                   = glm::normalize(line);
                    const glm::vec3 to_it  = f.centre - origin;
                    const float     reach    = glm::length(to_it);
                    const float     off    = reach > 1e-3f ? glm::degrees(std::acos(std::clamp(glm::dot(to_it / reach, line), -1.0f, 1.0f))) : 180.0f;
                    if (reach > 0.5f) {
                        const glm::vec3 guide = origin + line * reach;
                        const glm::vec3 to    = guide - f.centre;
                        if (glm::length(to) > 0.05f) {
                            const glm::vec3 cur   = f.velocity / speed;
                            const glm::vec3 want  = glm::normalize(to);
                            const float     angle = std::acos(std::clamp(glm::dot(cur, want), -1.0f, 1.0f));
                            const glm::vec3 axis  = glm::cross(cur, want);
                            if (angle > 1e-3f && glm::length(axis) > 1e-6f) {
                                const float turn = std::min(angle, kGuideTurn * dt);
                                f.velocity       = glm::angleAxis(turn, glm::normalize(axis)) * cur * kJediSpeed;
                                f.steer_total += turn;
                                if (!f.steered && f.steer_total > glm::radians(3.0f)) {
                                    f.steered = true;
                                    if (LogOk()) {
                                        spdlog::info("[Throw{}] throw {}: guided: it follows where your hand points while you hold the trigger ({:.1f} m out, your hand {:.0f} degrees off it)", S.tag,
                                            f.number, glm::length(f.centre - f.start), off);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // (One speed out, as homing and steering keep it.)
            if (glm::length(f.velocity) > 1e-3f) {
                f.velocity = glm::normalize(f.velocity) * kJediSpeed;
            }
            const glm::vec3 next = f.centre + f.velocity * dt;
            const float rate = glm::length(f.spin);
            if (rate > 1e-3f) {
                f.r = glm::mat3_cast(glm::angleAxis(rate * dt, f.spin / rate)) * f.r;
            }
            uintptr_t target = 0;
            glm::vec3 point{};
            const int hit = ImpactMelee::SweepWorld(S.left, f.centre, next, f.radius, target, point);
            if (hit < 0) {
                BackInHand(S, "the game's melee sweep is unavailable");
                return;
            }
            if (hit == 2) {
                // A person: struck once (each one, on the way out), and it flies on through them.
                if (!AlreadyStruck(f.struck_out, f.n_out, target)) {
                    Strike(S, target, point, false);
                    // (The one it was homing on struck: from here it keeps its course, or your hand guides it while you hold the trigger.)
                    if (f.homing != 0) {
                        GrenadeHand::WithReference(f.homing, [&](uintptr_t ref, uintptr_t) {
                            if (ref == target) {
                                f.homing = 0;
                            }
                        });
                    }
                }
            } else if (hit == 1) {
                // A wall, the ground, anything not a person: it comes back from there.
                const glm::vec3 dir = glm::length(f.velocity) > 1e-3f ? glm::normalize(f.velocity) : glm::vec3{ 0.0f, 0.0f, -1.0f };
                f.centre            = point - dir * (f.radius * 0.6f);
                g_surfaces.fetch_add(1);
                haptics::MeleePulse(S.left, 0.2f, 0.04f);
                StartReturn(S, now, std::format("it hit a wall {:.1f} m out ({:.2f} s in flight)", glm::length(point - f.start), (now - f.thrown_us) * 1e-6).c_str());
                return;
            }
            f.centre = next;
            const float out = glm::length(f.centre - f.start);
            if (guiding ? (out > kGuideRange || now - f.thrown_us > kGuideOutUs) : (out > kJediRange || now - f.thrown_us > kJediOutUs)) {
                StartReturn(S, now, std::format("it reached its range ({:.1f} m out, {:.1f} s)", out, (now - f.thrown_us) * 1e-6).c_str());
            }
        }

        void Return(Side& S, int64_t now, float dt)
        {
            Flight&   f = S.flight;
            glm::vec3 hand{};
            if (!HandWorld(S, hand)) {
                BackInHand(S, "your hand is not tracked");
                return;
            }
            const glm::vec3 to       = hand - f.centre;
            const float     distance = glm::length(to);
            const float     catch_at = std::max(0.30f, glm::length(f.centre_local) * f.s + 0.10f);
            if (distance < catch_at || now - f.phase_us > kReturnMaxUs) {
                haptics::MeleePulse(S.left, 0.8f, 0.12f);
                BackInHand(S, distance < catch_at ? "caught" : "it took too long, so it is simply back");
                return;
            }
            const float speed = std::clamp(distance / kReturnTime, kReturnMin, kReturnMax);
            const float step  = std::min(distance, speed * dt);
            const glm::vec3 next = f.centre + to / distance * step;
            const float rate = glm::length(f.spin);
            if (rate > 1e-3f) {
                f.r = glm::mat3_cast(glm::angleAxis(rate * dt, f.spin / rate)) * f.r;
            }
            uintptr_t target = 0;
            glm::vec3 point{};
            const int hit = ImpactMelee::SweepWorld(S.left, f.centre, next, f.radius, target, point);
            if (hit < 0) {
                BackInHand(S, "the game's melee sweep is unavailable");
                return;
            }
            if (hit == 2 && !AlreadyStruck(f.struck_back, f.n_back, target)) {
                Strike(S, target, point, true);
            }
            f.centre = next; // (it comes back through walls)
        }
    }

    void Install()
    {
        g_ready = ImpactMelee::CanSweepWorld();
        spdlog::info("[Throw] throwing your melee weapons: {}", g_ready.load() ? "ready, from either hand (the game's melee hit sweeps their flight; your melee hit "
                                                                                 "is applied to people they strike)"
                                                                               : "off (impact melee's hooks were not found on this game version)");
    }

    void OnWeaponInHand(bool left, const glm::mat3& r, const glm::vec3& t, float s, const glm::mat3& drawn_r, const glm::vec3& drawn_t, float drawn_s,
        const glm::vec3& bound_centre, float bound_radius)
    {
        if (!Finite(t) || !std::isfinite(s) || s <= 0.0f) {
            return;
        }
        const int64_t now = NowUs();
        std::scoped_lock _{ g_mutex };
        Held& h = g_sides[left ? 0 : 1].held;
        h.have  = true;
        h.us    = now;
        h.r     = r;
        h.t     = t;
        h.s     = s;
        // The model's middle in the weapon's space, from where it was last drawn (sensible values only).
        glm::vec3 local{ 0.0f };
        bool      ok = false;
        if (Finite(bound_centre) && std::isfinite(bound_radius) && Finite(drawn_t) && drawn_s > 0.0f && std::isfinite(drawn_s)) {
            local = glm::transpose(drawn_r) * (bound_centre - drawn_t) / drawn_s;
            ok    = Finite(local) && glm::length(local) < 1.5f && bound_radius > 0.03f && bound_radius < 1.5f;
        }
        h.bound_ok     = ok;
        h.centre_local = ok ? local : glm::vec3{ 0.0f };
        h.radius       = ok ? bound_radius : 0.35f;
        HeldSample sample;
        sample.us = now;
        sample.t  = t;
        sample.r  = r;
        sample.s  = s;
        h.samples[h.next] = sample;
        h.next            = (h.next + 1) % kHeldSamples;
        h.count           = std::min(h.count + 1, kHeldSamples);
    }

    bool BladeSegment(bool left, glm::vec3& hilt, glm::vec3& tip)
    {
        std::scoped_lock _{ g_mutex };
        const Held& h = g_sides[left ? 0 : 1].held;
        if (!h.have || !h.bound_ok || NowUs() - h.us > kHeldFreshUs) {
            return false;
        }
        // (fix172) The model's middle in the world; the blade runs from the hilt through it to as far beyond (a saber's hilt and blade).
        const glm::vec3 centre = h.t + h.r * (h.centre_local * h.s);
        hilt                   = h.t;
        tip                    = h.t + 2.0f * (centre - h.t);
        const float length     = glm::length(tip - hilt);
        return Finite(hilt) && Finite(tip) && std::isfinite(length) && length > 0.2f && length < 2.5f;
    }

    namespace
    {
        // One hand's throw, each frame (main thread). The right hand's weapon is the game's (the body's weapon bone; the body must be
        // shown); (fix156) the left hand's is LeftWeapon's (GrenadeHand's copy, moved where it flies).
        void UpdateSide(Side& S, int64_t now)
        {
            static auto vr       = VR::get();
            const float dt       = S.last_us != 0 ? std::clamp(static_cast<float>(now - S.last_us) * 1e-6f, 0.0f, 0.05f) : 0.0f;
            S.last_us            = now;
            const auto& settings = GameFlow::gStore.internalSettings;

            // The hand (stage space), when its pose changes.
            {
                const glm::vec3   at{ tracking::GripPose(S.left)[3] };
                const HandSample& last = S.hand[(S.hand_next - 1 + kHandSamples) % kHandSamples];
                if (Finite(at) && (S.hand_count == 0 || at != last.at)) {
                    S.hand[S.hand_next] = HandSample{ now, at };
                    S.hand_next         = (S.hand_next + 1) % kHandSamples;
                    S.hand_count        = std::min(S.hand_count + 1, kHandSamples);
                }
            }
            const bool  grip     = HandGrab::GripDown(S.left);
            const bool  pressed  = grip && !S.grip_was;
            const bool  released = !grip && S.grip_was;
            S.grip_was           = grip;
            const float pull     = HandGrab::TriggerPull(S.left);
            const bool  trigger  = S.trigger_was ? pull > 0.4f : pull > 0.7f;
            S.trigger_was        = trigger;
            // (fix171) The favorites wheel's hold on this grip ends as the grip is let go, whatever the hand holds (the review: cleared only
            // with a melee weapon out, it stayed set and ate the first throw after one was drawn).
            if (!grip) {
                g_hold_off[S.left ? 0 : 1] = false;
            }

            const uint32_t melee_id = S.left ? (LeftWeapon::Held() ? LeftWeapon::FormId() : 0u) : HandGrab::MeleeWeaponOut();
            // (fix158) The left hand's weapon out: LeftWeapon's (it can be out on its own, your weapon put away); the right's: drawn.
            const bool     out      = S.left ? melee_id != 0 : GameFlow::weaponState() == 3;
            const bool     usable   = settings.meleeThrow && g_ready.load() && ImpactMelee::CanSweepWorld() && (S.left || body::IsActive()) &&
                                vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() &&
                                !GameFlow::isWorkshopShown() && // (fix175: nor build mode)
                                !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && !ModSettings::showFlatScreenDisplay() &&
                                !LeftWeapon::PlayerSeated(); // (fix189: nor sitting or lying: chairs, beds, the pilot's seat without its HUD)
            S.available = S.flight.phase != Phase::InHand || (usable && melee_id != 0 && out);

            if (S.flight.phase != Phase::InHand) {
                // Back at once when it cannot stay away.
                const char* why = nullptr;
                if (!settings.meleeThrow) {
                    why = "throwing was turned off";
                } else if (GameFlow::isShowingMenu()) {
                    why = "a menu opened";
                } else if (GameFlow::isWorkshopShown()) {
                    why = "build mode opened (fix175)";
                } else if (GameFlow::lastLoadingScreen() != S.flight.loading) {
                    why = "a loading screen came";
                } else if (melee_id == 0 || !out) {
                    why = S.left ? "your weapon was put away (or the left hand's weapon taken out)" : "your weapon was put away";
                } else if (melee_id != S.flight.weapon_id) {
                    why = "another weapon is in your hands";
                } else if (LeftWeapon::PlayerSeated()) {
                    why = "you sat or lay down (fix189)";
                } else if (!usable) {
                    why = S.left ? "you are not playing in first person with your controllers"
                                 : "the body is not shown, or you are not playing in first person with your controllers";
                }
                if (why != nullptr) {
                    BackInHand(S, why);
                    return;
                }
                // (fix144) The trigger pulled while it flies (after the throw: one held through the throw does not count until let go)
                // guides it; let go of it, it comes back. The grip squeezed calls it back.
                if (!trigger) {
                    S.guide_armed = true;
                }
                const bool guiding = trigger && S.guide_armed && S.flight.phase == Phase::Flying;
                if (S.guided && !trigger && S.flight.phase == Phase::Flying) {
                    StartReturn(S, now, "you let go of the trigger");
                } else if (pressed && (S.flight.phase == Phase::Flying || S.flight.phase == Phase::Resting)) {
                    StartReturn(S, now, "you squeezed the grip");
                } else if (now - S.flight.thrown_us > kAutoReturnUs) {
                    StartReturn(S, now, "10 seconds passed");
                }
                S.guided = guiding;
                if (S.flight.phase == Phase::Flying) {
                    Fly(S, now, dt, guiding);
                } else if (S.flight.phase == Phase::Returning) {
                    Return(S, now, dt);
                }
                if (S.flight.phase != Phase::InHand) {
                    Publish(S, true);
                }
                return;
            }

            // In your hand: squeeze the grip, swing and let go.
            if (!usable || melee_id == 0 || !out) {
                S.grip_since_us = 0;
                Unlock(S);
                return;
            }
            // (fix171) The favorites wheel has this grip (it opens with the left grip held; a grip squeezed while it is open): no throw readied
            // with it, until it is let go (the grip let go after choosing would otherwise throw the weapon with the hand's last move).
            if (g_hold_off[S.left ? 0 : 1]) {
                S.grip_since_us = 0;
                Unlock(S);
                return;
            }
            // The model's middle and size, latched once per weapon.
            if (S.model.id != melee_id) {
                S.model           = Model{};
                S.model.id        = melee_id;
                S.model.out_since = now;
            }
            if (!S.model.ok && now - S.model.out_since > kModelSettleUs) {
                Held held;
                {
                    std::scoped_lock _{ g_mutex };
                    held = S.held;
                }
                if (held.have && now - held.us < kHeldFreshUs && held.bound_ok) {
                    S.model.ok           = true;
                    S.model.centre_local = held.centre_local;
                    S.model.radius       = held.radius;
                    spdlog::info("[Throw{}] your melee weapon {:08X}: its model is {:.2f} m across, its middle {:.2f} m from your grip (squeeze the "
                                 "grip, swing and let go to throw it)",
                        S.tag, melee_id, held.radius * 2.0f, glm::length(held.centre_local) * held.s);
                } else if (!S.model.told && now - S.model.out_since > kModelGiveUpUs) {
                    S.model.told = true;
                    spdlog::info("[Throw{}] your melee weapon {:08X}: its model's size could not be read; it is thrown from your grip and swept as 0.6 "
                                 "m across",
                        S.tag, melee_id);
                }
            }
            if (pressed) {
                S.grip_since_us = now;
                S.aim_seen      = AimSeen{}; // (this grip's, for the log at its let-go)
                Unlock(S);
            }
            // (fix141) Primed to throw: the person nearest your hand's pointing line locked on, while the grip is held.
            if (grip && S.grip_since_us != 0) {
                glm::vec3 moving{ 0.0f };
                AimAtPeople(S, now, HandPeak(S, now, glm::vec3{ 0.0f }, moving) ? glm::length(moving) : 0.0f);
            }
            if (released && S.grip_since_us != 0) {
                const int64_t held_us = now - S.grip_since_us;
                S.grip_since_us       = 0;
                glm::vec3       velocity{ 0.0f }, any_way{ 0.0f };
                const glm::mat4 head   = tracking::HeadPose();
                glm::vec3       facing = -glm::vec3{ head[2] };
                facing.y               = 0.0f;
                if (held_us < kMinGripUs) {
                    LogAim(S, held_us, false);
                    Unlock(S);
                    return;
                }
                const bool  moved     = HandPeak(S, now, facing, velocity);
                const float speed     = moved ? glm::length(velocity) : 0.0f;
                const float threshold = std::max(kMinThrowSpeed, settings.meleeSwingSpeed);
                if (moved && speed >= threshold) {
                    Throw(S, now, velocity, melee_id);
                    LogAim(S, held_us, true);
                } else {
                    LogAim(S, held_us, false);
                    // (For the log: how fast it went at all, any way; the log's lines counted only when one is written.)
                    const bool  any     = HandPeak(S, now, glm::vec3{ 0.0f }, any_way);
                    const float fastest = any ? glm::length(any_way) : 0.0f;
                    if (fastest > 1.0f && LogOk()) {
                        spdlog::info("[Throw{}] let go of the grip: not a throw (moving away from you at {:.1f} m/s, fastest any way {:.1f} m/s; a throw "
                                     "needs {:.1f} m/s away from you)",
                            S.tag, speed, fastest, threshold);
                    }
                }
            }
            if (!grip) {
                S.grip_since_us = 0;
                Unlock(S); // (its lock let go with the grip, after a throw used it)
            }
        }
    }

    void Update()
    {
        const int64_t now = NowUs();
        UpdateSide(g_sides[1], now);
        UpdateSide(g_sides[0], now);
    }

    void SetAimAtPeople(bool on)
    {
        g_aim_on.store(on, std::memory_order_relaxed);
    }

    bool Away(bool left)
    {
        return g_sides[left ? 0 : 1].away.load(std::memory_order_relaxed);
    }

    bool Available(bool left)
    {
        return g_sides[left ? 0 : 1].available.load(std::memory_order_relaxed);
    }

    bool FlightPose(bool left, glm::mat3& r, glm::vec3& t, float& s)
    {
        std::scoped_lock _{ g_mutex };
        const auto& pose = g_sides[left ? 0 : 1].pose;
        if (!pose.away) {
            return false;
        }
        r = pose.r;
        t = pose.t;
        s = pose.s;
        return true;
    }

    bool HandWithoutWeapon(bool left)
    {
        const auto& S = g_sides[left ? 0 : 1];
        return S.away.load(std::memory_order_relaxed) || NowUs() - S.caught_us.load(std::memory_order_relaxed) < kNoStrikeAfterUs;
    }

    bool BlocksRightTrigger()
    {
        return g_sides[1].away.load(std::memory_order_relaxed);
    }

    void HoldOffGrip(bool left) { g_hold_off[left ? 0 : 1] = true; }

    int PeopleNearby(const std::function<void(uint32_t handle, uintptr_t ref, const glm::vec3& middle)>& see) { return ForEachPerson(see); }

    std::string Status()
    {
        if (!g_ready.load()) {
            return "Throwing is off: impact melee's hooks were not found on this game version.";
        }
        return fmt::format("Ready, from either hand. This session: {} throws, {} people struck, {} times it struck something else, {} called back.",
            g_throws.load(), g_people.load(), g_surfaces.load(), g_returns.load());
    }
}
