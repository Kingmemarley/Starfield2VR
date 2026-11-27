#include "SeatView.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <RE/N/NiCamera.h>
#include "CreationEngineSingletonManager.h"
#include "NiMath.h"
#include "body/BodyIK.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"

namespace SeatView
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The eyes from the body's head bone (C_Head, at the top of the neck), as on a ladder (LadderClimb): this far up and ahead along
        // the body's level facing (a little ahead of the real eyes; the face is hidden while the view is there).
        constexpr float kHeadToEyesUp    = 0.08f;
        constexpr float kHeadToEyesAhead = 0.11f;
        // After sitting down (sitting, the camera back in first person), this long before measuring: the camera has come out of the
        // game's sit-down camera and the body has settled into its sitting pose. Your head's place for the lean limit is taken meanwhile.
        constexpr float kSettleS = 0.5f;
        // The gap is the average of this many frames in a row (the sitting idle sways a little); given up after this long without them.
        constexpr int   kSamples = 30;
        constexpr float kGiveUpS = 3.0f;
        // The gap changing more than this (metres) from one sample to the next: something moves (a moving seat: the camera and the head
        // bone are read at different moments of the frame); the samples so far dropped.
        constexpr float kStillGap = 0.005f;
        // A gap larger than this (metres) is not trusted: such a sample is dropped, and an average over it keeps the game's own place.
        constexpr float kMaxGap = 0.30f;
        // Sitting upright: the head bone this high above the body's root (metres), and at most this far from above it (level). Else lying
        // (a bed), where "up and ahead of the head" is not where the eyes are. (Both logged with each sit.)
        constexpr float kHeadAboveRootMin   = 0.8f;
        constexpr float kHeadAboveRootMax   = 1.7f;
        constexpr float kHeadLevelFromRoot  = 0.5f;
        // Eased in and out over this long (seconds).
        constexpr float kEaseS = 0.3f;
        // Your head's level lean from where it was as you sat: followed fully up to kLimitKnee, then less and less, never past kLimit.
        constexpr float kLimitKnee = 0.15f;
        constexpr float kLimit     = 0.30f;
        // The seated log line every this many seconds.
        constexpr float kPeriodS = 5.0f;
        // Not seated (for this) until the flight HUD has been gone this long (taking off, landing).
        constexpr float kAfterFlightS = 3.0f;

        enum class Phase
        {
            Off,       // not seated
            Settling,  // seated, waiting kSettleS
            Measuring, // seated, taking the samples
            Applied,   // the view at the eyes
            Kept,      // the game's place kept (no head bone, too far, lying, moving)
        };

        struct State
        {
            Phase     phase{ Phase::Off };
            float     settle_s{ 0.0f };
            float     measure_s{ 0.0f };
            int       n{ 0 };
            glm::vec3 sum{ 0.0f };          // samples of the game's camera less the eyes, in the body's frame (ahead, right, up)
            float     head_up_sum{ 0.0f };   // samples of the head bone's height above the root
            float     head_level_sum{ 0.0f }; // samples of its level distance from the root
            int       moving{ 0 }, no_head{ 0 }, far_n{ 0 };
            glm::vec3 last_d{ 0.0f };
            bool      have_last{ false };
            glm::vec3 gap{ 0.0f };                 // the measured gap (body frame)
            glm::vec3 forward{ 0.0f, 1.0f, 0.0f }; // the body's level forward, as last read
            float     offset_w{ 0.0f }, limit_w{ 0.0f };
            glm::vec3 ref{ 0.0f };    // your head (less the origin) as you sat: the lean is measured from it
            glm::vec3 origin{ 0.0f }; // the origin then (a recenter takes the head's place anew)
            // For the seated log line.
            float     period_s{ 0.0f };
            int       frames{ 0 }, limited{ 0 };
            double    lean_sum{ 0.0 };
            float     lean_max{ 0.0f }, cut_max{ 0.0f };
            glm::vec3 lean_vec_sum{ 0.0f };
        };
        State             g{};
        Clock::time_point g_last{};
        Clock::time_point g_flight_at{};
        std::atomic<bool> g_head_wanted{ false };
        std::atomic<bool> g_face_hidden{ false };
        std::atomic<bool> g_head_scale{ false }; // (decided once a sit: the game's camera not under the head bone)
        std::mutex        g_shift_mtx;
        glm::vec3         g_shift{ 0.0f };
        int               g_sit_logs = 0, g_period_logs = 0;

        bool ReadBytes(uintptr_t at, void* out, size_t size)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Sitting (the sit state, bits 11-12 of the player's +0xFC: 2, sitting or lying) with the game's own body shown in first person
        // ([player+0x112A] bit 3, the game's seat view), the camera in first person, no ladder, the mod's own body off, no loading screen,
        // the setting on. (The flight HUD is checked by the caller.)
        // (fix202) And the game's own seat-view flag ([player+0x112F] bit 3, set by its body-in-a-seat view): the third-person mark alone
        // is also set by a seat the game turns the player to third person for (the character-creation bench), which is not this view.
        bool Seated()
        {
            if (!GameFlow::gStore.internalSettings.seatViewAtEyes) {
                return false;
            }
            const auto player   = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uint32_t   state2   = 0;
            uint8_t    mark     = 0;
            uint8_t    seat_rig = 0;
            if (player == 0 || !ReadBytes(player + 0xFC, &state2, sizeof(state2)) || !ReadBytes(player + 0x112A, &mark, sizeof(mark)) ||
                !ReadBytes(player + 0x112F, &seat_rig, sizeof(seat_rig))) {
                return false;
            }
            return ((state2 >> 11) & 3u) == 2 && (mark & 8) != 0 && (seat_rig & 8) != 0 && GameFlow::isInFirstPerson() &&
                   !GameFlow::isOnLadder() && !GameFlow::isLoadingScreenShown() && !body::IsActive();
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        glm::vec3 RightOf(const glm::vec3& f) { return glm::vec3{ f.y, -f.x, 0.0f }; }

        // The body's root (its feet, or where the furniture puts it) and its level forward (kept from the last good read).
        bool ReadRoot(glm::vec3* root_out)
        {
            float root[3]{}, forward[3]{};
            if (!body::ReadRootWorld(root, forward)) {
                return false;
            }
            const glm::vec3 f{ forward[0], forward[1], 0.0f };
            if (std::isfinite(f.x) && std::isfinite(f.y) && glm::length(f) > 1e-3f) {
                g.forward = glm::normalize(f);
            }
            if (root_out != nullptr) {
                *root_out = glm::vec3{ root[0], root[1], root[2] };
                return Finite(*root_out);
            }
            return true;
        }

        void PublishShift(const glm::vec3& shift)
        {
            std::scoped_lock _{ g_shift_mtx };
            g_shift = shift;
        }

        // The move from the game's seat camera to the eyes, in the game's world, as eased in now.
        glm::vec3 ShiftNow()
        {
            if (!(g.offset_w > 0.0f)) {
                return glm::vec3{ 0.0f };
            }
            ReadRoot(nullptr);
            const glm::vec3 f     = g.forward;
            const glm::vec3 shift = -(f * g.gap.x + RightOf(f) * g.gap.y + glm::vec3{ 0.0f, 0.0f, g.gap.z }) * g.offset_w;
            return Finite(shift) ? shift : glm::vec3{ 0.0f };
        }

        void ResetSit()
        {
            const Phase phase = g.phase;
            g                 = State{};
            g.phase           = phase;
        }

        void Decide(const RE::NiCamera* world_camera)
        {
            g.gap                  = g.sum / static_cast<float>(g.n);
            const float head_up    = g.head_up_sum / static_cast<float>(g.n);
            const float head_level = g.head_level_sum / static_cast<float>(g.n);
            // (How the game's seat camera faces against the body, level, for the log.)
            float turn_deg = 0.0f;
            {
                const glm::vec3 c = NiMath::to_glm(world_camera->parent->world.rotate)[1];
                const glm::vec2 a{ c.x, c.y }, b{ g.forward.x, g.forward.y };
                if (glm::length(a) > 1e-3f && glm::length(b) > 1e-3f) {
                    const glm::vec2 na = glm::normalize(a), nb = glm::normalize(b);
                    turn_deg = glm::degrees(std::atan2(na.x * nb.y - na.y * nb.x, glm::dot(na, nb)));
                }
            }
            const char* why = nullptr;
            if (head_up < kHeadAboveRootMin || head_up > kHeadAboveRootMax) {
                why = "the body is not sitting upright (its head's height above its root: lying?)";
            } else if (head_level > kHeadLevelFromRoot) {
                why = "the body's head is far to the side of its root (lying?)";
            } else if (!Finite(g.gap) || glm::length(g.gap) > kMaxGap) {
                why = "more than 0.3 m apart: not trusted";
            }
            g.phase = why == nullptr ? Phase::Applied : Phase::Kept;
            // (The whole head shrunk as on a ladder only if the game's camera does not hang under the head bone: shrinking would move it.)
            const char* head_how = "";
            g_head_scale.store(false, std::memory_order_relaxed);
            if (why == nullptr) {
                bool       under = false;
                const bool known = body::SeatCameraUnderHead(under);
                g_head_scale.store(known && !under, std::memory_order_relaxed);
                head_how = !known ? "; only its face hidden (not known whether the game's camera hangs under its head)"
                           : under ? "; only its face hidden (the game's camera hangs under its head)"
                                   : "; its head hidden as on a ladder (the game's camera is not under it)";
            }
            if (g_sit_logs < 40) {
                ++g_sit_logs;
                spdlog::info("[Seat] (fix199) sitting: the game's seat camera is {:.3f} m ahead of the seated body's eyes, {:.3f} m to their "
                             "right, {:.3f} m above them ({:.3f} m in all; the body's frame, {} frames; the camera turned {:.0f} deg from the "
                             "body; its head bone {:.2f} m above its root and {:.2f} m from above it; {} moving, {} far samples dropped): {}{}{}",
                    g.gap.x, g.gap.y, g.gap.z, glm::length(g.gap), g.n, turn_deg, head_up, head_level, g.moving, g.far_n,
                    why == nullptr ? "the view moved to the eyes, eased in over 0.3 s" : "the game's place kept: ", why == nullptr ? "" : why,
                    head_how);
            }
        }

        void GiveUp()
        {
            g.phase = Phase::Kept;
            if (g_sit_logs < 40) {
                ++g_sit_logs;
                spdlog::info("[Seat] (fix199) sitting: the seated body's eyes not measured in {:.0f} s ({} of {} frames; {} without the head "
                             "bone, {} moving, {} more than 0.3 m apart): the game's place kept",
                    kGiveUpS, g.n, kSamples, g.no_head, g.moving, g.far_n);
            }
        }

        // Each seated frame while measuring (no menu): one sample of the game's camera less the eyes, if the head bone is fresh and the gap
        // has not changed since the last sample.
        void Measure(const RE::NiCamera* world_camera, float dt)
        {
            g.measure_s += dt;
            float     head[3]{};
            glm::vec3 root{ 0.0f };
            const bool have_root = ReadRoot(&root);
            const glm::vec3 cam  = NiMath::to_glm(world_camera->parent->world.translate);
            if (!body::LadderHeadWorld(head)) {
                ++g.no_head;
            } else if (have_root && Finite(cam)) {
                const glm::vec3 f     = g.forward;
                const glm::vec3 right = RightOf(f);
                const glm::vec3 h{ head[0], head[1], head[2] };
                const glm::vec3 eyes = h + glm::vec3{ 0.0f, 0.0f, kHeadToEyesUp } + f * kHeadToEyesAhead;
                const glm::vec3 d    = cam - eyes;
                if (!Finite(d) || !Finite(h)) {
                    // (not used)
                } else if (glm::length(d) > kMaxGap) {
                    ++g.far_n;
                } else {
                    const bool still = !g.have_last || glm::length(d - g.last_d) < kStillGap;
                    g.last_d         = d;
                    g.have_last      = true;
                    if (!still) {
                        ++g.moving;
                        g.n              = 0;
                        g.sum            = glm::vec3{ 0.0f };
                        g.head_up_sum    = 0.0f;
                        g.head_level_sum = 0.0f;
                    } else {
                        g.sum += glm::vec3{ glm::dot(d, f), glm::dot(d, right), d.z };
                        g.head_up_sum += h.z - root.z;
                        g.head_level_sum += glm::length(glm::vec2{ h.x - root.x, h.y - root.y });
                        ++g.n;
                    }
                }
            }
            if (g.n >= kSamples) {
                Decide(world_camera);
            } else if (g.measure_s > kGiveUpS) {
                GiveUp();
            }
        }

        // The level lean (the room's x and z) softly held: followed fully to kLimitKnee, then eased toward kLimit (never past it), by `w`
        // (0: not at all). Returns how much it was held back (metres).
        float LimitLean(glm::vec3& lean, float w)
        {
            const float r = std::sqrt(lean.x * lean.x + lean.z * lean.z);
            if (!(w > 0.0f) || !(r > kLimitKnee)) {
                return 0.0f;
            }
            const float span  = kLimit - kLimitKnee;
            const float soft  = kLimitKnee + span * std::tanh((r - kLimitKnee) / span);
            const float kept  = r + (soft - r) * std::min(w, 1.0f);
            const float scale = kept / r;
            lean.x *= scale;
            lean.z *= scale;
            return r - kept;
        }

        void Period(const glm::vec3& lean, float cut, float dt)
        {
            const float level = std::sqrt(lean.x * lean.x + lean.z * lean.z);
            ++g.frames;
            g.lean_sum += level;
            g.lean_max = std::max(g.lean_max, level);
            g.lean_vec_sum += lean;
            if (cut > 0.0005f) {
                ++g.limited;
                g.cut_max = std::max(g.cut_max, cut);
            }
            g.period_s += dt;
            if (g.period_s < kPeriodS) {
                return;
            }
            if (g_period_logs < 200 && g.frames > 0) {
                ++g_period_logs;
                const glm::vec3 mean = g.lean_vec_sum / static_cast<float>(g.frames);
                spdlog::info("[Seat] (fix199) seated, the last {:.0f} s: your head leaned {:.3f} m from where it was as you sat on average "
                             "(level), at most {:.3f} (on average {:.3f} ahead, {:.3f} to the right, {:.3f} up, in the room's axes); the soft "
                             "limit held it back in {} of {} frames, by at most {:.3f} m; the view {} ({:.0f}% in)",
                    g.period_s, g.lean_sum / g.frames, g.lean_max, -mean.z, mean.x, mean.y, g.limited, g.frames, g.cut_max,
                    g.phase == Phase::Applied ? "at the body's eyes" : g.phase == Phase::Kept ? "at the game's seat camera (not measured)"
                                                                                               : "at the game's seat camera (measuring)",
                    g.offset_w * 100.0f);
            }
            g.period_s = 0.0f;
            g.frames = g.limited = 0;
            g.lean_sum           = 0.0;
            g.lean_max = g.cut_max = 0.0f;
            g.lean_vec_sum       = glm::vec3{ 0.0f };
        }

        float StepTime()
        {
            const auto  now = Clock::now();
            const float dt  = g_last == Clock::time_point{} ? 0.0f : std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
            g_last          = now;
            return dt;
        }

        // Up from the seat and eased out: the next sit is measured anew.
        void EndIfDone()
        {
            if (g.phase != Phase::Off && !(g.offset_w > 0.0f) && !(g.limit_w > 0.0f)) {
                if (g.phase == Phase::Applied && g_sit_logs < 40) {
                    ++g_sit_logs;
                    spdlog::info("[Seat] (fix199) up from the seat: the view back at the game's camera (eased out)");
                }
                g.phase = Phase::Off;
                g_head_scale.store(false, std::memory_order_relaxed);
            }
        }
    }

    bool HeadWanted() { return g_head_wanted.load(std::memory_order_relaxed); }

    bool FaceHidden() { return g_face_hidden.load(std::memory_order_relaxed); }

    bool HeadScaleWanted() { return g_face_hidden.load(std::memory_order_relaxed) && g_head_scale.load(std::memory_order_relaxed); }

    glm::vec3 WorldShift()
    {
        std::scoped_lock _{ g_shift_mtx };
        return g_shift;
    }

    glm::vec3 Update(RE::NiCamera* world_camera, glm::vec3& head, const glm::vec3& origin)
    {
        const float dt  = StepTime();
        const auto  now = Clock::now();
        if (GameFlow::isPilotingShip()) {
            g_flight_at = now;
        }
        const bool after_flight = g_flight_at != Clock::time_point{} && std::chrono::duration<float>(now - g_flight_at).count() < kAfterFlightS;
        const bool seated       = world_camera != nullptr && world_camera->parent != nullptr && !after_flight && Seated();

        if (seated && g.phase == Phase::Off) {
            ResetSit();
            g.phase  = Phase::Settling;
            g.origin = origin;
        }
        // A recenter meanwhile (the origin moved): the lean measured from your head's place now.
        if (g.phase != Phase::Off && glm::length(origin - g.origin) > 0.001f) {
            g.origin = origin;
            g.ref    = head;
        }
        if (seated && g.phase == Phase::Settling) {
            g.ref = head; // (your head's place as you sat: the last of these frames)
            g.settle_s += dt;
            if (g.settle_s >= kSettleS) {
                g.phase = Phase::Measuring;
            }
        }
        if (seated && g.phase == Phase::Measuring && !GameFlow::isShowingMenu()) {
            Measure(world_camera, dt);
        }

        const bool apply = seated && g.phase == Phase::Applied;
        g.offset_w       = std::clamp(g.offset_w + (apply ? dt : -dt) / kEaseS, 0.0f, 1.0f);
        g.limit_w        = std::clamp(g.limit_w + (seated ? dt : -dt) / kEaseS, 0.0f, 1.0f);
        g_head_wanted.store(seated && (g.phase == Phase::Settling || g.phase == Phase::Measuring), std::memory_order_relaxed);
        g_face_hidden.store(g.offset_w > 0.0f, std::memory_order_relaxed);

        // The lean from your head's place as you sat, softly held; that place itself kept as it is.
        if (g.phase != Phase::Off) {
            glm::vec3   lean = head - g.ref;
            const float cut  = LimitLean(lean, g.limit_w);
            if (seated) {
                Period(head - g.ref, cut, dt);
            }
            head = g.ref + glm::vec3{ lean.x, head.y - g.ref.y, lean.z };
        }

        const glm::vec3 shift = ShiftNow();
        PublishShift(shift);
        glm::vec3 local{ 0.0f };
        if (shift != glm::vec3{ 0.0f } && world_camera != nullptr && world_camera->parent != nullptr) {
            // In the camera's parent's axes (world = R * local).
            const auto& parent = world_camera->parent->world;
            const float scale  = parent.scale > 1e-4f ? parent.scale : 1.0f;
            local              = glm::transpose(NiMath::to_glm(parent.rotate)) * shift / scale;
            if (!Finite(local)) {
                local = glm::vec3{ 0.0f };
            }
        }
        EndIfDone();
        return local;
    }

    glm::vec3 EaseOut()
    {
        if (g.phase == Phase::Off && !(g.offset_w > 0.0f) && !(g.limit_w > 0.0f)) {
            g_last = Clock::time_point{};
            return glm::vec3{ 0.0f };
        }
        const float dt = StepTime();
        g.offset_w     = std::clamp(g.offset_w - dt / kEaseS, 0.0f, 1.0f);
        g.limit_w      = std::clamp(g.limit_w - dt / kEaseS, 0.0f, 1.0f);
        g_head_wanted.store(false, std::memory_order_relaxed);
        g_face_hidden.store(g.offset_w > 0.0f, std::memory_order_relaxed);
        const glm::vec3 shift = ShiftNow();
        PublishShift(shift);
        EndIfDone();
        return shift;
    }

    void Off()
    {
        g_last = Clock::time_point{};
        if (g.phase == Phase::Off && !(g.offset_w > 0.0f) && !(g.limit_w > 0.0f)) {
            return;
        }
        g = State{};
        g_head_wanted.store(false, std::memory_order_relaxed);
        g_face_hidden.store(false, std::memory_order_relaxed);
        g_head_scale.store(false, std::memory_order_relaxed);
        PublishShift(glm::vec3{ 0.0f });
    }
}
