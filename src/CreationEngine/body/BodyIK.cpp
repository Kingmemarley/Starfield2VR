#include "BodyIK.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/N/NiCamera.h>
#include <RE/P/PlayerCamera.h>
#include <safetyhook/easy.hpp>
#include <mods/VR.hpp>

#include <CreationEngine/CreationEngineCameraManager.h>
#include <CreationEngine/CreationEngineSingletonManager.h>
#include <CreationEngine/Climbing.h>
#include <CreationEngine/HandGrab.h>
#include <CreationEngine/LadderClimb.h>
#include <CreationEngine/LeftWeapon.h>
#include <CreationEngine/MeleeThrow.h>
#include <CreationEngine/Physical.h>
#include <CreationEngine/SeatView.h>
#include <ModSettings.h>
#include <CreationEngine/HandActions.h>
#include <CreationEngine/Haptics.h>
#include <CreationEngine/memory/GameCode.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <CreationEngine/vr/TrackingSpace.h>

#include "BodyMath.h"
#include "SafeScan.h"
#include "SceneGraph.h"

// Third-person body shown in first person, as the game does for furniture that uses the 3P rig in first
// person; arms are posed with IK on the skeleton's pose buffer.
//
// Ported from SFVR (AetherianArtificer's fork of starfield2vr, src/CreationEngine/body/BodyIK.cpp),
// which in turn ports FRIK's arm solver (Fallout 4 VR Body). Changes here: signatures must match
// exactly once (body::ScanUnique), the body is placed from the head centre rather than the eye being
// rendered (this build draws one eye per frame), and node reads that can fail are guarded.
namespace body
{
    namespace
    {
        constexpr std::size_t kLoaded3DOffset   = 0xB8;   // TESObjectREFR loaded data; 3P root at +0x8
        constexpr std::size_t kFirstPerson3D    = 0xDE8;  // PlayerCharacter 1P root
        constexpr std::size_t kFlagsOffset      = 0x118;  // NiAVObject flags; bit 0 = app culled
        constexpr std::size_t kWorldBoundOffset = 0x100;  // NiAVObject world bound: centre (3 floats), radius (CommonLibSF's layout)
        constexpr float       kThrownWeaponReach = 200.0f; // a thrown weapon's bone may be this far from the hand (metres)
        constexpr std::size_t kModelNodeOffset  = 0x180;  // BGSModelNode of a 3D root
        constexpr int         kSetAppCulledSlot = 61;

        struct State
        {
            std::atomic<bool> active{ false }; // (read from other threads: IsActive)
            RE::NiAVObject* root{ nullptr };
            int             frame{ -1 };
            // (fix189) The root's model node (root +0x180), read as the root updates (it is alive then): the model update hook compares
            // pointers with it and never reads the root itself, which may already be freed (quit to the main menu, a load) before the
            // body is told it has gone.
            std::atomic<std::uint8_t*> model{ nullptr };
        };
        State g_state;
        // (fix189) The next root's first update owes the put-back (the shown body hidden, the arms shown) that the off switch would have
        // done: the body was turned off while the player had no third-person root (nothing could be touched then).
        bool g_put_back_owed{ false };
        // (fix202) When the switch hook (OnPlayerSwitch, below) last put the bodies as the game has them in first person, for the game's own
        // turn to third person (steady clock, ms; 0 never). The shown body is not put back on another thread for a moment after it.
        std::atomic<int64_t> g_switch_fixed_ms{ 0 };
        constexpr int64_t    kSwitchHoldMs = 300;
        // (fix202) The player's +0x112A: bit 3 (value 8) is the game's "third person" mark, set by its turn to third person and by its own
        // body-in-a-seat view (0x141A41950).
        constexpr std::size_t kPlayerViewMark = 0x112A;
        std::atomic<RE::NiAVObject*> g_body_muzzle{ nullptr };
        // (fix189) The engine frame the muzzle was last found in (PrepareFrame, under the weapon in your hands), and whether it is a
        // different node from the one kept before. Other threads take it only if it was found this frame or the last: the pointer is not
        // kept alive by us, and with the body's update not running (a ragdoll, the game paused) a weapon unequipped meanwhile frees it; a
        // shot given a freed node (MuzzleFire takes a reference on it) could break the game's memory.
        std::atomic<uint32_t> g_body_muzzle_frame{ 0 };
        bool                  g_muzzle_new{ false };

        // (fix189) A line logged on a timer: the first 60 at its own pace, then one a minute (the body's lines every few seconds grew the log
        // by about 1.7 MB an hour).
        struct TimedLog
        {
            std::chrono::steady_clock::time_point at{};
            int                                   told{ 0 };

            bool Due(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration every)
            {
                const std::chrono::steady_clock::duration wait = told < 60 ? every : std::chrono::steady_clock::duration{ std::chrono::seconds(60) };
                if (told > 0 && now - at <= wait) {
                    return false;
                }
                at = now;
                ++told;
                return true;
            }
        };
        float g_spine_twist{ 0.0f };  // (fix157) the hands' turn the spine takes while moving (the hips then face the way you go)
        float g_heading_turn{ 0.0f }; // (fix158) the game's heading's turn this frame, a snap turn not counted
        bool  g_right_melee_fist{ false }; // (fix159) the right hand holds a one-handed melee weapon by its own model this frame: a full fist
        // Set when the model update hook is installed; the body is only shown with working IK.
        std::atomic<bool> g_hook_ready{ false };
        // Set when reading the body's pose data faulted (a different game layout); the body stays off.
        std::atomic<bool> g_ik_failed{ false };
        // How far the view (and the hands with it) is lowered below the game's camera while crouching, in metres
        // (world z, <= 0). The body's crouch animation lowers its head further than the game lowers its camera.
        std::atomic<float> g_view_offset{ 0.0f };

        RE::NiAVObject* ThirdPersonRoot(RE::PlayerCharacter* player)
        {
            auto loaded = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(player) + kLoaded3DOffset);
            return loaded ? *reinterpret_cast<RE::NiAVObject**>(loaded + 0x8) : nullptr;
        }

        RE::NiAVObject* FirstPersonRoot(RE::PlayerCharacter* player)
        {
            return *reinterpret_cast<RE::NiAVObject**>(reinterpret_cast<std::uint8_t*>(player) + kFirstPerson3D);
        }

        bool IsAppCulled(const RE::NiAVObject* node)
        {
            return (*reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(node) + kFlagsOffset) & 1) != 0;
        }

        void SetAppCulled(RE::NiAVObject* node, bool culled)
        {
            if (!node || IsAppCulled(node) == culled) {
                return;
            }
            using fn_t = void (*)(RE::NiAVObject*, bool);
            (*reinterpret_cast<fn_t**>(node))[kSetAppCulledSlot](node, culled);
        }

        // The player's face node (hidden with the head). Found when the hooks are installed.
        using FaceNodeFn = RE::NiAVObject* (*)(RE::PlayerCharacter*);
        FaceNodeFn g_face_node_fn{ nullptr };

        RE::NiAVObject* FaceNode(RE::PlayerCharacter* player)
        {
            return g_face_node_fn ? g_face_node_fn(player) : nullptr;
        }

        bool Wanted()
        {
            static auto vr = VR::get();
            return GameFlow::gStore.internalSettings.showBody && g_hook_ready.load() && !g_ik_failed.load() && vr->is_hmd_active() && !ModSettings::cameraFlat() &&
                   !ModSettings::g_debug.disableHeadTracking && vr->is_using_controllers() && GameFlow::isInFirstPerson() &&
                   !GameFlow::isImmovable() && !GameFlow::isControlledByAI() && !GameFlow::isOnLadder(); // (fix143: not on a ladder: its climb is a third-person animation)
        }

        // Where the body is placed from: the centre of the head (between the eyes). The eye being
        // rendered alternates every frame here, and placing the body from it would shift the body
        // sideways by half the eye distance from one eye's frame to the other's.
        glm::vec3 HeadPosition(const RE::NiCamera* world_camera)
        {
            glm::vec3 head{ 0.0f };
            if (CreationEngineCameraManager::head_center_world(head)) {
                return head;
            }
            return ToVec(world_camera->world.translate);
        }

        bool IsAncestorGuarded(RE::NiAVObject* node, const RE::NiAVObject* ancestor)
        {
            __try {
                for (int i = 0; i < 32 && node; ++i) {
                    if (node == ancestor) {
                        return true;
                    }
                    node = node->parent;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return false;
        }

        // The first-person root read at PlayerCharacter+0xDE8 (SFVR's offset for 1.16.244), checked before
        // anything is hidden: it must hold the first-person camera or the first-person right wrist.
        RE::NiAVObject* CheckedFirstPersonRoot(RE::PlayerCharacter* player)
        {
            static RE::NiAVObject* checked{ nullptr };
            static bool            valid{ false };
            auto                   first_person = FirstPersonRoot(player);
            if (!first_person) {
                return nullptr;
            }
            if (first_person != checked) {
                checked = first_person;
                RE::NiAVObject* camera{ nullptr };
                if (auto pc = CreationEngineSingletonManager::GetPlayerCameraSingleton(); pc && pc->pFirstPersonModeState) {
                    camera = pc->pFirstPersonModeState->cameraRoot;
                }
                const bool holds_camera = camera && IsAncestorGuarded(camera, first_person);
                const bool holds_wrist  = !holds_camera && FindDescendant(first_person, "R_Wrist") != nullptr;
                valid                   = holds_camera || holds_wrist;
                if (valid) {
                    spdlog::info("[Body] first-person root checked ({})", holds_camera ? "holds the first-person camera" : "holds the right wrist");
                } else {
                    spdlog::error("[Body] PlayerCharacter+0xDE8 is not the first-person root on this game version; the body stays off");
                }
            }
            return valid ? first_person : nullptr;
        }

        int64_t SteadyMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (fix202) The game's "third person" mark (read under a guard; false if it cannot be read).
        bool GameThirdPersonMark(RE::PlayerCharacter* player)
        {
            __try {
                return player != nullptr && (*(reinterpret_cast<const uint8_t*>(player) + kPlayerViewMark) & 8) != 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Called when the 3P root updates, before its skeleton.
        void UpdateVisibility(RE::PlayerCharacter* player, RE::NiAVObject* root)
        {
            auto first_person = CheckedFirstPersonRoot(player);
            if (Wanted() && first_person) {
                if (g_state.root != root) {
                    g_state.root = root;
                    spdlog::info("[Body] 3P root '{}'", root->name.c_str());
                }
                // (fix189) Its model node read now, while the root updates (each time: the hook compares with this one).
                g_state.model    = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(root) + kModelNodeOffset);
                g_put_back_owed  = false;
                // (fix202) Just after the switch hook put the bodies as in first person for the game's turn to third person, they are left
                // as they are a moment: showing the body again on this thread before the game has read them would make it take the player
                // for already in third person again (no swap of the bodies and animations). Read right before the culls.
                if (SteadyMs() - g_switch_fixed_ms.load() >= kSwitchHoldMs) {
                    SetAppCulled(root, false);
                    SetAppCulled(first_person, true);
                    SetAppCulled(FaceNode(player), true);
                }
                g_state.active = true;
            } else if (g_state.active || g_put_back_owed) { // (fix189: or owed by a body turned off with no root)
                g_put_back_owed = false;
                g_body_muzzle = nullptr;
                g_view_offset = 0.0f;
                // (fix202) Not with the game's third-person mark set: the game has its own bodies then (third person, a seat that turns
                // the player to third person with the camera still in first person, or its own body-in-a-seat view), and hiding its
                // body and showing the arms would leave it drawing the arms on a third-person player.
                const bool game_third = GameThirdPersonMark(player);
                if (GameFlow::isInFirstPerson() && !game_third) {
                    SetAppCulled(root, true);
                    SetAppCulled(first_person ? first_person : FirstPersonRoot(player), false);
                }
                static std::atomic<int> kept_told{ 0 };
                if (game_third && GameFlow::isInFirstPerson() && kept_told.fetch_add(1) < 20) {
                    spdlog::info("[Body] (fix202) the body off with the game's third-person mark set (camera in first person): the game's own "
                                 "bodies left as it has them");
                }
                SetAppCulled(FaceNode(player), false);
                g_state.active = false;
                g_state.root   = nullptr;
                g_state.model  = nullptr;
            }
            // (fix145) On a ladder the view is at the body's eyes (LadderClimb): its face hidden meanwhile.
            static bool ladder_face = false;
            if (LadderClimb::ViewAtHead()) {
                SetAppCulled(FaceNode(player), true);
                ladder_face = true;
            } else if (ladder_face) {
                ladder_face = false;
                if (!g_state.active) {
                    SetAppCulled(FaceNode(player), false);
                }
            }
            // (fix199) Seated with the view at the seated body's eyes (SeatView): its face hidden meanwhile, as on a ladder.
            static bool seat_face = false;
            if (SeatView::FaceHidden()) {
                SetAppCulled(FaceNode(player), true);
                seat_face = true;
            } else if (seat_face) {
                seat_face = false;
                if (!g_state.active && !LadderClimb::ViewAtHead()) {
                    SetAppCulled(FaceNode(player), false);
                }
            }
        }

        // A tracked hand in game world space.
        struct HandTarget
        {
            glm::vec3 position;
            glm::vec3 forward;
            glm::vec3 up;
        };

        // (fix169) The left hand's weapon's copy's root place this frame (NoteLeftWeaponRoot, the main thread): the left hand is put from it
        // while it holds that weapon. After fix168 the left saber lagged behind the hand and the hand was not closed on it (a helper's
        // research): the copy is placed at the frame's start, before your character moves, with the root moved on by a guess of the frame's
        // move (tracking::FrameStartLead); the drawn hand later, with the root moved on by your character's real move (CameraRootPlace). The
        // controller's sample, the room's turn, the grip frame and the grip's math are the same in both (the right saber's FixedMeleeHold is
        // line for line Target's), so only the root differed: at a run, 5-6 cm at each change of the frame's length and up to a frame's
        // move (15-35 cm), the full fist closing on air beside the hilt. Now the hand takes the copy's root: hand and hilt from the same
        // place, the fist on the hilt as on the right, swinging or running (the guess itself made better: FrameStartLead).
        std::mutex                            g_left_copy_mtx;
        glm::vec3                             g_left_copy_root{ 0.0f };
        uint32_t                              g_left_copy_frame{ 0 };
        bool                                  g_left_copy_valid{ false };
        double                                g_left_shift_sum{ 0.0 };
        float                                 g_left_shift_max{ 0.0f };
        int                                   g_left_shift_n{ 0 }, g_left_shift_missed{ 0 }, g_left_shift_logs{ 0 };
        std::chrono::steady_clock::time_point g_left_shift_at{};

        // How far the left hand is moved to sit on the copy's hilt this frame (the copy's root place less the body's). False: the copy not
        // placed this frame (the hand at the controller then, as before).
        // (`place`: the root's place the hand was just put from, the view's lowering in (HandWorld): the same numbers, not worked out again; the
        // review. More than 0.25 m apart (a jump, a frame far off): the hand left at the controller.)
        bool LeftCopyShift(const glm::vec3& place, glm::vec3& out)
        {
            static auto    vr    = VR::get();
            const uint32_t frame = vr->m_engine_frame_count.load();
            const auto     now   = std::chrono::steady_clock::now();
            std::scoped_lock _{ g_left_copy_mtx };
            bool ok = false;
            if (g_left_copy_valid && g_left_copy_frame == frame) {
                const glm::vec3 shift = g_left_copy_root - place;
                const float     moved = glm::length(shift);
                if (std::isfinite(moved) && moved < 0.25f) {
                    out = shift;
                    ok  = true;
                    g_left_shift_sum += moved;
                    g_left_shift_max = std::max(g_left_shift_max, moved);
                    ++g_left_shift_n;
                }
            }
            if (!ok) {
                ++g_left_shift_missed;
            }
            if (now - g_left_shift_at > std::chrono::seconds(10)) {
                if (g_left_shift_n + g_left_shift_missed > 0 && g_left_shift_logs < 60 && g_left_shift_at != std::chrono::steady_clock::time_point{}) {
                    ++g_left_shift_logs;
                    spdlog::info("[BodyIK] (fix169) the left hand put on its weapon's hilt (from the copy's place at the frame's start): moved {:.3f} m on "
                                 "average (at most {:.3f}) in {} frames; the copy not placed in that frame {} times (the hand at the controller then)",
                        g_left_shift_n > 0 ? g_left_shift_sum / g_left_shift_n : 0.0, g_left_shift_max, g_left_shift_n, g_left_shift_missed);
                }
                g_left_shift_sum    = 0.0;
                g_left_shift_max    = 0.0f;
                g_left_shift_n      = 0;
                g_left_shift_missed = 0;
                g_left_shift_at     = now;
            }
            return ok;
        }

        // (fix169: `place_out`, the camera root's place it was put from, the view's lowering in: LeftCopyShift compares the copy's with that one.)
        // (fix195) `carry_turn`: the room's turn as it will be this frame (tracking::RoomRotationNow, no setting): the camera hook sets it
        // after the body is placed, so while you turned with the stick the gun was placed with the frame before's turn, a frame behind the
        // view. Only for the gun and the hands on it (the second review): held things, the left hand's weapon's copy, grenades and grips
        // on people and walls are placed with the frame before's room, and a hand carried on alone would separate from what it holds.
        std::optional<HandTarget> HandWorld(const glm::mat4& pose, glm::vec3* place_out = nullptr, bool carry_turn = false)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            const auto room   = carry_turn ? tracking::RoomRotationNow() : tracking::RoomRotation();
            if (!world_camera || !world_camera->parent || !room) {
                return std::nullopt;
            }
            const auto& anchor = world_camera->parent->world;
            const auto  offset = *room * tracking::ToHavokVector((glm::vec3{ pose[3] } - tracking::StandingOriginPosition()) * tracking::TrackingScale()) * anchor.scale;
            const auto  rot    = glm::mat3{ pose };
            const auto  place  = tracking::CameraRootPlace(world_camera->parent); // (fix147: this frame's)
            const auto  from   = glm::vec3{ place.x, place.y, place.z + g_view_offset.load() };
            if (place_out != nullptr) {
                *place_out = from;
            }
            return HandTarget{ from + offset,
                glm::normalize(*room * tracking::ToHavokVector(rot * glm::vec3{ 0.0f, 0.0f, -1.0f })),
                glm::normalize(*room * tracking::ToHavokVector(rot * glm::vec3{ 0.0f, 1.0f, 0.0f })) };
        }

        // (fix170) The second hand on a held object (both hands hold it: HandGrab's shared pose): drawn with its palm on its grip on the
        // object (moved by how far that grip is from where the controller puts the palm), as the left hand is put on its weapon's hilt.
        // More than 0.3 m apart: left at the controller (HandGrab lets the second hand go at 0.25 m).
        void SupportShift(bool left, std::optional<HandTarget>& grip, std::optional<HandTarget>& aim)
        {
            glm::vec3 at{}, surface{}, facing{}, forward{}, side{};
            if (!grip || !aim || !HandGrab::SupportGrip(left, at) || !HandGrab::HoldFrame(left, surface, facing, forward, side)) {
                return;
            }
            const glm::vec3 shift = at - surface;
            if (!(glm::length(shift) < 0.3f)) {
                return;
            }
            grip->position += shift;
            aim->position += shift;
        }

        // (fix179) How your drawn hand sat on the person it holds (each hand), for the log. (fix180) Frames drawn wholly locked and faded
        // toward your controller, your controller's palm from the locked one, the drawn hand's deepest point in them, the fingers touching
        // them and starting inside them (wall grips the same, by their own line).
        struct PersonHandStats
        {
            int                                   grab{ 0 };
            int                                   frames{ 0 }, locked_frames{ 0 }, faded_frames{ 0 }, deep_frames{ 0 }, bases_frames{ 0 };
            int                                   grip_frames{ 0 }, touch_sum{ 0 };
            float                                 apart_sum{ 0.0f }, apart_max{ 0.0f }, deep_max{ -1.0f };
            float                                 gap_sum{ 0.0f }, gap_max{ 0.0f }; // (the review) the arm short of the drawn hand (m)
            // (fix181) Which bone pose the hand was drawn from (0 Physical's, 1 their update this frame, 2 last frame's moved on, 3 last
            // frame's); where the drawn palm landed (after the arm and fingers were posed) from the locked palm, and from their bare body's
            // surface (positive: out of it), in the frames drawn wholly locked.
            int                                   source[4]{};
            int                                   landed_n{ 0 };
            float                                 landed_sum{ 0.0f }, landed_max{ 0.0f };
            float                                 bare_sum{ 0.0f }, bare_min{ 1e9f }, bare_max{ -1e9f };
            std::chrono::steady_clock::time_point log_at{};
        };
        // (fix180, the review) How far that arm's wrist stopped short of where the hand was drawn, the last time it was posed (SolveArm).
        float ArmGap(int hand);
        PersonHandStats  g_person_hand[2], g_wall_hand[2];
        std::atomic<int> g_person_logs{ 0 }, g_wall_logs{ 0 };
        int              g_grip_bases_inside[2]{}; // (fix179) finger bases inside the shape the hand closed on (ApplyGripOn, this frame)

        glm::vec3 ClosestOnLine(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
        {
            const glm::vec3 ab = b - a;
            const float     k  = glm::dot(ab, ab) > 1e-8f ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            return a + ab * k;
        }

        // (fix179) How deep a point is in the deepest of the held person's surfaces (negative: outside by that much), and the way out there.
        // (The review) `caps`: the first so many (1: the held part only).
        float DeepestIn(const Physical::HeldPersonGrip& held, const glm::vec3& p, glm::vec3& way, int caps = 4)
        {
            float deepest = -1.0f;
            for (int c = 0; c < held.caps && c < 4 && c < caps; ++c) {
                const glm::vec3 q    = ClosestOnLine(p, held.cap_a[c], held.cap_b[c]);
                const float     d    = glm::length(p - q);
                const float     deep = held.cap_r[c] - d;
                if (deep > deepest) {
                    deepest = deep;
                    way     = d > 1e-4f ? (p - q) / d : held.normal;
                }
            }
            return deepest;
        }

        // (fix179) The turn taking your palm's way `from` onto `to` (into the part held): the short way; past 110 degrees (the back of your
        // hand toward them) more and more by a roll about your fingers' own line instead (your hand turned over, as a hand turns to take
        // hold), then the short rest: the short way's own axis is ill-defined there and wobbled from frame to frame (why fix177 did not
        // turn the hand at all past 120 degrees). A roll of +180 or -180 degrees is the same turn: no jump between them.
        glm::quat PalmTurn(const glm::vec3& from, const glm::vec3& fingers, const glm::vec3& to)
        {
            const glm::quat none{ 1.0f, 0.0f, 0.0f, 0.0f };
            const auto      shortest = [&](const glm::vec3& x, const glm::vec3& y) {
                const float     angle = std::acos(std::clamp(glm::dot(x, y), -1.0f, 1.0f));
                const glm::vec3 about = glm::cross(x, y);
                return glm::length(about) > 1e-5f && angle > 1e-4f ? glm::angleAxis(angle, glm::normalize(about)) : none;
            };
            const float     angle     = std::acos(std::clamp(glm::dot(from, to), -1.0f, 1.0f));
            const bool      direct_ok = glm::length(glm::cross(from, to)) > 1e-3f;
            const glm::quat direct    = shortest(from, to);
            if (angle <= glm::radians(110.0f)) {
                return direct;
            }
            glm::vec3 f = from - fingers * glm::dot(from, fingers);
            glm::vec3 t = to - fingers * glm::dot(to, fingers);
            if (glm::length(f) < 1e-3f || glm::length(t) < 1e-3f) {
                return direct;
            }
            f                      = glm::normalize(f);
            t                      = glm::normalize(t);
            const float     roll   = std::atan2(glm::dot(glm::cross(f, t), fingers), glm::dot(f, t));
            const glm::quat first  = glm::angleAxis(roll, fingers);
            const glm::quat rolled = shortest(glm::normalize(first * from), to) * first;
            if (!direct_ok) {
                return rolled;
            }
            return glm::slerp(direct, rolled, std::clamp((angle - glm::radians(110.0f)) / glm::radians(20.0f), 0.0f, 1.0f));
        }

        // (fix180) Your palm in the shown body's own terms: the grip frame measured from the hand's own bones (MeasureGripFrame), placed as
        // WristTarget places the hand from these grip and aim targets: its point, the way out of the palm, the fingers' way and the line
        // across the knuckles (game world). False: not measured yet.
        bool PalmOf(bool left, const HandTarget& grip, const HandTarget& aim, glm::vec3& contact, glm::vec3& facing, glm::vec3& fingers, glm::vec3& across);

        // (fix180) A hand drawn locked to the spot it took hold of (after fix179 hands on people were better but still not right;
        // chosen: "Lock to the grabbed spot", as HIGGS draws a hand holding a body where the body is, m_adjustedHandTransform): placed once,
        // as the hold begins, with its palm on the surface (turned into it, the back of the hand turned over too), seated out so that none of
        // it is inside, and kept in the held bone's own frame (a wall: in the world); drawn there each frame, moving with them, not with your
        // controller (which still drags and bends them). Eased in from your controller's hand with the hold, and back to it as you let go;
        // farther than kLockFull from your controller's hand it fades back to it, all of it at kLockNone (a wall: kWallFull, kWallNone; the
        // arm reaching for it, stretched past its reach if need be).
        struct HandLock
        {
            int       key{ 0 };      // the hold's number (a person), or the wall grip's
            bool      ok{ false };
            glm::vec3 point{ 0.0f }; // (a wall) where it took hold
            // The locked grip and aim targets in the frame they are kept in.
            glm::vec3 grip_p{ 0.0f }, grip_f{ 0.0f, 1.0f, 0.0f }, grip_u{ 0.0f, 0.0f, 1.0f };
            glm::vec3 aim_p{ 0.0f }, aim_f{ 0.0f, 1.0f, 0.0f }, aim_u{ 0.0f, 0.0f, 1.0f };
            glm::quat last{ 1.0f, 0.0f, 0.0f, 0.0f }; // last frame's turn from your controller's hand to it (kept on its side)
            float     w{ 0.0f };                       // how much of it was drawn last frame
            float     seat{ 0.0f }, before{ 0.0f }, turn_deg{ 0.0f }; // as it was placed, for the log
            float     slide{ 0.0f };                                   // (a wall) its palm from the pick's hit point
            int       made{ 0 };
            // (The review) The hand as drawn last frame (game world), and a new lock's start from it (a wall grip taken again while the last
            // was still drawn): eased over 0.15 s from there.
            glm::vec3 drawn_grip_p{ 0.0f }, drawn_grip_f{ 0.0f, 1.0f, 0.0f }, drawn_grip_u{ 0.0f, 0.0f, 1.0f };
            glm::vec3 drawn_aim_p{ 0.0f }, drawn_aim_f{ 0.0f, 1.0f, 0.0f }, drawn_aim_u{ 0.0f, 0.0f, 1.0f };
            bool      handover{ false };
            glm::vec3 from_grip_p{ 0.0f }, from_grip_f{ 0.0f, 1.0f, 0.0f }, from_grip_u{ 0.0f, 0.0f, 1.0f };
            glm::vec3 from_aim_p{ 0.0f }, from_aim_f{ 0.0f, 1.0f, 0.0f }, from_aim_u{ 0.0f, 0.0f, 1.0f };
            std::chrono::steady_clock::time_point handover_at{};
        };
        HandLock         g_person_lock[2], g_wall_lock[2];
        // (fix181) Where each hand's locked palm is drawn this frame (game world), and the held person as this frame has them (Physical's
        // numbers moved onto their held bone as their own update drew it): for the fingers and for the log of where the hand landed.
        glm::vec3                g_lock_palm_now[2]{};
        bool                     g_lock_palm_ok[2]{};
        Physical::HeldPersonGrip g_person_held_now[2]{};
        int                      g_person_held_grab[2]{};

        // (fix181) The held bone as their own model update drew it (game world), each engine frame (NpcBendInner), the last two kept: the
        // hand locked on them is drawn from this frame's when their update came first, else from the last moved on by its last step
        // (Physical's numbers are from the frame's start: a frame late when they move; after fix180 the hand was not quite on them).
        struct NpcBoneSeen
        {
            uintptr_t model{ 0 };
            char      bone[24]{}; // (the review) the held bone's name, as their bend has it
            uint32_t  frame{ 0 }, prev_frame{ 0 };
            Xf        now{}, prev{};
            bool      ok{ false }, has_prev{ false };
        };
        std::mutex  g_npc_bone_mutex;
        NpcBoneSeen g_npc_bone;
        // (The review: 0.40 and 0.55 m put the hand past the arm's reach; the arm stopped short.)
        constexpr float  kLockFull = 0.25f; // your controller's palm this far from the locked one: drawn locked...
        constexpr float  kLockNone = 0.40f; // ...this far: at your controller (between: a share of each)
        constexpr float  kWallFull = 0.15f; // ...on a wall (your body follows your hand there: it stays near)
        constexpr float  kWallNone = 0.30f;
        constexpr float  kWallSlide = 0.10f; // a wall's palm: where your controller's palm meets the wall, at most this far from the hit point
        int              g_grip_touching[2]{}; // (fix180) fingers that closed until they touched the shape (ApplyGripOn, this frame)

        // (fix180) The hand's points kept outside: along the fingers, across the knuckles, out of the palm (m, from the palm's point): the
        // palm's corners and middle, the finger bases (the knuckles sit about a centimetre inside the hand, behind the palm's skin) and the
        // back of the hand.
        constexpr float kLockPoints[][3] = { { 0.010f, -0.040f, 0.0f }, { 0.010f, 0.0f, 0.0f }, { 0.010f, 0.040f, 0.0f }, { -0.040f, -0.040f, 0.0f },
            { -0.040f, 0.0f, 0.0f }, { -0.040f, 0.040f, 0.0f }, { -0.085f, -0.035f, 0.0f }, { -0.085f, 0.0f, 0.0f }, { -0.085f, 0.035f, 0.0f },
            { 0.020f, -0.036f, -0.010f }, { 0.022f, -0.012f, -0.010f }, { 0.022f, 0.012f, -0.010f }, { 0.020f, 0.036f, -0.010f },
            { 0.0f, 0.0f, -0.030f }, { -0.060f, 0.0f, -0.030f } };

        // (fix180) The hand placed on a surface at `at` (n: the way out of it): the palm's point put there, the hand turned about it so that
        // the palm faces in (PalmTurn), then moved out along n until no point of it is inside (`deepest(point, way, back of the hand)`: how
        // deep it is (negative: out) and its own way out; a point whose way out is against n is not pushed by it). `grip_out`/`aim_out`:
        // the targets that draw it so (game world); `seat` how far it was moved out; `before` how deep it was before that.
        template <class Deepest>
        bool PlaceOnSurface(bool left, const HandTarget& grip, const HandTarget& aim, const glm::vec3& at, const glm::vec3& n, Deepest&& deepest,
            HandTarget& grip_out, HandTarget& aim_out, float& seat, float& before, float& turn_deg)
        {
            glm::vec3 c{}, facing{}, fingers{}, across{};
            if (!PalmOf(left, grip, aim, c, facing, fingers, across) || !(glm::length(n) > 0.5f)) {
                return false;
            }
            const glm::vec3 into = -glm::normalize(n);
            const glm::quat turn = PalmTurn(facing, fingers, into);
            const glm::mat3 m    = glm::mat3_cast(turn);
            turn_deg             = glm::degrees(std::acos(std::clamp(glm::dot(facing, into), -1.0f, 1.0f)));
            const glm::vec3 f1 = m * facing, d1 = m * fingers, k1 = m * across, out = glm::normalize(n);
            const auto      point = [&](const float* h, float push) { return at + d1 * h[0] + k1 * h[1] + f1 * h[2] + out * push; };
            seat   = 0.0f;
            before = -1.0f;
            // (The review) At most 8 cm out (placed once and drawn locked, a hand pushed by a near part touched at a glancing angle floated
            // 10-30 cm off them the whole hold): out of every surface first, up to 6 cm; past that only out of the held part (the parts near
            // it overlap it: the jaw over the neck, the chest's round capsule over the shoulder). A point is pushed only when its own way out
            // is within about 70 degrees of the held point's.
            bool held_only = false;
            for (int it = 0; it < 12; ++it) {
                float     worst = -1.0f, worst_any = -1.0f;
                glm::vec3 worst_way{ out };
                for (const auto& h : kLockPoints) {
                    glm::vec3   way{ out };
                    const float deep = deepest(point(h, seat), way, held_only || h[2] < -0.02f);
                    worst_any        = std::max(worst_any, deep);
                    if (deep > worst && glm::dot(out, way) >= 0.35f) {
                        worst     = deep;
                        worst_way = way;
                    }
                }
                if (it == 0) {
                    before = worst_any;
                }
                if (worst <= 0.001f) {
                    break;
                }
                const float next = seat + worst / std::max(0.35f, glm::dot(out, worst_way)) + 0.002f;
                if (!held_only && next > 0.06f) {
                    held_only = true; // (this pass again, against the held part only)
                    continue;
                }
                seat = std::min(0.08f, next);
                if (seat >= 0.08f) {
                    break;
                }
            }
            const auto move = [&](const HandTarget& from, HandTarget& to) {
                to.position = at + m * (from.position - c) + out * seat;
                to.forward  = m * from.forward;
                to.up       = m * from.up;
            };
            move(grip, grip_out);
            move(aim, aim_out);
            return std::isfinite(seat) && std::isfinite(grip_out.position.x) && std::isfinite(grip_out.position.y) && std::isfinite(grip_out.position.z);
        }

        // (fix180) The controller's targets drawn `w` of the way to the locked ones: placed at a share of the way, turned a share of the turn
        // between the two hands (kept on last frame's side: near 180 degrees the two ways round are the same turn, not the same share of it).
        void BlendToLock(HandTarget& grip, HandTarget& aim, const HandTarget& lock_grip, const HandTarget& lock_aim, float w, glm::quat& last)
        {
            const glm::quat from = glm::quat_cast(FrameOf(aim.forward, aim.up));
            const glm::quat to   = glm::quat_cast(FrameOf(lock_aim.forward, lock_aim.up));
            glm::quat       turn = glm::normalize(to * glm::conjugate(from));
            if (glm::dot(turn, last) < 0.0f) {
                turn = -turn;
            }
            // (The review) Not kept past the far side for good (walking round a held person, or them turning, took it on past 360 degrees:
            // the share of a turn near a whole turn divides by almost nothing): drawn wholly locked, or past about 330 degrees, the short way.
            if (turn.w < 0.0f && (w >= 0.999f || turn.w < -0.9f)) {
                turn = -turn;
            }
            last           = turn;
            glm::quat part = glm::normalize(glm::mix(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, w));
            if (!std::isfinite(part.w) || !std::isfinite(part.x) || !std::isfinite(part.y) || !std::isfinite(part.z)) {
                part = glm::normalize(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, w));
            }
            if (!std::isfinite(part.w) || !std::isfinite(part.x) || !std::isfinite(part.y) || !std::isfinite(part.z)) {
                return; // (left at your controller this frame)
            }
            grip.position        = grip.position + (lock_grip.position - grip.position) * w;
            aim.position         = aim.position + (lock_aim.position - aim.position) * w;
            grip.forward         = part * grip.forward;
            grip.up              = part * grip.up;
            aim.forward          = part * aim.forward;
            aim.up               = part * aim.up;
        }

        // (fix180) How much of a locked hand to draw: its own ease, and less when your controller's palm is far from it.
        float LockShare(bool left, const HandTarget& grip, const HandTarget& aim, const HandTarget& lock_grip, const HandTarget& lock_aim, float ease, float& apart,
            float full = kLockFull, float none = kLockNone)
        {
            glm::vec3 c0{}, c1{}, f{}, d{}, k{};
            apart = 0.0f;
            if (!PalmOf(left, grip, aim, c0, f, d, k) || !PalmOf(left, lock_grip, lock_aim, c1, f, d, k)) {
                return 0.0f;
            }
            apart = glm::length(c1 - c0);
            return std::clamp(ease, 0.0f, 1.0f) * std::clamp((none - apart) / (none - full), 0.0f, 1.0f);
        }

        // (fix180, the review) The drawn hand moved on from where it was last drawn to these targets over a share `h` (a new lock taken while
        // the last was still drawn: no one-frame jump between them).
        void BlendFrom(HandTarget& grip, HandTarget& aim, const HandTarget& from_grip, const HandTarget& from_aim, float h)
        {
            HandTarget g = from_grip, a = from_aim;
            glm::quat  last{ 1.0f, 0.0f, 0.0f, 0.0f };
            BlendToLock(g, a, grip, aim, h, last);
            grip = g;
            aim  = a;
        }

        // (fix174) A hand holding a standing person (Physical's hold): drawn with its palm on where it holds them (their held point as their
        // bones were last drawn, after their own bend toward your hand), as the second hand on a held object. After fix173 the hand
        // did not look like it was grabbing them (not touching). (fix180) Locked to the grabbed spot (HandLock).
        void HoldPersonShift(bool left, std::optional<HandTarget>& grip, std::optional<HandTarget>& aim)
        {
            const int hand = left ? 0 : 1;
            Physical::HeldPersonGrip held{};
            if (!grip || !aim || !Physical::HeldPersonContact(left, held)) {
                g_person_lock[hand].w = 0.0f;
                return;
            }
            HandLock& lock = g_person_lock[hand];
            // (fix181) The held bone as their own update drew it this frame (or, when their update comes after yours, last frame's moved on
            // by its last step), and everything Physical gave (from the frame's start, a frame late when they move) moved onto it with it.
            {
                static auto    vr     = VR::get();
                const uint32_t frame  = vr->m_engine_frame_count.load();
                const Xf       phys{ held.bone_r, held.bone_t, held.bone_s > 0.01f ? held.bone_s : 1.0f };
                Xf             now    = phys;
                int            source = 0;
                {
                    std::scoped_lock _{ g_npc_bone_mutex };
                    // (The review: the same bone, not only the same person.)
                    if (g_npc_bone.ok && held.model != 0 && g_npc_bone.model == held.model && held.bone[0] != 0 &&
                        std::strncmp(g_npc_bone.bone, held.bone, sizeof(held.bone)) == 0) {
                        if (g_npc_bone.frame == frame) {
                            now    = g_npc_bone.now;
                            source = 1;
                        } else if (g_npc_bone.frame + 1 == frame) {
                            now    = g_npc_bone.now;
                            source = 3;
                            if (g_npc_bone.has_prev && g_npc_bone.prev_frame + 1 == g_npc_bone.frame) {
                                const glm::vec3 step = g_npc_bone.now.t - g_npc_bone.prev.t;
                                const glm::mat3 turn = g_npc_bone.now.r * glm::transpose(g_npc_bone.prev.r);
                                const float     cos_turn = std::clamp(0.5f * (turn[0][0] + turn[1][1] + turn[2][2] - 1.0f), -1.0f, 1.0f);
                                // (The review: half the last step, at most 2 cm, and only a small turn: a full step overshot as they
                                // stopped, turned or met something.)
                                const float step_len = glm::length(step);
                                if (step_len < 0.10f && cos_turn > std::cos(glm::radians(20.0f))) {
                                    const glm::vec3 half = step * (std::min(0.5f * step_len, 0.02f) / std::max(step_len, 1e-6f));
                                    now.t                = g_npc_bone.now.t + half;
                                    now.r = glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::quat_cast(turn), 0.5f)) * g_npc_bone.now.r;
                                    source = 2;
                                }
                            }
                        }
                    }
                }
                if (source != 0) {
                    const glm::mat3 back  = glm::transpose(phys.r);
                    const float     scale = now.s / phys.s;
                    const auto point = [&](const glm::vec3& p) { return now.t + now.r * (back * (p - phys.t) * scale); };
                    const auto way   = [&](const glm::vec3& v) { return now.r * (back * v); };
                    held.at          = point(held.at);
                    held.centre      = point(held.centre);
                    held.axis        = way(held.axis);
                    held.normal      = way(held.normal);
                    for (int c = 0; c < held.caps && c < 4; ++c) {
                        held.cap_a[c] = point(held.cap_a[c]);
                        held.cap_b[c] = point(held.cap_b[c]);
                    }
                    held.bone_t = now.t;
                    held.bone_r = now.r;
                    held.bone_s = now.s;
                }
                auto& st = g_person_hand[hand];
                if (st.grab == held.grab) {
                    ++st.source[std::clamp(source, 0, 3)];
                }
            }
            g_person_held_now[hand]  = held;
            g_person_held_grab[hand] = held.grab;
            const glm::mat3 to_bone = glm::transpose(held.bone_r);
            const float     s       = held.bone_s > 0.01f ? held.bone_s : 1.0f;
            // Placed once, as the hold begins (its own number): on their outer surface at the held point, kept in the held bone's frame
            // (tried again on the next frames, 30 at most, if your hand's grip frame was not there yet).
            if (lock.key != held.grab) {
                lock     = HandLock{};
                lock.key = held.grab;
            }
            if (!lock.ok && lock.made < 30) {
                ++lock.made;
                HandTarget lg{}, la{};
                float      seat = 0.0f, before = 0.0f, turn_deg = 0.0f;
                const auto deepest = [&](const glm::vec3& p, glm::vec3& way, bool back) { return DeepestIn(held, p, way, back ? 1 : 4); };
                if (glm::length(held.normal) > 0.5f && PlaceOnSurface(left, *grip, *aim, held.at, held.normal, deepest, lg, la, seat, before, turn_deg)) {
                    lock.ok       = true;
                    lock.grip_p   = to_bone * (lg.position - held.bone_t) / s;
                    lock.grip_f   = to_bone * lg.forward;
                    lock.grip_u   = to_bone * lg.up;
                    lock.aim_p    = to_bone * (la.position - held.bone_t) / s;
                    lock.aim_f    = to_bone * la.forward;
                    lock.aim_u    = to_bone * la.up;
                    lock.seat     = seat;
                    lock.before   = before;
                    lock.turn_deg = turn_deg;
                    static std::atomic<int> told{ 0 };
                    if (told.fetch_add(1) < 60) {
                        spdlog::info("[BodyIK] (fix180) your {} hand locked on the person it holds: its palm on their surface at the held point, turned {:.0f} "
                                     "degrees to face it, seated out {:.3f} m (it was {:.3f} m in them); drawn there from now on, eased in",
                            left ? "left" : "right", turn_deg, seat, std::max(0.0f, before));
                    }
                }
            }
            if (!lock.ok) {
                lock.w = 0.0f;
                return;
            }
            // Where the locked hand is now: the held bone as last drawn.
            HandTarget lg{ held.bone_t + held.bone_r * (lock.grip_p * s), held.bone_r * lock.grip_f, held.bone_r * lock.grip_u };
            HandTarget la{ held.bone_t + held.bone_r * (lock.aim_p * s), held.bone_r * lock.aim_f, held.bone_r * lock.aim_u };
            {
                glm::vec3 lf{}, ld{}, lk{};
                g_lock_palm_ok[hand] = PalmOf(left, lg, la, g_lock_palm_now[hand], lf, ld, lk); // (fix181) for where the hand landed
            }
            float       apart = 0.0f;
            const float w     = LockShare(left, *grip, *aim, lg, la, held.weight, apart);
            if (w > 0.0f) {
                BlendToLock(*grip, *aim, lg, la, w, lock.last);
            }
            lock.w = w;
            // (fix180) The log: each hold's numbers, a line each second while it lasts.
            float     drawn_deep = -1.0f;
            glm::vec3 c{}, f{}, d{}, k{};
            if (PalmOf(left, *grip, *aim, c, f, d, k)) {
                for (const auto& h : kLockPoints) {
                    glm::vec3 way{};
                    drawn_deep = std::max(drawn_deep, DeepestIn(held, c + d * h[0] + k * h[1] + f * h[2], way, h[2] < -0.02f ? 1 : 4));
                }
            }
            auto&      st  = g_person_hand[hand];
            const auto now = std::chrono::steady_clock::now();
            if (st.grab != held.grab) {
                st        = PersonHandStats{};
                st.grab   = held.grab;
                st.log_at = now;
            }
            ++st.frames;
            st.locked_frames += w >= 0.99f ? 1 : 0;
            st.faded_frames += held.weight >= 0.99f && w < 0.99f ? 1 : 0;
            st.apart_sum += apart;
            st.apart_max = std::max(st.apart_max, apart);
            st.deep_max  = std::max(st.deep_max, drawn_deep);
            st.deep_frames += drawn_deep > 0.01f ? 1 : 0;
            const float arm_gap = std::max(0.0f, ArmGap(hand));
            st.gap_sum += arm_gap;
            st.gap_max = std::max(st.gap_max, arm_gap);
            if (now - st.log_at >= std::chrono::seconds(1) && g_person_logs.fetch_add(1) < 120) {
                spdlog::info("[BodyIK] (fix180) your {} hand locked on the person it holds: {} frames, {} drawn wholly locked, {} faded toward your "
                             "controller (farther than {:.2f} m); your controller's palm {:.3f} m from the locked palm on average ({:.3f} at most); the arm "
                             "stretched past its reach by {:.3f} m on average ({:.3f} at most); the drawn hand's deepest point {:.3f} m in them at most ({} "
                             "frames over 1 cm; placed {:.3f} m out from {:.3f} in, turned {:.0f} degrees); fingers touching them {:.1f} of 5 on average, "
                             "starting inside them in {} frames; {} surfaces",
                    left ? "left" : "right", st.frames, st.locked_frames, st.faded_frames, kLockFull, st.apart_sum / std::max(1, st.frames), st.apart_max,
                    st.gap_sum / std::max(1, st.frames), st.gap_max, std::max(0.0f, st.deep_max), st.deep_frames, lock.seat, std::max(0.0f, lock.before),
                    lock.turn_deg, static_cast<double>(st.touch_sum) / std::max(1, st.grip_frames), st.bases_frames, held.caps);
                spdlog::info("[BodyIK] (fix181) your {} hand on the person it holds, as drawn: its palm landed {:.3f} m from the locked palm on average "
                             "({:.3f} at most, {} frames wholly locked), {:+.3f} m from their bare body's surface on average ({:+.3f} to {:+.3f}; clothes "
                             "taken as {:.3f}); their bone from their own update this frame in {} frames, last frame's moved on in {}, last frame's in {}, "
                             "the frame's start (Physical's) in {}",
                    left ? "left" : "right", st.landed_n > 0 ? st.landed_sum / static_cast<float>(st.landed_n) : 0.0f, st.landed_max, st.landed_n,
                    st.landed_n > 0 ? st.bare_sum / static_cast<float>(st.landed_n) : 0.0f, st.landed_n > 0 ? st.bare_min : 0.0f,
                    st.landed_n > 0 ? st.bare_max : 0.0f, held.caps > 0 ? held.cap_r[0] - held.bare_r : 0.0f, st.source[1], st.source[2], st.source[3],
                    st.source[0]);
                const int grab = st.grab;
                st             = PersonHandStats{};
                st.grab        = grab;
                st.log_at      = now;
            }
        }

        // (fix180) A hand holding on to a wall while you climb (Climbing::GripSurface): drawn locked where
        // it took hold, as on a person: its palm on the wall's surface facing it, none of it inside the wall (the wall taken as flat there:
        // the pick's hit point and normal), kept in the world (the wall does not move; your body does). Eased in and out with the grip.
        void HoldWallShift(bool left, std::optional<HandTarget>& grip, std::optional<HandTarget>& aim)
        {
            const int          hand = left ? 0 : 1;
            HandLock&          lock = g_wall_lock[hand];
            Climbing::WallGrip wall{};
            if (!grip || !aim) {
                lock.w = 0.0f;
                return;
            }
            if (!Climbing::GripSurface(left, wall) || !(glm::length(wall.normal) > 0.5f) || !std::isfinite(wall.point.x) || !std::isfinite(wall.point.y) ||
                !std::isfinite(wall.point.z)) {
                lock = HandLock{}; // (the next hold placed anew)
                return;
            }
            const glm::vec3 n   = glm::normalize(wall.normal);
            const auto      now = std::chrono::steady_clock::now();
            // A new hold (none yet, or a new place: the grip let go and took hold again): placed once. (The review) Taken again while the
            // last was still drawn (its ease-out): the new one eased in from where the hand was drawn, not from your controller.
            if (lock.key == 0 || glm::length(wall.point - lock.point) > 0.01f) {
                static int     walls = 0;
                const HandLock old   = lock;
                lock                 = HandLock{};
                lock.key             = 2'000'000 + ++walls;
                lock.point           = wall.point;
                if (old.ok && old.w > 0.01f) {
                    lock.handover    = true;
                    lock.handover_at = now;
                    lock.from_grip_p = old.drawn_grip_p;
                    lock.from_grip_f = old.drawn_grip_f;
                    lock.from_grip_u = old.drawn_grip_u;
                    lock.from_aim_p  = old.drawn_aim_p;
                    lock.from_aim_f  = old.drawn_aim_f;
                    lock.from_aim_u  = old.drawn_aim_u;
                }
            }
            if (!lock.ok && lock.made < 30) {
                ++lock.made;
                HandTarget lg{}, la{};
                float      seat = 0.0f, before = 0.0f, turn_deg = 0.0f;
                // 3 mm off the wall at least.
                const auto deepest = [&](const glm::vec3& p, glm::vec3& way, bool) {
                    way = n;
                    return 0.003f - glm::dot(p - wall.point, n);
                };
                // (The review) The palm where your controller's palm meets the wall (the pick's hit point can be 20-30 cm out, along its
                // ray), kept within 10 cm of the hit point.
                glm::vec3 at = wall.point;
                {
                    glm::vec3 c0{}, f0{}, d0{}, k0{};
                    if (PalmOf(left, *grip, *aim, c0, f0, d0, k0)) {
                        const glm::vec3 on    = c0 - n * glm::dot(c0 - wall.point, n);
                        const glm::vec3 slide = on - wall.point;
                        at = glm::length(slide) > kWallSlide ? wall.point + slide * (kWallSlide / glm::length(slide)) : on;
                    }
                }
                if (PlaceOnSurface(left, *grip, *aim, at, n, deepest, lg, la, seat, before, turn_deg)) {
                    lock.slide    = glm::length(at - wall.point);
                    lock.ok       = true;
                    lock.grip_p   = lg.position;
                    lock.grip_f   = lg.forward;
                    lock.grip_u   = lg.up;
                    lock.aim_p    = la.position;
                    lock.aim_f    = la.forward;
                    lock.aim_u    = la.up;
                    lock.seat     = seat;
                    lock.before   = before;
                    lock.turn_deg = turn_deg;
                    static std::atomic<int> told{ 0 };
                    if (told.fetch_add(1) < 60) {
                        spdlog::info("[BodyIK] (fix180) your {} hand locked on the wall it holds: its palm on the wall where it took hold, turned {:.0f} degrees "
                                     "to face it, seated out {:.3f} m (it was {:.3f} m in it); drawn there while you hold on",
                            left ? "left" : "right", turn_deg, seat, std::max(0.0f, before));
                    }
                }
            }
            if (!lock.ok) {
                lock.w = 0.0f;
                return;
            }
            const HandTarget lg{ lock.grip_p, lock.grip_f, lock.grip_u };
            const HandTarget la{ lock.aim_p, lock.aim_f, lock.aim_u };
            float            apart = 0.0f;
            const float      w     = LockShare(left, *grip, *aim, lg, la, wall.weight, apart, kWallFull, kWallNone);
            if (w > 0.0f) {
                BlendToLock(*grip, *aim, lg, la, w, lock.last);
            }
            if (lock.handover) {
                const float h = std::chrono::duration<float>(now - lock.handover_at).count() / 0.15f;
                if (h < 1.0f && std::isfinite(h)) {
                    BlendFrom(*grip, *aim, HandTarget{ lock.from_grip_p, lock.from_grip_f, lock.from_grip_u },
                        HandTarget{ lock.from_aim_p, lock.from_aim_f, lock.from_aim_u }, std::max(0.0f, h));
                } else {
                    lock.handover = false;
                }
            }
            lock.w            = lock.handover ? std::max(w, 0.02f) : w;
            lock.drawn_grip_p = grip->position;
            lock.drawn_grip_f = grip->forward;
            lock.drawn_grip_u = grip->up;
            lock.drawn_aim_p  = aim->position;
            lock.drawn_aim_f  = aim->forward;
            lock.drawn_aim_u  = aim->up;
            // The log: a line each second while it holds on.
            float     drawn_deep = -1.0f;
            glm::vec3 c{}, f{}, d{}, k{};
            if (PalmOf(left, *grip, *aim, c, f, d, k)) {
                for (const auto& h : kLockPoints) {
                    drawn_deep = std::max(drawn_deep, -glm::dot(c + d * h[0] + k * h[1] + f * h[2] - wall.point, n));
                }
            }
            auto& st = g_wall_hand[hand];
            if (st.grab != lock.key) {
                st        = PersonHandStats{};
                st.grab   = lock.key;
                st.log_at = now;
            }
            ++st.frames;
            st.locked_frames += w >= 0.99f ? 1 : 0;
            st.faded_frames += wall.weight >= 0.99f && w < 0.99f ? 1 : 0;
            st.apart_sum += apart;
            st.apart_max = std::max(st.apart_max, apart);
            st.deep_max  = std::max(st.deep_max, drawn_deep);
            st.deep_frames += drawn_deep > 0.01f ? 1 : 0;
            const float arm_gap = std::max(0.0f, ArmGap(hand));
            st.gap_sum += arm_gap;
            st.gap_max = std::max(st.gap_max, arm_gap);
            if (now - st.log_at >= std::chrono::seconds(1) && g_wall_logs.fetch_add(1) < 120) {
                spdlog::info("[BodyIK] (fix180) your {} hand locked on the wall: {} frames, {} drawn wholly locked, {} faded toward your controller (farther "
                             "than {:.2f} m); your controller's palm {:.3f} m from the locked palm on average ({:.3f} at most); the arm stretched past its "
                             "reach by {:.3f} m on average ({:.3f} at most); the drawn hand's deepest point {:.3f} m in the wall at most ({} frames over 1 cm; "
                             "placed {:.3f} m out, {:.3f} m from the pick's hit point, turned {:.0f} degrees); fingers touching it {:.1f} of 5 on average, "
                             "starting inside it in {} frames",
                    left ? "left" : "right", st.frames, st.locked_frames, st.faded_frames, kWallFull, st.apart_sum / std::max(1, st.frames), st.apart_max,
                    st.gap_sum / std::max(1, st.frames), st.gap_max, std::max(0.0f, st.deep_max), st.deep_frames, lock.seat,
                    lock.slide, lock.turn_deg, static_cast<double>(st.touch_sum) / std::max(1, st.grip_frames),
                    st.bases_frames);
                const int grab = st.grab;
                st             = PersonHandStats{};
                st.grab        = grab;
                st.log_at      = now;
            }
        }

        // Headset body and hand tracking for this frame, mapped into the game through the same tracking space as
        // the head and controllers.
        VR::BodyTrackingState g_tracked_body;
        VR::HandTrackingState g_tracked_hands[2];

        namespace joint
        {
            constexpr int kHips = 1, kNeck = 6;
            constexpr int kArmUpper[2] = { 10, 15 }, kArmLower[2] = { 11, 16 }, kWrist[2] = { 19, 45 };
            constexpr int kUpperLeg[2] = { 70, 77 }, kLowerLeg[2] = { 71, 78 }, kAnkle[2] = { 73, 80 };
        }

        struct TrackingDiag
        {
            bool supported{ false };
            bool active{ false };
            int  valid{ 0 };
            bool torso{ false };
            bool elbow[2]{};
            bool legs{ false };
            bool fingers[2]{};
            int  hand_source[2]{};
        };
        TrackingDiag g_track_diag;

        void SnapshotTracking()
        {
            static auto vr = VR::get();
            g_tracked_body = {};
            vr->get_body_tracking(g_tracked_body);
            for (int side = 0; side < 2; ++side) {
                g_tracked_hands[side] = {};
                vr->get_hand_tracking(side == 0, g_tracked_hands[side]);
            }
            g_track_diag           = {};
            g_track_diag.supported = g_tracked_body.supported;
            g_track_diag.active    = g_tracked_body.active;
            for (std::uint32_t i = 0; i < g_tracked_body.joint_count && i < g_tracked_body.joints.size(); ++i) {
                g_track_diag.valid += g_tracked_body.joints[i].position_valid ? 1 : 0;
            }
            for (int side = 0; side < 2; ++side) {
                g_track_diag.hand_source[side] = g_tracked_hands[side].active ? g_tracked_hands[side].data_source : -1;
            }
        }

        bool BodyTrackingOn()
        {
            return GameFlow::gStore.internalSettings.bodyTracking && g_tracked_body.active;
        }

        std::optional<glm::vec3> TrackedJoint(int index)
        {
            if (!BodyTrackingOn() || index < 0 || index >= static_cast<int>(g_tracked_body.joint_count) ||
                !g_tracked_body.joints[index].position_valid) {
                return std::nullopt;
            }
            return glm::vec3{ g_tracked_body.joints[index].position };
        }

        // A stage-space direction in game world space.
        std::optional<glm::vec3> StageDirection(const glm::vec3& d)
        {
            const auto room = tracking::RoomRotation();
            if (!room || glm::length(d) < 1e-5f) {
                return std::nullopt;
            }
            return glm::normalize(*room * tracking::ToHavokVector(d));
        }

        // Direction between two tracked joints, in game world space.
        std::optional<glm::vec3> TrackedDirection(int from, int to)
        {
            const auto a = TrackedJoint(from);
            const auto b = TrackedJoint(to);
            if (!a || !b) {
                return std::nullopt;
            }
            return StageDirection(*b - *a);
        }

        // Side the limb bends toward at its middle joint (elbow or knee), in game world space.
        std::optional<glm::vec3> TrackedBend(int root, int middle, int end)
        {
            const auto a = TrackedJoint(root);
            const auto b = TrackedJoint(middle);
            const auto c = TrackedJoint(end);
            if (!a || !b || !c) {
                return std::nullopt;
            }
            const auto axis = *c - *a;
            if (glm::length(axis) < 1e-3f) {
                return std::nullopt;
            }
            const auto n    = glm::normalize(axis);
            auto       bend = (*b - *a) - glm::dot(*b - *a, n) * n;
            if (glm::length(bend) < 5e-3f) {
                return std::nullopt;  // limb nearly straight; the bend side is not observable
            }
            return StageDirection(bend);
        }

        bool PlayerSeated()
        {
            static auto vr     = VR::get();
            const float height = vr->get_floor_eye_height();
            return height >= 0.0f && height < 1.3f;
        }

        // BSModelNode pose storage (see local RE notes): locals feed the batched world computation.
        // Animation does not rewrite every local each frame; bones we wrote are restored to their animated
        // value first so the IK never builds on its own output.
        struct WrittenBone
        {
            RE::NiTransform animated{};
            RE::NiTransform written{};
        };
        std::unordered_map<int, WrittenBone> g_written;

        struct Pose
        {
            std::uint8_t*    model{ nullptr };
            RE::NiTransform* local{ nullptr };
            RE::NiTransform* world{ nullptr };
            std::uint16_t*   parent{ nullptr };
            std::uint16_t    count{ 0 };
            std::uint16_t    top{ 0 };
            Xf               root{};      // the root transform top-level entries are relative to
            Xf               to_world{};  // pose space -> game world

            Xf World(int i, int depth = 0) const
            {
                if (i < top || depth > 64) {
                    return Compose(root, FromNi(local[i]));
                }
                return Compose(World(parent[i], depth + 1), FromNi(local[i]));
            }

            Xf GameWorld(int i) const { return Compose(to_world, World(i)); }

            void SetLocalRotation(int i, const RE::NiMatrix3& rotation) const
            {
                auto [entry, first] = g_written.try_emplace(i);
                if (first) {
                    entry->second.animated = local[i];
                }
                local[i].rotate       = rotation;
                entry->second.written = local[i];
            }

            void ScaleTranslation(int i, float k) const
            {
                auto [entry, first] = g_written.try_emplace(i);
                if (first) {
                    entry->second.animated = local[i];
                }
                local[i].translate.x *= k;
                local[i].translate.y *= k;
                local[i].translate.z *= k;
                entry->second.written = local[i];
            }

            Xf ParentWorld(int i) const { return i < top ? root : World(parent[i]); }

            // Sets bone i so its game-world transform becomes `desired`; descendants follow. (A bone more than `max_offset` from its
            // parent is not set: a guard against nonsense; a thrown weapon's bone may be far from the hand.)
            void SetGameWorld(int i, const Xf& desired, float max_offset = 10.0f) const
            {
                const auto pose_space = Compose(Inverse(to_world), desired);
                const auto result     = Compose(Inverse(ParentWorld(i)), pose_space);
                bool       finite     = std::isfinite(result.s) && result.s > 0.0f && result.s < 10.0f && glm::length(result.t) < max_offset;
                for (int a = 0; a < 3 && finite; ++a) {
                    finite = std::isfinite(result.t[a]) && std::isfinite(result.r[a][0]) && std::isfinite(result.r[a][1]) && std::isfinite(result.r[a][2]);
                }
                if (finite) {
                    auto [entry, first] = g_written.try_emplace(i);
                    if (first) {
                        entry->second.animated = local[i];
                    }
                    ToNi(result, local[i]);
                    entry->second.written = local[i];
                }
            }
        };

        // A hand's frame measured from its own bones, in the wrist bone's space: forward toward the finger
        // bases, up toward the thumb base, and the palm centre as a distance along forward.
        struct HandShape
        {
            bool      valid{ false };
            glm::vec3 forward{};
            glm::vec3 up{};
            float     palm{ 0.0f };
        };

        struct Arm
        {
            int              clavicle{ -1 }, biceps{ -1 }, forearm{ -1 }, wrist{ -1 }, thumb{ -1 };
            float            twist_prev{ 0.0f };
            // (fix143) Kept from the frame before, for the elbow to change smoothly whatever the frame rate: when it was solved, the
            // side the elbow went (its direction off the shoulder-to-hand line), and the forearm's share of the wrist's twist (its
            // full turn, unwrapped: no jump from +90 to -90 degrees as the wrist turns past half a turn).
            bool                                  has_time{ false };
            std::chrono::steady_clock::time_point last_time{};
            bool                                  has_side{ false };
            glm::vec3                             elbow_side{};
            bool                                  has_turn{ false };
            float                                 forearm_turn{ 0.0f };
            // Side of the upper arm the forearm bends toward (the elbow hinge), in the upper arm's space.
            bool             has_bend{ false };
            glm::vec3        bend_side{};
            std::vector<int> fingers;
            HandShape        shape;
        };

        struct Bones
        {
            const void* storage{ nullptr };
            Arm         right, left;
            int         head{ -1 }, neck{ -1 }, weapon{ -1 }, chest{ -1 };
            struct Leg
            {
                int thigh{ -1 }, calf{ -1 }, foot{ -1 };
            } left_leg, right_leg;
            int pelvis{ -1 };  // lowest bone above both a thigh and the neck
            std::vector<int> spine;  // bones from above the pelvis up to the neck, base first
            // Finger chains (base, middle, tip) per hand: thumb, index, middle, ring, pinky.
            std::array<std::array<int, 3>, 5> fingers[2]{};
            std::unordered_map<std::string, int> by_name;
            // The weapon bone (the "Weapon" under C_Chest, where the weapon in your hands hangs) ...
            RE::NiAVObject* weapon_node{ nullptr };
            // ...and any other pose node named "Weapon": a weapon whose own model's top node has that name (some modded weapons,
            // e.g. a lightsaber), hung elsewhere in the body.
            std::vector<std::pair<int, RE::NiAVObject*>> weapon_models;
            RE::NiAVObject* r_wrist_node{ nullptr };
            // Legacy accessors used by diagnostics.
            int r_biceps{ -1 }, r_forearm{ -1 }, r_wrist{ -1 }, l_wrist{ -1 };
            // (fix142) The node at each pose index when set up, and the top count: a new node list with the body's bones the same
            // nodes at the same places is not set up again (KeepBones).
            std::vector<RE::NiAVObject*> node_at;
            int                          top{ -1 };
        };
        Bones g_bones;

        // The model's nodes by pose index (null where none).
        std::vector<RE::NiAVObject*> NodesAt(const Pose& pose)
        {
            std::vector<RE::NiAVObject*> nodes(pose.count, nullptr);
            const auto entries = *reinterpret_cast<std::uint8_t**>(pose.model + 0x20);
            const auto count   = *reinterpret_cast<std::uint32_t*>(pose.model + 0x18);
            for (std::uint32_t e = 0; entries && e < count; ++e) {
                const auto idx  = *reinterpret_cast<std::uint16_t*>(entries + e * 16);
                const auto node = *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8);
                if (node && idx < pose.count) {
                    nodes[idx] = node;
                }
            }
            return nodes;
        }

        // The weapon bone is the "Weapon" under C_Chest. A modded weapon's model can be named "Weapon" too (its own top node, hung
        // under the skeleton's root): never taken for the bone. Without C_Chest, the first one (the skeleton's bones come first).
        void PickWeaponBone(const Pose& pose, const std::vector<std::pair<int, RE::NiAVObject*>>& weapons, bool log)
        {
            g_bones.weapon      = -1;
            g_bones.weapon_node = nullptr;
            g_bones.weapon_models.clear();
            for (const auto& [idx, node] : weapons) {
                if (g_bones.chest >= 0 && idx >= pose.top && pose.parent[idx] == g_bones.chest) {
                    g_bones.weapon      = idx;
                    g_bones.weapon_node = node;
                    break;
                }
            }
            if (g_bones.weapon < 0 && !weapons.empty()) {
                g_bones.weapon      = weapons.front().first;
                g_bones.weapon_node = weapons.front().second;
            }
            for (const auto& entry : weapons) {
                if (entry.first != g_bones.weapon) {
                    g_bones.weapon_models.push_back(entry);
                }
            }
            if (log && weapons.size() > 1) {
                std::string others;
                for (const auto& [idx, node] : g_bones.weapon_models) {
                    others += " " + std::to_string(idx);
                }
                spdlog::info("[BodyIK] {} nodes named Weapon: the weapon bone {} (under C_Chest: {}), and{} (a weapon's own model with that name)",
                    weapons.size(), g_bones.weapon, g_bones.chest >= 0 && g_bones.weapon >= pose.top && pose.parent[g_bones.weapon] == g_bones.chest, others);
            }
        }

        // Where the weapon in your hands hangs this frame (the node that is placed in your hand): the weapon bone, unless nothing
        // hangs on the bone but its hand IK targets while a weapon's own model named "Weapon" is shown elsewhere (a modded weapon
        // such as a lightsaber, whose model is not under the bone): then that model, as before. Chosen each frame (ChooseWeapon).
        struct WeaponPlace
        {
            int             idx{ -1 };
            RE::NiAVObject* node{ nullptr };
            bool            model{ false }; // a weapon's own model named "Weapon", not the bone
        };
        WeaponPlace g_place;

        void FindBones(const Pose& pose, const void* storage)
        {
            g_bones         = {};
            g_bones.storage = storage;
            std::vector<std::pair<int, std::string>> named;
            std::vector<std::pair<int, RE::NiAVObject*>> weapons;
            const auto entries = *reinterpret_cast<std::uint8_t**>(pose.model + 0x20);
            const auto count   = *reinterpret_cast<std::uint32_t*>(pose.model + 0x18);
            for (std::uint32_t e = 0; entries && e < count; ++e) {
                const auto idx  = *reinterpret_cast<std::uint16_t*>(entries + e * 16);
                const auto node = *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8);
                if (!node || idx >= pose.count) {
                    continue;
                }
                const std::string_view name{ node->name.c_str() };
                named.emplace_back(idx, std::string{ name });
                if (name == "R_Biceps") g_bones.right.biceps = idx;
                else if (name == "R_Forearm") g_bones.right.forearm = idx;
                else if (name == "R_Wrist") {
                    g_bones.right.wrist  = idx;
                    g_bones.r_wrist_node = node;
                }
                else if (name == "L_Biceps") g_bones.left.biceps = idx;
                else if (name == "L_Forearm") g_bones.left.forearm = idx;
                else if (name == "L_Wrist") g_bones.left.wrist = idx;
                else if (name == "C_Head") g_bones.head = idx;
                else if (name == "C_Neck") g_bones.neck = idx;
                else if (name == "L_Thigh") g_bones.left_leg.thigh = idx;
                else if (name == "L_Calf") g_bones.left_leg.calf = idx;
                else if (name == "L_Foot") g_bones.left_leg.foot = idx;
                else if (name == "R_Thigh") g_bones.right_leg.thigh = idx;
                else if (name == "R_Calf") g_bones.right_leg.calf = idx;
                else if (name == "R_Foot") g_bones.right_leg.foot = idx;
                else if (name == "C_Chest") g_bones.chest = idx;
                else if (name == "Weapon") {
                    weapons.emplace_back(idx, node);
                }
            }
            PickWeaponBone(pose, weapons, true);
            // Finger bases are the wrist's direct children.
            for (auto* arm : { &g_bones.right, &g_bones.left }) {
                std::string names;
                for (const auto& [idx, name] : named) {
                    if (arm->wrist < 0 || idx < pose.top || pose.parent[idx] != arm->wrist) {
                        continue;
                    }
                    names += " " + name;
                    if (Contains(name, "Thumb")) {
                        arm->thumb = idx;
                    } else if (Contains(name, "Index") || Contains(name, "Middle") || Contains(name, "Ring") || Contains(name, "Pinky") ||
                               Contains(name, "Little")) {
                        arm->fingers.push_back(idx);
                    }
                }
                // Unrecognised names: every child is a finger base, and the one farthest off their mean axis is the thumb.
                if (arm->thumb < 0 || arm->fingers.empty()) {
                    std::vector<int> children;
                    for (const auto& [idx, name] : named) {
                        if (arm->wrist >= 0 && idx >= pose.top && pose.parent[idx] == arm->wrist) {
                            children.push_back(idx);
                        }
                    }
                    if (children.size() >= 2) {
                        const auto wrist = pose.World(arm->wrist).t;
                        glm::vec3  mean{ 0.0f };
                        for (const int c : children) {
                            mean += pose.World(c).t - wrist;
                        }
                        const auto axis = glm::length(mean) > 1e-4f ? glm::normalize(mean) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                        float      best = -1.0f;
                        for (const int c : children) {
                            const auto d   = pose.World(c).t - wrist;
                            const float off = glm::length(d - glm::dot(d, axis) * axis);
                            if (off > best) {
                                best       = off;
                                arm->thumb = c;
                            }
                        }
                        arm->fingers.clear();
                        for (const int c : children) {
                            if (c != arm->thumb) {
                                arm->fingers.push_back(c);
                            }
                        }
                        names += " (thumb by geometry)";
                    }
                }
                spdlog::info("[BodyIK] {} wrist children:{}", arm == &g_bones.right ? "right" : "left", names);
            }
            for (auto* arm : { &g_bones.right, &g_bones.left }) {
                if (arm->biceps >= pose.top) {
                    arm->clavicle = pose.parent[arm->biceps];
                }
            }
            for (const auto& [idx, name] : named) {
                g_bones.by_name[name] = idx;
            }
            {
                static constexpr const char* kFingerNames[5][3] = { { "thumb", "Thumb1", "Thumb2" }, { "Index", "Index1", "Index2" },
                    { "Middle", "Middle1", "Middle2" }, { "Ring", "Ring1", "Ring2" }, { "Pinky", "Pinky1", "Pinky2" } };
                for (int side = 0; side < 2; ++side) {
                    const std::string prefix = side == 0 ? "L_" : "R_";
                    for (int f = 0; f < 5; ++f) {
                        for (int j = 0; j < 3; ++j) {
                            const auto it                = g_bones.by_name.find(prefix + kFingerNames[f][j]);
                            g_bones.fingers[side][f][j] = it != g_bones.by_name.end() ? it->second : -1;
                        }
                    }
                }
            }

            // Leg bones by name fragment when the exact names differ; twist helpers excluded.
            auto find_leg = [&](int& slot, std::string_view side, std::string_view part) {
                if (slot >= 0) {
                    return;
                }
                for (const auto& [idx, name] : named) {
                    if (name.rfind(side, 0) == 0 && Contains(name, part) && !Contains(name, "Twist")) {
                        slot = idx;
                        return;
                    }
                }
            };
            find_leg(g_bones.left_leg.thigh, "L_", "Thigh");
            find_leg(g_bones.left_leg.calf, "L_", "Calf");
            find_leg(g_bones.left_leg.foot, "L_", "Foot");
            find_leg(g_bones.right_leg.thigh, "R_", "Thigh");
            find_leg(g_bones.right_leg.calf, "R_", "Calf");
            find_leg(g_bones.right_leg.foot, "R_", "Foot");

            // Pelvis: the lowest common ancestor of a thigh and the neck.
            if (g_bones.left_leg.thigh >= 0 && g_bones.neck >= 0) {
                std::vector<int> neck_chain;
                for (int i = g_bones.neck, guard = 0; i >= 0 && guard < 64; ++guard) {
                    neck_chain.push_back(i);
                    i = i < pose.top ? -1 : pose.parent[i];
                }
                for (int i = g_bones.left_leg.thigh, guard = 0; i >= 0 && guard < 64; ++guard) {
                    if (std::find(neck_chain.begin(), neck_chain.end(), i) != neck_chain.end()) {
                        g_bones.pelvis = i;
                        break;
                    }
                    i = i < pose.top ? -1 : pose.parent[i];
                }
            }
            // Spine: from the neck down to (not including) the pelvis, then reversed.
            if (g_bones.neck >= 0 && g_bones.pelvis >= 0) {
                for (int i = g_bones.neck >= pose.top ? pose.parent[g_bones.neck] : -1, guard = 0; i >= 0 && i != g_bones.pelvis && guard < 16; ++guard) {
                    g_bones.spine.push_back(i);
                    i = i < pose.top ? -1 : pose.parent[i];
                }
                std::reverse(g_bones.spine.begin(), g_bones.spine.end());
            }
            {
                std::string all;
                for (const auto& [idx, name] : named) {
                    all += " " + std::to_string(idx) + ":" + name;
                }
                spdlog::info("[BodyIK] bones:{}", all);
                const auto pelvis_name = std::find_if(named.begin(), named.end(), [](const auto& n) { return n.first == g_bones.pelvis; });
                spdlog::info("[BodyIK] legs L {} {} {} R {} {} {} | pelvis {} '{}'", g_bones.left_leg.thigh, g_bones.left_leg.calf, g_bones.left_leg.foot,
                    g_bones.right_leg.thigh, g_bones.right_leg.calf, g_bones.right_leg.foot, g_bones.pelvis,
                    pelvis_name != named.end() ? pelvis_name->second : "");
            }
            g_bones.r_biceps  = g_bones.right.biceps;
            g_bones.r_forearm = g_bones.right.forearm;
            g_bones.r_wrist   = g_bones.right.wrist;
            g_bones.l_wrist   = g_bones.left.wrist;
            spdlog::info("[BodyIK] {} pose bones (top {}): R {} {} {} thumb {} fingers {} | L {} {} {} thumb {} fingers {} | head {} neck {} weapon {}",
                pose.count, pose.top, g_bones.right.biceps, g_bones.right.forearm, g_bones.right.wrist, g_bones.right.thumb,
                g_bones.right.fingers.size(), g_bones.left.biceps, g_bones.left.forearm, g_bones.left.wrist, g_bones.left.thumb,
                g_bones.left.fingers.size(), g_bones.head, g_bones.neck, g_bones.weapon);
            g_bones.node_at = NodesAt(pose);
            g_bones.top     = pose.top;
        }

        // (fix142) A new node list (the model's bone storage made again) with every bone the body uses still the same node at the
        // same index: the setup is kept (the arms' measured hinges and shapes, the elbows' smoothing), only the weapons and names
        // found again. In fix141's test a small unnamed node ("NoName0", most likely an effect: a hit or a swing) came and went
        // about once a second in fights, with any weapon or none; each time the body was set up again from nothing, the elbows'
        // smoothing starting from zero (the arms swung out and back) and the bones' log written (a hitch). False: set it up again.
        bool KeepBones(const Pose& pose, const void* storage)
        {
            if (g_bones.storage == nullptr || g_bones.node_at.empty() || g_bones.top != pose.top) {
                return false;
            }
            const auto nodes = NodesAt(pose);
            std::vector<int> used{ g_bones.head, g_bones.neck, g_bones.chest, g_bones.pelvis, g_bones.left_leg.thigh, g_bones.left_leg.calf,
                g_bones.left_leg.foot, g_bones.right_leg.thigh, g_bones.right_leg.calf, g_bones.right_leg.foot };
            for (const auto* arm : { &g_bones.right, &g_bones.left }) {
                used.insert(used.end(), { arm->clavicle, arm->biceps, arm->forearm, arm->wrist, arm->thumb });
                used.insert(used.end(), arm->fingers.begin(), arm->fingers.end());
            }
            used.insert(used.end(), g_bones.spine.begin(), g_bones.spine.end());
            for (const auto& hand : g_bones.fingers) {
                for (const auto& chain : hand) {
                    used.insert(used.end(), chain.begin(), chain.end());
                }
            }
            for (const int idx : used) {
                if (idx < 0) {
                    continue;
                }
                if (idx >= static_cast<int>(nodes.size()) || idx >= static_cast<int>(g_bones.node_at.size()) || nodes[idx] == nullptr ||
                    nodes[idx] != g_bones.node_at[idx]) {
                    return false;
                }
            }
            // The weapons and names found again; what came and went, for the log.
            std::vector<std::pair<int, RE::NiAVObject*>> weapons;
            std::string                                  added;
            int                                          gone = 0;
            g_bones.by_name.clear();
            for (int idx = 0; idx < static_cast<int>(nodes.size()); ++idx) {
                auto* node = nodes[idx];
                if (!node) {
                    continue;
                }
                const std::string name{ node->name.c_str() };
                g_bones.by_name[name] = idx;
                if (name == "Weapon") {
                    weapons.emplace_back(idx, node);
                }
                if (std::find(g_bones.node_at.begin(), g_bones.node_at.end(), node) == g_bones.node_at.end() && added.size() < 200) {
                    const int   parent = idx >= pose.top ? pose.parent[idx] : -1;
                    const auto* above  = parent >= 0 && parent < static_cast<int>(nodes.size()) ? nodes[parent] : nullptr;
                    added += std::format(" {}:{} (under {})", idx, name.empty() ? "(no name)" : name, above ? above->name.c_str() : "the top");
                }
            }
            for (auto* node : g_bones.node_at) {
                if (node && std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
                    ++gone;
                }
            }
            PickWeaponBone(pose, weapons, false);
            const auto before = g_bones.node_at.size();
            g_bones.node_at   = nodes;
            g_bones.storage   = storage;
            static int told   = 0;
            if (told < 40) {
                ++told;
                spdlog::info("[BodyIK] the model's node list changed ({} -> {} nodes), the body's bones the same: kept as set up; added{}; gone {}",
                    before, nodes.size(), added.empty() ? " none" : added, gone);
            }
            return true;
        }

        // Something other than the hand IK targets hangs on the weapon bone: a weapon's model.
        bool BoneHoldsModel(RE::NiAVObject* bone)
        {
            RE::NiAVObject* children[32]{};
            const auto      count = ReadChildren(bone, children, 32);
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::string_view name{ children[i]->name.c_str() };
                if (name != "R_HandIk" && name != "L_HandIk") {
                    return true;
                }
            }
            return false;
        }

        // Each frame (before the barrel is measured and the arms are posed): which node the weapon in your hands hangs from.
        void ChooseWeapon()
        {
            WeaponPlace place{ g_bones.weapon, g_bones.weapon_node, false };
            if (g_bones.weapon_node && !g_bones.weapon_models.empty() && !BoneHoldsModel(g_bones.weapon_node)) {
                for (const auto& [idx, node] : g_bones.weapon_models) {
                    if (node && !IsHidden(node)) {
                        place = WeaponPlace{ idx, node, true };
                        break;
                    }
                }
            }
            static int told = 0;
            if (place.node != g_place.node && !g_bones.weapon_models.empty() && told < 30) {
                ++told;
                spdlog::info("[BodyIK] the weapon in your hands hangs from {} ({})", place.model ? "its own model named Weapon" : "the weapon bone",
                    place.model ? "a modded weapon whose model is not under the bone" : "as the game's weapons do");
            }
            g_place = place;
        }

        // Port of FRIK's solveArmToHandWorldTarget (Fallout 4 VR Body, GPL-3.0): shoulder reach, elbow flexion floor
        // and ceiling, wrist-driven elbow direction, twist split, exact hand. FO4 distances are in units of 1/70 m.
        constexpr float kUnit = 1.0f / 70.0f;

        // (fix143) What changed how much, frame to frame, since the last 5 s log line (the body's jitter: fix142's test).
        struct Steadiness
        {
            uint32_t calls{ 0 };                       // body placements...
            uint32_t frames{ 0 };                      // ...over this many engine frames
            uint32_t last_frame{ 0 };
            float    facing_raw{ 0.0f };               // the last turn wanted toward the hands (degrees), before smoothing
            float    facing_spread{ 0.0f };            // ...and the hands' flat spread then (m)
            float    facing_step{ 0.0f };              // the largest change of the body's turn in one placement (degrees)
            uint32_t facing_clamped{ 0 };              // placements with the wanted turn at its limit
            float    side_min[2]{ 1e9f, 1e9f };        // per arm: the shortest elbow side before it is made unit length
            float    elbow_step[2]{ 0.0f, 0.0f };      // ...and the largest elbow turn in one placement (degrees)
            uint32_t retargets{ 0 };                   // the walk's steps re-aimed after slowing down
            float    speed_min{ 1e9f };                // the walk's raw speed (m/s)
            float    speed_max{ 0.0f };
            float    delta_forward{ 0.0f };            // the last placement: the neck's way to under the eyes, along the body's facing...
            float    delta_side{ 0.0f };               // ...and to its left (m)
            float    delta_max{ 0.0f };
            float    real_lean{ 0.0f };                // your head's own lean from where you recentred (m, flat)
        };
        Steadiness g_steady;

        // Turns `from` toward `to` (both unit length) by at most `max_angle` radians.
        glm::vec3 TurnToward(const glm::vec3& from, const glm::vec3& to, float max_angle)
        {
            const float c     = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
            const float angle = std::acos(c);
            if (angle <= max_angle || angle < 1e-5f) {
                return to;
            }
            glm::vec3 across = to - from * c;
            if (glm::length(across) < 1e-6f) {
                return to; // (opposite: no one way to turn)
            }
            across = glm::normalize(across);
            return glm::normalize(from * std::cos(max_angle) + across * std::sin(max_angle));
        }

        float SolveArm(const Pose& pose, Arm& arm, bool is_left, const Xf& hand_target, const std::optional<glm::vec3>& tracked_bend = std::nullopt)
        {
            const float neg_left = is_left ? -1.0f : 1.0f;
            const auto  up_axis  = glm::vec3{ 0.0f, 0.0f, 1.0f };

            auto forward = pose.root.r[1];
            // (fix158) The chest's forward while moving (the spine takes the turn toward the hands then, fix157), not the hips'.
            if (std::abs(g_spine_twist) > 1e-4f && std::isfinite(g_spine_twist)) {
                forward = glm::mat3_cast(glm::angleAxis(g_spine_twist, up_axis)) * forward;
            }
            forward.z    = 0.0f;
            if (glm::length(forward) < 1e-3f) {
                return 0.0f;
            }
            forward              = glm::normalize(forward);
            const auto sideways_r = glm::vec3{ forward.y, -forward.x, 0.0f };
            const auto sideways   = sideways_r * neg_left;

            const auto hand_pos = hand_target.t;
            const auto hand_rot = hand_target.r;

            const float orig_upper   = glm::length(pose.GameWorld(arm.forearm).t - pose.GameWorld(arm.biceps).t);
            const float orig_forearm = glm::length(pose.GameWorld(arm.wrist).t - pose.GameWorld(arm.forearm).t);
            if (orig_upper < 1e-3f || orig_forearm < 1e-3f) {
                return 0.0f;
            }
            float       upper_len   = orig_upper;
            float       forearm_len = orig_forearm;
            const float arm_length  = orig_upper + orig_forearm;

            const auto shoulder_to_hand = hand_pos - pose.GameWorld(arm.biceps).t;
            if (glm::length(shoulder_to_hand) > arm_length * 2.25f) {
                return 0.0f;
            }

            // Shoulder reach.
            if (arm.clavicle >= 0) {
                const float adjust = std::clamp(glm::length(shoulder_to_hand) - arm_length * 0.5f, 0.0f, arm_length * 0.85f) / (arm_length * 0.85f);
                auto        offset = glm::normalize(shoulder_to_hand) * (adjust * arm_length * 0.15f);
                if (offset.z < 0.0f) {
                    offset.z *= 0.4f;
                }
                const auto clavicle  = pose.GameWorld(arm.clavicle);
                const auto shoulder  = pose.GameWorld(arm.biceps).t;
                auto       clav_new  = clavicle;
                clav_new.r           = RotationBetween(shoulder - clavicle.t, shoulder + offset - clavicle.t) * clavicle.r;
                pose.SetGameWorld(arm.clavicle, clav_new);
            }

            const auto  upper_world = pose.GameWorld(arm.biceps);
            const auto  hand_to_shoulder = upper_world.t - hand_pos;
            const float hs_len      = std::max(glm::length(hand_to_shoulder), 0.1f * kUnit);

            // Flexion floor (10 degrees): stretch both bones rather than straighten the arm.
            const float min_flex = glm::radians(10.0f);
            const float reach_min = std::sqrt(upper_len * upper_len + forearm_len * forearm_len + 2.0f * upper_len * forearm_len * std::cos(min_flex));
            if (hs_len > reach_min) {
                const float stretch = hs_len / reach_min;
                upper_len *= stretch;
                forearm_len *= stretch;
            }
            // Flexion ceiling (145 degrees).
            const float max_flex      = glm::radians(145.0f);
            const float reach_max     = std::sqrt(std::max(upper_len * upper_len + forearm_len * forearm_len + 2.0f * upper_len * forearm_len * std::cos(max_flex), 0.0f));
            const float triangle_len  = std::max(hs_len, reach_max);

            // Wrist-driven elbow twist.
            const auto hand_back  = hand_rot * glm::vec3{ -1.0f, 0.0f, 0.0f };
            const auto hand_side  = hand_rot * glm::vec3{ 0.0f, -1.0f, 0.0f };
            float      twist      = std::asin(std::clamp(hand_back.z, -0.999f, 0.999f));
            const float twist2    = -std::asin(std::clamp(hand_side.z, -0.599f, 0.999f));
            const float interp    = std::clamp((hand_back.z + 0.866f) * 1.155f, 0.45f, 0.8f);
            twist                 = twist + interp * (twist2 - twist);
            // (fix143) Smoothed by the time passed, as FRIK does (1 - 0.75^(90 dt): a quarter at 90 frames a second); before, a quarter
            // each placement, faster at a high frame rate and slower at a low one, and twice in a frame placed twice.
            const auto now_arm = std::chrono::steady_clock::now();
            const float arm_dt = arm.has_time ? std::clamp(std::chrono::duration<float>(now_arm - arm.last_time).count(), 0.0f, 0.1f) : 1.0f / 90.0f;
            arm.last_time      = now_arm;
            arm.has_time       = true;
            twist                 = arm.twist_prev + (twist - arm.twist_prev) * (1.0f - std::pow(0.75f, arm_dt * 90.0f));
            arm.twist_prev        = twist;

            const auto shoulder_bone = arm.clavicle >= 0 ? pose.GameWorld(arm.clavicle).t : upper_world.t;
            const float behind_d      = -(forward.x * shoulder_bone.x + forward.y * shoulder_bone.y) - 10.0f * kUnit;
            const float hand_behind   = -(hand_pos.x * forward.x + hand_pos.y * forward.y + behind_d);
            const float behind_amount = std::clamp(hand_behind / (40.0f * kUnit), 0.0f, 1.0f);

            const float plane_angle = neg_left * glm::radians(135.0f);
            const auto  plane_dir   = glm::vec3{ forward.x * std::cos(plane_angle) - forward.y * std::sin(plane_angle),
                forward.x * std::sin(plane_angle) + forward.y * std::cos(plane_angle), 0.0f };
            const float plane_d     = -(plane_dir.x * shoulder_bone.x + plane_dir.y * shoulder_bone.y) + 16.0f * kUnit;
            const float cross_amount = std::clamp((hand_pos.x * plane_dir.x + hand_pos.y * plane_dir.y + plane_d) / (20.0f * kUnit), 0.0f, 1.0f);

            const float chest_z     = g_bones.chest >= 0 ? pose.GameWorld(g_bones.chest).t.z : shoulder_bone.z;
            const float lift_thresh = 60.0f * kUnit;
            const float lift_limit  = std::clamp((chest_z + lift_thresh - hand_pos.z) / lift_thresh, 0.0f, 1.0f);
            const float up_limit    = std::clamp((1.0f - lift_limit) * 1.4f, 0.0f, 1.0f);

            const float adjust_min = std::max(behind_amount, std::min(cross_amount, lift_limit));
            const float twist_min  = glm::radians(-85.0f) + glm::radians(50.0f) * adjust_min;
            float       twist_max  = glm::radians(55.0f) - std::max(glm::radians(90.0f) * cross_amount, glm::radians(70.0f) * up_limit);
            twist_max              = std::max(twist_max, twist_min + glm::radians(15.0f));
            const float twist_limit = twist_min + (twist + glm::half_pi<float>()) / glm::pi<float>() * (twist_max - twist_min);

            // Forward pitched about the right axis by the limit, then yawed outward and back.
            const auto bend_down = glm::mat3_cast(glm::angleAxis(twist_limit, sideways_r)) * forward;

            const float side_d       = -(sideways.x * shoulder_bone.x + sideways.y * shoulder_bone.y) - 8.0f * kUnit;
            float       across       = -(hand_pos.x * sideways.x + hand_pos.y * sideways.y + side_d) / (16.0f * kUnit);
            const float side_outward = glm::dot(hand_side, glm::normalize(sideways + forward * 0.5f));
            const float arm_twist    = std::clamp(side_outward - std::max(0.0f, across + 0.25f), 0.0f, 1.0f);
            if (across < 0.0f) {
                across *= 0.2f;
            }
            const float behind_head  = std::clamp(hand_behind / (15.0f * kUnit), 0.0f, 1.0f) * std::clamp(up_limit * 1.2f, 0.0f, 1.0f);
            // (fix158) At most a little over a hand's width across: further, the wanted elbow swept through the arm's own line and the
            // elbow flipped to the other side (the body bounced all around in place as if spinning, with the head turned from
            // the hands; a helper's research: 120-180 degree elbow turns).
            across = std::min(across, 1.3f);
            const float twist_fwd    = std::max(across * glm::radians(90.0f), behind_head * glm::radians(120.0f));
            const float yaw          = -neg_left * (glm::radians(150.0f) - arm_twist * glm::radians(25.0f) - twist_fwd);
            const auto  elbow_dir    = tracked_bend ? *tracked_bend
                                                    : glm::vec3{ bend_down.x * std::cos(yaw) - bend_down.y * std::sin(yaw),
                                                          bend_down.x * std::sin(yaw) + bend_down.y * std::cos(yaw), bend_down.z };

            const auto x_dir = glm::normalize(hand_to_shoulder);
            auto       y_dir = elbow_dir - x_dir * glm::dot(elbow_dir, x_dir);
            // (fix143) The side the elbow goes: the wanted direction off the shoulder-to-hand line. When that direction lies nearly along
            // the line (a hand low by the side, the wrist pointing down), what is left of it is short and swings with small hand
            // moves; then it leans on last frame's side (fully below 0.05 of its length, not at all from 0.35). And it turns at most
            // 360 degrees a second (not with tracked elbows, which are followed as they are).
            const float side_length = glm::length(y_dir);
            const int   arm_index   = is_left ? 0 : 1;
            g_steady.side_min[arm_index] = std::min(g_steady.side_min[arm_index], side_length);
            glm::vec3 previous{ 0.0f };
            bool      have_previous = false;
            if (arm.has_side) {
                // (Kept in the body's own frame: a snap turn turns it with the body.)
                const glm::vec3 kept = pose.root.r * arm.elbow_side;
                previous             = kept - x_dir * glm::dot(kept, x_dir);
                if (glm::length(previous) > 1e-3f) {
                    previous      = glm::normalize(previous);
                    have_previous = true;
                }
            }
            if (side_length < 1e-4f && !have_previous) {
                return 0.0f;
            }
            y_dir = side_length >= 1e-4f ? y_dir / side_length : previous;
            if (have_previous && !tracked_bend) {
                if (side_length < 0.35f) {
                    const float own = std::clamp((side_length - 0.05f) / 0.30f, 0.0f, 1.0f);
                    const auto  mix = previous * (1.0f - own) + y_dir * own;
                    y_dir           = glm::length(mix) > 1e-4f ? glm::normalize(mix) : previous;
                }
                const float step = std::acos(std::clamp(glm::dot(previous, y_dir), -1.0f, 1.0f));
                g_steady.elbow_step[arm_index] = std::max(g_steady.elbow_step[arm_index], glm::degrees(step));
                y_dir = TurnToward(previous, y_dir, glm::two_pi<float>() * arm_dt);
            }
            arm.elbow_side = glm::transpose(pose.root.r) * y_dir;
            arm.has_side   = true;

            float wrist_angle = std::acos(std::clamp((forearm_len * forearm_len + triangle_len * triangle_len - upper_len * upper_len) /
                                                         (2.0f * forearm_len * triangle_len), -1.0f, 1.0f));
            const auto elbow = hand_pos + x_dir * (std::cos(wrist_angle) * forearm_len) + y_dir * (std::sin(wrist_angle) * forearm_len);

            // Upper arm toward the elbow, then rolled about its length so the elbow hinge faces the forearm.
            auto upper_new = upper_world;
            upper_new.r    = RotationBetween(pose.GameWorld(arm.forearm).t - upper_world.t, elbow - upper_world.t) * upper_world.r;
            if (arm.has_bend) {
                const auto axis    = glm::normalize(elbow - upper_world.t);
                auto       current = upper_new.r * arm.bend_side;
                auto       wanted  = hand_pos - elbow;
                current -= glm::dot(current, axis) * axis;
                wanted -= glm::dot(wanted, axis) * axis;
                if (glm::length(current) > 1e-4f && glm::length(wanted) > 1e-4f) {
                    current          = glm::normalize(current);
                    wanted           = glm::normalize(wanted);
                    const float roll = std::atan2(glm::dot(glm::cross(current, wanted), axis), glm::dot(current, wanted));
                    upper_new.r      = glm::mat3_cast(glm::angleAxis(roll, axis)) * upper_new.r;
                }
            }
            pose.SetGameWorld(arm.biceps, upper_new);
            const float upper_scale = glm::length(elbow - upper_world.t) / orig_upper;
            if (std::abs(upper_scale - 1.0f) > 1e-3f) {
                pose.ScaleTranslation(arm.forearm, upper_scale);
            }

            // Forearm toward the hand.
            const auto forearm_now = pose.GameWorld(arm.forearm);
            auto       forearm_new = forearm_now;
            forearm_new.r          = RotationBetween(pose.GameWorld(arm.wrist).t - forearm_now.t, hand_pos - forearm_now.t) * forearm_now.r;
            pose.SetGameWorld(arm.forearm, forearm_new);
            const float forearm_scale = glm::length(hand_pos - forearm_now.t) / orig_forearm;
            if (std::abs(forearm_scale - 1.0f) > 1e-3f) {
                pose.ScaleTranslation(arm.wrist, forearm_scale);
            }

            // Forearm takes half of the wrist's added twist; the hand is then set exactly.
            {
                const auto forearm  = pose.GameWorld(arm.forearm);
                const auto axis_w   = glm::normalize(pose.GameWorld(arm.wrist).t - forearm.t);
                const auto relative = glm::transpose(forearm.r) * hand_rot * glm::transpose(FromNi(pose.local[arm.wrist]).r);
                const auto axis_l   = glm::transpose(forearm.r) * axis_w;
                const auto q        = glm::quat_cast(relative);
                const auto proj     = glm::dot(glm::vec3{ q.x, q.y, q.z }, axis_l) * axis_l;
                auto       twist_q  = glm::quat{ q.w, proj.x, proj.y, proj.z };
                if (glm::length(twist_q) > 1e-4f) {
                    twist_q = glm::normalize(twist_q);
                    // (fix143) The wrist's added twist as a signed turn about the forearm, kept on the side of last frame's (unwrapped:
                    // up to three quarters of a turn either way); half of it to the forearm. Before, half of the shortest turn: as the
                    // wrist passed half a turn, the forearm's share jumped from +90 to -90 degrees in one frame.
                    constexpr float pi   = glm::pi<float>();
                    float           turn = 2.0f * std::atan2(glm::dot(glm::vec3{ twist_q.x, twist_q.y, twist_q.z }, axis_l), twist_q.w);
                    while (turn > pi) {
                        turn -= 2.0f * pi;
                    }
                    while (turn <= -pi) {
                        turn += 2.0f * pi;
                    }
                    if (arm.has_turn) {
                        if (turn - arm.forearm_turn > pi) {
                            turn -= 2.0f * pi;
                        } else if (turn - arm.forearm_turn < -pi) {
                            turn += 2.0f * pi;
                        }
                        if (std::abs(turn) > 1.5f * pi) {
                            turn -= std::copysign(2.0f * pi, turn);
                        }
                    }
                    arm.forearm_turn = turn;
                    arm.has_turn     = true;
                    auto forearm_rot = forearm;
                    forearm_rot.r    = forearm.r * glm::mat3_cast(glm::angleAxis(turn * 0.5f, glm::normalize(axis_l)));
                    pose.SetGameWorld(arm.forearm, forearm_rot);
                }
            }
            auto hand_new = pose.GameWorld(arm.wrist);
            hand_new.r    = hand_rot;
            pose.SetGameWorld(arm.wrist, hand_new);
            return hs_len - reach_min;
        }

        // Two-bone leg IK as in FRIK's setSingleLeg: the foot is kept at its target, the knee bends toward `knee_dir`.
        void SolveLeg(const Pose& pose, const Bones::Leg& leg, const Xf& foot_target, glm::vec3 knee_dir)
        {
            const auto hip   = pose.GameWorld(leg.thigh);
            const auto knee  = pose.GameWorld(leg.calf);
            const auto foot  = pose.GameWorld(leg.foot);
            const float a    = glm::length(knee.t - hip.t);
            const float b    = glm::length(foot.t - knee.t);
            if (a < 1e-3f || b < 1e-3f) {
                return;
            }
            const auto  to_foot = foot_target.t - hip.t;
            const float c       = std::clamp(glm::length(to_foot), std::abs(a - b) + 1e-3f, a + b - 1e-3f);
            const auto  x_dir   = glm::normalize(to_foot);
            auto        y_dir   = knee_dir - glm::dot(knee_dir, x_dir) * x_dir;
            if (glm::length(y_dir) < 1e-4f) {
                return;
            }
            y_dir                = glm::normalize(y_dir);
            const float cos_a    = std::clamp((a * a + c * c - b * b) / (2.0f * a * c), -1.0f, 1.0f);
            const auto  knee_pos = hip.t + x_dir * (a * cos_a) + y_dir * (a * std::sqrt(1.0f - cos_a * cos_a));

            auto hip_new = hip;
            hip_new.r    = RotationBetween(knee.t - hip.t, knee_pos - hip.t) * hip.r;
            pose.SetGameWorld(leg.thigh, hip_new);

            const auto knee_now = pose.GameWorld(leg.calf);
            auto       knee_new = knee_now;
            knee_new.r          = RotationBetween(pose.GameWorld(leg.foot).t - knee_now.t, foot_target.t - knee_now.t) * knee_now.r;
            pose.SetGameWorld(leg.calf, knee_new);

            auto foot_new = pose.GameWorld(leg.foot);
            foot_new.r    = foot_target.r;
            pose.SetGameWorld(leg.foot, foot_new);
        }

        // Crouching. In first person the game crouches by lowering its camera (about 0.6 m); the third-person body
        // shown here does not play the crouch animation, so it is lowered to match: the pelvis drops by the camera's
        // drop and the legs bend to keep the feet planted. Should the body's own animation crouch further than the
        // camera, the view is lowered to the body's eyes instead (g_view_offset). Heights are measured above the feet
        // on the animated pose, before any IK.
        struct CrouchState
        {
            bool  has{ false };
            float head_stand{ 0.0f };  // animated head height above the feet while standing (smoothed)
            float eye_stand{ 0.0f };   // game camera height above the feet while standing
            bool  body_crouched{ false };
            float below_time{ 0.0f };
            float offset{ 0.0f };      // view offset (<= 0)
            float body_drop{ 0.0f };   // animated head below its standing height (negative = lower)
            float game_drop{ 0.0f };   // game camera below its standing height (<= 0)
            float pelvis_drop{ 0.0f }; // how far the body is lowered for the game's crouch (<= 0)
            float above_time{ 0.0f };  // how long the camera has been well above its standing height
            bool  has_time{ false };
            std::chrono::steady_clock::time_point time{};
        };
        CrouchState g_crouch;
        // (fix143) How fast the body walks (m/s, smoothed: UpdateWalk), for the crouch: on the move the game camera's dips (steps, landings) are
        // followed down gently, not snapped to.
        float g_body_speed{ 0.0f };

        void UpdateCrouch(const Pose& pose, float feet_z, float game_eye_z)
        {
            auto& c = g_crouch;
            if (g_bones.head < 0) {
                g_view_offset = 0.0f;
                c.pelvis_drop = 0.0f;
                return;
            }
            const auto  now = std::chrono::steady_clock::now();
            const float dt  = c.has_time ? std::clamp(std::chrono::duration<float>(now - c.time).count(), 0.0f, 0.1f) : 0.0f;
            c.time          = now;
            c.has_time      = true;

            const float head_h = pose.GameWorld(g_bones.head).t.z - feet_z;
            const float eye_h  = game_eye_z - feet_z;
            if (!c.has) {
                c.head_stand = head_h;
                c.eye_stand  = eye_h;
                c.has        = true;
            }
            // The game camera's height above the feet while standing: an average taken while it is near it (head
            // bob and small steps), so it cannot stick too high. Crouches and jumps are left out; if the camera
            // stays well above it for 2 s (a new height), that becomes the standing height.
            {
                const float diff = eye_h - c.eye_stand;
                if (std::abs(diff) < 0.15f) {
                    c.eye_stand += diff * std::min(1.0f, dt * 0.5f);
                }
                c.above_time = diff > 0.15f ? c.above_time + dt : 0.0f;
                if (c.above_time > 2.0f) {
                    c.eye_stand  = eye_h;
                    c.above_time = 0.0f;
                }
            }
            const float previous_drop = c.game_drop;
            c.game_drop = std::min(0.0f, eye_h - c.eye_stand);
            // (fix142) The game's crouch lowers its camera about 0.6 m; steps, landings and attacks dip it 0.1-0.4 m for a moment
            // (taken for crouches at 0.10 m: "crouching / standing again" several times a second). Crouched past 0.30 m, standing
            // again above 0.20 m. (The body follows the camera's dips either way, below; this is for the log and the animated head's
            // standing height, kept while crouched.)
            static bool crouched_state = false;
            crouched_state             = crouched_state ? c.game_drop < -0.20f : c.game_drop < -0.30f;
            const bool game_crouched   = crouched_state;
            // The animated head moves with idles and steps: its standing height is an average, taken while the
            // game is not crouching.
            if (!game_crouched) {
                c.head_stand += (head_h - c.head_stand) * std::min(1.0f, dt * 1.0f);
            }
            c.body_drop = head_h - c.head_stand;

            // Button crouch: the body is lowered with the game's camera every frame, by what its animation did not.
            // Going down it is never behind: the camera's movement over one more frame is added (this is read a
            // frame before the view is drawn). Going up it rises a little after the camera, so the view stays above
            // the neck either way. A few centimetres of head bob are eased out.
            // (fix142) The movement added is at most 3 cm: a crouch moves the camera 2-3 cm a frame, while a sudden dip (a step, a
            // landing, an attack's camera) came in one frame and was added again, the body dropping twice as far as the camera
            // and springing back (fix141's test: the camera 0.24 m lower, the body lowered 0.47 m; 0.37 m, 0.71 m).
            {
                const float speed     = dt > 0.0f ? (c.game_drop - previous_drop) / dt : 0.0f;
                const float ahead     = std::max(-0.03f, std::min(0.0f, speed) * std::clamp(dt, 0.0f, 0.06f));
                const float predicted = c.game_drop + ahead;
                constexpr float kBob  = 0.06f;
                const float t         = std::clamp(-predicted / kBob, 0.0f, 1.0f);
                const float follow    = predicted * (t * t * (3.0f - 2.0f * t));
                // (The body's own animated dip taken off from a 0.10 m drop, as before fix142: a landing's or an attack's, else the body
                // went down by both.)
                const float animated  = c.game_drop < -0.10f ? std::min(0.0f, c.body_drop) : 0.0f;
                const float target    = std::clamp(follow - animated, -1.2f, 0.0f);
                if (target < c.pelvis_drop && g_body_speed > 1.0f) {
                    c.pelvis_drop += (target - c.pelvis_drop) * (1.0f - std::exp(-dt / 0.1f)); // (fix143: on the move, eased down)
                } else if (target < c.pelvis_drop) {
                    c.pelvis_drop = target;
                } else {
                    c.pelvis_drop += (target - c.pelvis_drop) * (1.0f - std::exp(-dt / 0.08f));
                }
                if (target == 0.0f && std::abs(c.pelvis_drop) < 0.001f) {
                    c.pelvis_drop = 0.0f;
                }
            }

            // The body's own animation crouching below the camera (held for a moment, so a jump's dip is ignored).
            if (!c.body_crouched) {
                c.below_time = c.body_drop - c.game_drop < -0.20f ? c.below_time + dt : 0.0f;
                if (c.below_time > 0.25f) {
                    c.body_crouched = true;
                    spdlog::info("[Body] body crouched below the view: head {:.2f} m lower, game camera {:.2f} m lower", -c.body_drop, -c.game_drop);
                }
            } else if (c.body_drop - c.game_drop > -0.12f) {
                c.body_crouched = false;
                c.below_time    = 0.0f;
            }
            const float target = GameFlow::gStore.internalSettings.crouchView && c.body_crouched ? std::clamp(c.body_drop - c.game_drop, -1.2f, 0.0f) : 0.0f;
            c.offset += (target - c.offset) * (1.0f - std::exp(-dt / 0.12f));
            if (target == 0.0f && std::abs(c.offset) < 0.001f) {
                c.offset = 0.0f;
            }
            g_view_offset = c.offset;

            static bool was_crouched = false;
            if (game_crouched != was_crouched) {
                was_crouched = game_crouched;
                spdlog::info("[Body] {} (game camera {:.2f} m lower, body animation {:.2f} m lower -> body lowered {:.2f} m)",
                    game_crouched ? "crouching" : "standing again", -c.game_drop, -c.body_drop, -c.pelvis_drop);
            }
        }

        // Lowers the pelvis for the game's crouch and, with "Real crouch" on, for your real head being below where
        // you recentred; the legs bend to keep the feet where the animation planted them.
        void ApplyCrouch(const Pose& pose)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            const auto& l = g_bones.left_leg;
            const auto& r = g_bones.right_leg;
            if (!world_camera || !world_camera->parent || g_bones.pelvis < 0 || l.thigh < 0 || l.calf < 0 || l.foot < 0 || r.thigh < 0 ||
                r.calf < 0 || r.foot < 0) {
                return;
            }
            float drop = g_crouch.pelvis_drop;
            if (GameFlow::gStore.internalSettings.realCrouch) {
                // Real head below the recentre point (the view offset for crouching is not part of it).
                const float real = HeadPosition(world_camera).z - g_view_offset.load() - tracking::CameraRootPlace(world_camera->parent).z; // (fix147: as the head's)
                drop += std::min(0.0f, real);
            }
            drop = std::clamp(drop, -1.2f, 0.0f);
            if (drop > -0.005f) {
                return;
            }
            auto pelvis = pose.GameWorld(g_bones.pelvis);
            pelvis.t.z += drop;
            pose.SetGameWorld(g_bones.pelvis, pelvis);
        }

        // Procedural walking, after FRIK's walk(): when the body moves, the feet step in turn, each lifting along an
        // arc toward a target ahead of it while the other stays planted. FO4 units converted to metres (1/70).
        struct Walk
        {
            int       state{ 0 };     // 0 standing, 1 walking, 2 stopping, 3 re-target after deceleration
            int       stepping{ 0 };  // 1 right foot, 2 left foot
            glm::vec3 l_start{}, l_target{}, l_pos{}, r_start{}, r_target{}, r_pos{}, step_dir{};
            float     step_time_in_step{ 0.0f }, current_step_time{ 0.0f }, prev_speed{ 0.0f }, speed{ 0.0f };
            int       delay_frame{ 0 };
            bool      has_last{ false };
            glm::vec3 last_pos{};
            std::chrono::steady_clock::time_point last_time{};
            // (fix143) The engine frame last stepped (a second placement in the same frame steps nothing), the speed smoothed over
            // about 0.1 s, and since when it has been well below that (a real slow-down re-aims the step, a frame's dip does not).
            bool      has_frame{ false };
            uint32_t  frame{ 0 };
            float     smooth_speed{ 0.0f };
            std::chrono::steady_clock::time_point slow_since{};
            bool      slowing{ false };
        };
        Walk g_walk;
        // (fix143) The character's own place this frame (its root as the game moved it, before the body is moved under your eyes): the
        // walk's speed is taken from it. The body's place also moves with your head and its lean, which made the speed jump.
        glm::vec3 g_root_unshifted{ 0.0f };

        // (fix157) The walk turned about `pivot` (the character's place) by `angle` about the vertical: its planted spots, its step targets and
        // starts, the feet's places and its step direction (a snap turn).
        void TurnWalk(float angle, const glm::vec3& pivot)
        {
            auto&           w    = g_walk;
            const glm::mat3 turn = glm::mat3_cast(glm::angleAxis(angle, glm::vec3{ 0.0f, 0.0f, 1.0f }));
            const glm::vec3 p{ pivot.x, pivot.y, 0.0f };
            auto            at = [&](glm::vec3& v) {
                const float z = v.z;
                v             = p + turn * glm::vec3{ v.x - p.x, v.y - p.y, 0.0f };
                v.z           = z;
            };
            at(w.l_start);
            at(w.l_target);
            at(w.l_pos);
            at(w.r_start);
            at(w.r_target);
            at(w.r_pos);
            w.step_dir = turn * w.step_dir;
            static int told = 0;
            if (told++ < 10) {
                spdlog::info("[BodyIK] (fix157) the heading turned {:.0f} deg at once: the planted feet turned with you", glm::degrees(angle));
            }
        }

        void UpdateWalk(const glm::vec3& body_pos, const glm::vec3& l_anim, const glm::vec3& r_anim)
        {
            auto& w = g_walk;
            {
                static auto    vr    = VR::get();
                const uint32_t frame = vr->m_engine_frame_count.load();
                if (w.has_frame && frame == w.frame) {
                    return; // (placed again in the same frame: its steps stay as they are)
                }
                w.has_frame = true;
                w.frame     = frame;
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = w.has_last ? std::chrono::duration<float>(now - w.last_time).count() : 0.0f;
            (void)body_pos;
            glm::vec3 cur{ g_root_unshifted.x, g_root_unshifted.y, 0.0f };
            glm::vec3 dir = w.has_last ? cur - glm::vec3{ w.last_pos.x, w.last_pos.y, 0.0f } : glm::vec3{ 0.0f };
            w.last_pos  = cur;
            w.last_time = now;
            w.has_last  = true;
            if (dt <= 1e-4f || dt > 0.25f) {
                w.state = 0;
                w.l_pos = l_anim;
                w.r_pos = r_anim;
                w.smooth_speed = 0.0f;
                w.slowing      = false;
                return;
            }

            const float raw_speed = std::clamp(glm::length(dir) / dt, 0.0f, 5.0f);
            g_steady.speed_min    = std::min(g_steady.speed_min, raw_speed);
            g_steady.speed_max    = std::max(g_steady.speed_max, raw_speed);
            w.smooth_speed += (raw_speed - w.smooth_speed) * (1.0f - std::exp(-dt / 0.1f));
            const float speed = w.smooth_speed;
            g_body_speed      = speed;
            const float step_time = std::clamp(std::cos(speed / (140.0f * kUnit)), 0.28f, 0.50f);
            dir = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3{ 0.0f };
            // (fix143) Re-aimed after a slow-down only once the raw speed has stayed under 60% of the smoothed one for 0.1 s; before, any
            // frame 0.29 m/s slower than the one before (uneven frame times) cut the stepping foot's stride to a tenth: shuffling.
            if (speed > 20.0f * kUnit && raw_speed < 0.6f * speed) {
                if (!w.slowing) {
                    w.slowing    = true;
                    w.slow_since = now;
                } else if (now - w.slow_since >= std::chrono::milliseconds(100) && w.state == 1) {
                    w.state = 3;
                    ++g_steady.retargets;
                    w.slow_since = now; // (and again only after another 0.1 s of it)
                }
            } else {
                w.slowing = false;
            }
            w.prev_speed = speed;
            w.speed      = speed;

            switch (w.state) {
            case 0:
                if (speed >= 35.0f * kUnit) {
                    w.state             = 1;
                    w.stepping          = (std::rand() % 2) + 1;
                    w.step_dir          = dir;
                    w.step_time_in_step = step_time;
                    w.delay_frame       = 2;
                    w.l_start = w.l_target = w.l_pos = l_anim;
                    w.r_start = w.r_target = w.r_pos = r_anim;
                    if (w.stepping == 1) {
                        w.r_target = r_anim + w.step_dir * (speed * step_time * 1.5f);
                    } else {
                        w.l_target = l_anim + w.step_dir * (speed * step_time * 1.5f);
                    }
                    w.current_step_time = step_time / 2.0f;
                    break;
                }
                w.current_step_time = 0.0f;
                w.stepping          = 0;
                break;
            case 1:
                if (speed < 20.0f * kUnit) {
                    w.state             = 2;
                    w.current_step_time = 0.0f;
                }
                break;
            case 2:
                if (speed >= 20.0f * kUnit) {
                    w.state             = 1;
                    w.current_step_time = 0.0f;
                }
                break;
            case 3:
                w.step_dir = dir;
                if (w.stepping == 1) {
                    w.r_target = r_anim + w.step_dir * (speed * step_time * 0.1f);
                } else {
                    w.l_target = l_anim + w.step_dir * (speed * step_time * 0.1f);
                }
                w.state = 1;
                break;
            default:
                w.state = 0;
                break;
            }

            if (w.state == 0 || w.state == 2) {
                w.l_pos = l_anim;
                w.r_pos = r_anim;
                w.state = 0;
                return;
            }

            const float dot   = glm::dot(dir, w.step_dir);
            const float scale = std::min(speed * step_time * 1.5f, 140.0f * kUnit);
            const auto  dir_offset = (dir - w.step_dir) * scale;
            w.current_step_time += dt;
            const float interp = std::clamp(w.current_step_time / std::max(w.step_time_in_step, 1e-3f), 0.0f, 1.0f);

            auto step = [&](glm::vec3& target, glm::vec3& start, glm::vec3& pos, float ground) {
                if (dot < 0.9f) {
                    if (!w.delay_frame) {
                        target += dir_offset;
                        w.step_dir    = dir;
                        w.delay_frame = 2;
                    } else {
                        --w.delay_frame;
                    }
                } else {
                    w.delay_frame = w.delay_frame == 2 ? w.delay_frame : w.delay_frame + 1;
                }
                target.z = ground;
                start.z  = ground;
                pos      = start + (target - start) * interp;
                const float amount = std::clamp(glm::length(target - start) / (150.0f * kUnit), 0.0f, 1.0f);
                const float height = std::max(amount * 9.0f, 1.0f) * kUnit;
                pos.z += std::sin(interp * glm::pi<float>()) * height;
            };
            if (w.stepping == 1) {
                step(w.r_target, w.r_start, w.r_pos, r_anim.z);
                w.l_pos.z = l_anim.z;
            } else {
                step(w.l_target, w.l_start, w.l_pos, l_anim.z);
                w.r_pos.z = r_anim.z;
            }

            if (w.current_step_time > step_time) {
                w.current_step_time = 0.0f;
                w.step_dir          = dir;
                w.step_time_in_step = step_time;
                if (w.stepping == 1) {
                    w.stepping = 2;
                    w.l_target = l_anim + w.step_dir * scale;
                    w.l_start  = w.l_pos;
                } else {
                    w.stepping = 1;
                    w.r_target = r_anim + w.step_dir * scale;
                    w.r_start  = w.r_pos;
                }
            }
        }

        // Legs every frame: feet at their animated place when standing, stepping when the body moves.
        void ApplyLegs(const Pose& pose, const Xf& left_anim, const Xf& right_anim)
        {
            const auto& l = g_bones.left_leg;
            const auto& r = g_bones.right_leg;
            if (l.thigh < 0 || l.calf < 0 || l.foot < 0 || r.thigh < 0 || r.calf < 0 || r.foot < 0) {
                return;
            }
            UpdateWalk(pose.root.t, left_anim.t, right_anim.t);
            auto left  = left_anim;
            auto right = right_anim;
            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            forward      = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            glm::vec3 knee_dir[2] = { forward, forward };
            if (GameFlow::gStore.internalSettings.walkingLegs && g_walk.state == 1) {
                left.t  = g_walk.l_pos;
                right.t = g_walk.r_pos;
            } else if (g_tracked_body.full_body && !PlayerSeated() && g_bones.pelvis >= 0) {
                // Standing in place: feet placed where the tracked ankles are relative to the tracked hips, on the
                // animated ground height; knees bend toward the tracked knees.
                const auto hips = TrackedJoint(joint::kHips);
                const auto pelvis = pose.GameWorld(g_bones.pelvis).t;
                Xf* feet[2] = { &left, &right };
                bool placed = false;
                for (int side = 0; side < 2; ++side) {
                    const auto ankle = TrackedJoint(joint::kAnkle[side]);
                    if (!hips || !ankle) {
                        continue;
                    }
                    if (const auto offset = StageDirection(*ankle - *hips)) {
                        const float distance = glm::length(*ankle - *hips) * tracking::TrackingScale();
                        auto        target   = pelvis + *offset * distance;
                        target.z             = feet[side]->t.z;
                        feet[side]->t        = target;
                        placed               = true;
                    }
                    if (const auto bend = TrackedBend(joint::kUpperLeg[side], joint::kLowerLeg[side], joint::kAnkle[side])) {
                        knee_dir[side] = *bend;
                    }
                }
                g_track_diag.legs = placed;
            }
            SolveLeg(pose, l, left, knee_dir[0]);
            SolveLeg(pose, r, right, knee_dir[1]);
        }

        // Body yaw toward the hands, after FRIK's getNeckYaw: the torso turns 0.7 of the angle between the head's
        // facing and the direction between the hands (clamped to 50 degrees), less when a hand is above the head.
        float g_facing_yaw{ 0.0f };

        // (fix143) The turn wanted from the hands, as FRIK's getNeckYaw takes it: the hands seen from the head, a weight lowered for a hand
        // above the head (3.5 per metre above) and for hands crossed in front of you (1.4 per metre crossed, in the head's own left and
        // right); none with a hand within 0.14 m of the eyes. Two angles: the flat one (the hands' middle against the body's facing), and,
        // once the hands are far below where you look (more than 60-80 degrees below), how far to the side of straight down they are.
        // And it counts only as much as the hands are spread out in front (their sum, flat: nothing under 0.15 m, all of it from 0.40 m):
        // seated with the hands low by the body, the sum is a few centimetres long and its angle swung the body up to 35 degrees
        // within a few frames (fix142's test: the jitter, the odd walk, gone with the hands lifted forward).
        float FacingTarget(const glm::vec3& forward, const glm::vec3& eye, float& spread)
        {
            spread           = 0.0f;
            const auto left  = HandWorld(tracking::GripPose(true));
            const auto right = HandWorld(tracking::GripPose(false));
            const auto head  = HandWorld(tracking::HeadPose());
            if (!left || !right || !head) {
                return 0.0f;
            }
            const auto to_left  = left->position - eye;
            const auto to_right = right->position - eye;
            if (glm::length(to_left) < 0.14f || glm::length(to_right) < 0.14f) {
                return 0.0f;
            }
            float weight = 1.0f;
            if (to_left.z > 0.0f) weight = std::max(weight - 3.5f * to_left.z, 0.0f);
            if (to_right.z > 0.0f) weight = std::max(weight - 3.5f * to_right.z, 0.0f);
            // (The head's own right, flat: crossed hands have the left one further right than the right one.)
            glm::vec3 head_right = glm::cross(head->forward, glm::vec3{ 0.0f, 0.0f, 1.0f });
            head_right.z         = 0.0f;
            if (glm::length(head_right) > 1e-3f) {
                head_right        = glm::normalize(head_right);
                const float cross = glm::dot(to_left, head_right) - glm::dot(to_right, head_right);
                if (cross > 0.0f) {
                    weight = std::max(weight - 1.4f * cross, 0.0f);
                }
            }
            const auto sum = to_left + to_right;
            glm::vec3  flat{ sum.x, sum.y, 0.0f };
            spread = glm::length(flat);
            float flat_angle = 0.0f;
            if (spread > 1e-3f) {
                const auto f = flat / spread;
                flat_angle   = std::atan2(forward.x * f.y - forward.y * f.x, glm::dot(forward, f));
            }
            const float confidence = std::clamp((spread - 0.15f) / 0.25f, 0.0f, 1.0f);
            float       angle      = flat_angle * confidence * confidence * (3.0f - 2.0f * confidence);
            // The hands far below where you look: how far to the side of straight down (to the left positive, as the flat angle).
            const float look_pitch = std::asin(std::clamp(head->forward.z, -1.0f, 1.0f));
            const float sum_length = glm::length(sum);
            if (sum_length > 1e-3f && glm::length(head_right) > 1e-3f) {
                const float hands_pitch = std::asin(std::clamp(sum.z / sum_length, -1.0f, 1.0f));
                const float below       = look_pitch - hands_pitch;
                const float steep       = std::clamp((below - glm::radians(60.0f)) / glm::radians(20.0f), 0.0f, 1.0f);
                if (steep > 0.0f) {
                    const float down_angle = std::atan2(-glm::dot(sum, head_right), std::max(-sum.z, 1e-3f));
                    angle                  = angle * (1.0f - steep) + std::clamp(down_angle, glm::radians(-30.0f), glm::radians(30.0f)) * steep;
                }
            }
            return angle * weight;
        }

        float FacingYaw(const Pose& pose, const glm::vec3& eye)
        {
            auto  forward = pose.root.r[1];
            forward.z     = 0.0f;
            float target  = 0.0f;
            float spread  = 0.0f;
            if (glm::length(forward) > 1e-3f) {
                float raw = FacingTarget(glm::normalize(forward), eye, spread);
                if (!std::isfinite(raw)) {
                    raw = 0.0f; // (fix143's review: a bad number would have stayed in the turn for good)
                }
                // (fix158) Toward the hands, as far as 75 degrees from the head (FRIK's 0.7 of the hands' angle within 50 degrees was made for
                // players who turn with their hands; seated, turning only the head, the torso turned with it and ended up to 55 degrees off
                // the hands: the elbows flipped; a helper's research).
                target          = std::clamp(raw, glm::radians(-75.0f), glm::radians(75.0f));
                if (std::abs(raw) >= glm::radians(75.0f)) {
                    ++g_steady.facing_clamped;
                }
            }
            g_steady.facing_raw    = glm::degrees(target);
            g_steady.facing_spread = spread;
            // (fix143) Followed over about 0.12 s and at most 180 degrees a second, by the time passed (before: a quarter of the way each
            // placement, whatever the frame rate).
            static std::chrono::steady_clock::time_point last{};
            const auto  now  = std::chrono::steady_clock::now();
            const float dt   = last == std::chrono::steady_clock::time_point{} ? 1.0f / 90.0f
                                                                              : std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
            // (fix158) Held where it is in the world while only the head turns (the facing is the root's, which turns with the head): the
            // heading's own turn this frame taken back off before easing (a snap turn not: the hands turn with the room then).
            if (std::isfinite(g_heading_turn)) {
                g_facing_yaw = std::clamp(g_facing_yaw - g_heading_turn, glm::radians(-75.0f), glm::radians(75.0f));
            }
            last             = now;
            float step       = (target - g_facing_yaw) * (1.0f - std::exp(-dt / 0.12f));
            const float most = glm::pi<float>() * dt;
            step             = std::clamp(step, -most, most);
            g_facing_yaw += step;
            if (!std::isfinite(g_facing_yaw)) {
                g_facing_yaw = 0.0f;
            }
            g_steady.facing_step = std::max(g_steady.facing_step, std::abs(glm::degrees(step)));
            return g_facing_yaw;
        }

        // Support hand on the held weapon this frame.
        struct Support
        {
            bool held{ false };
            Xf   target{};
        };
        Support g_support;
        // Published for other threads (controller vibration).
        std::atomic<bool> g_support_on_gun{ false };

        // Body yaw from the tracked hips: their forward (from the hip line, or the shoulder line without legs).
        std::optional<float> TrackedFacingYaw(const Pose& pose)
        {
            auto side = g_tracked_body.full_body ? TrackedDirection(joint::kUpperLeg[0], joint::kUpperLeg[1]) : std::nullopt;
            if (!side) {
                side = TrackedDirection(joint::kArmUpper[0], joint::kArmUpper[1]);
            }
            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            if (!side || glm::length(forward) < 1e-3f) {
                return std::nullopt;
            }
            auto tracked = glm::cross(glm::vec3{ 0.0f, 0.0f, 1.0f }, *side);
            tracked.z    = 0.0f;
            if (glm::length(tracked) < 1e-3f) {
                return std::nullopt;
            }
            forward             = glm::normalize(forward);
            tracked             = glm::normalize(tracked);
            const float target  = std::atan2(forward.x * tracked.y - forward.y * tracked.x, glm::dot(forward, tracked));
            g_facing_yaw += (target - g_facing_yaw) * 0.25f;
            return g_facing_yaw;
        }

        // Horizontal offset of the head from the neck that the spine absorbs this frame (the rest moves the body).
        glm::vec3 g_lean{ 0.0f };
        float     g_lean_applied{ 0.0f };

        // Spine lean: the bones between pelvis and neck share the rotation that brings the neck to its target, so
        // the hips stay planted (FRIK's posture bends the spine toward the head the same way).
        void ApplyLean(const Pose& pose)
        {
            g_lean_applied = 0.0f;
            if (!GameFlow::gStore.internalSettings.bodyLean || g_bones.spine.empty() || g_bones.neck < 0 || glm::length(g_lean) < 0.005f) {
                return;
            }
            const auto target = pose.GameWorld(g_bones.neck).t + g_lean;
            const auto count  = static_cast<int>(g_bones.spine.size());
            for (int k = 0; k < count; ++k) {
                const int  bone   = g_bones.spine[k];
                const auto world  = pose.GameWorld(bone);
                const auto neck   = pose.GameWorld(g_bones.neck).t;
                const auto turn   = glm::quat_cast(RotationBetween(neck - world.t, target - world.t));
                const auto share  = glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, 1.0f / static_cast<float>(count - k));
                auto       bent   = world;
                bent.r            = glm::mat3_cast(share) * world.r;
                pose.SetGameWorld(bone, bent);
            }
            g_lean_applied = glm::length(g_lean);
        }

        // (fix157) While moving: the hands' turn about the vertical shared out over the spine (each bone a part, as the tracked torso's), the
        // hips left facing the way you go.
        void ApplySpineTwist(const Pose& pose)
        {
            if (g_bones.spine.empty() || !(std::abs(g_spine_twist) > 1e-4f) || !std::isfinite(g_spine_twist)) {
                return;
            }
            const auto count = static_cast<float>(g_bones.spine.size());
            const auto share = glm::mat3_cast(glm::angleAxis(g_spine_twist / count, glm::vec3{ 0.0f, 0.0f, 1.0f }));
            for (const int bone : g_bones.spine) {
                auto bent = pose.GameWorld(bone);
                bent.r    = share * bent.r;
                pose.SetGameWorld(bone, bent);
            }
        }

        // Torso from body tracking: the spine bones share the rotation that turns the body's hips-to-neck axis and
        // shoulder line onto the tracked ones.
        bool ApplyTrackedTorso(const Pose& pose)
        {
            if (g_bones.spine.empty() || g_bones.neck < 0 || g_bones.pelvis < 0 || g_bones.right.biceps < 0 || g_bones.left.biceps < 0) {
                return false;
            }
            const auto up_t   = TrackedDirection(joint::kHips, joint::kNeck);
            const auto side_t = TrackedDirection(joint::kArmUpper[0], joint::kArmUpper[1]);
            if (!up_t || !side_t) {
                return false;
            }
            const auto up_g   = glm::normalize(pose.GameWorld(g_bones.neck).t - pose.GameWorld(g_bones.pelvis).t);
            const auto side_g = glm::normalize(pose.GameWorld(g_bones.right.biceps).t - pose.GameWorld(g_bones.left.biceps).t);
            auto frame = [](const glm::vec3& up, const glm::vec3& side) { return FrameOf(glm::cross(up, side), up); };
            const auto turn  = glm::quat_cast(frame(*up_t, *side_t) * glm::transpose(frame(up_g, side_g)));
            const auto count = static_cast<int>(g_bones.spine.size());
            const auto share = glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, 1.0f / static_cast<float>(count)));
            for (const int bone : g_bones.spine) {
                auto bent = pose.GameWorld(bone);
                bent.r    = share * bent.r;
                pose.SetGameWorld(bone, bent);
            }
            g_track_diag.torso = true;
            return true;
        }

        // Finger poses. The side the fingers curl toward (the palm) is measured from the animated hand.
        struct FingerPalm
        {
            bool      valid{ false };
            glm::vec3 palm{};  // wrist space
        };
        FingerPalm g_finger_palm[2];

        void MeasurePalmSide(const Pose& pose, int side, const HandShape& shape, int wrist)
        {
            auto& out = g_finger_palm[side];
            if (out.valid || !shape.valid || wrist < 0) {
                return;
            }
            const auto w      = pose.GameWorld(wrist);
            const auto normal = glm::normalize(glm::cross(shape.forward, shape.up));  // wrist space, one of the palm/back sides
            float      score  = 0.0f;
            for (int f = 1; f < 5; ++f) {
                const auto& chain = g_bones.fingers[side][f];
                if (chain[0] < 0 || chain[2] < 0) {
                    continue;
                }
                const auto bend = glm::transpose(w.r) * (pose.GameWorld(chain[2]).t - pose.GameWorld(chain[0]).t) / w.s;
                score += glm::dot(bend, normal);
            }
            if (std::abs(score) < 0.005f) {
                return;  // fingers straight in this pose; measure later
            }
            out.palm  = score > 0.0f ? normal : -normal;
            out.valid = true;
            spdlog::info("[BodyIK] {} palm side ({:.2f},{:.2f},{:.2f}) in wrist space", side == 0 ? "left" : "right", out.palm.x, out.palm.y, out.palm.z);
        }

        // Where this hand holds things (hand grab), in its controller's aim frame: measured once from the hand's own bones.
        struct GripFrameStore
        {
            std::atomic<bool> valid{ false };
            HandGripFrame     frame{};
        };
        GripFrameStore g_grip_frame[2];

        void MeasureGripFrame(const Pose& pose, int side, const Arm& arm)
        {
            auto& store = g_grip_frame[side];
            if (store.valid.load(std::memory_order_relaxed) || !arm.shape.valid || !g_finger_palm[side].valid || arm.wrist < 0) {
                return;
            }
            const auto w = pose.GameWorld(arm.wrist);
            if (!(w.s > 0.01f)) {
                return;
            }
            // The four knuckles (finger bases) in the wrist's own space.
            const glm::mat3 to_wrist = glm::transpose(w.r);
            glm::vec3       base[5]{};
            for (int f = 1; f < 5; ++f) {
                const auto& chain = g_bones.fingers[side][f];
                if (chain[0] < 0 || chain[1] < 0) {
                    return;
                }
                base[f] = to_wrist * (pose.GameWorld(chain[0]).t - w.t) / w.s;
            }
            const auto& middle  = g_bones.fingers[side][2];
            const float phalanx = glm::length(pose.GameWorld(middle[1]).t - pose.GameWorld(middle[0]).t) / w.s; // its first bone
            const glm::vec3 knuckles_mid = 0.25f * (base[1] + base[2] + base[3] + base[4]);
            const float     reach        = glm::length(knuckles_mid);
            if (reach < 0.03f || phalanx < 0.01f) {
                return;
            }
            const glm::vec3 facing  = glm::normalize(g_finger_palm[side].palm);
            const glm::vec3 fingers = glm::normalize(arm.shape.forward);
            glm::vec3       line    = base[1] - base[4]; // index to little
            line -= facing * glm::dot(line, facing);
            if (glm::length(line) < 0.01f) {
                return;
            }
            line = glm::normalize(line);
            // The palm's skin under the knuckles: the joints sit inside the hand, about a quarter of the first finger bone from
            // the skin. A held object rests a little short of the knuckles, on the palm's last crease (HIGGS's palm point is at
            // the base of the fingers, 8.6 cm along them from the wrist).
            const float     skin    = std::clamp(0.25f * phalanx, 0.006f, 0.02f);
            const glm::vec3 contact = knuckles_mid + facing * skin - fingers * (0.15f * reach);
            // The body places the wrist so that wrist + forward x palm is on the grip point, forward along the aim's -z and
            // the thumb side (up) along its +y (WristTarget): a wrist-space vector v is (right.v, up.v, -forward.v) there.
            const glm::vec3 right = glm::normalize(glm::cross(fingers, arm.shape.up));
            const glm::vec3 up    = glm::cross(right, fingers);
            auto            aim   = [&](const glm::vec3& v) { return glm::vec3{ glm::dot(right, v), glm::dot(up, v), -glm::dot(fingers, v) }; };
            const glm::vec3 at    = aim(contact * w.s - fingers * arm.shape.palm);
            const glm::vec3 vecs[4] = { at, aim(facing), aim(fingers), aim(line) };
            float* outs[4] = { store.frame.contact, store.frame.facing, store.frame.fingers, store.frame.knuckles };
            for (int i = 0; i < 4; ++i) {
                if (!std::isfinite(vecs[i].x) || !std::isfinite(vecs[i].y) || !std::isfinite(vecs[i].z)) {
                    return;
                }
                outs[i][0] = vecs[i].x;
                outs[i][1] = vecs[i].y;
                outs[i][2] = vecs[i].z;
            }
            store.valid.store(true, std::memory_order_release);
            spdlog::info("[BodyIK] {} hand holds things at ({:.3f},{:.3f},{:.3f}) m from its grip point in the aim's frame (knuckles {:.3f} m "
                         "from the wrist, palm skin {:.3f} m under them), palm facing ({:.2f},{:.2f},{:.2f}), knuckle line ({:.2f},{:.2f},{:.2f})",
                side == 0 ? "left" : "right", at.x, at.y, at.z, reach * w.s, skin * w.s, vecs[1].x, vecs[1].y, vecs[1].z, vecs[3].x, vecs[3].y,
                vecs[3].z);
        }

        bool PalmOf(bool left, const HandTarget& grip, const HandTarget& aim, glm::vec3& contact, glm::vec3& facing, glm::vec3& fingers, glm::vec3& across)
        {
            const auto& store = g_grip_frame[left ? 0 : 1];
            if (!store.valid.load(std::memory_order_acquire)) {
                return false;
            }
            const HandGripFrame& frame = store.frame;
            // The aim's frame as WristTarget turns the hand to it (FrameOf): right = forward x up, up made square to forward; a vector v of
            // the grip frame is (right.v, up.v, -forward.v) there (MeasureGripFrame).
            const glm::vec3 f = glm::normalize(aim.forward);
            glm::vec3       r = glm::cross(f, aim.up);
            if (!(glm::length(r) > 1e-4f) || !std::isfinite(f.x)) {
                return false;
            }
            r                 = glm::normalize(r);
            const glm::vec3 u = glm::cross(r, f);
            const auto world  = [&](const float* v) { return r * v[0] + u * v[1] - f * v[2]; };
            contact           = grip.position + world(frame.contact);
            facing            = world(frame.facing);
            across            = world(frame.knuckles);
            if (!(glm::length(facing) > 1e-3f)) {
                return false;
            }
            facing = glm::normalize(facing);
            across -= facing * glm::dot(across, facing);
            if (!(glm::length(across) > 1e-3f)) {
                return false;
            }
            across  = glm::normalize(across);
            fingers = glm::normalize(glm::cross(across, facing));
            if (glm::dot(fingers, world(frame.fingers)) < 0.0f) {
                fingers = -fingers;
            }
            return std::isfinite(contact.x) && std::isfinite(contact.y) && std::isfinite(contact.z) && std::isfinite(fingers.x);
        }

        // (fix181) Where the hand's palm is as drawn now (after the arm and the fingers were posed): the same point the grip frame was
        // measured at (MeasureGripFrame: the palm's skin under the knuckles, a little short of them), from the posed bones themselves. Then,
        // for the log, how far it landed from the palm locked on the person it holds and from their bare body (the frames drawn wholly locked).
        void NoteDrawnPalm(const Pose& pose, int side, const Arm& arm)
        {
            const HandLock& lock = g_person_lock[side];
            auto&           st   = g_person_hand[side];
            if (!lock.ok || lock.w < 0.99f || !g_lock_palm_ok[side] || st.grab != lock.key || g_person_held_grab[side] != lock.key || !arm.shape.valid ||
                !g_finger_palm[side].valid || arm.wrist < 0) {
                return;
            }
            const auto w = pose.GameWorld(arm.wrist);
            if (!(w.s > 0.01f)) {
                return;
            }
            glm::vec3 mid{ 0.0f };
            for (int f = 1; f < 5; ++f) {
                const auto& chain = g_bones.fingers[side][f];
                if (chain[0] < 0) {
                    return;
                }
                mid += 0.25f * pose.GameWorld(chain[0]).t;
            }
            const auto& middle = g_bones.fingers[side][2];
            if (middle[0] < 0 || middle[1] < 0) {
                return;
            }
            const float     phalanx = glm::length(pose.GameWorld(middle[1]).t - pose.GameWorld(middle[0]).t) / w.s;
            const float     reach   = glm::length(mid - w.t);
            const glm::vec3 facing  = glm::normalize(w.r * g_finger_palm[side].palm);
            const glm::vec3 fingers = glm::normalize(w.r * arm.shape.forward);
            const float     skin    = std::clamp(0.25f * phalanx, 0.006f, 0.02f) * w.s;
            const glm::vec3 palm    = mid + facing * skin - fingers * (0.15f * reach);
            if (!std::isfinite(palm.x) || !std::isfinite(palm.y) || !std::isfinite(palm.z)) {
                return;
            }
            const auto&     held  = g_person_held_now[side];
            const float     off   = glm::length(palm - g_lock_palm_now[side]);
            const glm::vec3 q     = ClosestOnLine(palm, held.cap_a[0], held.cap_b[0]);
            const float     bare  = glm::length(palm - q) - held.bare_r;
            ++st.landed_n;
            st.landed_sum += off;
            st.landed_max = std::max(st.landed_max, off);
            st.bare_sum += bare;
            st.bare_min = std::min(st.bare_min, bare);
            st.bare_max = std::max(st.bare_max, bare);
        }

        // Bends a finger chain toward the palm: base and middle joints by the given angles.
        void CurlFinger(const Pose& pose, const std::array<int, 3>& chain, int wrist, const glm::vec3& palm_l, float base_deg, float mid_deg)
        {
            if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0) {
                return;
            }
            const auto wrist_w = pose.GameWorld(wrist);
            const auto palm    = glm::normalize(wrist_w.r * palm_l);
            glm::vec3  parent_dir{ 0.0f };
            {
                const auto base = pose.GameWorld(chain[0]);
                parent_dir      = glm::normalize(base.t - wrist_w.t);
            }
            const float angles[2] = { glm::radians(base_deg), glm::radians(mid_deg) };
            for (int j = 0; j < 2; ++j) {
                const auto joint = pose.GameWorld(chain[j]);
                const auto child = pose.GameWorld(chain[j + 1]).t;
                const auto dir   = child - joint.t;
                if (glm::length(dir) < 1e-4f) {
                    return;
                }
                auto axis = glm::cross(parent_dir, palm);
                if (glm::length(axis) < 1e-4f) {
                    return;
                }
                axis              = glm::normalize(axis);
                const auto wanted = glm::mat3_cast(glm::angleAxis(angles[j], axis)) * parent_dir;
                auto       posed  = joint;
                posed.r           = RotationBetween(dir, wanted) * joint.r;
                pose.SetGameWorld(chain[j], posed);
                parent_dir = glm::normalize(wanted);
            }
        }

        // (fix173) A finger posed as FRIK poses one (Fallout 4 VR: each joint blended from a relaxed open hand to a fist by the trigger or
        // the grip; HandPose.cpp / HandPoseMath.cpp): after fix172 the fingers still looked stringy (stereo_1_left.jpg 23:12: both open
        // hands' fingers fanned out wide, straight and tilted back). CurlFinger aimed each finger along the line from the wrist to its
        // knuckle: those lines fan out from the wrist (and rise to the knuckles), so the fingers splayed like a star and bent back; the
        // last bone kept the animation's own bend and the rest bent 10 degrees. Here the finger starts from the hand's own forward line
        // flattened onto the palm, turned out by only part of its bone's fan (more open, less as it closes), and all three joints bend
        // toward the palm in one plane, each by its own angle between the open and the closed pose.
        struct FingerAngles
        {
            float open[3];   // degrees at the knuckle, the middle and the last joint: a relaxed open hand
            float closed[3]; // ...a fist
        };
        // A relaxed open hand bends a little, more toward the little finger; a fist closes the knuckles and middle joints about 90 degrees.
        constexpr FingerAngles kIndexAngles{ { 8.0f, 12.0f, 6.0f }, { 80.0f, 100.0f, 60.0f } };
        constexpr FingerAngles kMiddleAngles{ { 10.0f, 16.0f, 8.0f }, { 85.0f, 100.0f, 65.0f } };
        constexpr FingerAngles kRingAngles{ { 12.0f, 20.0f, 10.0f }, { 88.0f, 100.0f, 65.0f } };
        constexpr FingerAngles kLittleAngles{ { 14.0f, 24.0f, 12.0f }, { 90.0f, 95.0f, 60.0f } };
        constexpr float kOpenFan   = 0.4f;  // the share of its bone's fan an open finger is turned out by
        constexpr float kClosedFan = 0.15f; // ...a closed one

        void PoseFinger(const Pose& pose, const std::array<int, 3>& chain, int wrist, const glm::vec3& palm_l, const glm::vec3& forward_l, float curl,
            const FingerAngles& angles)
        {
            if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0 || wrist < 0) {
                return;
            }
            curl                = std::clamp(curl, 0.0f, 1.0f);
            const auto wrist_w  = pose.GameWorld(wrist);
            glm::vec3  palm     = wrist_w.r * palm_l;
            glm::vec3  forward  = wrist_w.r * forward_l;
            if (glm::length(palm) < 1e-4f || glm::length(forward) < 1e-4f) {
                return;
            }
            palm = glm::normalize(palm);
            forward -= palm * glm::dot(forward, palm);
            if (glm::length(forward) < 1e-4f) {
                return;
            }
            forward = glm::normalize(forward);
            // The finger's own share of the fan: its knuckle's line from the wrist, flattened onto the palm, against the hand's forward.
            glm::vec3 meta = pose.GameWorld(chain[0]).t - wrist_w.t;
            meta -= palm * glm::dot(meta, palm);
            float fan = 0.0f;
            if (glm::length(meta) > 1e-4f) {
                meta = glm::normalize(meta);
                fan  = std::atan2(glm::dot(glm::cross(forward, meta), palm), glm::dot(forward, meta));
            }
            const float     spread = fan * (kOpenFan + (kClosedFan - kOpenFan) * curl);
            const glm::vec3 line   = glm::mat3_cast(glm::angleAxis(spread, palm)) * forward;
            const glm::vec3 axis   = glm::cross(line, palm); // (turning `line` about it by a positive angle bends it toward the palm)
            if (glm::length(axis) < 1e-4f) {
                return;
            }
            const glm::vec3 bend = glm::normalize(axis);
            // The bones' own long axis in their own frames (the rig's convention, measured on the middle bone), for the last bone, which
            // has no child in the chain to aim at.
            glm::vec3 long_l{ 0.0f };
            {
                const auto middle = pose.GameWorld(chain[1]);
                const auto to     = pose.GameWorld(chain[2]).t - middle.t;
                if (glm::length(to) < 1e-5f) {
                    return;
                }
                long_l = glm::transpose(middle.r) * glm::normalize(to);
            }
            float total = 0.0f;
            for (int j = 0; j < 3; ++j) {
                total += glm::radians(angles.open[j] + (angles.closed[j] - angles.open[j]) * curl);
                const glm::vec3 wanted = glm::mat3_cast(glm::angleAxis(total, bend)) * line;
                const auto      joint  = pose.GameWorld(chain[j]);
                const glm::vec3 now    = j < 2 ? pose.GameWorld(chain[j + 1]).t - joint.t : joint.r * long_l;
                if (glm::length(now) < 1e-5f) {
                    return;
                }
                auto posed = joint;
                posed.r    = RotationBetween(now, wanted) * joint.r;
                pose.SetGameWorld(chain[j], posed);
            }
        }

        struct FingerDiag
        {
            float trigger{ 0.0f };
            float grip{ 0.0f };
            bool  thumb{ false };
            int   copied{ 0 };
        };
        FingerDiag g_finger_diag[2];

        struct FingerSprings
        {
            float                                 index{ 0.0f };
            float                                 grip{ 0.0f };
            float                                 thumb{ 0.0f };
            bool                                  has_time{ false };
            std::chrono::steady_clock::time_point time{};
        };
        FingerSprings g_finger_springs[2];


        // Measures the hand frame from the animated pose (before any write this frame).
        void MeasureElbow(const Pose& pose, Arm& arm, const char* label)
        {
            if (arm.has_bend || arm.biceps < 0 || arm.forearm < 0 || arm.wrist < 0) {
                return;
            }
            const auto upper   = pose.GameWorld(arm.biceps);
            const auto forearm = pose.GameWorld(arm.forearm).t;
            const auto wrist   = pose.GameWorld(arm.wrist).t;
            const auto axis    = glm::normalize(forearm - upper.t);
            auto       side    = wrist - forearm;
            side -= glm::dot(side, axis) * axis;
            if (glm::length(side) < 0.03f) {
                return;  // elbow too straight in this pose to tell the hinge
            }
            arm.bend_side = glm::normalize(glm::transpose(upper.r) * glm::normalize(side));
            arm.has_bend  = true;
            spdlog::info("[BodyIK] {} elbow bends toward ({:.2f},{:.2f},{:.2f}) in upper-arm space", label, arm.bend_side.x, arm.bend_side.y,
                arm.bend_side.z);
        }

        void MeasureHand(const Pose& pose, Arm& arm, const char* label)
        {
            if (arm.shape.valid || arm.wrist < 0 || arm.thumb < 0 || arm.fingers.empty()) {
                return;
            }
            const auto wrist = pose.GameWorld(arm.wrist);
            glm::vec3  bases{ 0.0f };
            for (const int finger : arm.fingers) {
                bases += pose.GameWorld(finger).t;
            }
            bases /= static_cast<float>(arm.fingers.size());
            const auto to_bases = bases - wrist.t;
            const auto to_thumb = pose.GameWorld(arm.thumb).t - wrist.t;
            if (glm::length(to_bases) < 0.02f) {
                return;
            }
            const auto forward = glm::normalize(to_bases);
            auto       up      = to_thumb - glm::dot(to_thumb, forward) * forward;
            if (glm::length(up) < 0.005f) {
                return;
            }
            up               = glm::normalize(up);
            const auto to_l  = glm::transpose(wrist.r);
            arm.shape.forward = glm::normalize(to_l * forward);
            arm.shape.up      = glm::normalize(to_l * up);
            arm.shape.palm    = 0.5f * glm::length(to_bases);
            arm.shape.valid   = true;
            spdlog::info("[BodyIK] {} hand measured: forward ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) in wrist space, palm {:.3f} m", label,
                arm.shape.forward.x, arm.shape.forward.y, arm.shape.forward.z, arm.shape.up.x, arm.shape.up.y, arm.shape.up.z, arm.shape.palm);
        }

        // Wrist world transform that puts the hand's palm centre on the controller's grip point, fingers along
        // the grip's -Z (index finger direction) and the thumb side along +Y.
        Xf WristTarget(const Pose& pose, const Arm& arm, const HandTarget& aim, const HandTarget& grip)
        {
            Xf target = pose.GameWorld(arm.wrist);
            target.r  = FrameOf(aim.forward, aim.up) * glm::transpose(FrameOf(arm.shape.forward, arm.shape.up));
            target.t  = grip.position - aim.forward * arm.shape.palm;
            return target;
        }

        struct Diagnostics
        {
            float right_gap{ 0.0f };
            float left_gap{ 0.0f };
            bool  weapon_aligned{ false };
            float support_distance{ -1.0f };
            bool  support_held{ false };
        };
        Diagnostics g_diag;

        float ArmGap(int hand) { return hand == 0 ? g_diag.left_gap : g_diag.right_gap; }

        // Shots leave along the muzzle's local +Y (ComputeLaunchOrigin 0x1b52e8a). The muzzle's transform within the
        // weapon is read from the engine's final worlds after each update and used to aim the weapon exactly.
        struct Barrel
        {
            RE::NiAVObject* weapon{ nullptr };
            RE::NiAVObject* muzzle{ nullptr };
            bool            has_relation{ false };
            glm::vec3       shot_in_weapon{ 0.0f, 1.0f, 0.0f };
            bool            placed{ false };
            Xf              placed_weapon{};
            glm::vec3       aim_forward{ 0.0f, 1.0f, 0.0f };
            float           error_deg{ 0.0f };
            float           placement_error{ 0.0f };
            // (fix146) The muzzle in the weapon's own space (both as last drawn, the same frame): with this frame's placing of the weapon it
            // gives the muzzle where the gun is drawn now (the laser's start).
            Xf              muzzle_in_weapon{};
            bool            has_muzzle_in_weapon{ false };
        };
        Barrel g_barrel;

        // How the game holds the weapon in first person: its barrel at the crosshair and level with the view. The
        // view's forward and up in the weapon's space give the weapon's basis (ROCK derives its controller-to-weapon
        // basis from the game's own hold the same way). Sampled when the player fires; a slow estimate before that.
        // The camera parent frame is havok-space: +Y forward, +Z up.
        struct WeaponBasis
        {
            RE::NiAVObject* weapon{ nullptr };
            bool            has{ false };
            bool            fired{ false };
            glm::vec3       forward{ 0.0f, 1.0f, 0.0f };
            glm::vec3       up{ 0.0f, 0.0f, 1.0f };
            // (fix132) The gun's own muzzle: the barrel taken from it once it has settled (frames seen), kept for that gun.
            RE::NiAVObject* muzzle{ nullptr };
            int             muzzle_frames{ 0 };
            bool            from_muzzle{ false };
            // (fix135) Since when the weapon out has been a gun by its read type (the muzzle taken only after a quarter second of it).
            std::chrono::steady_clock::time_point gun_since{};
            // (fix195) The barrel from the model's own transforms is in use this frame but not kept yet (the estimate starts over if it
            // stops before it is kept).
            bool provisional{ false };
            // (fix206) A multi-beam weapon (the cutter: ProjectileNodeChild nodes under its ProjectileNode, its three lenses): its
            // ProjectileNode is not its barrel (in first person the game swings that node up to about 40 degrees while it fires, Homard's
            // Starfield VR; ours sits under a dummy receiver), so the beam and the laser, which go along that node's axis, left the cutter's
            // lenses at a slant. It is held instead the way the game holds it while firing (Homard's "natural grip"), learned once on a
            // pull, eased in, and kept: the beam and the laser then go where the cutter points. Its ProjectileNode's axis is the start.
            bool      multi_beam{ false };
            bool      multi_beam_checked{ false }; // (its muzzle looked at once)
            bool      axis_kept{ false };          // its ProjectileNode's axis (with the game's up square to it) kept as the start
            glm::vec3 axis_forward{ 0.0f, 1.0f, 0.0f };
            glm::vec3 axis_up{ 0.0f, 0.0f, 1.0f };
            bool      hold_learned{ false };       // its firing hold learned on a pull (or kept from an earlier draw of this weapon)
            glm::vec3 hold_forward{ 0.0f, 1.0f, 0.0f };
            glm::vec3 hold_up{ 0.0f, 0.0f, 1.0f };
            std::chrono::steady_clock::time_point pull_start{}, last_fire{}, last_ease{};
            bool      window_done{ false };        // this pull's learning window over
            glm::vec3 sum_forward{ 0.0f }, sum_up{ 0.0f };
            int       samples{ 0 }, refused{ 0 };
        };
        WeaponBasis       g_basis;
        std::atomic<bool> g_fire_sample{ false };
        // (fix206) A continuous beam's frame fired from the gun in your hand (MuzzleFire, NotifyBeamFrame): the only frames a multi-beam
        // weapon's firing hold is learned on (an automatic gun's recoiling fire hold is never taken, the review).
        std::atomic<bool> g_beam_sample{ false };

        // (fix206) A multi-beam weapon's firing hold, kept by its form for this game session (a redraw or a load makes a new model and starts
        // the basis over: the hold learned for the same weapon is used again at once). In the weapon's own space.
        struct LearnedHold
        {
            glm::vec3 forward{ 0.0f, 1.0f, 0.0f };
            glm::vec3 up{ 0.0f, 0.0f, 1.0f };
        };
        std::unordered_map<std::uint32_t, LearnedHold> g_learned_holds; // (the body's update only, as g_basis)
        constexpr float kMultiBeamHoldLimitDeg = 25.0f;                // a firing hold further than this from the node's axis: not taken
        constexpr auto  kHoldWindowStart       = std::chrono::milliseconds(150);
        constexpr auto  kHoldWindowEnd         = std::chrono::milliseconds(400);
        constexpr int   kHoldMinSamples        = 5;
        constexpr float kHoldEaseSeconds       = 0.25f; // (the gun turns toward the learned hold with this time constant: ~95% in 0.75 s)

        // The first-person hand's anatomical frame (fingers forward, thumb up, palm centre) in its wrist's space, so a
        // grip held by that hand can be transferred onto the body's hand whatever the two skeletons' bone axes.
        struct FirstPersonHand
        {
            RE::NiAVObject* wrist{ nullptr };
            HandShape       shape;
        };
        FirstPersonHand g_first_hands[2];  // left, right

        void MeasureFirstPersonHand(RE::NiAVObject* wrist, bool left)
        {
            auto& g_first_hand = g_first_hands[left ? 0 : 1];
            if (!wrist || (g_first_hand.wrist == wrist && g_first_hand.shape.valid)) {
                return;
            }
            g_first_hand = {};
            g_first_hand.wrist = wrist;
            RE::NiAVObject* children[32]{};
            const auto      count = ReadChildren(wrist, children, 32);
            const auto      w     = FromNi(wrist->world);
            glm::vec3       bases{ 0.0f };
            int             fingers = 0;
            std::optional<glm::vec3> thumb;
            for (std::uint16_t i = 0; i < count; ++i) {
                if (!children[i]) {
                    continue;
                }
                const std::string_view name{ children[i]->name.c_str() };
                const auto pos = ToVec(children[i]->world.translate);
                if (Contains(name, "Thumb")) {
                    thumb = pos;
                } else if (Contains(name, "Index") || Contains(name, "Middle") || Contains(name, "Ring") || Contains(name, "Pinky") || Contains(name, "Little")) {
                    bases += pos;
                    ++fingers;
                }
            }
            if (!thumb || fingers == 0) {
                // (fix189) Told once for each wrist (and 20 in all): a hand that cannot be measured is tried again every frame.
                static RE::NiAVObject* told_wrist[2]{};
                static int             told_n = 0;
                if (told_wrist[left ? 0 : 1] != wrist && told_n < 20) {
                    told_wrist[left ? 0 : 1] = wrist;
                    ++told_n;
                    spdlog::info("[BodyIK] 1P hand: fingers {} thumb {} ({} hand; told once for this wrist)", fingers, thumb.has_value(), left ? "left" : "right");
                }
                return;
            }
            bases /= static_cast<float>(fingers);
            const auto to_bases = bases - w.t;
            const auto to_thumb = *thumb - w.t;
            if (glm::length(to_bases) < 0.02f * w.s) {
                return;
            }
            const auto forward = glm::normalize(to_bases);
            auto       up      = to_thumb - glm::dot(to_thumb, forward) * forward;
            if (glm::length(up) < 1e-4f) {
                return;
            }
            up = glm::normalize(up);
            const auto to_l             = glm::transpose(w.r);
            g_first_hand.shape.forward  = glm::normalize(to_l * forward);
            g_first_hand.shape.up       = glm::normalize(to_l * up);
            g_first_hand.shape.palm     = 0.5f * glm::length(to_bases) / w.s;
            g_first_hand.shape.valid    = true;
            spdlog::info("[BodyIK] 1P {} hand measured: forward ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) palm {:.3f}", left ? "left" : "right", g_first_hand.shape.forward.x,
                g_first_hand.shape.forward.y, g_first_hand.shape.forward.z, g_first_hand.shape.up.x, g_first_hand.shape.up.y, g_first_hand.shape.up.z,
                g_first_hand.shape.palm);
        }

        // The body's wrist transform equivalent to the first-person wrist: same anatomical frame, same palm centre.
        Xf BodyWristFromFirstPerson(const Xf& first_wrist, const HandShape& body, bool left)
        {
            const auto& f    = g_first_hands[left ? 0 : 1].shape;
            Xf          out  = first_wrist;
            out.r            = first_wrist.r * FrameOf(f.forward, f.up) * glm::transpose(FrameOf(body.forward, body.up));
            const auto palm  = first_wrist.t + first_wrist.r * (f.forward * f.palm) * first_wrist.s;
            out.t            = palm - out.r * (body.forward * body.palm) * out.s;
            return out;
        }

        // (fix206) The angle between two directions, in degrees.
        float AngleDeg(const glm::vec3& a, const glm::vec3& b)
        {
            const float la = glm::length(a), lb = glm::length(b);
            if (!(la > 1e-6f) || !(lb > 1e-6f)) {
                return 180.0f;
            }
            return glm::degrees(std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f)));
        }

        // (fix206) A multi-beam weapon's muzzle: ProjectileNodeChild nodes under it (the cutter's three lenses; Homard's test).
        bool HasMultiBeamChild(const RE::NiAVObject* muzzle)
        {
            RE::NiAVObject* children[8]{};
            const auto      n = ReadChildren(muzzle, children, 8);
            for (std::uint16_t i = 0; i < n; ++i) {
                const char* name = children[i] ? children[i]->name.c_str() : nullptr;
                if (name != nullptr && Contains(name, "ProjectileNodeChild")) {
                    return true;
                }
            }
            return false;
        }

        // (fix206) Once each draw (20 in all): the multi-beam weapon's lenses (its ProjectileNodeChild nodes) in the weapon's own space,
        // where each sits and which way it points, against its ProjectileNode's axis: what the beams leave along.
        void LogMultiBeamNodes(const RE::NiAVObject* muzzle, const RE::NiAVObject* weapon)
        {
            static int told = 0;
            if (muzzle == nullptr || weapon == nullptr || told >= 20) {
                return;
            }
            ++told;
            const Xf    to_weapon = Inverse(FromNi(weapon->world));
            const Xf    m         = Compose(to_weapon, FromNi(muzzle->world));
            std::string text      = std::format("its ProjectileNode at ({:.3f},{:.3f},{:.3f}) pointing ({:.3f},{:.3f},{:.3f})", m.t.x, m.t.y, m.t.z,
                     m.r[1].x, m.r[1].y, m.r[1].z);
            RE::NiAVObject* children[8]{};
            const auto      n = ReadChildren(muzzle, children, 8);
            for (std::uint16_t i = 0; i < n; ++i) {
                const char* name = children[i] ? children[i]->name.c_str() : nullptr;
                if (name == nullptr || !Contains(name, "ProjectileNodeChild")) {
                    continue;
                }
                const Xf c = Compose(to_weapon, FromNi(children[i]->world));
                text += std::format("; {} at ({:.3f},{:.3f},{:.3f}) pointing ({:.3f},{:.3f},{:.3f}), {:.1f} deg from the node's axis", name, c.t.x, c.t.y,
                    c.t.z, c.r[1].x, c.r[1].y, c.r[1].z, AngleDeg(c.r[1], m.r[1]));
            }
            spdlog::info("[BodyIK] (fix206) the multi-beam weapon {:08X} in its own space (weapon units): {}", HandGrab::WeaponInHandsId(), text);
        }

        // (fix206) A multi-beam weapon (the cutter), after its ProjectileNode's axis is kept: its firing hold (`hold_forward`/`hold_up`, the
        // game's first-person hold of it this frame: the camera's forward and up in that weapon's space) learned once, from 0.15 to 0.40 s
        // into a pull of a continuous beam (its frames only, NotifyBeamFrame: a weapon that fires no beam keeps its node's axis, as other
        // guns do) (the averaged frames within 25 degrees of the node's axis; at least 5 of them, else the next pull tries again),
        // then kept for this weapon (and its form, for later draws). The basis used eases from the node's axis toward it (0.25 s time
        // constant, no snap), and stays there. Nothing of the hand's motion is in it: the first-person model is the game's own, not ours.
        void UpdateMultiBeamHold(const glm::vec3& hold_forward_now, const glm::vec3& hold_up_now, std::chrono::steady_clock::time_point now)
        {
            using clock = std::chrono::steady_clock;
            g_fire_sample.store(false); // (any shot: not used here; only a continuous beam's frames below)
            if (g_beam_sample.exchange(false)) {
                if (g_basis.last_fire == clock::time_point{} || now - g_basis.last_fire > std::chrono::milliseconds(300)) {
                    g_basis.pull_start  = now; // (a new pull)
                    g_basis.window_done = false;
                    g_basis.sum_forward = glm::vec3{ 0.0f };
                    g_basis.sum_up      = glm::vec3{ 0.0f };
                    g_basis.samples     = 0;
                    g_basis.refused     = 0;
                }
                g_basis.last_fire = now;
            }
            const bool firing = g_basis.last_fire != clock::time_point{} && now - g_basis.last_fire < std::chrono::milliseconds(100);
            if (!g_basis.hold_learned && firing && !g_basis.window_done) {
                const auto into = now - g_basis.pull_start;
                if (into >= kHoldWindowStart && into <= kHoldWindowEnd) {
                    if (std::isfinite(hold_forward_now.x) && std::isfinite(hold_up_now.x) &&
                        AngleDeg(hold_forward_now, g_basis.axis_forward) <= kMultiBeamHoldLimitDeg) {
                        g_basis.sum_forward += hold_forward_now;
                        g_basis.sum_up += hold_up_now;
                        ++g_basis.samples;
                    } else {
                        ++g_basis.refused;
                    }
                } else if (into > kHoldWindowEnd) {
                    g_basis.window_done = true;
                    static int told     = 0;
                    bool       learned  = false;
                    if (g_basis.samples >= kHoldMinSamples && glm::length(g_basis.sum_forward) > 1e-3f) {
                        const glm::vec3 f  = glm::normalize(g_basis.sum_forward);
                        glm::vec3       u  = g_basis.sum_up - f * glm::dot(g_basis.sum_up, f);
                        if (glm::length(u) > 0.1f && AngleDeg(f, g_basis.axis_forward) <= kMultiBeamHoldLimitDeg) {
                            u                    = glm::normalize(u);
                            g_basis.hold_learned = true;
                            g_basis.hold_forward = f;
                            g_basis.hold_up      = u;
                            learned              = true;
                            if (const auto form = HandGrab::WeaponInHandsId(); form != 0) {
                                g_learned_holds[form] = LearnedHold{ f, u };
                            }
                            if (told < 20) {
                                ++told;
                                // (Its way against the node's axis: up and right in the axis' own frame.)
                                const glm::vec3 right = glm::normalize(glm::cross(g_basis.axis_forward, g_basis.axis_up));
                                const float     upd   = glm::degrees(std::asin(std::clamp(glm::dot(f, g_basis.axis_up), -1.0f, 1.0f)));
                                const float     rightd = glm::degrees(std::atan2(glm::dot(f, right), glm::dot(f, g_basis.axis_forward)));
                                spdlog::info("[BodyIK] (fix206) the multi-beam weapon {:08X}'s firing hold learned on a pull ({} frames, {} over 25 deg "
                                             "left out): ({:.3f},{:.3f},{:.3f}) in its space, {:.1f} deg from its ProjectileNode's axis ({:+.1f} up, "
                                             "{:+.1f} right): the gun eased into it and held so from now on (the beam and the laser go along it)",
                                    HandGrab::WeaponInHandsId(), g_basis.samples, g_basis.refused, f.x, f.y, f.z, AngleDeg(f, g_basis.axis_forward), upd,
                                    rightd);
                            }
                        }
                    }
                    if (!learned && told < 20) {
                        ++told;
                        spdlog::info("[BodyIK] (fix206) the multi-beam weapon {:08X}'s firing hold not learned on this pull ({} frames within 25 deg "
                                     "of its ProjectileNode's axis, {} further; {} needed): held by that axis, tried again on the next pull",
                            HandGrab::WeaponInHandsId(), g_basis.samples, g_basis.refused, kHoldMinSamples);
                    }
                }
            }
            // The basis in use eased toward the learned hold (else the node's axis), its up kept square to its forward.
            const glm::vec3 want_forward = g_basis.hold_learned ? g_basis.hold_forward : g_basis.axis_forward;
            const glm::vec3 want_up      = g_basis.hold_learned ? g_basis.hold_up : g_basis.axis_up;
            float           dt           = 0.0f; // (the review: no first-frame jump; last_ease is also set as the axis is kept)
            if (g_basis.last_ease != clock::time_point{}) {
                dt = std::clamp(std::chrono::duration<float>(now - g_basis.last_ease).count(), 0.0f, 0.1f);
            }
            g_basis.last_ease = now;
            const float a     = g_basis.has ? 1.0f - std::exp(-dt / kHoldEaseSeconds) : 1.0f;
            glm::vec3   f     = glm::mix(g_basis.forward, want_forward, a);
            f                 = glm::length(f) > 1e-4f ? glm::normalize(f) : want_forward;
            glm::vec3 u       = glm::mix(g_basis.up, want_up, a);
            u                 = u - f * glm::dot(u, f);
            u                 = glm::length(u) > 0.1f ? glm::normalize(u) : want_up;
            if (std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && std::isfinite(u.x) && std::isfinite(u.y) && std::isfinite(u.z)) {
                g_basis.forward = f;
                g_basis.up      = u;
            }
            g_basis.has   = true;
            g_basis.fired = true; // (BarrelKnown: the laser's beam shown, as for a gun's kept muzzle)
        }

        void SampleWeaponBasis(RE::NiAVObject* first_weapon)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            if (!first_weapon || !world_camera || !world_camera->parent) {
                return;
            }
            // (Another weapon: the skeleton's weapon bone stays the same node through a swap, so its muzzle tells: a new one, or
            // none (a melee weapon), and what was learned of the last one is forgotten, fix132's review.)
            const auto muzzle_now = g_body_muzzle.load();
            if (g_basis.weapon != g_place.node || g_basis.muzzle != muzzle_now) {
                g_basis        = {};
                g_basis.weapon = g_place.node;
                g_basis.muzzle = muzzle_now;
                g_beam_sample.store(false); // (fix206: not the last weapon's beam)
            }
            // (fix206) Whether it is a multi-beam weapon (the cutter), looked at once for each muzzle.
            if (!g_basis.multi_beam_checked && muzzle_now != nullptr) {
                g_basis.multi_beam_checked = true;
                g_basis.multi_beam         = HasMultiBeamChild(muzzle_now);
                static int told = 0;
                if (g_basis.multi_beam && told < 20) {
                    ++told;
                    spdlog::info("[BodyIK] (fix206) a multi-beam weapon {:08X} (ProjectileNodeChild nodes under its muzzle, the cutter): held the way "
                                 "the game holds it while firing, learned on a pull ({} for it already)",
                        HandGrab::WeaponInHandsId(), g_learned_holds.contains(HandGrab::WeaponInHandsId()) ? "a hold learned" : "none learned");
                }
            }
            const auto to_weapon = glm::transpose(FromNi(first_weapon->world).r);
            const auto view_r    = FromNi(world_camera->parent->world).r;
            const auto forward   = glm::normalize(to_weapon * view_r[1]);
            const auto up        = glm::normalize(to_weapon * view_r[2]);
            // The barrel from the gun's own muzzle (its ProjectileNode, along which the game fires): the gun turned so that is the
            // aim, so it points where its shots and its laser go (fix132: from the game's hold, which is not straight ahead before
            // a shot, the barrel was 17-21 degrees off the aim in fix131's log: the laser seemed to point up out of the gun). Its up
            // from the hold, square to it. Not when it is far from the hold's (a model whose muzzle is turned otherwise).
            // (Taken once the gun has been in the hand a few frames (a model just put on may not have its place yet), then kept for
            // it: the muzzle's turn in the weapon is fixed, so nothing of the hand's motion that frame is copied into it.)
            if (g_basis.from_muzzle) {
                g_fire_sample.store(false);
                return;
            }
            // (fix135) A gun's only: a melee weapon may have a muzzle too (a modded lightsaber: along its hilt, toward its end), and
            // while it is being drawn the game's hold of it can line up with that, so it was held backwards from then on; its basis stays
            // the game's hold, as before fix132. A weapon not yet known to be a gun by its type (being drawn, its type not read, or the
            // last weapon's answer still kept just after a swap) waits: a gun for a quarter second and 10 frames first.
            const bool gun = HandGrab::GunKnownOut();
            const auto gun_now = std::chrono::steady_clock::now();
            if (!gun) {
                g_basis.muzzle_frames = 0;
                g_basis.gun_since     = {};
            } else if (g_basis.gun_since == std::chrono::steady_clock::time_point{}) {
                g_basis.gun_since = gun_now;
            }
            // (fix206) A multi-beam weapon whose ProjectileNode's axis is kept (below): from then on held by its firing hold, learned on a pull
            // (the muzzle's axis meanwhile).
            if (g_basis.multi_beam && g_basis.axis_kept) {
                UpdateMultiBeamHold(forward, up, gun_now);
                return;
            }
            // (fix195) The muzzle's way in the weapon from the model's own transforms (its local transforms up to the weapon node), as
            // Homard's Starfield VR reads it from the model: right from the gun's first frame in the hand, with no wait for the model's
            // first drawn frames (before, the gun was held by the game's draw-animation hold for its first quarter second, up to 73 degrees
            // off in the last test, then snapped to the barrel). It is used once the weapon has been a gun by its read type for 0.12 s
            // (past HandGrab's 0.1 s re-read of the type: just after a gun-to-saber swap the type can still be the gun's, and a saber's
            // model held along its ProjectileNode is held backwards, fix135; the review), and kept for good once the draw is over (the
            // weapon out, state 3: the up below is the game's hold, which turns during the draw), after 3 frames and 0.15 s. Before it is
            // kept, a frame without it starts the estimate over. (No setting: it replaces the old wait.)
            if (auto muzzle = g_basis.muzzle; muzzle != nullptr && g_place.node != nullptr && gun &&
                gun_now - g_basis.gun_since >= std::chrono::milliseconds(120)) {
                glm::vec3 muzzle_way{ 0.0f };
                glm::mat3 chain{ 1.0f };
                RE::NiAVObject* node = muzzle;
                for (int i = 0; i < 24 && node != nullptr && node != g_place.node; ++i) {
                    chain = FromNi(node->local).r * chain;
                    node  = node->parent;
                }
                if (node == g_place.node) {
                    muzzle_way = chain[1];
                }
                // (fix147: a gun's muzzle trusted however far it is from the hold's estimate: the gun's shot point is where the game fires along;
                // fix146's test had a gun drawn 67-74 degrees off for its first moments, the estimate from its draw animation, the muzzle refused for
                // being 45 degrees from it; only a gun's (HandGrab::GunKnownOut), as before: a saber's is along its hilt, fix135.)
                // (A loose bound kept: a muzzle pointing back along the hold, more than 150 degrees off, is not taken; the review.)
                if (glm::length(muzzle_way) > 0.5f && glm::dot(glm::normalize(muzzle_way), forward) > std::cos(glm::radians(150.0f))) {
                    const glm::vec3 barrel = glm::normalize(muzzle_way);
                    const glm::vec3 square = up - barrel * glm::dot(up, barrel);
                    if (glm::length(square) > 0.1f) {
                        g_basis.forward     = barrel;
                        g_basis.up          = glm::normalize(square);
                        g_basis.has         = true;
                        g_basis.provisional = true;
                        // (Kept after 0.6 s whatever the weapon's state says, as before fix195 it was after 0.25 s: never left unkept.)
                        const auto since    = gun_now - g_basis.gun_since;
                        const bool keep     = ++g_basis.muzzle_frames >= 3 && since >= std::chrono::milliseconds(150) &&
                                          (GameFlow::weaponState() == 3 || since >= std::chrono::milliseconds(600));
                        if (keep) {
                            spdlog::info("[BodyIK] the barrel from the gun's muzzle: ({:.2f},{:.2f},{:.2f}) in weapon space (fix195: from the model's "
                                         "own transforms, used from its first frame, kept after {} frames), {:.1f} deg from the game's hold (this "
                                         "gun's from now on)",
                                barrel.x, barrel.y, barrel.z, g_basis.muzzle_frames,
                                glm::degrees(std::acos(std::clamp(glm::dot(barrel, forward), -1.0f, 1.0f))));
                            g_basis.fired       = true; // (as good as a shot's: not changed by the hold any more)
                            g_basis.provisional = false;
                            if (g_basis.multi_beam) {
                                // (fix206) A multi-beam weapon: its node's axis kept as the start, its firing hold then learned on a pull
                                // (UpdateMultiBeamHold; or the one learned for this weapon before, eased in from here).
                                g_basis.axis_kept    = true;
                                g_basis.axis_forward = g_basis.forward;
                                g_basis.axis_up      = g_basis.up;
                                g_basis.last_ease    = gun_now; // (a hold kept from an earlier draw eases in from here, no snap)
                                if (const auto it = g_learned_holds.find(HandGrab::WeaponInHandsId()); it != g_learned_holds.end() &&
                                    AngleDeg(it->second.forward, g_basis.axis_forward) <= kMultiBeamHoldLimitDeg) {
                                    g_basis.hold_learned = true;
                                    g_basis.hold_forward = it->second.forward;
                                    g_basis.hold_up      = it->second.up;
                                }
                                LogMultiBeamNodes(muzzle, g_place.node);
                            } else {
                                g_basis.from_muzzle = true;
                            }
                        }
                        g_fire_sample.store(false);
                        return;
                    }
                }
            }
            if (g_basis.provisional) {
                g_basis.provisional = false;
                g_basis.has         = false; // (the estimate starts over from the hold, not from the barrel used meanwhile)
            }
            const bool fired     = g_fire_sample.exchange(false);
            if (fired || !g_basis.has) {
                g_basis.forward = forward;
                g_basis.up      = up;
            } else if (!g_basis.fired) {
                g_basis.forward = glm::normalize(glm::mix(g_basis.forward, forward, 0.02f));
                g_basis.up      = glm::normalize(glm::mix(g_basis.up, up, 0.02f));
            }
            if (fired && !g_basis.fired) {
                spdlog::info("[BodyIK] firing grip sampled: barrel ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) in weapon space", forward.x,
                    forward.y, forward.z, up.x, up.y, up.z);
            }
            g_basis.fired = g_basis.fired || fired;
            g_basis.has   = true;

            // Check of the view-axis assumption: with hand aiming the view forward is the aim ray. (fix189: every 2 s for the first 60, then
            // one a minute.)
            static TimedLog aim_log;
            if (const auto now = std::chrono::steady_clock::now(); aim_log.Due(now, std::chrono::seconds(2))) {
                spdlog::info("[BodyIK] view forward vs aim ray: {:.1f} deg | basis from {}", glm::degrees(std::acos(std::clamp(
                    glm::dot(glm::normalize(view_r[1]), g_barrel.aim_forward), -1.0f, 1.0f))), g_basis.fired ? "firing grip" : "estimate");
            }
        }

        // Where the muzzle was (game world) when the barrel was last measured, for the gun sight on other threads.
        std::mutex g_muzzle_mutex;
        glm::vec3  g_muzzle_world{ 0.0f };
        bool       g_muzzle_valid{ false };
        // (fix146) ...and where it is drawn this frame in the aiming hand's own axes (x right, y along its aim, z up; room metres), with the
        // barrel's way in them.
        glm::vec3                             g_muzzle_in_aim{ 0.0f };
        glm::vec3                             g_barrel_in_aim{ 0.0f, 1.0f, 0.0f };
        std::chrono::steady_clock::time_point g_muzzle_in_aim_at{};
        bool                                  g_muzzle_in_aim_set{ false };

        void MeasureBarrel()
        {
            // (fix189) The muzzle found just now (RefreshMuzzle), measured from its second frame on, as before (when it was found in the
            // frame before's IK): its place is the last update's, which a muzzle new this frame may not have had yet.
            auto muzzle = g_muzzle_new ? nullptr : g_body_muzzle.load();
            auto weapon = g_place.node;
            if (!muzzle || !weapon) {
                g_barrel.has_relation = false;
                g_barrel.has_muzzle_in_weapon = false;
                g_barrel.placed       = false;
                std::scoped_lock _{ g_muzzle_mutex };
                g_muzzle_valid = false;
                return;
            }
            {
                std::scoped_lock _{ g_muzzle_mutex };
                g_muzzle_world = glm::vec3{ muzzle->world.translate.x, muzzle->world.translate.y, muzzle->world.translate.z };
                g_muzzle_valid = std::isfinite(g_muzzle_world.x) && std::isfinite(g_muzzle_world.y) && std::isfinite(g_muzzle_world.z);
            }
            const auto weapon_world = FromNi(weapon->world);
            const auto shot_world   = glm::normalize(FromNi(muzzle->world).r[1]);
            if (g_barrel.placed) {
                g_barrel.error_deg       = glm::degrees(std::acos(std::clamp(glm::dot(shot_world, g_barrel.aim_forward), -1.0f, 1.0f)));
                g_barrel.placement_error = glm::length(weapon_world.t - g_barrel.placed_weapon.t);
            }
            g_barrel.weapon         = weapon;
            g_barrel.muzzle         = muzzle;
            g_barrel.shot_in_weapon = glm::normalize(glm::transpose(weapon_world.r) * shot_world);
            g_barrel.has_relation   = true;
            g_barrel.muzzle_in_weapon     = Compose(Inverse(weapon_world), FromNi(muzzle->world));
            g_barrel.has_muzzle_in_weapon = std::isfinite(g_barrel.muzzle_in_weapon.t.x) && weapon_world.s > 1e-4f;
            g_barrel.placed         = false;
        }

        // A hand holding an object (hand grab), like HIGGS in Skyrim VR: each finger curls along its own path (the same
        // curves the controller fingers use) until it meets the object, here its bounding box, so the hand closes around
        // what it holds instead of making the same fist for everything. The knuckle closes until the finger touches, then the
        // outer joints keep closing until they touch too, so a finger wraps around the object instead of stopping straight
        // where it first meets it.
        struct HeldGripState
        {
            float                                 curl[5]{};  // the knuckles
            float                                 outer[5]{}; // the outer joints
            int                                   grab{ -1 };
            std::chrono::steady_clock::time_point time{};
            std::chrono::steady_clock::time_point start{};
            bool                                  logged{ false };
        };
        HeldGripState g_held_grip[2];

        // Per finger (thumb, index, middle, ring, little): base joint degrees at curl 0 and added at curl 1, then the
        // middle joint's.
        constexpr float kCurlAngles[5][4] = { { 5, 20, 5, 25 }, { 10, 50, 10, 70 }, { 10, 65, 10, 85 }, { 10, 65, 10, 85 }, { 10, 65, 10, 85 } };

        bool SegmentHitsBox(const glm::vec3& a, const glm::vec3& b, const glm::vec3& lo, const glm::vec3& hi)
        {
            const glm::vec3 d  = b - a;
            float           t0 = 0.0f, t1 = 1.0f;
            for (int i = 0; i < 3; ++i) {
                if (std::abs(d[i]) < 1e-9f) {
                    if (a[i] < lo[i] || a[i] > hi[i]) {
                        return false;
                    }
                    continue;
                }
                float u0 = (lo[i] - a[i]) / d[i];
                float u1 = (hi[i] - a[i]) / d[i];
                if (u0 > u1) {
                    std::swap(u0, u1);
                }
                t0 = std::max(t0, u0);
                t1 = std::min(t1, u1);
                if (t0 > t1) {
                    return false;
                }
            }
            return true;
        }

        // A segment (object space) reaching into a cylinder along `axis` through `mid`, radius r, from lo to hi along the
        // axis (sampled: fingers are short and the steps fine).
        bool SegmentHitsCylinder(const glm::vec3& a, const glm::vec3& b, int axis, const glm::vec3& mid, float r, float lo, float hi)
        {
            constexpr int kSamples = 8;
            for (int i = 0; i <= kSamples; ++i) {
                const glm::vec3 p = a + (b - a) * (static_cast<float>(i) / kSamples);
                if (p[axis] < lo || p[axis] > hi) {
                    continue;
                }
                glm::vec3 radial = p - mid;
                radial[axis]     = 0.0f;
                if (glm::dot(radial, radial) <= r * r) {
                    return true;
                }
            }
            return false;
        }

        bool ApplyGripOn(const Pose& pose, int side, int wrist, const HandGrab::HeldShape& shape);

        bool ApplyHeldGrip(const Pose& pose, int side, int wrist)
        {
            HandGrab::HeldShape shape;
            if (!HandGrab::HeldBox(side == 0, shape)) {
                return false;
            }
            return ApplyGripOn(pose, side, wrist, shape);
        }

        // (fix177) A hand holding a person: its fingers closed round the held part as round a held object: a cylinder along the part's line
        // (its bone to its other end; the head upright) as thick as the part, 0.3 m long, where the hand holds it.
        bool ApplyPersonGrip(const Pose& pose, int side, int wrist, const Physical::HeldPersonGrip& held)
        {
            glm::vec3 x = held.axis;
            if (glm::length(x) < 1e-4f) {
                return false;
            }
            x = glm::normalize(x);
            // (fix179) A part of the trunk (waist, belly, chest): the fingers lie flat on it, along the surface at the held point (a slab
            // under it, as wide as a hand's reach), not round its capsule (the trunk is wide, the capsule's roundness is not its shape there).
            if (held.flat && glm::length(held.normal) > 1e-3f) {
                const glm::vec3 out    = glm::normalize(held.normal);
                glm::vec3       across = x - out * glm::dot(x, out);
                if (glm::length(across) < 1e-3f) {
                    across = glm::cross(out, glm::vec3{ 0.0f, 0.0f, 1.0f });
                }
                if (glm::length(across) < 1e-3f) {
                    across = glm::cross(out, glm::vec3{ 1.0f, 0.0f, 0.0f });
                }
                across = glm::normalize(across);
                HandGrab::HeldShape slab{};
                slab.r        = glm::mat3{ across, glm::cross(out, across), out };
                slab.t        = held.at;
                slab.s        = 1.0f;
                slab.min      = glm::vec3{ -0.25f, -0.25f, -0.5f };
                slab.max      = glm::vec3{ 0.25f, 0.25f, 0.0f };
                slab.cylinder = false;
                slab.axis     = -1;
                slab.grab     = 1'000'000 + held.grab;
                return ApplyGripOn(pose, side, wrist, slab);
            }
            glm::vec3 y = glm::cross(x, glm::vec3{ 0.0f, 0.0f, 1.0f });
            if (glm::length(y) < 1e-3f) {
                y = glm::cross(x, glm::vec3{ 1.0f, 0.0f, 0.0f });
            }
            y = glm::normalize(y);
            HandGrab::HeldShape shape{};
            shape.r        = glm::mat3{ x, y, glm::cross(x, y) };
            shape.t        = held.centre;
            shape.s        = 1.0f;
            shape.min      = glm::vec3{ -0.15f, -held.radius, -held.radius };
            shape.max      = glm::vec3{ 0.15f, held.radius, held.radius };
            shape.cylinder = true;
            shape.axis     = 0;
            shape.radius   = held.radius;
            shape.grab     = 1'000'000 + held.grab; // (each hold: the hand opens, then closes on them)
            return ApplyGripOn(pose, side, wrist, shape);
        }

        bool ApplyGripOn(const Pose& pose, int side, int wrist, const HandGrab::HeldShape& shape)
        {
            if (!g_finger_palm[side].valid || shape.s <= 0.0f) {
                return false;
            }
            const auto      wrist_w = pose.GameWorld(wrist);
            const auto&     palm_l  = g_finger_palm[side].palm;
            const glm::vec3 palm    = glm::normalize(wrist_w.r * palm_l);
            const float     pad     = 0.008f / shape.s; // a finger's thickness
            const glm::vec3 lo      = shape.min - glm::vec3{ pad };
            const glm::vec3 hi      = shape.max + glm::vec3{ pad };
            const glm::mat3 to_box  = glm::transpose(shape.r);
            auto            in_box  = [&](const glm::vec3& p) { return to_box * (p - shape.t) / shape.s; };

            auto&       state = g_held_grip[side];
            const auto  now   = std::chrono::steady_clock::now();
            const bool  fresh = state.grab != shape.grab;
            const float dt    = fresh ? 0.0f : std::clamp(std::chrono::duration<float>(now - state.time).count(), 0.0f, 0.1f);
            state.time        = now;
            if (fresh) {
                state.grab   = shape.grab;
                state.start  = now;
                state.logged = false;
                for (int f = 0; f < 5; ++f) {
                    state.curl[f]  = 0.0f; // the hand opens, then closes on the object
                    state.outer[f] = 0.0f;
                }
            }
            const float k = 1.0f - std::exp(-dt / 0.05f);

            const auto& chains = g_bones.fingers[side];
            float       wanted[5]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            float       wanted_outer[5]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            int         bases_inside = 0; // (fix179) for the log: a finger's base already inside the shape (it then stays straight)
            int         touching     = 0; // (fix180) for the log: fingers that closed until they touched it
            for (int f = 0; f < 5; ++f) {
                const auto& chain = chains[f];
                if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0) {
                    continue;
                }
                const glm::vec3 p0 = pose.GameWorld(chain[0]).t;
                {
                    const glm::vec3 b = in_box(p0);
                    bool            inside = false;
                    if (shape.cylinder && shape.axis >= 0 && shape.axis < 3) {
                        const int       a      = shape.axis;
                        glm::vec3       radial = b - 0.5f * (shape.min + shape.max);
                        radial[a]              = 0.0f;
                        inside = b[a] > shape.min[a] && b[a] < shape.max[a] && glm::dot(radial, radial) < shape.radius * shape.radius;
                    } else {
                        inside = b.x > shape.min.x && b.x < shape.max.x && b.y > shape.min.y && b.y < shape.max.y && b.z > shape.min.z && b.z < shape.max.z;
                    }
                    bases_inside += inside ? 1 : 0;
                }
                const glm::vec3 p1 = pose.GameWorld(chain[1]).t;
                const glm::vec3 p2 = pose.GameWorld(chain[2]).t;
                const float     l0 = glm::length(p1 - p0);
                const float     l1 = glm::length(p2 - p1);
                const float     l2 = 0.8f * l1; // the last segment (no bone of its own)
                glm::vec3       parent = p0 - wrist_w.t;
                if (l0 < 1e-4f || l1 < 1e-4f || glm::length(parent) < 1e-4f) {
                    continue;
                }
                parent = glm::normalize(parent);
                // The finger with its knuckle at curl c and its outer joints at curl o, as CurlFinger will pose it: does it
                // reach into the box (its segments from `first` on: 0 the first bone, 1 the second, 2 the tip)?
                auto touches = [&](float c, float o, int first) {
                    const glm::vec3 axis0 = glm::cross(parent, palm);
                    if (glm::length(axis0) < 1e-4f) {
                        return false;
                    }
                    const glm::vec3 d0 = glm::mat3_cast(glm::angleAxis(glm::radians(kCurlAngles[f][0] + kCurlAngles[f][1] * c), glm::normalize(axis0))) * parent;
                    const glm::vec3 axis1 = glm::cross(d0, palm);
                    if (glm::length(axis1) < 1e-4f) {
                        return false;
                    }
                    const glm::vec3 d1 = glm::mat3_cast(glm::angleAxis(glm::radians(kCurlAngles[f][2] + kCurlAngles[f][3] * o), glm::normalize(axis1))) * d0;
                    const glm::vec3 q[4] = { in_box(p0), in_box(p0 + d0 * l0), in_box(p0 + d0 * l0 + d1 * l1), in_box(p0 + d0 * l0 + d1 * (l1 + l2)) };
                    for (int s = first; s < 3; ++s) {
                        if (shape.cylinder && shape.axis >= 0 && shape.axis < 3) {
                            const int       a   = shape.axis;
                            const glm::vec3 mid = 0.5f * (shape.min + shape.max);
                            if (SegmentHitsCylinder(q[s], q[s + 1], a, mid, shape.radius + pad, lo[a], hi[a])) {
                                return true;
                            }
                        } else if (SegmentHitsBox(q[s], q[s + 1], lo, hi)) {
                            return true;
                        }
                    }
                    return false;
                };
                // From a little straighter than the usual open hand (so a finger can lie flat along a side under the
                // hand) to a fist: first the knuckle (the outer joints straight) until any part touches, then the outer
                // joints until the second bone or the tip touches.
                constexpr int   kSteps   = 20;
                constexpr float kOpenest = -0.15f;
                const float     step     = (1.0f - kOpenest) / kSteps;
                for (int i = 0; i <= kSteps; ++i) {
                    const float c = kOpenest + step * static_cast<float>(i);
                    if (touches(c, kOpenest, 0)) {
                        wanted[f] = std::max(kOpenest, c - 0.5f * step); // stop just at the surface
                        ++touching; // (fix180) for the log
                        break;
                    }
                }
                for (int i = 0; i <= kSteps; ++i) {
                    const float o = kOpenest + step * static_cast<float>(i);
                    if (touches(wanted[f], o, 1)) {
                        wanted_outer[f] = std::max(kOpenest, o - 0.5f * step);
                        break;
                    }
                }
            }
            g_grip_bases_inside[side] = bases_inside;
            g_grip_touching[side]     = touching;
            for (int f = 0; f < 5; ++f) {
                state.curl[f] += (wanted[f] - state.curl[f]) * k;
                state.outer[f] += (wanted_outer[f] - state.outer[f]) * k;
                CurlFinger(pose, chains[f], wrist, palm_l, kCurlAngles[f][0] + kCurlAngles[f][1] * state.curl[f],
                    kCurlAngles[f][2] + kCurlAngles[f][3] * state.outer[f]);
            }
            if (!state.logged && now - state.start > std::chrono::milliseconds(400)) {
                state.logged = true;
                spdlog::info("[BodyIK] {} hand closes around the held {} (knuckle/outer joints): thumb {:.2f}/{:.2f}, index {:.2f}/{:.2f}, middle "
                             "{:.2f}/{:.2f}, ring {:.2f}/{:.2f}, little {:.2f}/{:.2f} (0 open, 1 fist, below 0 flat)",
                    side == 0 ? "left" : "right", shape.cylinder ? "round object" : "box", wanted[0], wanted_outer[0], wanted[1], wanted_outer[1],
                    wanted[2], wanted_outer[2], wanted[3], wanted_outer[3], wanted[4], wanted_outer[4]);
            }
            return true;
        }

        // Fingers from the controller: index from the trigger (extended off the trigger), the other three from the
        // grip, the thumb from touch. A hand holding the weapon keeps the first-person grip's finger pose and only
        // the index finger follows the trigger.
        bool ApplyTrackedFingers(const Pose& pose, int side, int wrist);

        void ApplyFingers(const Pose& pose, int side, int wrist, RE::NiAVObject* first_wrist)
        {
            if (!GameFlow::gStore.internalSettings.fingerPoses || wrist < 0) {
                return;
            }
            static auto vr    = VR::get();
            // (fix198) The fingers of the controller this hand follows: finger input is by physical controller (index 1 the physical left),
            // the hands by "Swap Controller Inputs" as their poses (HandGrab, HandActions); before, swapped controllers crossed them.
            auto        input = vr->get_finger_state((side == 0 ? vr->get_left_controller_index() : vr->get_right_controller_index()) == 1);
            // (fix159) A hand holding a melee weapon (the right one held by its own model; the left hand's weapon in it): a full fist (the fingers
            // kept closed while holding a melee weapon; closed around the grip's small box, the ring and little fingers stayed out).
            const bool melee_fist = side == 1 ? g_right_melee_fist : LeftWeapon::Held() && !MeleeThrow::Away(true);
            if (melee_fist) {
                first_wrist         = nullptr; // (the fist, not the first-person grip's fingers; the review)
                input.grip          = 1.0f;
                input.trigger       = 1.0f;
                input.trigger_touch = true;
                input.thumb_touch   = true;
            }
            // (fix174) A hand holding a person: closed part way, round their arm or onto their body (not a full fist).
            if (Physical::HoldingPerson(side == 0) && !melee_fist) {
                input.grip          = std::min(input.grip, 0.6f);
                input.trigger       = std::min(input.trigger, 0.55f);
                input.trigger_touch = true;
                input.thumb_touch   = true;
            }
            // A hand holding an object (hand grab) closes around it.
            const float held = HandGrab::HeldCurl(side == 0);
            if (held > 0.0f) {
                input.grip          = std::max(input.grip, held);
                input.trigger       = std::max(input.trigger, held);
                input.trigger_touch = true;
                input.thumb_touch   = true;
            }
            auto& diag = g_finger_diag[side];
            diag       = { input.trigger, input.grip, input.thumb_touch, 0 };

            // (fix177) Holding a person: closed round the part held, as round a held object (else the part-way close above).
            if (!melee_fist && Physical::HoldingPerson(side == 0)) {
                Physical::HeldPersonGrip person{};
                // (The review: only while the hand is drawn on them; farther, no finger reaches the part and the hand closed to a full fist in
                // the air: the part-way close above then.) (fix180) Drawn on them: its lock drawn this frame (HoldPersonShift).
                const HandLock& lock = g_person_lock[side];
                // (The review: only while it is drawn nearly wholly locked; mostly at your controller no finger touches and they closed to a
                // fist in the air.)
                const bool have = Physical::HeldPersonContact(side == 0, person);
                if (have && g_person_held_grab[side] == person.grab) {
                    person = g_person_held_now[side]; // (fix181) as this frame has them (HoldPersonShift)
                }
                if (have && lock.ok && lock.key == person.grab && lock.w >= 0.9f && person.weight > 0.0f && ApplyPersonGrip(pose, side, wrist, person)) {
                    // (fix179) For the log (HoldPersonShift's line): fingers whose base was already inside them this frame; (fix180) and
                    // the fingers that closed until they touched them.
                    if (g_person_hand[side].grab == person.grab) {
                        g_person_hand[side].bases_frames += g_grip_bases_inside[side] > 0 ? 1 : 0;
                        ++g_person_hand[side].grip_frames;
                        g_person_hand[side].touch_sum += g_grip_touching[side];
                    }
                    return;
                }
            }
            // (fix180) Holding on to a wall (climbing): the fingers laid against it where the hand is drawn on it (a flat surface under the
            // held point, as on a person's trunk), closing until they touch it.
            {
                const HandLock&    lock = g_wall_lock[side];
                Climbing::WallGrip wall{};
                if (!melee_fist && lock.ok && lock.w >= 0.9f && Climbing::GripSurface(side == 0, wall) && glm::length(wall.normal) > 0.5f) {
                    Physical::HeldPersonGrip on{};
                    on.at     = wall.point;
                    on.normal = glm::normalize(wall.normal);
                    on.axis   = glm::length(glm::cross(on.normal, glm::vec3{ 0.0f, 0.0f, 1.0f })) > 1e-3f ? glm::cross(on.normal, glm::vec3{ 0.0f, 0.0f, 1.0f })
                                                                                                         : glm::vec3{ 1.0f, 0.0f, 0.0f };
                    on.centre = wall.point;
                    on.radius = 0.05f;
                    on.weight = wall.weight;
                    on.grab   = lock.key;
                    on.flat   = true;
                    if (ApplyPersonGrip(pose, side, wrist, on)) {
                        if (g_wall_hand[side].grab == lock.key) {
                            g_wall_hand[side].bases_frames += g_grip_bases_inside[side] > 0 ? 1 : 0;
                            ++g_wall_hand[side].grip_frames;
                            g_wall_hand[side].touch_sum += g_grip_touching[side];
                        }
                        return;
                    }
                }
            }
            if (held <= 0.0f && !melee_fist && ApplyTrackedFingers(pose, side, wrist)) {
                return;
            }
            if (held > 0.0f && !melee_fist && ApplyHeldGrip(pose, side, wrist)) {
                return; // closed around the held object's box (without one: the plain fist below)
            }

            // Each finger follows its input through a fast spring and an ease-in/out curve.
            auto&       state = g_finger_springs[side];
            const auto  now   = std::chrono::steady_clock::now();
            const float dt    = state.has_time ? std::clamp(std::chrono::duration<float>(now - state.time).count(), 0.0f, 0.1f) : 0.0f;
            state.time        = now;
            state.has_time    = true;
            const float k     = 1.0f - std::exp(-dt / 0.06f);
            auto spring = [&](float& value, float target) {
                value += (target - value) * k;
                return value * value * (3.0f - 2.0f * value);
            };
            const float index = spring(state.index, input.trigger_touch ? 0.3f + 0.7f * input.trigger : 0.0f);
            const float grip  = spring(state.grip, input.grip);
            const float thumb = spring(state.thumb, input.thumb_touch ? 1.0f : 0.0f);

            const auto& chains = g_bones.fingers[side];
            if (first_wrist) {
                // A hand holding the weapon takes the first-person grip's finger directions, transferred through the
                // hands' anatomical frames so the rigs' bone axes do not matter.
                const auto& first_shape = g_first_hands[side].shape;
                const auto& body_shape  = side == 0 ? g_bones.left.shape : g_bones.right.shape;
                if (first_shape.valid && body_shape.valid) {
                    static constexpr const char* kFingerNames[5][3] = { { "thumb", "Thumb1", "Thumb2" }, { "Index", "Index1", "Index2" },
                        { "Middle", "Middle1", "Middle2" }, { "Ring", "Ring1", "Ring2" }, { "Pinky", "Pinky1", "Pinky2" } };
                    const auto first_frame = FromNi(first_wrist->world).r * FrameOf(first_shape.forward, first_shape.up);
                    const auto body_frame  = pose.GameWorld(wrist).r * FrameOf(body_shape.forward, body_shape.up);
                    const auto to_body     = body_frame * glm::transpose(first_frame);
                    const std::string prefix = side == 0 ? "L_" : "R_";
                    for (int f = 0; f < 5; ++f) {
                        if (f == 1) {
                            continue;  // the index finger follows the trigger
                        }
                        for (int j = 0; j < 2; ++j) {
                            const int bone  = chains[f][j];
                            const int child = chains[f][j + 1];
                            if (bone < 0 || child < 0) {
                                continue;
                            }
                            auto first_joint = FindDescendant(first_wrist, prefix + kFingerNames[f][j]);
                            auto first_child = first_joint ? FindDescendant(first_joint, prefix + kFingerNames[f][j + 1]) : nullptr;
                            if (!first_child) {
                                continue;
                            }
                            const auto wanted = to_body * (ToVec(first_child->world.translate) - ToVec(first_joint->world.translate));
                            const auto joint  = pose.GameWorld(bone);
                            const auto dir    = pose.GameWorld(child).t - joint.t;
                            if (glm::length(wanted) < 1e-5f || glm::length(dir) < 1e-5f) {
                                continue;
                            }
                            auto posed = joint;
                            posed.r    = RotationBetween(dir, wanted) * joint.r;
                            pose.SetGameWorld(bone, posed);
                            ++diag.copied;
                        }
                    }
                }
            }

            if (!g_finger_palm[side].valid) {
                return;
            }
            const auto& palm = g_finger_palm[side].palm;
            // (fix173) The four fingers as FRIK poses them (PoseFinger); the thumb as before.
            const auto& shape = side == 0 ? g_bones.left.shape : g_bones.right.shape;
            if (!shape.valid) {
                return;
            }
            PoseFinger(pose, chains[1], wrist, palm, shape.forward, index, kIndexAngles);
            if (!first_wrist) {
                PoseFinger(pose, chains[2], wrist, palm, shape.forward, grip, kMiddleAngles);
                PoseFinger(pose, chains[3], wrist, palm, shape.forward, grip, kRingAngles);
                PoseFinger(pose, chains[4], wrist, palm, shape.forward, grip, kLittleAngles);
                CurlFinger(pose, chains[0], wrist, palm, 5.0f + 20.0f * thumb, 5.0f + 25.0f * thumb);
            }
        }

        // Fingers from hand tracking: each finger joint points where the tracked joint does, through the hands'
        // anatomical frames (tracked: wrist to middle knuckle forward, thumb side up).
        bool ApplyTrackedFingers(const Pose& pose, int side, int wrist)
        {
            const auto& hand = g_tracked_hands[side];
            const auto& body = side == 0 ? g_bones.left.shape : g_bones.right.shape;
            if (!GameFlow::gStore.internalSettings.handTracking || !hand.active || !body.valid) {
                return false;
            }
            // (fix170) A hand the runtime makes up from the controller (data source 2: Virtual Desktop's, from its touch sensors) is not
            // copied: after fix169 the fingers did not look straight (stereo_1_left.jpg 05:59: both open
            // hands' fingers bent back and splayed, the thumbs turned out; the log: "fingers L true (src 2) R true (src 2)"). This copy
            // (ported from SFVR, never tuned for it) lines the two hands up by the thumb, measured at its root on the body and at its
            // knuckle on the tracked hand: each finger's bend lands askew. Such a hand knows no more than the controller's trigger, grip and
            // thumb touch, which the mod's own finger model reads (ApplyFingers: straight when open, curled as you squeeze). Real hand
            // tracking (data source 1) is still copied.
            if (hand.data_source == 2) {
                static bool told[2]{};
                if (!told[side]) {
                    told[side] = true;
                    spdlog::info("[BodyIK] (fix170) the {} hand from the controller (the runtime's own hand from it): the mod's finger model, not its "
                                 "joints", side == 0 ? "left" : "right");
                }
                return false;
            }
            auto at = [&](int i) { return glm::vec3{ hand.joints[i].position }; };
            for (const int i : { 1, 3, 12 }) {
                if (!hand.joints[i].position_valid) {
                    return false;
                }
            }
            const auto fwd = at(12) - at(1);
            auto       up  = at(3) - at(1);
            if (glm::length(fwd) < 1e-3f) {
                return false;
            }
            up -= glm::dot(up, glm::normalize(fwd)) * glm::normalize(fwd);
            if (glm::length(up) < 1e-4f) {
                return false;
            }
            const auto room = tracking::RoomRotation();
            if (!room) {
                return false;
            }
            // Tracked frame in game axes, then onto the body's hand frame.
            const auto tracked_frame = FrameOf(*room * tracking::ToHavokVector(fwd), *room * tracking::ToHavokVector(up));
            const auto body_frame    = pose.GameWorld(wrist).r * FrameOf(body.forward, body.up);
            const auto to_body       = body_frame * glm::transpose(tracked_frame);

            // XrHandJointEXT: thumb metacarpal 2..tip 5; fingers proximal..tip at 7, 12, 17, 22 (+0..3).
            static constexpr int kTracked[5][3] = { { 2, 3, 4 }, { 7, 8, 9 }, { 12, 13, 14 }, { 17, 18, 19 }, { 22, 23, 24 } };
            const auto& chains = g_bones.fingers[side];
            for (int f = 0; f < 5; ++f) {
                for (int j = 0; j < 2; ++j) {
                    const int bone  = chains[f][j];
                    const int child = chains[f][j + 1];
                    const int a = kTracked[f][j], b = kTracked[f][j + 1];
                    if (bone < 0 || child < 0 || !hand.joints[a].position_valid || !hand.joints[b].position_valid) {
                        continue;
                    }
                    const auto wanted = to_body * (*room * tracking::ToHavokVector(at(b) - at(a)));
                    const auto joint  = pose.GameWorld(bone);
                    const auto dir    = pose.GameWorld(child).t - joint.t;
                    if (glm::length(wanted) < 1e-5f || glm::length(dir) < 1e-5f) {
                        continue;
                    }
                    auto posed = joint;
                    posed.r    = RotationBetween(dir, wanted) * joint.r;
                    pose.SetGameWorld(bone, posed);
                }
            }
            g_track_diag.fingers[side] = true;
            return true;
        }

        void RestoreAnimated(const Pose& pose)
        {
            for (auto it = g_written.begin(); it != g_written.end();) {
                if (it->first < pose.count && std::memcmp(&pose.local[it->first], &it->second.written, sizeof(RE::NiTransform)) == 0) {
                    pose.local[it->first] = it->second.animated;
                }
                it = g_written.erase(it);
            }
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // (fix159) The right hand's melee weapon's light at half (the lightsaber glow was too
        // bright with 2 lightsabers out, so about half; a helper's research): the modded sabers carry an aim-optics light (AOPS ->
        // LIGH, 3000 lumens, 8 m) that the game makes for the equipped weapon (0x14102CB90: a BSLight, type table 0x144D702E0, under the
        // weapon's node); its intensity at +0x198 (lumens / 4 pi, written by 0x142439930, which then sets +0x367), half of it lights half as
        // much with the same reach. Found under the weapon's nodes (first and third person) each frame while a melee weapon is out; the
        // game's own value halved whenever it is not the one written here (made anew, or set again). Only the player's weapon: other
        // sabers as they are. The blades' own glow (their self-lit material) is not changed.
        constexpr uintptr_t kLightTypeRva     = 0x4D702E0;
        constexpr uintptr_t kShadowLightTypeRva = 0x4D77A80; // (BSShadowLight: a "Shadow Spotlight" saber's; the review)
        constexpr uintptr_t kLightIntensity   = 0x198;
        constexpr uintptr_t kLightChanged     = 0x367;
        constexpr float     kSaberLightShare  = 0.5f;
        struct DimmedLight
        {
            uintptr_t light{ 0 };
            float     written{ 0.0f };
            int       refresh{ 0 }; // (frames left to mark it changed again: the light system may clear the mark just as it was written)
        };
        std::array<DimmedLight, 8> g_dimmed{};

        bool LightGuarded(uintptr_t object, uintptr_t type, float& intensity)
        {
            const uintptr_t shadow_type = At(kShadowLightTypeRva); // (fix190: BSShadowLight's type table; a type 0 when not known in this game version)
            __try {
                const uintptr_t table = *reinterpret_cast<const uintptr_t*>(object);
                if ((type == 0 || table != type) && (shadow_type == 0 || table != shadow_type)) {
                    return false;
                }
                intensity = *reinterpret_cast<const float*>(object + kLightIntensity);
                return std::isfinite(intensity);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void WriteLightGuarded(uintptr_t light, float intensity)
        {
            __try {
                *reinterpret_cast<float*>(light + kLightIntensity) = intensity;
                *reinterpret_cast<std::uint8_t*>(light + kLightChanged) = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        void DimWeaponLights(RE::NiAVObject* root)
        {
            const auto type = At(kLightTypeRva);
            if (root == nullptr || type == 0) { // (fix190: 0 when not known in this game version)
                return;
            }
            // (The weapon's nodes, a few levels down: the light hangs under its LaserNode.)
            RE::NiAVObject* stack[64]{};
            int             count = 0, visited = 0;
            stack[count++] = root;
            while (count > 0 && visited < 96) {
                RE::NiAVObject* node = stack[--count];
                ++visited;
                float intensity = 0.0f;
                if (LightGuarded(reinterpret_cast<uintptr_t>(node), type, intensity)) {
                    const auto light = reinterpret_cast<uintptr_t>(node);
                    DimmedLight* entry = nullptr;
                    for (auto& d : g_dimmed) {
                        if (d.light == light) {
                            entry = &d;
                        }
                    }
                    if (entry != nullptr && std::abs(intensity - entry->written) <= 1e-3f) {
                        if (entry->refresh > 0) {
                            --entry->refresh;
                            WriteLightGuarded(light, entry->written); // (marked changed again: the review)
                        }
                    } else {
                        if (entry == nullptr) {
                            static int next = 0;
                            entry           = &g_dimmed[next];
                            next            = (next + 1) % static_cast<int>(g_dimmed.size());
                        }
                        entry->light   = light;
                        entry->written = intensity * kSaberLightShare;
                        entry->refresh = 5;
                        WriteLightGuarded(light, entry->written);
                        static int told = 0;
                        if (told++ < 5) {
                            spdlog::info("[BodyIK] (fix159) the melee weapon's light at half: {:.1f} -> {:.1f}", intensity, entry->written);
                        }
                    }
                    continue;
                }
                RE::NiAVObject* children[16]{};
                const auto      n = ReadChildren(node, children, 16);
                for (std::uint16_t i = 0; i < n && count < 64; ++i) {
                    if (children[i] != nullptr) {
                        stack[count++] = children[i];
                    }
                }
            }
        }

        // (fix160) The same light found through the game's own pointer to it (fix159's walk found none in a test; a helper's
        // research): the sight that makes it (AimOpticalSightStates' Ready, 0x14102CB90) keeps it at its +0x90. The path: PlayerCharacter
        // +0x228 its AI process, +0x8 that one's middle-high data, +0x270 its equipped items (count, capacity, items of 0x28 bytes), an
        // item's +0x20 its equipped weapon data, that one's +0x20 its aim module (type table 0x144C44498), +0x18 the module's sight; the
        // sight's +0xA0 whether it is on. The game makes the light only while the sight is on, as a child of LaserNode in the hidden
        // first-person weapon; logged so the next test tells whether a saber light exists at all (the left saber tested has none, yet its
        // floor glow looked the same: then the brightness is the blades' own glow).
        constexpr uintptr_t kPlayerProcess      = 0x228;
        constexpr uintptr_t kProcessMiddleHigh  = 0x8;
        constexpr uintptr_t kMiddleEquipped     = 0x270;
        constexpr uintptr_t kEquippedStride     = 0x28;
        constexpr uintptr_t kEquippedData       = 0x20;
        constexpr uintptr_t kDataAimModule      = 0x20;
        constexpr uintptr_t kAimModuleTypeRva   = 0x4C44498;
        constexpr uintptr_t kAimModuleSight     = 0x18;
        constexpr uintptr_t kSightLight         = 0x90;
        constexpr uintptr_t kSightOn            = 0xA0;
        constexpr uintptr_t kNodeParent         = 0x38;
        constexpr int       kSightsMax          = 4;

        struct SightSeen
        {
            uintptr_t sight{ 0 };
            uintptr_t light{ 0 };
            uint8_t   on{ 0 };
        };

        // One equipped item's sight, under its own guard (fix160's test: the whole list read as unreadable, most likely one item whose
        // +0x20 is not a weapon's data). 1 found, 0 none, -1 the item could not be read.
        // (fix190: `aim_type` the aim module's type table, At, given in place of the game's base.)
        int SightOfItemGuarded(uintptr_t item, uintptr_t aim_type, SightSeen& out)
        {
            __try {
                const uintptr_t data = *reinterpret_cast<const uintptr_t*>(item + kEquippedData);
                if (data == 0) {
                    return 0;
                }
                const uintptr_t aim = *reinterpret_cast<const uintptr_t*>(data + kDataAimModule);
                if (aim == 0 || *reinterpret_cast<const uintptr_t*>(aim) != aim_type) {
                    return 0;
                }
                const uintptr_t sight = *reinterpret_cast<const uintptr_t*>(aim + kAimModuleSight);
                if (sight == 0) {
                    return 0;
                }
                out.sight = sight;
                out.light = *reinterpret_cast<const uintptr_t*>(sight + kSightLight);
                out.on    = *reinterpret_cast<const uint8_t*>(sight + kSightOn);
                return 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // The equipped list's size and items (false: not readable as a list; what was read is kept for the log).
        bool EquippedListGuarded(uintptr_t player, uintptr_t& process, uintptr_t& middle, uint32_t& size, uint32_t& capacity, uintptr_t& items)
        {
            __try {
                process = *reinterpret_cast<const uintptr_t*>(player + kPlayerProcess);
                middle  = process != 0 ? *reinterpret_cast<const uintptr_t*>(process + kProcessMiddleHigh) : 0;
                if (middle == 0) {
                    return false;
                }
                size     = *reinterpret_cast<const uint32_t*>(middle + kMiddleEquipped);
                capacity = *reinterpret_cast<const uint32_t*>(middle + kMiddleEquipped + 4);
                items    = *reinterpret_cast<const uintptr_t*>(middle + kMiddleEquipped + 8);
                return size <= capacity && capacity <= 256 && (size == 0 || items != 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The equipped weapons' sights (at most kSightsMax): -1 the list could not be read, -2 it changed while read (read without the
        // game's lock on it, at +0x268: that frame's reading dropped; the review). `faulted`: items that could not be read.
        int ReadSights(uintptr_t player, uintptr_t aim_type, SightSeen* out, int& faulted)
        {
            faulted          = 0;
            uintptr_t process = 0, middle = 0, items = 0;
            uint32_t  size = 0, capacity = 0;
            if (!EquippedListGuarded(player, process, middle, size, capacity, items)) {
                static int told = 0;
                if (told++ < 3) {
                    spdlog::info("[BodyIK] (fix161) your equipped list could not be read: process {:x}, its data {:x}, {} item(s) of {}, at {:x}", process,
                        middle, size, capacity, items);
                }
                return -1;
            }
            int found = 0;
            for (uint32_t i = 0; i < size && i < 16 && found < kSightsMax; ++i) {
                const int got = SightOfItemGuarded(items + i * kEquippedStride, aim_type, out[found]);
                if (got > 0) {
                    ++found;
                } else if (got < 0) {
                    ++faulted;
                }
            }
            uintptr_t process2 = 0, middle2 = 0, items2 = 0;
            uint32_t  size2 = 0, capacity2 = 0;
            if (!EquippedListGuarded(player, process2, middle2, size2, capacity2, items2) || middle2 != middle || size2 != size || items2 != items) {
                return -2;
            }
            return found;
        }

        // A node's name and its parent, read under a guard.
        uintptr_t NameAndParentGuarded(uintptr_t at, char (&name)[48])
        {
            __try {
                const char* text = reinterpret_cast<RE::NiAVObject*>(at)->name.c_str();
                if (text != nullptr) {
                    strncpy_s(name, text, _TRUNCATE);
                }
                return *reinterpret_cast<const uintptr_t*>(at + kNodeParent);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // The light's parents' names, its own first (for the log).
        std::string LightPlace(uintptr_t light)
        {
            std::string place;
            uintptr_t   at = light;
            for (int i = 0; i < 8 && at != 0; ++i) {
                char name[48]{};
                at = NameAndParentGuarded(at, name);
                place += i == 0 ? "" : " < ";
                place += name[0] != 0 ? name : "(no name)";
            }
            return place;
        }

        // The sight still holds this light (read just before it is written: the game may let it go meanwhile; the review).
        bool SightHoldsGuarded(uintptr_t sight, uintptr_t light)
        {
            __try {
                return *reinterpret_cast<const uintptr_t*>(sight + kSightLight) == light;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void DimSightLights()
        {
            const auto base   = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            // (fix190: the aim module's type table not known in this game version: no sight looked for.)
            if (base == 0 || player == 0 || At(kAimModuleTypeRva) == 0) {
                return;
            }
            SightSeen sights[kSightsMax]{};
            int       faulted = 0;
            const int count   = ReadSights(player, At(kAimModuleTypeRva), sights, faulted);
            if (count == -2) {
                return; // (the list changed while read: next frame)
            }
            // (Logged when what is seen changes: a light made or gone, the sight on or off; a few times.)
            static uintptr_t last_light = 1;
            static int       last_on = -1, last_count = -2, tells = 0;
            const uintptr_t  light0 = count > 0 ? sights[0].light : 0;
            const int        on0    = count > 0 ? sights[0].on : -1;
            if ((light0 != last_light || on0 != last_on || count != last_count) && tells < 12) {
                ++tells;
                float intensity = 0.0f;
                const auto type = At(kLightTypeRva);
                if (light0 != 0 && LightGuarded(light0, type, intensity)) {
                    spdlog::info("[BodyIK] (fix160) the melee weapon's sight (on {}) has its light: {:.1f} now, at {}", on0, intensity, LightPlace(light0));
                } else if (count > 0) {
                    spdlog::info("[BodyIK] (fix160) the melee weapon's sight (on {}) has no light{} ({} sight(s); {} item(s) unreadable)", on0,
                        light0 != 0 ? " of a known kind" : "", count, faulted);
                } else {
                    spdlog::info("[BodyIK] (fix160) no sight on your equipped weapons ({}; {} item(s) unreadable)", count < 0 ? "the list could not be read" : "none has one",
                        faulted);
                }
                last_light = light0;
                last_on    = on0;
                last_count = count;
            }
            for (int i = 0; i < count; ++i) {
                if (sights[i].light != 0 && SightHoldsGuarded(sights[i].sight, sights[i].light)) {
                    DimWeaponLights(reinterpret_cast<RE::NiAVObject*>(sights[i].light));
                }
            }
        }

        // (fix158) The right hand's one-handed melee weapon held by its own model, as the left hand's weapon's copy is (GrenadeHand::Target:
        // its model's origin, the grip, just out from the palm; its blade, from the origin to its model's middle, along the fist's line
        // across the palm; its model's +y made square to the blade along the fingers). Before, its turn was copied from the hidden
        // first-person rig (SampleWeaponBasis), which neither follows the controller nor stays still: the saber was drawn at a
        // wrong angle and drifted (a helper's research: 83 deg off after 3 s). Guns and two-handed melee are as before. The hand frame:
        // the body's right hand's grip frame (MeasureGripFrame, in the aim's frame), put where the controller is, as the hand grab does.
        // The blade's way in the right controller's aim frame is kept for the hits (ImpactMelee, as the left's).
        constexpr float kMeleeGripOut = 0.025f; // (as the left's copy, kWeaponGripOut)
        std::mutex      g_right_blade_mutex;
        glm::vec3       g_right_blade_local{ 0.0f, 0.0f, -1.0f };
        std::chrono::steady_clock::time_point g_right_blade_at{};

        bool FixedMeleeHold(RE::NiAVObject* bone, bool bone_is_model, const HandTarget& aim, const HandTarget& grip, Xf& out_bone)
        {
            const auto& store = g_grip_frame[1];
            if (bone == nullptr || !store.valid.load(std::memory_order_acquire)) {
                return false;
            }
            const glm::vec3 right = glm::normalize(glm::cross(aim.forward, aim.up));
            const auto      world = [&](const float* v) { return right * v[0] + aim.up * v[1] - aim.forward * v[2]; };
            const glm::vec3 surface = grip.position + world(store.frame.contact);
            const glm::vec3 facing  = glm::normalize(world(store.frame.facing));
            glm::vec3       across  = world(store.frame.knuckles);
            across -= facing * glm::dot(across, facing);
            if (glm::length(across) < 1e-3f) {
                return false;
            }
            const glm::vec3 way = glm::normalize(across);
            glm::vec3       fwd = glm::cross(way, facing);
            if (glm::dot(fwd, world(store.frame.fingers)) < 0.0f) {
                fwd = -fwd;
            }
            fwd = glm::normalize(fwd - way * glm::dot(fwd, way));
            // The model: the bone itself (a weapon whose own model is the placed node), or its child that is not an IK target.
            RE::NiAVObject* model = bone_is_model ? bone : nullptr;
            if (model == nullptr) {
                RE::NiAVObject* children[16]{};
                const auto      n = ReadChildren(bone, children, 16);
                for (std::uint16_t i = 0; i < n && model == nullptr; ++i) {
                    const std::string_view name = children[i] ? children[i]->name.c_str() : "";
                    if (children[i] != nullptr && name != "R_HandIk" && name != "L_HandIk") {
                        model = children[i];
                    }
                }
            }
            if (model == nullptr) {
                return false;
            }
            const Xf bone_w  = FromNi(bone->world);
            const Xf model_w = FromNi(model->world);
            if (!(bone_w.s > 1e-4f) || !(model_w.s > 1e-4f)) {
                return false;
            }
            const Xf model_in_bone = Compose(Inverse(bone_w), model_w);
            // (Its middle in its own space, read once for this model and kept, as the left copy's is: read each frame it wobbled with a
            // swing (the bound a frame apart from the transform) and a saber's blade lighting up moved it; the review.)
            static RE::NiAVObject* cached_model = nullptr;
            static glm::vec3       cached_centre{ 0.0f };
            static int             cached_frames = 0;
            if (model != cached_model) {
                cached_model  = model;
                cached_frames = 0;
            }
            if (cached_frames < 10) {
                float bound[4]{};
                std::memcpy(bound, reinterpret_cast<const std::uint8_t*>(model) + kWorldBoundOffset, sizeof(bound));
                const glm::vec3 c = glm::transpose(model_w.r) * (glm::vec3{ bound[0], bound[1], bound[2] } - model_w.t) / model_w.s;
                const float     l = glm::length(c);
                if (std::isfinite(l) && l >= 0.05f && l <= 1.5f && bound[3] > 0.03f && bound[3] <= 1.6f) {
                    cached_centre = c;
                    ++cached_frames; // (taken again for its first ten good frames: its 3D settling; then kept)
                }
            }
            if (cached_frames == 0) {
                return false;
            }
            const glm::vec3 centre = cached_centre;
            const float     blade  = glm::length(centre);
            const glm::vec3 m_way = centre / blade;
            glm::vec3       m_fwd = glm::vec3{ 0.0f, 1.0f, 0.0f } - m_way * m_way.y;
            if (glm::length(m_fwd) < 0.2f) {
                m_fwd = glm::vec3{ 1.0f, 0.0f, 0.0f } - m_way * m_way.x;
            }
            m_fwd = glm::normalize(m_fwd);
            const glm::mat3 hand_axes{ glm::cross(fwd, way), fwd, way };
            const glm::mat3 model_axes{ glm::cross(m_fwd, m_way), m_fwd, m_way };
            Xf model_new{};
            model_new.r = hand_axes * glm::transpose(model_axes);
            model_new.t = surface + facing * kMeleeGripOut;
            model_new.s = model_w.s;
            out_bone    = Compose(model_new, Inverse(model_in_bone));
            if (!std::isfinite(out_bone.t.x) || !std::isfinite(out_bone.r[0][0]) || std::abs(glm::determinant(model_new.r) - 1.0f) > 0.05f) {
                return false;
            }
            // The blade's way in the right controller's aim frame (right, up, back), for the hits.
            const glm::vec3 blade_world = glm::normalize(model_new.r * m_way);
            const glm::mat3 aim_axes{ right, aim.up, -aim.forward };
            const glm::vec3 local       = glm::transpose(aim_axes) * blade_world;
            if (std::isfinite(local.x) && glm::length(local) > 0.5f) {
                std::scoped_lock _{ g_right_blade_mutex };
                g_right_blade_local = glm::normalize(local);
                g_right_blade_at    = std::chrono::steady_clock::now();
            }
            static int told = 0;
            if (told++ < 5) {
                spdlog::info("[BodyIK] (fix158) the right hand's melee weapon held by its own model (its blade {:.2f} m to its middle, along the fist's "
                             "line across the palm), as the left hand's",
                    blade * model_w.s);
            }
            return true;
        }

        // ---- (fix195) "Lock the gun's grip" ----
        // How the hand holds the gun (hand_in_weapon: the right wrist in the weapon's space) and where the left hand's support grip is
        // on it (support_in_weapon) were copied from the hidden first-person rig every frame. Its animation moves them a little all the
        // time (idle sway, a shot's kick, walking), and with the gun placed by your controller that moved the gun in your hand, and the
        // support point moved the two-handed aim line with it. Homard's Starfield VR locks both for each weapon once they hold still (FRIK
        // in Fallout 4 VR keeps a fixed offset per weapon). Here each is taken once it has held still for kGripSteadyFrames frames with
        // the gun out (not being drawn or put away), no reload and no shot in the last kShotQuietUs, and then kept for that gun. Live (the
        // animation's, as before) while the gun is drawn or put away and during a reload (kReloadLiveUs after the reload button), then
        // taken again; from scratch for a new gun. A lock the animation stays far from (kGripRetakeMove/Turn) for kGripRetakeUs with no
        // reload or shot, while the animation's grip itself holds still (a bad moment taken), is taken again there. "Holding still": the
        // whole window of kGripSteadyFrames within kGripSteadyMove and kGripSteadyTurn of its first frame (the review: judged frame by frame
        // a slow drift, the settling after a draw, passed as still).
        constexpr int     kGripSteadyFrames = 12;
        constexpr float   kGripSteadyMove   = 0.003f;    // m over the window
        constexpr float   kGripSteadyTurn   = 1.0f;      // degrees over the window
        constexpr int64_t kReloadLiveUs     = 2'500'000; // Starfield's reloads take about 1.5-3 s
        constexpr int64_t kShotQuietUs      = 300'000;
        constexpr float   kGripRetakeMove   = 0.04f;
        constexpr float   kGripRetakeTurn   = 12.0f;
        constexpr int64_t kGripRetakeUs     = 1'000'000;
        // (fix195, the second review) After a reload: the lock from before it kept as the reference; once the reload's live time is over,
        // locked again as soon as the animation's grip is back within kReloadBackMove/Turn of it (the live grip taken then), else once it
        // has held still for kReloadStillUs; live while it moves, up to kReloadSettleUs after the button (then the usual rule). At the button
        // the lock goes over to the live grip over kReloadBlendUs (no one-frame jump).
        constexpr float   kReloadBackMove   = 0.01f;
        constexpr float   kReloadBackTurn   = 4.0f;
        constexpr int64_t kReloadStillUs    = 1'000'000;
        constexpr int64_t kReloadSettleUs   = 6'000'000;
        constexpr int64_t kReloadBlendUs    = 150'000;
        // (fix198) After a draw: no lock until the gun has been out (state 3) for kDrawSettleUs and the animation's grip has held still
        // for kLockStillUs (a test locked the Grendel's grip mid-draw, 88 degrees off, for over a second). A lock the animation is far
        // from (over kGripFarMove or kGripFarTurn) for kGripFarFrames frames with no reload or shot is taken again at once there.
        constexpr int64_t kDrawSettleUs     = 500'000;
        constexpr int64_t kLockStillUs      = 500'000;
        constexpr float   kGripFarMove      = 0.03f;
        constexpr float   kGripFarTurn      = 30.0f;
        constexpr int     kGripFarFrames    = 3;
        // (fix195) Two-handed: no steering of the gun by the left hand when its grip on the gun is this close to the gun hand's (a pistol's
        // cupped hand): a hand's few millimetres of tremor turned the gun by degrees over so short a line (FRIK asks 15 game units
        // between the hands before the off hand grips). The left hand is still drawn on the gun.
        constexpr float kSupportMinLever = 0.15f;
        // (fix195) "Virtual stock" (Homard's): two-handed with the gun hand within kStockReach of its shoulder, the aim line starts up to
        // kStockShare of the way from the gun hand toward the shoulder (full share at kStockReach - kStockFade), which is steadier than a
        // hand. Not with the other hand within kStockMinFront of the shoulder. Onward and Pavlov steady two-handed aim the same way. The
        // shoulder: below and beside your head, by the body's facing (stage space: kShoulderSide out, kShoulderDown down, kShoulderBack
        // behind).
        constexpr float kStockShare    = 0.25f;
        constexpr float kStockReach    = 0.42f;
        constexpr float kStockFade     = 0.18f;
        constexpr float kStockMinFront = 0.18f;
        constexpr float kShoulderSide  = 0.19f;
        constexpr float kShoulderDown  = 0.15f;
        constexpr float kShoulderBack  = 0.05f;

        struct GripLockHand
        {
            bool    have_start{ false };
            Xf      start{};          // the animation's grip as the window of still frames began
            int64_t start_us{ 0 };    // ...and when
            int     steady{ 0 };      // frames since, all within the window's bounds of it
            bool    locked{ false };
            Xf      held{};
            int64_t far_since_us{ 0 };
            bool    has_ref{ false }; // (a reload) the lock from before it
            Xf      ref{};
            bool    blend{ false };   // (a reload) from the lock to the live grip
            Xf      blend_from{};
            int64_t blend_at_us{ 0 };
            int     far_frames{ 0 };  // (fix198) frames in a row the animation's grip was far from the lock
            // For the log: how far the animation's live grip was from the lock (what the lock kept out of the gun), and frames live.
            double  drift_sum{ 0.0 };
            float   drift_max{ 0.0f };
            double  drift_turn_sum{ 0.0 };
            float   drift_turn_max{ 0.0f };
            int     drift_n{ 0 };
            int     live_frames{ 0 };
        };
        struct GripLock
        {
            uint32_t        form{ 0 };
            RE::NiAVObject* weapon{ nullptr };       // the body's node placed in the hand (g_place.node)
            RE::NiAVObject* first_weapon{ nullptr }; // the hidden first-person rig's
            RE::NiAVObject* muzzle{ nullptr };       // the gun's muzzle: a new model at a swap has a new one
            int64_t         reload_seen_us{ 0 };
            bool            capture_ok{ false };     // this frame: the gun out, no reload, no shot just now
            bool            live_forced{ false };    // this frame: being drawn or put away, or reloading
            bool            settling{ false };       // this frame: within kReloadSettleUs of the reload button
            int64_t         out_since_us{ 0 };       // (fix198) since when the gun has been out (state 3); 0 while it is not
            GripLockHand    hand[2];                 // 0 the support (left) hand's grip, 1 the gun hand's
            int             locks{ 0 }, retakes{ 0 }, reloads{ 0 }, short_frames{ 0 }, stock_frames{ 0 }, frames{ 0 };
            float           stock_max{ 0.0f };
            int             lock_logs{ 0 };
            int             far_logs{ 0 };           // (fix198: the at-once retakes' own cap)
            int             two_handed_frames{ 0 }, near_frames{ 0 }; // (fix198) for the log: held in both hands; near, drawn on, not held
            int             unlocked_frames{ 0 };    // (fix198) held in both hands but not steered: its support grip not locked yet
        };
        GripLock g_grip_lock;

        int64_t SteadyNowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The turn between two rotations, degrees (from the quaternion of their difference: exact near 0, unlike acos of the trace).
        float TurnDegrees(const glm::mat3& a, const glm::mat3& b)
        {
            const glm::quat q = glm::normalize(glm::quat_cast(a * glm::transpose(b)));
            return glm::degrees(2.0f * std::atan2(glm::length(glm::vec3{ q.x, q.y, q.z }), std::abs(q.w)));
        }

        // From one grip to another, t from 0 (a) to 1 (b).
        Xf BlendXf(const Xf& a, const Xf& b, float t)
        {
            Xf out{};
            out.r = glm::mat3_cast(glm::slerp(glm::normalize(glm::quat_cast(a.r)), glm::normalize(glm::quat_cast(b.r)), t));
            out.t = glm::mix(a.t, b.t, t);
            out.s = a.s + (b.s - a.s) * t;
            return out;
        }

        // Once a frame with a gun out: a new gun (form, model, muzzle) starts over; a reload tap lets go of both locks; whether a grip may
        // be taken this frame, or must be the animation's.
        void GripLockFrame(uint32_t form, RE::NiAVObject* first_weapon, int64_t now)
        {
            auto&                 g      = g_grip_lock;
            RE::NiAVObject* const muzzle = g_body_muzzle.load();
            if (form != g.form || g_place.node != g.weapon || first_weapon != g.first_weapon || muzzle != g.muzzle) {
                g.form         = form;
                g.weapon       = g_place.node;
                g.first_weapon = first_weapon;
                g.muzzle       = muzzle;
                g.hand[0]      = GripLockHand{};
                g.hand[1]      = GripLockHand{};
                g.out_since_us = 0; // (fix198: a new gun's draw settles anew)
            }
            const int64_t tap = haptics::LastReloadTapUs();
            if (tap > g.reload_seen_us) {
                g.reload_seen_us = tap;
                if (g.reload_seen_us > 0) {
                    ++g.reloads;
                    for (auto& h : g.hand) {
                        if (h.locked) {
                            h.has_ref     = true;
                            h.ref         = h.held;
                            h.blend       = true;
                            h.blend_from  = h.held;
                            h.blend_at_us = now;
                        }
                        h.locked     = false;
                        h.steady     = 0;
                        h.have_start = false;
                    }
                }
            }
            const bool reloading = g.reload_seen_us > 0 && now - g.reload_seen_us < kReloadLiveUs;
            g.settling           = g.reload_seen_us > 0 && now - g.reload_seen_us < kReloadSettleUs;
            const bool out       = GameFlow::weaponState() == 3;
            if (!out) {
                g.out_since_us = 0;
            } else if (g.out_since_us == 0) {
                g.out_since_us = now;
            }
            g.live_forced        = !out || reloading;
            g.capture_ok         = !g.live_forced && now - haptics::LastFiredUs() > kShotQuietUs;
            ++g.frames;
        }

        // The grip to use this frame (which: 0 the support hand's, 1 the gun hand's): the locked one, or the animation's live one.
        Xf GripLockUse(int which, const Xf& live, int64_t now)
        {
            auto& g = g_grip_lock;
            auto& h = g.hand[which];
            if (h.have_start && glm::length(live.t - h.start.t) < kGripSteadyMove && TurnDegrees(live.r, h.start.r) < kGripSteadyTurn) {
                ++h.steady;
            } else {
                h.start      = live;
                h.start_us   = now;
                h.have_start = true;
                h.steady     = 0;
            }
            // The live grip, eased in from the lock over kReloadBlendUs after the reload button.
            const auto live_now = [&]() -> Xf {
                ++h.live_frames;
                if (h.blend && now - h.blend_at_us < kReloadBlendUs) {
                    return BlendXf(h.blend_from, live, std::clamp(static_cast<float>(now - h.blend_at_us) / kReloadBlendUs, 0.0f, 1.0f));
                }
                h.blend = false;
                return live;
            };
            if (g.live_forced) {
                h.locked       = false;
                h.far_since_us = 0;
                return live_now();
            }
            if (!h.locked) {
                bool take = false;
                if (h.has_ref && g.settling) {
                    // After a reload: back where it was held before, or holding still a second.
                    const bool back  = glm::length(live.t - h.ref.t) < kReloadBackMove && TurnDegrees(live.r, h.ref.r) < kReloadBackTurn;
                    const bool still = h.steady > 0 && now - h.start_us >= kReloadStillUs;
                    take             = g.capture_ok && (back || still);
                } else {
                    // (fix198) Out long enough for the draw to have settled, and still for kLockStillUs (not only kGripSteadyFrames).
                    h.has_ref = false;
                    take      = g.capture_ok && h.steady >= kGripSteadyFrames && now - h.start_us >= kLockStillUs && g.out_since_us != 0 &&
                           now - g.out_since_us >= kDrawSettleUs;
                }
                if (!take) {
                    return live_now();
                }
                h.locked       = true;
                h.held         = live;
                h.far_since_us = 0;
                h.far_frames   = 0;
                h.blend        = false;
                ++g.locks;
                if (g.lock_logs < 40) {
                    ++g.lock_logs;
                    spdlog::info("[BodyIK] (fix195) the {} grip on the gun {:08X} locked ({}; fix198: {:.1f} s after the gun came out): kept for this "
                                 "gun, live again only while it is drawn, put away or reloaded",
                        which == 1 ? "gun hand's" : "support hand's", g.form,
                        h.has_ref ? "after the reload, back as before it or holding still" : "held still",
                        g.out_since_us != 0 ? static_cast<double>(now - g.out_since_us) * 1e-6 : 0.0);
                }
                h.has_ref = false;
            }
            const float move = glm::length(live.t - h.held.t);
            const float turn = TurnDegrees(live.r, h.held.r);
            if (std::isfinite(move) && std::isfinite(turn)) {
                h.drift_sum += move;
                h.drift_max = std::max(h.drift_max, move);
                h.drift_turn_sum += turn;
                h.drift_turn_max = std::max(h.drift_turn_max, turn);
                ++h.drift_n;
            }
            // (fix198) Far off (a wrong lock, such as one taken mid-draw): taken again at once, after kGripFarFrames frames, but only at a
            // pose the animation rests in (its grip held still kGripSteadyFrames, the review: not in the middle of a move, such as a reload
            // the button did not start).
            h.far_frames = g.capture_ok && (move > kGripFarMove || turn > kGripFarTurn) && h.steady >= kGripSteadyFrames ? h.far_frames + 1 : 0;
            if (h.far_frames >= kGripFarFrames) {
                h.held         = live;
                h.far_frames   = 0;
                h.far_since_us = 0;
                ++g.retakes;
                if (g.far_logs < 40) {
                    ++g.far_logs;
                    spdlog::info("[BodyIK] (fix198) the {} grip on the gun {:08X}: the animation {:.3f} m and {:.1f} deg from the lock for {} frames "
                                 "with no reload or shot: taken again there at once",
                        which == 1 ? "gun hand's" : "support hand's", g.form, move, turn, kGripFarFrames);
                }
                return live;
            }
            if (g.capture_ok && (move > kGripRetakeMove || turn > kGripRetakeTurn)) {
                if (h.far_since_us == 0) {
                    h.far_since_us = now;
                } else if (now - h.far_since_us > kGripRetakeUs && h.steady >= kGripSteadyFrames) {
                    // (The review: only once the animation's grip itself holds still there, not in the middle of a move such as a reload
                    // the button did not start, an empty magazine's.)
                    h.held         = live;
                    h.far_since_us = 0;
                    ++g.retakes;
                    if (g.lock_logs < 40) {
                        ++g.lock_logs;
                        spdlog::info("[BodyIK] (fix195) the {} grip on the gun {:08X}: the animation held still {:.3f} m and {:.1f} deg from the lock "
                                     "for over a second with no reload or shot: taken again there",
                            which == 1 ? "gun hand's" : "support hand's", g.form, move, turn);
                    }
                    return live;
                }
            } else {
                h.far_since_us = 0;
            }
            return h.held;
        }

        // Every 10 s while a gun is out (60 times, then one a minute): the locks and what they kept out of the gun.
        void GripLockLog()
        {
            static TimedLog log;
            auto&           g = g_grip_lock;
            if (!log.Due(std::chrono::steady_clock::now(), std::chrono::seconds(10)) || g.frames == 0) {
                return;
            }
            const auto& r = g.hand[1];
            const auto& l = g.hand[0];
            spdlog::info("[BodyIK] (fix195) the gun's grip, the last 10 s ({} frames, lock {}): gun hand {} ({} frames live), the animation's grip "
                         "{:.4f} m and {:.2f} deg from the lock on average (at most {:.4f} m, {:.2f} deg); support hand {} ({} live), {:.4f} m and "
                         "{:.2f} deg (at most {:.4f}, {:.2f}); {} locks, {} taken again, {} reloads; the left hand not steering a short gun {} frames; "
                         "the virtual stock in {} frames (its share up to {:.2f}); (fix198) held in both hands (the left grip) {} frames (not steered "
                         "yet, the support grip not locked: {}), the left hand near and drawn on the gun but not holding it {} frames",
                g.frames, GameFlow::gStore.internalSettings.lockGunGrip ? "on" : "off", r.locked ? "locked" : "live", r.live_frames,
                r.drift_n > 0 ? r.drift_sum / r.drift_n : 0.0, r.drift_n > 0 ? r.drift_turn_sum / r.drift_n : 0.0, r.drift_max, r.drift_turn_max,
                l.locked ? "locked" : "live", l.live_frames, l.drift_n > 0 ? l.drift_sum / l.drift_n : 0.0,
                l.drift_n > 0 ? l.drift_turn_sum / l.drift_n : 0.0, l.drift_max, l.drift_turn_max, g.locks, g.retakes, g.reloads, g.short_frames,
                g.stock_frames, g.stock_max, g.two_handed_frames, g.unlocked_frames, g.near_frames);
            for (auto& h : g.hand) {
                h.drift_sum = h.drift_turn_sum = 0.0;
                h.drift_max = h.drift_turn_max = 0.0f;
                h.drift_n = h.live_frames = 0;
            }
            g.locks = g.retakes = g.reloads = g.short_frames = g.stock_frames = g.frames = 0;
            g.two_handed_frames = g.near_frames = g.unlocked_frames = 0;
            g.stock_max = 0.0f;
        }

        // (fix198) The support grip to steer the gun by, two-handed: the locked one, else the one locked before a reload (the review: the
        // animation's live one moved the aim line with it); with "Lock the gun's grip" on and neither (just drawn, or mid-reload): none, the
        // gun not steered yet. With the lock off (or not a gun): the live one, as before.
        bool SupportForSteering(bool lock_grip, const Xf& live, Xf& out)
        {
            if (!lock_grip) {
                out = live;
                return true;
            }
            const auto& h = g_grip_lock.hand[0];
            if (h.locked) {
                out = h.held;
                return true;
            }
            if (h.has_ref) {
                out = h.ref;
                return true;
            }
            return false;
        }

        // ---- (fix198) Two-handed steering only while the left grip holds the gun ----
        // The left hand turned the gun onto the line from the gun hand to it whenever it was near the gun's support grip (12 cm in, 25 cm
        // out), with no button: resting it near the gun while aiming steered the gun off where the right controller pointed (a test with
        // the cutter: steered in most frames with the left grip not squeezed at all). Now, as in Homard's Starfield VR: squeezing the left
        // grip with the hand at the gun's support grip (within kGrabReach during the press's attempt: its first kGrabAttemptUs, or as long
        // as the hand stays within kGrabNear) takes the gun in both hands, and letting go of the grip gives it back to the right hand
        // alone; so does the left hand ending up over kGrabLetGo from the support grip on the gun as steered (the gun follows the hand, so
        // only a hand taken off along the gun), the grip becoming another thing's (the favorites wheel open, the grip as LB in the star
        // map, build mode or the scanner, a climb), a gap of over kGrabGapUs in these updates, or another gun. Without the grip the left
        // hand is still drawn on the gun when near it, as before, but does not steer it. The steering eases in and out over kGrabBlendUs, with
        // a tick in the left controller each way (as Homard's). The grip's squeeze is read from the physical controller the left hand
        // follows ("Swap Controller Inputs", the review).
        constexpr float   kGrabOn        = 0.6f;
        constexpr float   kGrabOff       = 0.4f;
        constexpr float   kGrabReach     = 0.18f;
        constexpr float   kGrabNear      = 0.30f;
        constexpr float   kGrabLetGo     = 0.30f;
        constexpr int64_t kGrabAttemptUs = 300'000;
        constexpr int64_t kGrabGapUs     = 100'000;
        constexpr int64_t kGrabBlendUs   = 100'000;
        struct SupportGrab
        {
            bool     squeezed{ false };
            bool     attempt{ false };
            bool     grabbing{ false };
            bool     let_go{ false };  // the hand was found off the gun as steered (after the last update): let go now
            int64_t  pressed_us{ 0 };
            int64_t  seen_us{ 0 };
            uint32_t form{ 0 };
            float    weight{ 0.0f };   // the steering's share now (0..1), eased over kGrabBlendUs
            int64_t  weight_us{ 0 };
            int      logs{ 0 };
        };
        SupportGrab g_support_grab;
        // (fix198) For the gamepad's combos (ModConfig): the gun is held in both hands this update (false when the support was not
        // looked at: no gun, the support grip off, the left hand's own weapon).
        std::atomic<bool> g_gun_both_hands{ false };

        // The left grip this frame (its squeeze, with a band) against the support grip's distance: whether the gun is held in both hands.
        bool SupportGrabUpdate(float distance, uint32_t form, int64_t now)
        {
            static auto vr      = VR::get();
            auto&       sg      = g_support_grab;
            const float squeeze = vr->get_finger_state(vr->get_left_controller_index() == 1).grip;
            const bool  before  = sg.grabbing;
            const char* why     = nullptr;
            // Not looked at for a while (the support off, a menu, a swap) or another gun: from scratch; a grip held through it is no new press.
            if (now - sg.seen_us > kGrabGapUs || form != sg.form) {
                sg.grabbing = false;
                sg.attempt  = false;
                sg.squeezed = squeeze > kGrabOn;
                sg.form     = form;
                sg.weight   = 0.0f;
                why         = "a gap or another gun";
            }
            sg.seen_us      = now;
            const bool was  = sg.squeezed;
            sg.squeezed     = squeeze > (sg.squeezed ? kGrabOff : kGrabOn);
            if (sg.squeezed && !was) {
                sg.pressed_us = now;
                sg.attempt    = true;
            }
            if (!sg.squeezed || (now - sg.pressed_us >= kGrabAttemptUs && !(distance < kGrabNear))) {
                sg.attempt = false;
            }
            const bool other = GameFlow::isFavoritesDrawnNow() || HandActions::GripIsShoulder(true) || Climbing::HoldsGrip(true);
            if (!sg.squeezed || other || sg.let_go) {
                if (sg.grabbing && why == nullptr) {
                    why = !sg.squeezed ? "the grip let go" : other ? "the grip is the favorites wheel's, LB's or a climb's" : "the hand off the gun";
                }
                sg.grabbing = false;
                if (other || sg.let_go) {
                    sg.attempt = false; // (a new press needed)
                }
            } else if (!sg.grabbing) {
                sg.grabbing = sg.attempt && distance < kGrabReach;
            }
            sg.let_go = false;
            // The steering's share, eased.
            const float dt = sg.weight_us != 0 ? std::clamp(static_cast<float>(now - sg.weight_us) * 1e-6f, 0.0f, 0.1f) : 0.0f;
            sg.weight_us   = now;
            sg.weight      = std::clamp(sg.weight + (sg.grabbing ? dt : -dt) / (kGrabBlendUs * 1e-6f), 0.0f, 1.0f);
            if (sg.grabbing != before) {
                haptics::MeleePulse(true, sg.grabbing ? 0.35f : 0.2f, sg.grabbing ? 0.025f : 0.015f);
                if (sg.logs < 40) {
                    ++sg.logs;
                    spdlog::info("[BodyIK] (fix198) two-handed: the left hand {} the gun (left grip {:.2f}, {:.2f} m from its support grip{}{})",
                        sg.grabbing ? "takes" : "lets go of", squeeze, distance, why != nullptr ? "; " : "", why != nullptr ? why : "");
                }
            }
            return sg.grabbing;
        }

        // ---- (fix198) The virtual stock steadies the aim, it does not move it ----
        // fix195's stock turned the gun onto the line from a point partly (up to 25%) at the shoulder to the left hand. That line is not the
        // hands' line: with the gun held anywhere but against the shoulder it pointed off where the hands did, by several degrees (a test
        // with the cutter: in use in nearly every frame while mining, aiming off). Now the gun follows the stock line only for fast changes:
        // the difference between the hands' line and the stock line is followed slowly (kStockBiasTau) and added back, so held still the
        // gun points exactly along the hands' line (no offset at all), and a quick shake of the gun hand moves it up to a quarter less (the
        // stock's share). The difference is kept in the stock line's own frame (the review: kept in the world's, a quick turn of the gun
        // would carry it off the line it belongs to), and with no stock this frame (its share 0) the stock line is the hands' line, so the
        // difference fades to nothing instead of stopping at once.
        constexpr float kStockBiasTau = 0.25f; // seconds
        struct StockFilter
        {
            bool      ok{ false };
            glm::vec3 bias{ 0.0f }; // in the stock line's frame (x right, y along it, z up)
            int64_t   at_us{ 0 };
        };
        StockFilter g_stock_filter;

        // The way the gun points, two-handed: the hands' line (`hands`, from the gun hand's grip to the left hand), steadied by the stock's
        // (`stock`; the hands' line itself when the stock has no share) for fast changes only. `side_hint` gives the frame its second axis
        // when the line is near straight up or down (the body's forward). The filter starts over when it was not used for 0.25 s.
        glm::vec3 SteadiedLine(const glm::vec3& hands, const glm::vec3& stock, const glm::vec3& side_hint, int64_t now)
        {
            auto&     f     = g_stock_filter;
            glm::vec3 up_ax = glm::vec3{ 0.0f, 0.0f, 1.0f };
            if (std::abs(glm::dot(stock, up_ax)) > 0.95f) {
                up_ax = side_hint;
            }
            const glm::mat3 frame = FrameOf(stock, up_ax);
            const glm::vec3 diff  = glm::transpose(frame) * (hands - stock);
            const float     dt    = static_cast<float>(now - f.at_us) * 1e-6f;
            if (!f.ok || !(dt >= 0.0f && dt < 0.25f) || !std::isfinite(f.bias.x) || !std::isfinite(f.bias.y) || !std::isfinite(f.bias.z)) {
                f.bias = diff;
            } else {
                f.bias += (diff - f.bias) * (1.0f - std::exp(-dt / kStockBiasTau));
            }
            f.ok    = true;
            f.at_us = now;
            const glm::vec3 way = stock + frame * f.bias;
            if (!(glm::length(way) > 1e-4f) || !std::isfinite(way.x) || !std::isfinite(way.y) || !std::isfinite(way.z)) {
                f.ok = false;
                return hands;
            }
            return glm::normalize(way);
        }

        // (fix195) The virtual stock's rear end of the aim line for a gun held with both hands: the gun hand's grip moved up to kStockShare
        // toward that hand's shoulder when the hand is near it. `pivot` the gun hand's grip (game world), `body_forward` the body's own
        // facing (game world: the review: from the headset's look, turning only the head swung the shoulder round and the aim with it);
        // its share returned in `share`. The shoulder on the side of the physical controller holding the gun (the second review: by where
        // the hand was beside the head it flipped 0.38 m as the hand crossed the middle; swapped controllers: the left), smoothed over
        // kShoulderSmooth so a nod or a lean of the head does not steer the aim. The share fades out over kStockFrontFade as the other hand
        // comes within kStockMinFront of the shoulder (no one-frame switch).
        constexpr float kShoulderSmooth  = 0.3f;  // seconds
        constexpr float kStockFrontFade  = 0.05f; // m
        glm::vec3 StockRear(const glm::vec3& pivot, const glm::vec3& body_forward, float& share)
        {
            share = 0.0f;
            if (!GameFlow::gStore.internalSettings.virtualStock) {
                return pivot;
            }
            glm::vec3 ahead{ 0.0f };
            if (!tracking::WorldToStageDirection(body_forward, ahead)) {
                return pivot;
            }
            ahead.y = 0.0f;
            if (!(glm::length(ahead) > 0.2f)) {
                return pivot;
            }
            static auto     vr       = VR::get();
            const glm::mat4 head     = tracking::HeadPose();
            ahead                    = glm::normalize(ahead);
            const glm::vec3 up       = glm::vec3{ 0.0f, 1.0f, 0.0f };
            const glm::vec3 right    = glm::normalize(glm::cross(ahead, up));
            const glm::vec3 gun_at   = glm::vec3{ tracking::GripPose(false)[3] };
            const glm::vec3 left_at  = glm::vec3{ tracking::GripPose(true)[3] };
            const float     side     = vr->get_right_controller_index() == 1 ? -1.0f : 1.0f; // (index 1: the physical left controller)
            const glm::vec3 raw      = glm::vec3{ head[3] } + right * (side * kShoulderSide) - up * kShoulderDown - ahead * kShoulderBack;
            // Smoothed (stage space): at once after a pause or a jump of over 0.15 m (a recentre, the other side).
            static glm::vec3 smooth{ 0.0f };
            static auto      smooth_at = std::chrono::steady_clock::time_point{};
            const auto       now       = std::chrono::steady_clock::now();
            const float      dt        = std::chrono::duration<float>(now - smooth_at).count();
            if (smooth_at == std::chrono::steady_clock::time_point{} || !(dt < 0.5f) || !(glm::length(raw - smooth) < 0.15f)) {
                smooth = raw;
            } else {
                smooth += (raw - smooth) * (1.0f - std::exp(-std::max(dt, 0.0f) / kShoulderSmooth));
            }
            smooth_at                = now;
            const glm::vec3 shoulder = smooth;
            const float     front    = std::clamp((glm::length(left_at - shoulder) - kStockMinFront) / kStockFrontFade, 0.0f, 1.0f);
            const float     w = std::clamp((kStockReach - glm::length(gun_at - shoulder)) / kStockFade, 0.0f, 1.0f) * kStockShare * front;
            if (!(w > 0.005f)) {
                return pivot;
            }
            glm::mat4 shoulder_pose{ 1.0f };
            shoulder_pose[3] = glm::vec4{ shoulder, 1.0f };
            const auto at    = HandWorld(shoulder_pose, nullptr, HandGrab::GunKnownOut()); // (fix195: as the gun)
            if (!at || !std::isfinite(at->position.x)) {
                return pivot;
            }
            share = w;
            return pivot + (at->position - pivot) * w;
        }

        void ApplyIK(const Pose& pose)
        {
            if (g_bones.head >= 0) {
                pose.local[g_bones.head].scale = 0.0f;
            }
            MeasureHand(pose, g_bones.right, "right");
            MeasureHand(pose, g_bones.left, "left");
            MeasureElbow(pose, g_bones.right, "right");
            MeasureElbow(pose, g_bones.left, "left");
            // Animated feet, captured before the pelvis moves (the feet hang below it).
            std::optional<std::pair<Xf, Xf>> feet;
            if (g_bones.left_leg.foot >= 0 && g_bones.right_leg.foot >= 0) {
                feet = std::make_pair(pose.GameWorld(g_bones.left_leg.foot), pose.GameWorld(g_bones.right_leg.foot));
            }
            ApplyCrouch(pose);
            if (!ApplyTrackedTorso(pose)) {
                ApplyLean(pose);
                ApplySpineTwist(pose); // (fix157)
            }
            if (feet) {
                ApplyLegs(pose, feet->first, feet->second);
            }
            MeasurePalmSide(pose, 0, g_bones.left.shape, g_bones.left.wrist);
            MeasurePalmSide(pose, 1, g_bones.right.shape, g_bones.right.wrist);
            MeasureGripFrame(pose, 0, g_bones.left);
            MeasureGripFrame(pose, 1, g_bones.right);

            const auto room = tracking::RoomRotation();
            if (!room) {
                return;
            }
            std::optional<Xf>   held_weapon;
            RE::NiAVObject*     first_weapon_node{ nullptr };
            RE::NiAVObject*     first_right_wrist{ nullptr };
            RE::NiAVObject*     first_left_wrist{ nullptr };

            g_support = {};
            // Stored once, when this update is done (other threads read it: no false in between while it is held).
            bool support_on_gun = false;
            struct StoreSupport
            {
                bool& value;
                ~StoreSupport() { g_support_on_gun = value; }
            } store_support{ support_on_gun };
            // (fix198) The same for the gun held in both hands (the gamepad's left-grip combos stand down meanwhile, ModConfig).
            bool both_hands_now = false;
            struct StoreBothHands
            {
                bool& value;
                ~StoreBothHands() { g_gun_both_hands.store(value, std::memory_order_relaxed); }
            } store_both_hands{ both_hands_now };
            auto& r = g_bones.right;
            // (fix195) The gun and the hands on it placed with this frame's room turn (HandWorld's carry_turn): only with a gun out.
            const bool gun_turn = HandGrab::GunKnownOut();
            if (r.biceps >= 0 && r.forearm >= 0 && r.wrist >= 0 && r.shape.valid) {
                auto grip = HandWorld(tracking::GripPose(false), nullptr, gun_turn);
                auto aim  = HandWorld(tracking::AimPose(false), nullptr, gun_turn);
                SupportShift(false, grip, aim); // (fix170: the second hand on a held object, drawn on its grip)
                HoldPersonShift(false, grip, aim); // (fix174: on the person it holds)
                HoldWallShift(false, grip, aim);   // (fix180: on the wall it holds on to)
                if (grip && aim) {
                    auto wrist_target = WristTarget(pose, r, *aim, *grip);

                    g_diag.weapon_aligned = false;
                    auto player           = CreationEngineSingletonManager::GetPlayerRef();
                    g_right_melee_fist    = false; // (fix159: set again below when it is held so)
                    // A thrown melee weapon (MeleeThrow): the hand is empty and the weapon is placed where it flies, below.
                    const bool thrown     = MeleeThrow::Away(false);
                    if (player && player->IsWeaponDrawn() && g_place.idx >= 0 && !thrown) {
                        // The first-person rig's weapon bone (the "Weapon" under its C_Chest), kept while the rig and the bone's place
                        // in it stay the same. A weapon's own model can be named "Weapon" too (some modded weapons): it is never taken
                        // for the bone (it comes and goes with the weapon).
                        static RE::NiAVObject* first_root{ nullptr };
                        static RE::NiAVObject* first_wrist{ nullptr };
                        static RE::NiAVObject* first_left{ nullptr };
                        static RE::NiAVObject* first_chest{ nullptr };
                        static RE::NiAVObject* first_bone{ nullptr };
                        auto root = FirstPersonRoot(player);
                        if (root != first_root || (first_bone && ParentOf(first_bone) != first_chest)) {
                            first_root  = root;
                            first_wrist = root ? FindDescendant(root, "R_Wrist") : nullptr;
                            first_left  = root ? FindDescendant(root, "L_Wrist") : nullptr;
                            first_chest = root ? FindDescendant(root, "C_Chest") : nullptr;
                            first_bone  = first_chest ? FindChild(first_chest, "Weapon") : nullptr;
                            spdlog::info("[BodyIK] 1P rig: R_Wrist {} L_Wrist {} weapon bone {} (the Weapon under C_Chest)", first_wrist != nullptr,
                                first_left != nullptr, first_bone != nullptr);
                        }
                        // The first-person node matching the one placed: the bone; for a weapon whose own model is named "Weapon", that
                        // model in the first-person rig; in a rig without that bone, the first node named Weapon (found again each frame).
                        RE::NiAVObject* first_weapon = first_bone;
                        if (g_place.model && root) {
                            if (auto model = FindDescendantOther(root, "Weapon", first_bone)) {
                                first_weapon = model;
                            }
                        } else if (!first_bone && root) {
                            first_weapon = FindDescendant(root, "Weapon");
                        }
                        first_weapon_node = first_weapon;
                        first_right_wrist = first_wrist;
                        first_left_wrist  = first_left;
                        // (fix189) The muzzle (g_body_muzzle) was found this frame before the barrel was measured (RefreshMuzzle, PrepareFrame);
                        // before, it was found only here, so with the IK not running it kept an old node, read again next frame.
                        // (fix156) The weapon in your hands changed (its form): what was learned of the last one forgotten here too (the
                        // bone, and a muzzle-less weapon's none, can stay the same through a swap); and the first-person rig's state logged 1 s
                        // and 3 s after (a saber was held wrong after a gun -> saber swap from the menu until a loading screen: the
                        // hold is copied from the hidden first-person rig every frame, which the load rebuilt; a helper's research).
                        {
                            static uint32_t form_seen = 0;
                            static uint32_t form_last = 0; // (the last weapon, not fists: gun -> fists -> saber is a change too; the review)
                            static auto     changed_at = std::chrono::steady_clock::time_point{};
                            static int      told       = 2;
                            static int      told_lines = 0;
                            const uint32_t  form_now   = HandGrab::WeaponInHandsId();
                            const auto      at_now     = std::chrono::steady_clock::now();
                            if (form_now != form_seen) {
                                if (form_now != 0 && form_last != 0 && form_now != form_last) {
                                    g_basis = {};
                                }
                                if (form_now != 0) {
                                    form_last = form_now;
                                }
                                form_seen  = form_now;
                                changed_at = at_now;
                                told       = 0;
                            }
                            if (told < 2 && form_now != 0 && told_lines < 40 && at_now - changed_at > std::chrono::seconds(told == 0 ? 1 : 3)) {
                                ++told;
                                ++told_lines;
                                std::string kids;
                                RE::NiAVObject* children[8]{};
                                const auto      n = first_bone ? ReadChildren(first_bone, children, 8) : 0;
                                for (std::uint16_t i = 0; i < n; ++i) {
                                    kids += std::string{ i ? ", " : "" } + (children[i] ? children[i]->name.c_str() : "?");
                                }
                                std::string body_kids;
                                RE::NiAVObject* body_children[8]{};
                                const auto      bn = g_place.node ? ReadChildren(g_place.node, body_children, 8) : 0;
                                for (std::uint16_t i = 0; i < bn; ++i) {
                                    body_kids += std::string{ i ? ", " : "" } + (body_children[i] ? body_children[i]->name.c_str() : "?");
                                }
                                float       sampled_deg = -1.0f;
                                auto* const rig_camera  = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                                if (first_weapon && rig_camera && rig_camera->parent && g_basis.has) {
                                    const auto to_weapon = glm::transpose(FromNi(first_weapon->world).r);
                                    const auto view_r    = FromNi(rig_camera->parent->world).r;
                                    sampled_deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(to_weapon * view_r[1]), g_basis.forward), -1.0f, 1.0f)));
                                }
                                spdlog::info("[BodyIK] (fix156) {} s after the weapon {:08X} came: the first-person rig {} (hidden {}); its weapon bone holds [{}]; the "
                                             "body's holds [{}]; the hold sampled now {:.1f} deg from the one in use (basis from {})",
                                    told == 1 ? 1 : 3, form_now, root ? "found" : "not found", root ? IsAppCulled(root) : false, kids, body_kids, sampled_deg,
                                    g_basis.from_muzzle                             ? "the muzzle"
                                    : g_basis.multi_beam && g_basis.hold_learned    ? "the firing hold (fix206: a multi-beam weapon)"
                                    : g_basis.multi_beam && g_basis.axis_kept       ? "the muzzle until the firing hold is learned (fix206)"
                                    : g_basis.fired                                 ? "a shot"
                                                                                    : "the estimate");
                            }
                        }
                        SampleWeaponBasis(first_weapon);
                        if (first_wrist && first_weapon) {
                            // The animation's grip: the hand in the weapon's space, transferred onto the body's hand by anatomy.
                            MeasureFirstPersonHand(first_wrist, false);
                            Xf first_hand_world = FromNi(first_wrist->world);
                            if (g_first_hands[1].shape.valid) {
                                first_hand_world = BodyWristFromFirstPerson(first_hand_world, r.shape, false);
                            }
                            auto hand_in_weapon = Compose(Inverse(FromNi(first_weapon->world)), first_hand_world);

                            // Weapon models carry an authored right-hand IK target (R_HandIk). The hand's offset from it is
                            // taken from the first-person rig and applied to the third-person model's own target.
                            auto first_ik = FindDescendant(first_weapon, "R_HandIk");
                            auto body_ik  = g_place.node ? FindDescendant(g_place.node, "R_HandIk") : nullptr;
                            static RE::NiAVObject* logged_ik{ nullptr };
                            if (logged_ik != g_place.node) {
                                logged_ik = g_place.node;
                                spdlog::info("[BodyIK] R_HandIk: 1P {} 3P {}", first_ik != nullptr, body_ik != nullptr);
                            }
                            if (first_ik && body_ik) {
                                const auto hand_from_ik = Compose(Inverse(FromNi(first_ik->world)), first_hand_world);
                                const auto ik_in_weapon = Compose(Inverse(FromNi(g_place.node->world)), FromNi(body_ik->world));
                                hand_in_weapon          = Compose(ik_in_weapon, hand_from_ik);
                            }
                            // (fix195) "Lock the gun's grip": a gun's hand-in-weapon kept once it has held still (GripLockUse above), so the
                            // animation's sway and kick no longer move the gun in the hand.
                            const bool    lock_grip = GameFlow::gStore.internalSettings.lockGunGrip && HandGrab::GunKnownOut();
                            const int64_t lock_now  = SteadyNowUs();
                            if (lock_grip) {
                                GripLockFrame(HandGrab::WeaponInHandsId(), first_weapon, lock_now);
                                hand_in_weapon = GripLockUse(1, hand_in_weapon, lock_now);
                            }
                            Xf         weapon_xf      = Compose(wrist_target, Inverse(hand_in_weapon));
                            // (fix158) A one-handed melee weapon: held by its own model (FixedMeleeHold), the hand where the controller puts it.
                            const bool fixed_melee = HandGrab::OneHandedMeleeOut() && FixedMeleeHold(g_place.node, g_place.model, *aim, *grip, weapon_xf);
                            g_right_melee_fist     = fixed_melee;
                            // (fix159) Its light (a lightsaber's) at half.
                            if (HandGrab::MeleeWeaponOut() != 0) {
                                DimWeaponLights(g_place.node);
                                DimWeaponLights(first_weapon);
                                DimSightLights(); // (fix160: through the game's own pointer to it)
                            }
                            if (!fixed_melee && g_basis.has && g_basis.weapon == g_place.node) {
                                // Weapon from the aim ray: barrel forward, weapon up along the controller's up.
                                weapon_xf.r = FrameOf(aim->forward, aim->up) * glm::transpose(FrameOf(g_basis.forward, g_basis.up));
                                // Hand from the weapon, then both seated so the palm centre is on the grip point.
                                wrist_target           = Compose(weapon_xf, hand_in_weapon);
                                const auto palm_world  = wrist_target.t + wrist_target.r * (r.shape.forward * r.shape.palm) * wrist_target.s;
                                const auto seat        = grip->position - palm_world;
                                weapon_xf.t += seat;
                                wrist_target.t += seat;
                            }
                            g_barrel.aim_forward = aim->forward;

                            // Two-handed: the left hand is drawn on the support grip when it comes near it; (fix198) taken with the left
                            // grip squeezed there, the weapon then turns about the right grip so the support point lies toward the left
                            // hand (ROCK; Homard's grip-held rule).
                            g_support = {};
                            const auto& l_arm = g_bones.left;
                            const auto  left_grip = HandWorld(tracking::GripPose(true), nullptr, gun_turn); // (fix195: as the gun)
                            // (fix154: not while the left hand holds its own weapon, dual wielding.)
                            if (GameFlow::gStore.internalSettings.supportHand && first_left && l_arm.shape.valid && left_grip && !LeftWeapon::Held() && !fixed_melee) {
                                MeasureFirstPersonHand(first_left, true);
                                if (g_first_hands[0].shape.valid) {
                                    auto support_in_weapon = Compose(Inverse(FromNi(first_weapon->world)),
                                        BodyWristFromFirstPerson(FromNi(first_left->world), l_arm.shape, true));
                                    // (fix195) The support grip's place on the gun locked as the gun hand's is: the aim line no longer
                                    // follows the animation's left hand.
                                    if (lock_grip) {
                                        support_in_weapon = GripLockUse(0, support_in_weapon, lock_now);
                                    }
                                    auto support = Compose(weapon_xf, support_in_weapon);
                                    auto palm    = support.t + support.r * (l_arm.shape.forward * l_arm.shape.palm) * support.s;
                                    const float distance    = glm::length(palm - left_grip->position);
                                    static bool held        = false;
                                    // (fix198) Taken in both hands (the left grip squeezed at the support grip, SupportGrabUpdate): steers the
                                    // gun, easing in and out (its weight). Only near it: the left hand drawn on the gun, as before, without
                                    // steering it.
                                    const bool  two_handed  = SupportGrabUpdate(distance, HandGrab::WeaponInHandsId(), lock_now);
                                    const float steer_w     = g_support_grab.weight;
                                    held                    = two_handed || steer_w > 0.0f || distance < (held ? 0.25f : 0.12f);
                                    g_diag.support_distance = distance;
                                    both_hands_now          = two_handed;
                                    bool steered            = false;
                                    if (held) {
                                        const auto pivot = grip->position;
                                        // (fix198) The support point the gun is steered by: the locked one, else the one locked before a reload;
                                        // with "Lock the gun's grip" on and neither, none (the review: not one the animation still moves); off,
                                        // the live one as before. The left hand is still drawn at the support grip as it is now (below).
                                        Xf         steer_support{};
                                        const bool have_steer = SupportForSteering(lock_grip, support_in_weapon, steer_support);
                                        const Xf   steer_world = Compose(weapon_xf, steer_support);
                                        const auto steer_palm  = steer_world.t + steer_world.r * (l_arm.shape.forward * l_arm.shape.palm) * steer_world.s;
                                        const float lever = glm::length(steer_palm - pivot);
                                        // (fix195) A short gun (the left hand cupping the gun hand): held, not steered. With a band (steered
                                        // from kSupportMinLever + 1 cm, not from under kSupportMinLever - 2 cm), so a grip near the limit does
                                        // not flip between the two (the review). Part of "Lock the gun's grip" (off: steered as before fix195).
                                        static bool steer = true;
                                        steer = !GameFlow::gStore.internalSettings.lockGunGrip ||
                                                (steer ? lever >= kSupportMinLever - 0.02f : lever >= kSupportMinLever + 0.01f);
                                        if (!(steer_w > 0.0f)) {
                                            // (fix198) Not taken with the grip: the gun stays on the right hand's aim.
                                        } else if (!have_steer) {
                                            ++g_grip_lock.unlocked_frames;
                                        } else if (!steer || !std::isfinite(lever)) {
                                            ++g_grip_lock.short_frames;
                                        } else {
                                            // (fix195) The aim line's rear end: the gun hand, or partly the shoulder (the virtual stock);
                                            // (fix198) the stock's line used for fast changes only (SteadiedLine): held still, the hands' line.
                                            // With no share this frame the stock's line is the hands' own, so the filter's difference fades.
                                            const glm::vec3 body_forward = pose.to_world.r * pose.root.r[1];
                                            float           share        = 0.0f;
                                            const glm::vec3 rear         = StockRear(pivot, body_forward, share);
                                            if (share > 0.0f) {
                                                ++g_grip_lock.stock_frames;
                                                g_grip_lock.stock_max = std::max(g_grip_lock.stock_max, share);
                                            }
                                            const glm::vec3 hands_way = left_grip->position - pivot;
                                            const glm::vec3 stock_way = left_grip->position - rear;
                                            if (glm::length(hands_way) > 1e-3f && glm::length(stock_way) > 1e-3f &&
                                                glm::length(steer_palm - pivot) > 1e-3f) {
                                                const glm::vec3 hands_n = glm::normalize(hands_way);
                                                const glm::vec3 way     = SteadiedLine(hands_n, share > 0.0f ? glm::normalize(stock_way) : hands_n,
                                                    body_forward, lock_now);
                                                glm::mat3 turn = RotationBetween(steer_palm - pivot, way);
                                                if (steer_w < 1.0f) {
                                                    // (fix198) Easing in or out: part of the turn (the review: no one-frame jump at a grab or let-go).
                                                    turn = glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::normalize(glm::quat_cast(turn)),
                                                        steer_w));
                                                }
                                                weapon_xf.r     = turn * weapon_xf.r;
                                                weapon_xf.t     = pivot + turn * (weapon_xf.t - pivot);
                                                wrist_target.r  = turn * wrist_target.r;
                                                wrist_target.t  = pivot + turn * (wrist_target.t - pivot);
                                                steered         = true;
                                                // (fix198) The left hand taken off along the gun (the gun follows it, so the distance before the turn
                                                // says nothing then): over kGrabLetGo from the support grip as steered, let go at the next update.
                                                if (two_handed && steer_w >= 1.0f) {
                                                    const Xf   after      = Compose(weapon_xf, steer_support);
                                                    const auto after_palm = after.t + after.r * (l_arm.shape.forward * l_arm.shape.palm) * after.s;
                                                    if (!(glm::length(after_palm - left_grip->position) < kGrabLetGo)) {
                                                        g_support_grab.let_go = true;
                                                    }
                                                }
                                            }
                                        }
                                        support          = Compose(weapon_xf, support_in_weapon);
                                        g_support.held   = true;
                                        support_on_gun   = true;
                                        g_support.target = support;
                                        if (g_basis.has) {
                                            g_barrel.aim_forward = glm::normalize(weapon_xf.r * g_basis.forward);
                                        }
                                    }
                                    if (two_handed) {
                                        ++g_grip_lock.two_handed_frames;
                                    } else if (held) {
                                        ++g_grip_lock.near_frames;
                                    }
                                    if (!steered) {
                                        g_stock_filter.ok = false; // (fix198: the stock's filter starts over at the next two-handed hold)
                                    }
                                }
                            }
                            if (lock_grip) {
                                GripLockLog(); // (fix195)
                            }

                            g_barrel.placed        = true;
                            g_barrel.placed_weapon = weapon_xf;
                            // (fix146) For the laser: the muzzle where the gun is drawn this frame and its barrel's way, in your aiming hand's
                            // own axes (its controller's aim, from which the gun was just placed): the laser then starts where the gun is drawn
                            // in every frame, running or turning (before, the muzzle as last drawn, put in the room with this frame's camera:
                            // behind by your speed times a frame or two, after fix145: out of line with the gun until you stopped).
                            // (This gun's own measure only: not the last gun's for a frame after a swap; the review.)
                            if (g_barrel.has_muzzle_in_weapon && g_barrel.weapon == g_place.node && g_barrel.muzzle == g_body_muzzle.load()) {
                                auto*       scene_root = CreationEngineSingletonManager::GetSceneGraphRoot();
                                auto*       camera     = scene_root ? scene_root->worldCamera : nullptr;
                                const float units      = tracking::TrackingScale() * (camera && camera->parent ? camera->parent->world.scale : 1.0f);
                                const Xf    muzzle_now = Compose(weapon_xf, g_barrel.muzzle_in_weapon);
                                const auto  axes       = glm::transpose(FrameOf(aim->forward, aim->up)); // (right, forward, up)
                                if (std::isfinite(units) && units > 1e-4f) {
                                    const glm::vec3 at  = axes * (muzzle_now.t - aim->position) / units;
                                    const glm::vec3 way = glm::normalize(axes * g_barrel.aim_forward);
                                    if (std::isfinite(at.x) && std::isfinite(way.x) && glm::length(at) < 1.5f) {
                                        std::scoped_lock _{ g_muzzle_mutex };
                                        g_muzzle_in_aim     = at;
                                        g_barrel_in_aim     = way;
                                        g_muzzle_in_aim_at  = std::chrono::steady_clock::now();
                                        g_muzzle_in_aim_set = true;
                                    }
                                }
                            }
                            // For throwing it (melee weapons): where it is in your hand now, and its model as last drawn.
                            if (auto node = g_place.node) {
                                const Xf drawn = FromNi(node->world);
                                float    bound[4]{};
                                std::memcpy(bound, reinterpret_cast<const std::uint8_t*>(node) + kWorldBoundOffset, sizeof(bound));
                                MeleeThrow::OnWeaponInHand(false, weapon_xf.r, weapon_xf.t, weapon_xf.s, drawn.r, drawn.t, drawn.s,
                                    glm::vec3{ bound[0], bound[1], bound[2] }, bound[3]);
                            }
                            pose.SetGameWorld(g_place.idx, weapon_xf);
                            g_diag.weapon_aligned = true;
                            held_weapon           = weapon_xf;
                        }
                    } else {
                        g_body_muzzle = nullptr;
                    }
                    const auto bend = TrackedBend(joint::kArmUpper[1], joint::kArmLower[1], joint::kWrist[1]);
                    g_track_diag.elbow[1] = bend.has_value();
                    g_diag.right_gap      = SolveArm(pose, r, false, wrist_target, bend);
                    ApplyFingers(pose, 1, r.wrist, held_weapon && !g_right_melee_fist ? first_right_wrist : nullptr);
                    NoteDrawnPalm(pose, 1, r); // (fix181)
                    // The thrown weapon where it flies (after the arm, which is its bone's parent, is final).
                    if (thrown && player && player->IsWeaponDrawn() && g_place.idx >= 0) {
                        g_body_muzzle = nullptr;
                        Xf flight{};
                        if (MeleeThrow::FlightPose(false, flight.r, flight.t, flight.s)) {
                            pose.SetGameWorld(g_place.idx, flight, kThrownWeaponReach);
                        }
                    }
                }
            }

            auto& l = g_bones.left;
            if (l.biceps >= 0 && l.forearm >= 0 && l.wrist >= 0 && l.shape.valid) {
                glm::vec3 grip_from{ 0.0f };
                auto      grip = HandWorld(tracking::GripPose(true), &grip_from);
                auto      aim  = HandWorld(tracking::AimPose(true));
                // (fix169) Holding the left hand's weapon (not thrown): the hand from the copy's own place this frame, on its hilt.
                if (glm::vec3 shift{}; grip && aim && LeftWeapon::Held() && !MeleeThrow::Away(true) && LeftCopyShift(grip_from, shift)) {
                    grip->position += shift;
                    aim->position += shift;
                }
                SupportShift(true, grip, aim); // (fix170: the second hand on a held object, drawn on its grip)
                HoldPersonShift(true, grip, aim); // (fix174: on the person it holds)
                HoldWallShift(true, grip, aim);   // (fix180: on the wall it holds on to)
                if (grip && aim) {
                    auto       target  = WristTarget(pose, l, *aim, *grip);
                    const bool on_grip = held_weapon && g_support.held;
                    if (on_grip) {
                        target = g_support.target;
                    }
                    g_diag.support_held = on_grip;
                    const auto bend       = TrackedBend(joint::kArmUpper[0], joint::kArmLower[0], joint::kWrist[0]);
                    g_track_diag.elbow[0] = bend.has_value();
                    g_diag.left_gap       = SolveArm(pose, l, true, target, bend);
                    ApplyFingers(pose, 0, l.wrist, on_grip ? first_left_wrist : nullptr);
                    NoteDrawnPalm(pose, 0, l); // (fix181)
                }
            }
        }

        safetyhook::InlineHook g_model_update_hook;

        // The pose-buffer conventions are measured against the engine before any write.
        struct Calibration
        {
            enum class Status { Measuring, Ready, Failed } status{ Status::Ready };
            int  world_variant{ 3 };  // pose worlds are game world (synced verbatim to the nodes)
            int  good_frames{ 0 };
            int  frames{ 0 };
            bool has_measured{ false };
            Xf   measured{};  // pose -> game world measured from the engine's last update
        };
        Calibration g_calibration;
        constexpr int kWorldVariants = 5;
        constexpr const char* kWorldVariantNames[kWorldVariants] = { "root world", "root translation", "root transposed", "identity", "measured" };

        // Pose space -> game world.
        Xf ToWorldVariant(int variant)
        {
            const RE::NiAVObject* const state_root = g_state.root; // (fix189: read once: another thread may clear it)
            const auto root = state_root ? FromNi(state_root->world) : Xf{};
            switch (variant) {
            case 0:
                return root;
            case 1:
                return { glm::mat3{ 1.0f }, root.t, root.s };
            case 2:
                return { glm::transpose(root.r), root.t, root.s };
            case 4:
                return g_calibration.measured;
            default:
                return Xf{};
            }
        }

        float AngleBetween(const glm::mat3& a, const glm::mat3& b)
        {
            const auto d = a * glm::transpose(b);
            return glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
        }

        // (fix173) `player` false: another model (a held person's): its worlds are game world as they are (the calibration's identity),
        // and the player's root is not read (from that model's update thread; the review).
        Pose ReadPose(std::uint8_t* model, const RE::NiTransform* root_local, bool player = true)
        {
            Pose pose;
            const auto storage = *reinterpret_cast<std::uint8_t**>(model + 0x10);
            if (!storage) {
                return pose;
            }
            const auto locals  = *reinterpret_cast<std::uint8_t**>(storage + 0x08);
            const auto worlds  = *reinterpret_cast<std::uint8_t**>(storage + 0x10);
            pose.parent        = *reinterpret_cast<std::uint16_t**>(storage + 0x28);
            if (!locals || !worlds || !pose.parent) {
                return pose;
            }
            // Each array object holds its data pointer at +0x10 (0x2be9424..0x2be9435).
            pose.local = *reinterpret_cast<RE::NiTransform**>(locals + 0x10);
            pose.world = *reinterpret_cast<RE::NiTransform**>(worlds + 0x10);
            if (!pose.local || !pose.world) {
                pose.local = nullptr;
                return pose;
            }
            pose.model    = model;
            pose.count    = *reinterpret_cast<std::uint16_t*>(model + 0x78);
            pose.top      = static_cast<std::uint16_t>(*reinterpret_cast<std::uint16_t*>(model + 0x7c) + *reinterpret_cast<std::uint16_t*>(model + 0x7e));
            pose.root     = FromNi(*root_local);  // top-level entries: world = local * root (0x2be953d)
            pose.to_world = player ? ToWorldVariant(g_calibration.world_variant) : Xf{};
            return pose;
        }

        bool RagdollActive(std::uint8_t* model)
        {
            auto component = *reinterpret_cast<void**>(model + 0x70);
            if (!component) {
                return false;
            }
            using fn_t = bool (*)(void*);
            return (*reinterpret_cast<fn_t**>(component))[4](component);
        }

        // (fix189) Each frame, after the weapon in your hands is chosen and before its barrel is measured: its muzzle (the ProjectileNode
        // under it) found anew, with the frame it was found in, when the IK will place that weapon in your hand this frame (as ApplyIK
        // found it before: the weapon drawn and not thrown, the right arm and the room known); none otherwise. Before, it was found only in
        // ApplyIK, so the barrel was measured from the frame before's node, and while the IK did not run the node was kept and handed to the
        // game's shots: freed with a weapon unequipped meanwhile.
        void RefreshMuzzle()
        {
            static auto vr     = VR::get();
            auto*       player = CreationEngineSingletonManager::GetPlayerRef();
            auto*       scene  = CreationEngineSingletonManager::GetSceneGraphRoot();
            auto*       camera = scene ? scene->worldCamera : nullptr;
            const auto& r      = g_bones.right;
            RE::NiAVObject* muzzle = nullptr;
            if (g_calibration.status == Calibration::Status::Ready && r.biceps >= 0 && r.forearm >= 0 && r.wrist >= 0 && r.shape.valid && camera &&
                camera->parent && tracking::RoomRotation().has_value() && player && player->IsWeaponDrawn() && g_place.idx >= 0 && g_place.node &&
                !MeleeThrow::Away(false)) {
                muzzle = FindDescendant(g_place.node, "ProjectileNode");
            }
            // (Measured this frame only if it is the node the barrel would have been measured from before: the one kept from the last
            // time, or none if the body was off; compared only.)
            g_muzzle_new = muzzle != g_body_muzzle.load(std::memory_order_relaxed);
            g_body_muzzle.store(muzzle, std::memory_order_relaxed);
            g_body_muzzle_frame.store(vr->m_engine_frame_count.load(), std::memory_order_release); // (after the node: FreshMuzzle)
        }

        // Where the body's neck is put behind your eyes (flat, metres; seated and standing alike).
        // - kNeckBehindEyes: along where your head looks (since fix159). It is also the radius your eyes turn about the neck by when
        //   only your head turns, taken off your lean in PrepareFrame.
        // - (fix195) kBodyBackLevel, kBodyBackLookDown: the whole body moved further back along its own forward, 0.05 m, and 0.03 m more
        //   times how far you look down (0 looking level or up, 1 straight down). Looking level along the body the neck is then 0.17 m
        //   behind the eyes, looking straight down 0.20 m. The third-person model's neck sat too far forward: looking down, the chest and
        //   legs were under or ahead of the eyes. The values follow Homard's Starfield VR body (the neck 0.09 m behind the eyes plus its
        //   body slider's 0.075 m back, and 0.03 m more times how far the player looks down: 0.165 m looking level) and FRIK in Fallout 4 VR (the
        //   body 3 game units back, seated and standing, and moved back further as you look down).
        //   The extra is along the body's forward, not your look (as both of those do), so turning only your head does not swing the
        //   body round: along your look, 0.17 m would put your eyes over a shoulder with the head turned to the side (fix159's cloak in
        //   front of the eye) and a head turn alone would bend the spine sideways by several centimetres.
        constexpr float kNeckBehindEyes   = 0.12f;
        constexpr float kBodyBackLevel    = 0.05f;
        constexpr float kBodyBackLookDown = 0.03f;

        // Before the engine turns the pose locals into worlds: place the body under the head and pose it.
        // Returns true when the shifted root should be used. Game memory is read here, so it runs under
        // PrepareFrameGuarded.
        bool PrepareFrame(std::uint8_t* model, const RE::NiTransform* root_local, Pose& pose, RE::NiTransform& shifted_root)
        {
            bool       use_shifted = false;
            // (fix189: the model compared with the one read as the root last updated; the root itself not read: it may be gone.)
            const bool ours        = g_state.active && g_calibration.status != Calibration::Status::Failed && g_state.root && root_local &&
                              model == g_state.model.load();
            if (ours && !RagdollActive(model)) {
                pose = ReadPose(model, root_local);
                if (pose.local) {
                    const auto storage = *reinterpret_cast<void**>(model + 0x10);
                    if (g_bones.storage != storage) {
                        // (fix142) What the body wrote last frame put back first (each bone only where it still holds what was written:
                        // safe whatever moved); before, it was dropped, last frame's IK staying under this frame's added steps.
                        RestoreAnimated(pose);
                        if (!KeepBones(pose, storage)) {
                            FindBones(pose, storage);
                        }
                        g_written.clear();
                    }
                    RestoreAnimated(pose);
                    ChooseWeapon();
                    RefreshMuzzle(); // (fix189)
                    // Both transforms are final from the previous frame here.
                    MeasureBarrel();

                    // Body placed so the neck is under and behind the eyes (horizontal only; kNeckBehindEyes and, fix195,
                    // kBodyBackLevel/kBodyBackLookDown above). Your own lean is taken by the spine (up to kLeanReach); the rest moves the body.
                    constexpr float kLeanReach      = 0.20f;
                    shifted_root                    = *root_local;
                    g_lean                          = glm::vec3{ 0.0f };
                    g_root_unshifted                = ToVec(root_local->translate);
                    {
                        static auto    vr    = VR::get();
                        const uint32_t frame = vr->m_engine_frame_count.load();
                        ++g_steady.calls;
                        if (frame != g_steady.last_frame) {
                            g_steady.last_frame = frame;
                            ++g_steady.frames;
                        }
                    }
                    auto world_camera               = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    SnapshotTracking();
                    if (world_camera && world_camera->parent) {
                        UpdateCrouch(pose, root_local->translate.z, tracking::CameraRootPlace(world_camera->parent).z); // (fix147: this frame's)
                    }
                    g_spine_twist = 0.0f; // (fix157: none unless the facing below sets it this frame; the review)
                    if (world_camera && g_bones.neck >= 0) {
                        const auto eye = HeadPosition(world_camera);
                        // (fix157) The game's heading turned at once (a snap turn, 45 degrees) (with or without the body facing your hands; the review): the planted feet and the steps turned with
                        // you about your place, so a foot is not left behind on the old line until its next step.
                        {
                            auto heading = FromNi(shifted_root).r[1];
                            heading.z    = 0.0f;
                            static float heading_was = 0.0f;
                            static bool  have_was    = false;
                            g_heading_turn = 0.0f;
                            if (glm::length(heading) > 1e-3f) {
                                const float now_yaw = std::atan2(heading.y, heading.x);
                                float       delta   = now_yaw - heading_was;
                                delta               = std::remainder(delta, glm::two_pi<float>());
                                // (fix158) The room's own turn this frame (smooth or snap turning: the hands turn with it), so only the head's own turn
                                // is held off the body's facing; the review.)
                                static float room_was  = 0.0f;
                                static bool  have_room = false;
                                float        room_turn = 0.0f;
                                if (const auto room = tracking::RoomRotation(); room) {
                                    const glm::vec3 ahead    = *room * glm::vec3{ 0.0f, 1.0f, 0.0f };
                                    const float     room_yaw = std::atan2(ahead.y, ahead.x);
                                    if (have_room && std::isfinite(room_yaw)) {
                                        room_turn = std::remainder(room_yaw - room_was, glm::two_pi<float>());
                                    }
                                    room_was  = room_yaw;
                                    have_room = std::isfinite(room_yaw);
                                }
                                if (have_was && std::abs(delta) > glm::radians(14.0f) && std::isfinite(delta)) {
                                    TurnWalk(delta, g_root_unshifted);
                                } else if (have_was && std::isfinite(delta)) {
                                    const float head = std::remainder(delta - room_turn, glm::two_pi<float>());
                                    g_heading_turn   = std::isfinite(head) ? head : 0.0f; // (fix158: the head's own turn: the body's facing holds its place)
                                }
                                heading_was = now_yaw;
                                have_was    = true;
                            }
                        }
                        // Torso partly toward the hands (FRIK: 0.7 of the hands' yaw, within 50 degrees).
                        if (GameFlow::gStore.internalSettings.bodyFacing) {
                            // (fix143: the hands' turn worked out only without tracked hips; before, value_or worked it out every time, and
                            // both moved the body's turn.)
                            const auto tracked = TrackedFacingYaw(pose);
                            float      yaw     = tracked ? *tracked : FacingYaw(pose, eye);
                            // (fix157) While moving, the hips and legs face the way you go and the hands' turn is the spine's (the chest and
                            // arms still toward your hands): the steps plant each foot on the line you travel, and hips turned up to 35
                            // degrees toward your hands swept the planted feet across under them: the legs crossed (walking looked odd
                            // when turning too much, as if tripping over your own feet; a helper's research: 0.7 m a step at a run).
                            // Standing still, the whole body as before (the feet then the animation's own); tracked hips as they are.
                            g_spine_twist = 0.0f;
                            if (!tracked) {
                                const float moving = std::clamp((g_body_speed - 0.3f) / 0.7f, 0.0f, 1.0f);
                                g_spine_twist      = yaw * moving;
                                yaw -= g_spine_twist;
                            }
                            Xf         root = FromNi(shifted_root);
                            const auto turn = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3{ 0.0f, 0.0f, 1.0f }));
                            const auto pivot = glm::vec3{ eye.x, eye.y, root.t.z };
                            root.r          = turn * root.r;
                            root.t          = pivot + turn * (root.t - pivot);
                            ToNi(root, shifted_root);
                            pose.root = root;
                        } else {
                            g_facing_yaw = 0.0f;
                            g_spine_twist = 0.0f;
                        }
                        auto forward = pose.root.r[1];
                        forward.z    = 0.0f;
                        if (glm::length(forward) > 1e-3f) {
                            forward            = glm::normalize(forward);
                            // (fix159) Behind the eyes along where the head looks (flat), as a neck is: along the body's forward, with the head
                            // turned far from the body the eyes came out by a shoulder (a screenshot: the cloak in front of the eye).
                            // Looking steeply down or up (the look nearly vertical): eased toward the body's forward (no jump; the review).
                            glm::vec3 behind = forward;
                            glm::vec3 room_ahead{ 0.0f };
                            if (const auto room = tracking::RoomRotation(); room) {
                                glm::vec3 look = *room * tracking::ToHavokVector(-glm::vec3{ tracking::HeadPose()[2] });
                                look.z         = 0.0f;
                                const float flat = glm::length(look);
                                if (flat > 0.05f && std::isfinite(flat)) {
                                    const float w = std::clamp((flat - 0.2f) / 0.3f, 0.0f, 1.0f);
                                    const glm::vec3 mixed = forward * (1.0f - w) + (look / flat) * w;
                                    if (glm::length(mixed) > 1e-3f) {
                                        behind = glm::normalize(mixed);
                                    }
                                }
                                room_ahead   = *room * glm::vec3{ 0.0f, 1.0f, 0.0f };
                                room_ahead.z = 0.0f;
                                room_ahead   = glm::length(room_ahead) > 1e-3f ? glm::normalize(room_ahead) : glm::vec3{ 0.0f };
                            }
                            // (fix195) How far you look down: the downward share of where the headset looks, against the room's own up
                            // (stage space: the game's view tilt does not count), 0 looking level or up, 1 straight down. It is 0 on
                            // both sides of level, so the body does not jump as your head's pitch crosses it. The body goes back
                            // kBodyBackLevel, and kBodyBackLookDown more times this, along its own forward.
                            float look_down = 0.0f;
                            {
                                const glm::vec3 ahead = -glm::vec3{ tracking::HeadPose()[2] };
                                const float     n     = glm::length(ahead);
                                if (n > 1e-3f && std::isfinite(n)) {
                                    look_down = std::clamp(-ahead.y / n, 0.0f, 1.0f);
                                }
                            }
                            const float body_back = kBodyBackLevel + kBodyBackLookDown * look_down;
                            const auto neck    = pose.GameWorld(g_bones.neck).t;
                            auto       delta   = eye - behind * kNeckBehindEyes - forward * body_back - neck;
                            delta.z            = 0.0f;
                            const float length = glm::length(delta);
                            // (fix143) Your head's own lean: where it is against where you recentred, flat, in the game's world (as the hands
                            // are placed: the room's turn, the game's axes, the tracking scale and the camera's scale). Only that bends the
                            // spine; the rest of the gap (the run animation's own neck leaning ahead, the camera a frame apart from the body)
                            // moves the body. Before, any gap up to 0.20 m bent the spine: running, it sat at 0.20 m all the time, the torso
                            // arched and dragged along (fix142's test).
                            glm::vec3 real_lean{ 0.0f };
                            if (const auto room = tracking::RoomRotation(); room && world_camera->parent) {
                                const glm::vec3 head_stage{ tracking::HeadPose()[3] };
                                real_lean = *room * tracking::ToHavokVector((head_stage - tracking::StandingOriginPosition()) * tracking::TrackingScale()) *
                                            world_camera->parent->world.scale;
                                real_lean.z = 0.0f;
                                // (fix159) At the neck, not the eyes: a head turned alone moves the eyes round the neck (9 cm at 60 deg), which is no
                                // lean (with the neck now behind the eyes along the look, that arc slid the hips; the review). Recentred, you look
                                // along the room's forward. (fix195: only the part along your look turns with your head; the body's extra
                                // back, along its own forward, is no lean and stays out of this: it moves the whole body, not the spine.)
                                if (glm::length(room_ahead) > 0.5f) {
                                    real_lean -= (behind - room_ahead) * kNeckBehindEyes;
                                    real_lean.z = 0.0f;
                                }
                                if (!(glm::length(real_lean) < 2.0f)) {
                                    real_lean = glm::vec3{ 0.0f }; // (not a real number, or far off: none)
                                }
                            }
                            g_steady.real_lean     = glm::length(real_lean);
                            const glm::vec3 left_of{ -forward.y, forward.x, 0.0f };
                            g_steady.delta_forward = glm::dot(delta, forward);
                            g_steady.delta_side    = glm::dot(delta, left_of);
                            g_steady.delta_max     = std::max(g_steady.delta_max, length);
                            if (length < 1.0f) {
                                auto body = delta;
                                if (GameFlow::gStore.internalSettings.bodyLean) {
                                    const float lean_length = glm::length(real_lean);
                                    g_lean = lean_length > kLeanReach ? real_lean * (kLeanReach / lean_length) : real_lean;
                                    body   = delta - g_lean;
                                }
                                shifted_root.translate.x += body.x;
                                shifted_root.translate.y += body.y;
                            }
                        }
                    }
                    if (world_camera && world_camera->parent) {
                        tracking::UpdateTrackingScale(tracking::CameraRootPlace(world_camera->parent).z - root_local->translate.z); // (fix147: this frame's)
                    }
                    pose.root   = FromNi(shifted_root);
                    use_shifted = true;

                    // Raw arm chain as read before any write, every 10 seconds (fix189: for the first 60, then one a minute).
                    static TimedLog wrist_log;
                    if (const auto now = std::chrono::steady_clock::now(); g_bones.r_wrist >= 0 && wrist_log.Due(now, std::chrono::seconds(10))) {
                        std::string chain;
                        int         i = g_bones.r_wrist;
                        for (int guard = 0; guard < 16 && i >= 0 && i < pose.count; ++guard) {
                            const auto& l = pose.local[i];
                            const auto& w = pose.world[i];
                            chain += std::format(" [{} p{} lt({:.3f},{:.3f},{:.3f}) ls{:.3f} lr0({:.2f},{:.2f},{:.2f}) wt({:.3f},{:.3f},{:.3f})]", i,
                                i < pose.top ? -1 : pose.parent[i], l.translate.x, l.translate.y, l.translate.z, l.scale, l.rotate.entry[0].pt[0],
                                l.rotate.entry[0].pt[1], l.rotate.entry[0].pt[2], w.translate.x, w.translate.y, w.translate.z);
                            if (i < pose.top) {
                                break;
                            }
                            i = pose.parent[i];
                        }
                        spdlog::info("[BodyIK] wrist chain:{}", chain);
                    }
                    if (g_calibration.status == Calibration::Status::Ready) {
                        // A held object goes where the hand is drawn: the play area as it is now, for the hands placed next.
                        HandGrab::OnHandsPlaced();
                        ApplyIK(pose);
                    }
                }
            }
            return use_shifted;
        }

        // Diagnostics and calibration from the engine's result (after the original update).
        void AfterFrame(const Pose& pose)
        {
            if (pose.local && g_bones.r_wrist >= 0 && g_bones.r_wrist_node) {
                // Exact pose -> world transform from this update: synced node world vs engine pose world.
                const auto engine_wrist = FromNi(pose.world[g_bones.r_wrist]);
                const auto node_wrist   = FromNi(g_bones.r_wrist_node->world);
                const auto measured     = Compose(node_wrist, Inverse(engine_wrist));
                // A real bone transform has a proper rotation, unit-ish scale and the wrist within arm's reach of the root.
                const float det         = glm::determinant(engine_wrist.r);
                const bool  plausible   = std::abs(det - 1.0f) < 0.05f && engine_wrist.s > 0.5f && engine_wrist.s < 2.0f &&
                                       glm::length(engine_wrist.t - pose.root.t) > 0.2f && glm::length(engine_wrist.t - pose.root.t) < 3.0f &&
                                       std::isfinite(measured.t.x) &&
                                       std::isfinite(measured.t.y) && std::isfinite(measured.t.z);

                if (g_calibration.status == Calibration::Status::Measuring && !plausible) {
                    ++g_calibration.frames;
                    if (g_calibration.frames % 30 == 1) {
                        spdlog::info("[BodyIK] calibration frame {}: wrist pose data implausible (det {:.3f} scale {:.3f} t ({:.3f},{:.3f},{:.3f}))",
                            g_calibration.frames, det, engine_wrist.s, engine_wrist.t.x, engine_wrist.t.y, engine_wrist.t.z);
                    }
                    if (g_calibration.frames > 600) {
                        g_calibration.status = Calibration::Status::Failed;
                        g_ik_failed = true;
                        spdlog::error("[BodyIK] calibration failed; the body is turned off");
                    }
                } else if (g_calibration.status == Calibration::Status::Measuring) {
                    // Nothing was written this frame, so every variant is scored against the engine's own result.
                    float error[kWorldVariants]{};
                    for (int variant = 0; variant < kWorldVariants; ++variant) {
                        error[variant] = variant == 4 && !g_calibration.has_measured
                                             ? 1e9f
                                             : glm::length(Compose(ToWorldVariant(variant), engine_wrist).t - node_wrist.t);
                    }
                    // Prefer an exact rule; the previous-frame measurement only when no rule fits.
                    int best = 0;
                    for (int variant = 1; variant < 4; ++variant) {
                        if (error[variant] < error[best]) {
                            best = variant;
                        }
                    }
                    if (error[best] > 0.005f && error[4] < 0.05f) {
                        best = 4;
                    }
                    const bool good = error[best] < (best == 4 ? 0.05f : 0.005f);
                    g_calibration.good_frames   = good && best == g_calibration.world_variant ? g_calibration.good_frames + 1 : (good ? 1 : 0);
                    g_calibration.world_variant = best;
                    ++g_calibration.frames;
                    if (g_calibration.frames % 30 == 1 || g_calibration.good_frames >= 10) {
                        const auto root = ToWorldVariant(0);
                        spdlog::info("[BodyIK] calibration frame {}: errors {:.4f}/{:.4f}/{:.4f}/{:.4f}/{:.4f} -> {} | measured vs root: "
                                     "dt ({:.3f},{:.3f},{:.3f}) angle {:.1f} (transposed {:.1f}, identity {:.1f}) scale {:.3f}/{:.3f}",
                            g_calibration.frames, error[0], error[1], error[2], error[3], error[4], kWorldVariantNames[best], measured.t.x - root.t.x,
                            measured.t.y - root.t.y, measured.t.z - root.t.z, AngleBetween(measured.r, root.r),
                            AngleBetween(measured.r, glm::transpose(root.r)), AngleBetween(measured.r, glm::mat3{ 1.0f }), measured.s, root.s);
                    }
                    if (g_calibration.good_frames >= 10) {
                        g_calibration.status = Calibration::Status::Ready;
                        spdlog::info("[BodyIK] calibrated ({}); IK enabled", kWorldVariantNames[best]);
                    } else if (g_calibration.frames > 600) {
                        g_calibration.status = Calibration::Status::Failed;
                        g_ik_failed = true;
                        spdlog::error("[BodyIK] calibration failed; the body is turned off");
                    }
                }
                if (plausible) {
                    g_calibration.measured     = measured;
                    g_calibration.has_measured = true;
                }
            }

            if (pose.local && g_calibration.status == Calibration::Status::Ready && g_bones.r_wrist >= 0) {
                const auto mine   = pose.World(g_bones.r_wrist).t;
                const auto engine = FromNi(pose.world[g_bones.r_wrist]).t;
                const float error = glm::length(mine - engine);


                static std::chrono::steady_clock::time_point last_log{};
                static TimedLog                              lines_log; // (fix189: the five lines for the first 60 times, then one a minute)
                const auto                                   now   = std::chrono::steady_clock::now();
                const bool                                   cycle = now - last_log > std::chrono::seconds(5);
                if (cycle && !lines_log.Due(now, std::chrono::seconds(5))) {
                    // (fix189) Not logged this time: the steadiness still counted over the last 5 s only, as its line says.
                    last_log = now;
                    g_steady = Steadiness{};
                } else if (cycle) {
                    last_log          = now;
                    auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    const auto eye    = world_camera ? HeadPosition(world_camera) : glm::vec3{ 0.0f };
                    const auto r_sh   = g_bones.r_biceps >= 0 ? pose.GameWorld(g_bones.r_biceps).t : glm::vec3{ 0.0f };
                    spdlog::info("[BodyIK] layout error {:.4f} | reach gap R {:.3f} L {:.3f} | weapon aligned {} barrel error {:.2f} deg placement error {:.3f} m | shoulder-eye ({:.2f},{:.2f},{:.2f}) | thread {}",
                        error, g_diag.right_gap, g_diag.left_gap, g_diag.weapon_aligned, g_barrel.error_deg, g_barrel.placement_error, r_sh.x - eye.x, r_sh.y - eye.y, r_sh.z - eye.z,
                        GetCurrentThreadId());
                    spdlog::info("[BodyIK] walk state {} speed {:.2f} m/s | lean {:.3f} m | facing {:.1f} deg | support {} ({:.2f} m) | fingers R trig {:.2f} grip {:.2f} thumb {} copied {} | L trig {:.2f} grip {:.2f} thumb {} copied {}",
                        g_walk.state, g_walk.speed, g_lean_applied, glm::degrees(g_facing_yaw), g_diag.support_held, g_diag.support_distance, g_finger_diag[1].trigger,
                        g_finger_diag[1].grip, g_finger_diag[1].thumb, g_finger_diag[1].copied, g_finger_diag[0].trigger, g_finger_diag[0].grip,
                        g_finger_diag[0].thumb, g_finger_diag[0].copied);
                    spdlog::info("[BodyIK] tracking: body supported {} active {} valid {} | torso {} elbows L {} R {} legs {} | fingers L {} (src {}) R {} (src {})",
                        g_track_diag.supported, g_track_diag.active, g_track_diag.valid, g_track_diag.torso, g_track_diag.elbow[0], g_track_diag.elbow[1],
                        g_track_diag.legs, g_track_diag.fingers[0], g_track_diag.hand_source[0], g_track_diag.fingers[1], g_track_diag.hand_source[1]);
                    spdlog::info("[BodyIK] crouch: game camera {:.2f} m below standing, body animation head {:.2f} m below | body lowered {:.2f} m | view lowered {:.2f} m | real crouch {}",
                        -g_crouch.game_drop, -g_crouch.body_drop, -g_crouch.pelvis_drop, -g_view_offset.load(),
                        GameFlow::gStore.internalSettings.realCrouch ? "on" : "off");
                    // (fix143) How steady the body was since the last line (the jitter, fix142's test).
                    const auto& s = g_steady;
                    spdlog::info("[BodyIK] steadiness (5 s): placed {} times in {} frames | facing wanted {:.1f} deg (hands spread {:.2f} m), its largest "
                                 "step {:.1f} deg, at the limit {} times | elbow side shortest L {:.2f} R {:.2f}, largest turn L {:.1f} R {:.1f} deg | "
                                 "steps re-aimed {} times, raw speed {:.2f}-{:.2f} m/s | neck to under the eyes: {:.2f} ahead, {:.2f} to the left "
                                 "(largest {:.2f}) m; your own lean {:.2f} m",
                        s.calls, s.frames, s.facing_raw, s.facing_spread, s.facing_step, s.facing_clamped, s.side_min[0] < 1e8f ? s.side_min[0] : 0.0f,
                        s.side_min[1] < 1e8f ? s.side_min[1] : 0.0f, s.elbow_step[0], s.elbow_step[1], s.retargets,
                        s.speed_min < 1e8f ? s.speed_min : 0.0f, s.speed_max, s.delta_forward, s.delta_side, s.delta_max, s.real_lean);
                    g_steady = Steadiness{};
                }
            }
        }

        // Access violations from a different game layout turn the body off instead of crashing.
        bool PrepareFrameGuarded(std::uint8_t* model, const RE::NiTransform* root_local, Pose* pose, RE::NiTransform* shifted_root, bool* use_shifted)
        {
            __try {
                *use_shifted = PrepareFrame(model, root_local, *pose, *shifted_root);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *use_shifted = false;
                return false;
            }
        }

        bool AfterFrameGuarded(const Pose* pose)
        {
            __try {
                AfterFrame(*pose);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix148) The head bone for the view on a ladder: its node in the model's bone list (as FindBones reads it: the list's entries, a
        // pose index and the bone's node; fix145 searched the scene under the body's root and never found it), its world place as the
        // model's update has synced it. Found again when the model or its pose storage changes.
        struct LadderHead {
            std::mutex             mtx;
            glm::vec3              at{ 0.0f };
            std::chrono::steady_clock::time_point when{};
            // (The model's thread.)
            const std::uint8_t*    model{ nullptr };
            const void*            storage{ nullptr };
            RE::NiAVObject*        node{ nullptr };
            int                    index{ -1 }; // (fix151) its place in the pose (scaled to nothing while on a ladder)
            bool                   searched{ false };
            int                    logs{ 0 };
            std::chrono::steady_clock::time_point retry_at{}; // (after a fault: searched again a second on)
        };
        LadderHead g_ladder_head;

        void NoteLadderHead(std::uint8_t* model)
        {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            auto root   = player ? ThirdPersonRoot(player) : nullptr;
            if (root == nullptr || model != *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(root) + kModelNodeOffset)) {
                return;
            }
            auto&       h       = g_ladder_head;
            const void* storage = *reinterpret_cast<void**>(model + 0x10);
            if (h.model != model || h.storage != storage || (!h.searched && h.node == nullptr && std::chrono::steady_clock::now() < h.retry_at)) {
                if (h.model == model && h.storage == storage) {
                    return; // (a fault a moment ago: not again until retry_at)
                }
                h.model    = model;
                h.storage  = storage;
                h.node     = nullptr;
                h.index    = -1;
                h.searched = false;
            }
            if (!h.searched) {
                h.searched         = true;
                const auto entries = *reinterpret_cast<std::uint8_t**>(model + 0x20);
                const auto count   = *reinterpret_cast<std::uint32_t*>(model + 0x18);
                for (std::uint32_t e = 0; entries && e < count && e < 4096; ++e) {
                    const auto node = *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8);
                    if (node != nullptr && std::string_view{ node->name.c_str() } == "C_Head") {
                        h.node  = node;
                        h.index = *reinterpret_cast<const std::uint16_t*>(entries + e * 16);
                        break;
                    }
                }
                if (h.logs < 4) {
                    ++h.logs;
                    spdlog::info("[BodyIK] (fix148) the body's head bone for the ladder's view: {} (of {} bones)", h.node != nullptr ? "found" : "not found",
                        count);
                }
            }
            if (h.node == nullptr) {
                return;
            }
            const glm::vec3 at{ h.node->world.translate.x, h.node->world.translate.y, h.node->world.translate.z };
            if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
                return;
            }
            std::scoped_lock _{ h.mtx };
            h.at   = at;
            h.when = std::chrono::steady_clock::now();
        }

        // (fix151) On a ladder the head's whole look hidden as first person hides it (ApplyIK: the head bone scaled to nothing, so its face,
        // hair and what is worn on it, a hood or a helmet, shrink into it): after fix150 the hood was clipping through the view (only
        // the face's own mesh was hidden there). The head bone's scale in the player's model's pose, for the update; null without it.
        float* LadderHeadScale(std::uint8_t* model)
        {
            const auto& h = g_ladder_head;
            if (h.index < 0 || h.model != model) {
                return nullptr;
            }
            __try {
                const auto storage = *reinterpret_cast<std::uint8_t**>(model + 0x10);
                if (storage == nullptr || storage != h.storage) {
                    return nullptr;
                }
                const auto locals = *reinterpret_cast<std::uint8_t**>(storage + 0x08);
                const auto count  = *reinterpret_cast<std::uint16_t*>(model + 0x78);
                if (locals == nullptr || h.index >= count) {
                    return nullptr;
                }
                auto* local = *reinterpret_cast<RE::NiTransform**>(locals + 0x10);
                if (local == nullptr) {
                    return nullptr;
                }
                float* scale = &local[h.index].scale;
                return std::isfinite(*scale) ? scale : nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        void NoteLadderHeadGuarded(std::uint8_t* model)
        {
            __try {
                NoteLadderHead(model);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // (Searched again a second on, not left off for good; the review.)
                g_ladder_head.node     = nullptr;
                g_ladder_head.searched = false;
                g_ladder_head.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }

        // ---- (fix173) A held person's arm (or chest, neck, head) bent toward your hand (Physical's hold) ----
        // After fix172: gripping a standing person should hold them, not knock them down. The game's grab makes no
        // spring on a living person's bones (keyframed: the spring maker refuses them, 0x141A52423), and PLANCK's own hold of the living
        // is cosmetic too (a force-capped pull on the animated ragdoll; "the AI is totally unaware"): here the held part is posed in that
        // person's own model update, where the body's own arms are posed: the pose locals before the game makes them worlds, the bones
        // written put back first on each update (the animation does not rewrite every local each frame). Pose worlds are game world
        // (the calibration's "identity", the same for every model).
        struct NpcBendState
        {
            std::mutex     mutex;
            HeldPersonBend want{};
            bool           active{ false };
            uintptr_t      put_back{ 0 }; // a model whose bones are put back on its next update (the hold ended)
            // (The hook's own, under the mutex:)
            uintptr_t      written_model{ 0 };
            const void*    storage{ nullptr };
            std::string    bone;          // the held bone's name the indices below were found for
            int            held{ -1 }, biceps{ -1 }, forearm{ -1 }, wrist{ -1 }, pivot{ -1 };
            int            kind{ 0 };     // 0 none, 1 the upper arm, 2 the forearm or hand, 3 a lean (chest, neck, head)
            std::unordered_map<int, WrittenBone> written;
            int            seen{ 0 }, bent{ 0 };
            float          asked{ 0.0f }, given{ 0.0f };
            // (fix182) The gentle bend's own: the move given, eased from frame to frame, and when it was last given; the elbow's hinge as
            // last seen bent (kept for a nearly straight arm); the elbow's angle as animated and as bent, summed for the log.
            glm::vec3      smooth{ 0.0f };
            std::chrono::steady_clock::time_point smooth_at{};
            glm::vec3      hinge{ 0.0f };
            double         elbow_anim_sum{ 0.0 }, elbow_bent_sum{ 0.0 };
            int            elbow_n{ 0 };
        };
        NpcBendState           g_npc;
        std::atomic<uintptr_t> g_npc_model{ 0 };    // (the hook's quick test: the held model...)
        std::atomic<uintptr_t> g_npc_put_back{ 0 }; // (...or the one to put back)
        std::atomic<bool>      g_npc_failed{ false };
        // (fix182) After fix181 a person's arm bent oddly when grabbed by the hand (the log: a forearm held 0.15-0.41 m from your hand,
        // the bend at its 0.30 m cap nearly always; the wrist moved by up to 4 times the held point's move for a point near the elbow, by a
        // two-bone IK with no joint limits, your hand locked on it swung with it). Chosen: "Gentle, natural bend": the held point gives at most
        // 12 cm toward your pull, eased from frame to frame; the upper arm swings at most 20 degrees, the elbow bends only as an elbow does
        // (about its own hinge, in the animated arm's plane, 5 to 150 degrees, at most 25 degrees from the animation), the body leans at most
        // 10 degrees; the drag brings the whole person to your hand instead (Physical's catch-up, faster).
        constexpr float        kNpcArmReach  = 0.12f; // m the held point is moved at most (fix182: was 0.30)
        constexpr float        kNpcLeanReach = 0.12f; // (fix182: was 0.25)
        constexpr float        kNpcUpperTurn = glm::radians(20.0f); // (fix182: was 45)
        constexpr float        kNpcLeanTurn  = glm::radians(10.0f); // (fix182: was 20)
        constexpr float        kNpcElbowTurn = glm::radians(25.0f); // (fix182) the elbow bent or opened at most this much from the animation
        constexpr float        kNpcElbowMin  = glm::radians(5.0f);  // ...never straighter than this (no bending backwards)...
        constexpr float        kNpcElbowMax  = glm::radians(150.0f); // ...nor more closed than this
        constexpr float        kNpcEaseS     = 0.10f; // (fix182) the move eased in with this time constant (s)

        // The turn taking `from` toward `to`, at most `max` radians.
        glm::mat3 LimitedTurn(const glm::vec3& from, const glm::vec3& to, float max)
        {
            if (glm::length(from) < 1e-5f || glm::length(to) < 1e-5f) {
                return glm::mat3{ 1.0f };
            }
            const glm::vec3 a     = glm::normalize(from);
            const glm::vec3 b     = glm::normalize(to);
            const float     angle = std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f));
            if (angle <= max) {
                return RotationBetween(a, b);
            }
            const glm::vec3 axis = glm::cross(a, b);
            if (glm::length(axis) < 1e-6f) {
                return glm::mat3{ 1.0f };
            }
            return glm::mat3_cast(glm::angleAxis(max, glm::normalize(axis)));
        }

        // Bone i set to the game-world transform `desired` (its descendants follow), remembered to be put back.
        void NpcSetWorld(const Pose& pose, int i, const Xf& desired)
        {
            const auto result = Compose(Inverse(pose.ParentWorld(i)), desired);
            bool       finite = std::isfinite(result.s) && result.s > 0.0f && result.s < 10.0f && glm::length(result.t) < 10.0f;
            for (int a = 0; a < 3 && finite; ++a) {
                finite = std::isfinite(result.t[a]) && std::isfinite(result.r[a][0]) && std::isfinite(result.r[a][1]) && std::isfinite(result.r[a][2]);
            }
            if (!finite) {
                return;
            }
            auto [entry, first] = g_npc.written.try_emplace(i);
            if (first) {
                entry->second.animated = pose.local[i];
            }
            ToNi(result, pose.local[i]);
            entry->second.written = pose.local[i];
        }

        void NpcFindBones(const Pose& pose)
        {
            g_npc.held = g_npc.biceps = g_npc.forearm = g_npc.wrist = g_npc.pivot = -1;
            g_npc.kind  = 0;
            g_npc.hinge = glm::vec3{ 0.0f }; // (fix182, the review: another bone or model: its own hinge)
            const std::string& bone = g_npc.bone;
            const bool         arm  = bone.size() > 2 && (bone[0] == 'R' || bone[0] == 'L') && bone[1] == '_';
            const std::string  side = arm ? bone.substr(0, 2) : std::string{};
            // (The head leans about the neck; the chest and the neck about the bone below them (the review: about itself the chest's lean
            // moved the held point only by a few cm).)
            const std::string  pivot_name = bone == "C_Head" ? "C_Neck" : "";
            const auto entries = *reinterpret_cast<std::uint8_t**>(pose.model + 0x20);
            const auto count   = *reinterpret_cast<std::uint32_t*>(pose.model + 0x18);
            for (std::uint32_t e = 0; entries && e < count && e < 4096; ++e) {
                const auto idx  = *reinterpret_cast<std::uint16_t*>(entries + e * 16);
                const auto node = *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8);
                if (!node || idx >= pose.count) {
                    continue;
                }
                const std::string_view name{ node->name.c_str() };
                if (name == bone) g_npc.held = idx;
                if (arm && name == side + "Biceps") g_npc.biceps = idx;
                if (arm && name == side + "Forearm") g_npc.forearm = idx;
                if (arm && name == side + "Wrist") g_npc.wrist = idx;
                if (!pivot_name.empty() && name == pivot_name) g_npc.pivot = idx;
            }
            if (g_npc.held < 0) {
                return;
            }
            if (!arm && (bone == "C_Chest" || bone == "C_Neck")) {
                g_npc.pivot = g_npc.held >= pose.top && pose.parent[g_npc.held] < pose.count ? pose.parent[g_npc.held] : g_npc.held;
            }
            if (arm && g_npc.biceps >= 0 && g_npc.forearm >= 0 && g_npc.wrist >= 0) {
                g_npc.kind = g_npc.held == g_npc.biceps ? 1 : (g_npc.held == g_npc.forearm || g_npc.held == g_npc.wrist ? 2 : 0);
            }
            // (fix174) Any other part (the spine's bones, a shoulder's clavicle): a lean about the bone it hangs from.
            if (g_npc.kind == 0 && g_npc.pivot < 0) {
                g_npc.pivot = g_npc.held >= pose.top && pose.parent[g_npc.held] < pose.count ? pose.parent[g_npc.held] : g_npc.held;
            }
            if (g_npc.kind == 0 && g_npc.pivot >= 0) {
                g_npc.kind = 3;
            }
        }

        // In the held model's own update (its thread), the mutex held.
        void NpcBendInner(std::uint8_t* model, const RE::NiTransform* root_local)
        {
            Pose pose = ReadPose(model, root_local, false);
            if (!pose.local) {
                return;
            }
            const auto  m         = reinterpret_cast<uintptr_t>(model);
            const void* storage   = *reinterpret_cast<void**>(model + 0x10);
            // What was written on its last update, put back where it still holds it.
            if (g_npc.written_model == m && g_npc.storage == storage) {
                for (const auto& [i, w] : g_npc.written) {
                    if (i < pose.count && std::memcmp(&pose.local[i], &w.written, sizeof(RE::NiTransform)) == 0) {
                        pose.local[i] = w.animated;
                    }
                }
            }
            g_npc.written.clear();
            g_npc.written_model = m;
            if (g_npc.put_back == m) {
                g_npc.put_back = 0;
                g_npc_put_back = 0;
            }
            if (!g_npc.active || g_npc.want.model != m) {
                g_npc.storage = storage;
                return;
            }
            ++g_npc.seen;
            if (g_npc.storage != storage || g_npc.bone != g_npc.want.bone) {
                g_npc.storage = storage;
                g_npc.bone    = g_npc.want.bone;
                NpcFindBones(pose);
            }
            // (fix181) The held bone as this update draws it, for your hand locked on it (HoldPersonShift).
            const auto note = [&](const Xf& x) {
                static auto    vr    = VR::get();
                const uint32_t frame = vr->m_engine_frame_count.load();
                if (!std::isfinite(x.t.x) || !std::isfinite(x.t.y) || !std::isfinite(x.t.z) || !(x.s > 0.01f)) {
                    return;
                }
                std::scoped_lock _{ g_npc_bone_mutex };
                // (The review: the bone too, not only the model: another part of the same person grabbed while the last eases out.)
                const bool same = g_npc_bone.ok && g_npc_bone.model == m && std::strncmp(g_npc_bone.bone, g_npc.bone.c_str(), sizeof(g_npc_bone.bone)) == 0;
                if (same && g_npc_bone.frame != frame) {
                    g_npc_bone.prev       = g_npc_bone.now;
                    g_npc_bone.prev_frame = g_npc_bone.frame;
                    g_npc_bone.has_prev   = true;
                } else if (!same) {
                    g_npc_bone.has_prev = false;
                }
                g_npc_bone.model = m;
                std::snprintf(g_npc_bone.bone, sizeof(g_npc_bone.bone), "%s", g_npc.bone.c_str());
                g_npc_bone.frame = frame;
                g_npc_bone.now   = x;
                g_npc_bone.ok    = true;
            };
            // (Noted as this update leaves it, bent or not. The review: called on each way out, not from an object's destructor: this runs
            // under NpcBendTry's guard, and a destructor run while a fault unwinds that read the pose again could end the game.)
            const auto note_held = [&] {
                if (g_npc.held >= 0 && g_npc.held < static_cast<int>(pose.count)) {
                    note(pose.World(g_npc.held));
                }
            };
            const float weight = std::clamp(g_npc.want.weight, 0.0f, 1.0f);
            if (g_npc.kind == 0 || weight <= 0.0f) {
                note_held();
                return;
            }
            // The bend itself (its own early ways out return from it, then the bone is noted).
            [&] {
            const glm::vec3 target{ g_npc.want.target[0], g_npc.want.target[1], g_npc.want.target[2] };
            const glm::vec3 held_l{ g_npc.want.held_local[0], g_npc.want.held_local[1], g_npc.want.held_local[2] };
            const auto      held  = pose.World(g_npc.held);
            const glm::vec3 point = held.t + held.r * (held_l * held.s);
            glm::vec3       delta = target - point;
            const float     asked = glm::length(delta);
            const float     cap   = g_npc.kind == 3 ? kNpcLeanReach : kNpcArmReach;
            if (!std::isfinite(asked)) {
                return;
            }
            if (asked > cap) {
                delta *= cap / asked;
            }
            g_npc.asked = asked;
            // (fix182) Eased from frame to frame (anew after a pause of 0.3 s). (The review) The move eased before the hold's own ease is
            // applied (eased after it, the let-go's 0.15 s ease outran it: the last bent frame held over half the move, then the bones were
            // put back at once); a pause (a menu, a hitch) eases on by one frame's share, not by a tenth of a second's.
            {
                const auto  now_s = std::chrono::steady_clock::now();
                const float gap   = g_npc.smooth_at == std::chrono::steady_clock::time_point{} ? 1.0f
                                                                                                : std::chrono::duration<float>(now_s - g_npc.smooth_at).count();
                if (!(gap < 0.3f) || !std::isfinite(g_npc.smooth.x) || !std::isfinite(g_npc.smooth.y) || !std::isfinite(g_npc.smooth.z)) {
                    g_npc.smooth = glm::vec3{ 0.0f };
                }
                const float step = gap < 0.05f ? std::max(gap, 0.0f) : 1.0f / 60.0f;
                const float k    = 1.0f - std::exp(-step / kNpcEaseS);
                g_npc.smooth    += (delta - g_npc.smooth) * k;
                g_npc.smooth_at  = now_s;
                delta            = g_npc.smooth * weight;
            }
            if (g_npc.kind == 1) {
                // The upper arm: swung about the shoulder.
                auto upper = pose.World(g_npc.biceps);
                upper.r    = LimitedTurn(point - upper.t, point + delta - upper.t, kNpcUpperTurn) * upper.r;
                NpcSetWorld(pose, g_npc.biceps, upper);
            } else if (g_npc.kind == 2) {
                // The forearm or hand. (fix182) No more the wrist moved by the held point's move divided by how far along the forearm it
                // is (up to 4 times) with a two-bone IK free of limits: first the upper arm swung about the shoulder, by the share of the
                // move the elbow would take (all of it for a point at the elbow, half for one at the wrist or the hand; at most
                // kNpcUpperTurn), then the rest by the elbow about its own hinge (the animated arm's plane: no twist), only within an
                // elbow's range and never past the animation's own angle out of it.
                const glm::vec3 s = pose.World(g_npc.biceps).t;
                const glm::vec3 e = pose.World(g_npc.forearm).t;
                const glm::vec3 w = pose.World(g_npc.wrist).t;
                const float     a = glm::length(e - s);
                const float     b = glm::length(w - e);
                if (a < 0.05f || b < 0.05f) {
                    return;
                }
                const glm::vec3 up_dir    = (e - s) / a;
                const glm::vec3 down_dir  = (w - e) / b;
                const float     elbow_was = std::acos(std::clamp(glm::dot(up_dir, down_dir), -1.0f, 1.0f));
                glm::vec3       hinge     = glm::cross(up_dir, down_dir);
                // (The review) The last hinge kept in the upper arm's own frame (it turns with them and their arm; reset with each hold
                // and each bone): taken back only when it still lies across the upper arm.
                const glm::mat3 upper_r = pose.World(g_npc.biceps).r;
                if (glm::length(hinge) > 0.05f) {
                    hinge       = glm::normalize(hinge);
                    g_npc.hinge = glm::transpose(upper_r) * hinge;
                } else if (glm::length(g_npc.hinge) > 0.5f) {
                    const glm::vec3 kept   = upper_r * g_npc.hinge;
                    const glm::vec3 across = kept - up_dir * glm::dot(kept, up_dir);
                    hinge                  = glm::length(across) > 0.3f ? glm::normalize(across) : glm::vec3{ 0.0f };
                } else {
                    hinge = glm::vec3{ 0.0f };
                }
                const float along = g_npc.held == g_npc.forearm ? std::clamp(glm::dot(point - e, w - e) / (b * b), 0.0f, 1.0f) : 1.0f;
                // The upper arm: the elbow toward its share of the move.
                auto upper = pose.World(g_npc.biceps);
                const glm::mat3 swing = LimitedTurn(e - s, e + delta * (1.0f - 0.5f * along) - s, kNpcUpperTurn);
                upper.r = swing * upper.r;
                NpcSetWorld(pose, g_npc.biceps, upper);
                // The elbow: the rest, about its hinge (turned with the upper arm), within its range.
                float elbow_now = elbow_was;
                if (glm::length(hinge) > 0.5f) {
                    const glm::vec3 axis    = glm::normalize(swing * hinge);
                    const auto      lower   = pose.World(g_npc.forearm);
                    const auto      held_w  = pose.World(g_npc.held);
                    const glm::vec3 point1  = held_w.t + held_w.r * (held_l * held_w.s);
                    const glm::vec3 wanted  = point + delta;
                    glm::vec3       from_v  = point1 - lower.t;
                    glm::vec3       to_v    = wanted - lower.t;
                    from_v -= axis * glm::dot(from_v, axis);
                    to_v -= axis * glm::dot(to_v, axis);
                    if (glm::length(from_v) > 0.02f && glm::length(to_v) > 0.02f) {
                        float turn = std::atan2(glm::dot(glm::cross(from_v, to_v), axis), glm::dot(from_v, to_v));
                        turn       = std::clamp(turn, -kNpcElbowTurn, kNpcElbowTurn);
                        // Turning about the hinge (upper arm x forearm) by +x bends the elbow x more (the angle between the two
                        // bones' ways grows: 0 a straight arm), -x straightens it.
                        // (The review) Never pushed past the animation's own angle out of the range: an arm animated straighter than
                        // 5 degrees, or more closed than 150 (a drink, a hand to the face), is not forced in without a pull.
                        turn       = std::clamp(turn, std::min(0.0f, kNpcElbowMin - elbow_was), std::max(0.0f, kNpcElbowMax - elbow_was));
                        if (std::isfinite(turn) && std::abs(turn) > 1e-4f) {
                            auto bent = lower;
                            bent.r    = glm::mat3_cast(glm::angleAxis(turn, axis)) * lower.r;
                            NpcSetWorld(pose, g_npc.forearm, bent);
                            elbow_now = elbow_was + turn;
                        }
                    }
                }
                g_npc.elbow_anim_sum += glm::degrees(elbow_was);
                g_npc.elbow_bent_sum += glm::degrees(elbow_now);
                ++g_npc.elbow_n;
            } else {
                // The chest, neck or head: a lean about the chest or the neck.
                auto pivot = pose.World(g_npc.pivot);
                pivot.r    = LimitedTurn(point - pivot.t, point + delta - pivot.t, kNpcLeanTurn) * pivot.r;
                NpcSetWorld(pose, g_npc.pivot, pivot);
            }
            const auto moved = pose.World(g_npc.held);
            g_npc.given      = glm::length(moved.t + moved.r * (held_l * moved.s) - point);
            ++g_npc.bent;
            }();
            note_held(); // (fix181)
        }

        bool NpcBendTry(std::uint8_t* model, const RE::NiTransform* root_local)
        {
            __try {
                NpcBendInner(model, root_local);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void NpcBend(std::uint8_t* model, const RE::NiTransform* root_local)
        {
            std::scoped_lock lock{ g_npc.mutex };
            if (!NpcBendTry(model, root_local)) {
                g_npc_failed   = true;
                g_npc_model    = 0;
                g_npc_put_back = 0;
                g_npc.active   = false;
                g_npc.written.clear();
                spdlog::error("[BodyIK] (fix173) a held person's pose could not be read; their bending is off for this session");
            }
        }

        // BSModelNode::UpdateTransforms: pose locals -> worlds -> synced to nodes, skin and geometry.
        void* ModelNodeUpdateTransforms(std::uint8_t* model, const RE::NiTransform* root_local, RE::NiUpdateData* data, void* out)
        {
            Pose            pose;
            RE::NiTransform shifted_root{};
            bool            use_shifted = false;
            if (g_state.active && !g_ik_failed.load() && g_state.root && root_local) {
                if (!PrepareFrameGuarded(model, root_local, &pose, &shifted_root, &use_shifted)) {
                    g_ik_failed = true;
                    use_shifted = false;
                    pose        = Pose{};
                    spdlog::error("[BodyIK] the body's pose data could not be read (different game version?); the body is turned off");
                }
            }
            // (fix173) A held person's model: their held part bent toward your hand (or put back after the hold).
            if (!pose.local && root_local != nullptr) {
                const auto m = reinterpret_cast<uintptr_t>(model);
                if (m != 0 && (m == g_npc_model.load(std::memory_order_relaxed) || m == g_npc_put_back.load(std::memory_order_relaxed))) {
                    NpcBend(model, root_local);
                }
            }
            // (fix151) On a ladder (the body's own work off): the head bone scaled to nothing for this update, as first person's IK does, and
            // put back after it (the update has made its worlds, skin and geometry by then; the animation's scale stays the animation's).
            // (fix199) Seated with the view at the seated body's eyes, the game's camera not under the head bone (SeatView): the same.
            const bool ladder     = !pose.local && (LadderClimb::HeadWanted() || SeatView::HeadScaleWanted());
            float*     head_scale = ladder ? LadderHeadScale(model) : nullptr;
            const float head_was  = head_scale != nullptr ? *head_scale : 1.0f;
            if (head_scale != nullptr) {
                *head_scale = 0.0f;
            }
            auto result = g_model_update_hook.call<void*>(model, use_shifted ? &shifted_root : root_local, data, out);
            // (Put back only into the same pose array: an update that made its storage anew (a change of what is worn) would leave the
            // old one freed; the review.)
            if (head_scale != nullptr && LadderHeadScale(model) == head_scale) {
                *head_scale = head_was;
            }

            // (fix148) On a ladder (the body's own work off: the game's third person), the head bone's place as this update put it.
            // (fix199) Seated with the game's own body shown, while SeatView measures its eyes: the same (its scale left alone then).
            if (ladder || (!pose.local && SeatView::HeadWanted())) {
                NoteLadderHeadGuarded(model);
            }
            if (pose.local && !g_ik_failed.load() && !AfterFrameGuarded(&pose)) {
                g_ik_failed = true;
                spdlog::error("[BodyIK] the body's pose data could not be read after the update; the body is turned off");
            }
            return result;
        }
    }

    void OnUpdateWorld(RE::NiAVObject* obj, int engine_frame)
    {
        if (engine_frame != g_state.frame || obj == g_state.root) {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            auto root   = player ? ThirdPersonRoot(player) : nullptr;
            // (fix189) The player's third-person root is not the one the body was put on any more (a load, the main menu): that one may be
            // freed, so it is forgotten without being read or touched (pointers compared only). Before, only its own update turned the
            // body off, which a freed root never has: the body stayed on and every model update read the old root, and a fault there
            // turned the body off until restart. With a new root, the body stays as it was until the new root's first update decides
            // (as before); with no root at all, the body is off now and that update owes the put-back the off switch would have done.
            if (g_state.root != nullptr && root != g_state.root) {
                g_state.root  = nullptr;
                g_state.model = nullptr;
                g_body_muzzle = nullptr; // (the old 3D's muzzle with it: it may be freed too; the review)
                static int told = 0;
                if (told < 20) {
                    ++told;
                    spdlog::info("[Body] (fix189) the player's third-person root changed ({}): the body's old one forgotten without reading it",
                        root != nullptr ? "a new one" : "none now");
                }
            }
            if (root == nullptr && g_state.active) {
                g_body_muzzle   = nullptr;
                g_view_offset   = 0.0f;
                g_put_back_owed = true;
                g_state.active  = false;
            }
            if (obj == root) {
                g_state.frame = engine_frame;
                UpdateVisibility(player, root);
            }
        }
    }

    void SetHeldPersonBend(const HeldPersonBend& bend)
    {
        if (g_npc_failed.load() || bend.model == 0) {
            return;
        }
        std::scoped_lock lock{ g_npc.mutex };
        if (!g_npc.active || g_npc.want.model != bend.model) {
            g_npc.seen  = 0;
            g_npc.bent  = 0;
            g_npc.asked = 0.0f;
            g_npc.given = 0.0f;
            // (fix182, the review) A new hold: nothing of the last one's ease, hinge or elbow numbers carried in.
            g_npc.smooth         = glm::vec3{ 0.0f };
            g_npc.smooth_at      = std::chrono::steady_clock::time_point{};
            g_npc.hinge          = glm::vec3{ 0.0f };
            g_npc.elbow_anim_sum = 0.0;
            g_npc.elbow_bent_sum = 0.0;
            g_npc.elbow_n        = 0;
            if (g_npc.active && g_npc.want.model != 0) {
                g_npc.put_back = g_npc.want.model; // (another model now: the first put back on its next update)
                g_npc_put_back = g_npc.put_back;
            }
        }
        g_npc.want                       = bend;
        g_npc.want.bone[sizeof(g_npc.want.bone) - 1] = 0;
        g_npc.active                     = true;
        g_npc_model                      = bend.model;
    }

    void EndHeldPersonBend()
    {
        std::scoped_lock lock{ g_npc.mutex };
        if (!g_npc.active) {
            return;
        }
        g_npc.active   = false;
        g_npc.put_back = g_npc.want.model;
        g_npc_put_back = g_npc.put_back;
        g_npc_model    = 0;
        g_npc.smooth    = glm::vec3{ 0.0f }; // (fix182, the review)
        g_npc.smooth_at = std::chrono::steady_clock::time_point{};
    }

    void HeldPersonBendCounts(int& seen, int& bent, float& asked, float& given)
    {
        std::scoped_lock lock{ g_npc.mutex };
        seen  = g_npc.seen;
        bent  = g_npc.bent;
        asked = g_npc.asked;
        given = g_npc.given;
    }

    bool HeldPersonElbow(float& animated_deg, float& bent_deg, int& updates)
    {
        std::scoped_lock lock{ g_npc.mutex };
        updates = g_npc.elbow_n;
        if (g_npc.elbow_n <= 0) {
            return false;
        }
        animated_deg         = static_cast<float>(g_npc.elbow_anim_sum / g_npc.elbow_n);
        bent_deg             = static_cast<float>(g_npc.elbow_bent_sum / g_npc.elbow_n);
        g_npc.elbow_anim_sum = 0.0;
        g_npc.elbow_bent_sum = 0.0;
        g_npc.elbow_n        = 0;
        return true;
    }

    void InstallModelNodeHook()
    {
        const auto address = ScanUnique("BSModelNode::UpdateTransforms", "48 8B C4 53 57 41 56 41 57 48 81 EC", 0x2be93f0);
        if (!address) {
            spdlog::error("[BodyIK] BSModelNode::UpdateTransforms not found; the body stays off");
            return;
        }
        g_face_node_fn      = reinterpret_cast<FaceNodeFn>(ScanUnique("face node lookup", "48 89 5C 24 18 57 48 83 EC 20 33 DB 48 89", 0x195d930));
        g_model_update_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ModelNodeUpdateTransforms));
        g_hook_ready        = static_cast<bool>(g_model_update_hook);
        spdlog::info("[BodyIK] model node hook: {}", g_hook_ready.load());
        // (fix195) The body's place behind your eyes, once a start.
        static bool placement_told = false;
        if (!placement_told) {
            placement_told = true;
            spdlog::info("[BodyIK] (fix195) the body's neck behind your eyes: {:.2f} m along your look, the body {:.2f} m further back along its "
                         "forward ({:.2f} m in all looking level along the body) and {:.2f} m more times how far you look down ({:.2f} m looking "
                         "straight down); seated and standing alike",
                kNeckBehindEyes, kBodyBackLevel, kNeckBehindEyes + kBodyBackLevel, kBodyBackLookDown,
                kNeckBehindEyes + kBodyBackLevel + kBodyBackLookDown);
        }
    }

    bool IsActive()
    {
        return g_state.active;
    }

    bool LadderHeadWorld(float head[3])
    {
        auto&            h = g_ladder_head;
        std::scoped_lock _{ h.mtx };
        if (h.when == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() - h.when > std::chrono::milliseconds(100)) {
            return false;
        }
        head[0] = h.at.x;
        head[1] = h.at.y;
        head[2] = h.at.z;
        return true;
    }

    bool SeatCameraUnderHead(bool& under)
    {
        // (fix199) The first-person camera's nodes (FirstPersonState +0xA0, the camera; +0xA8 and +0xB0, the two the game finds by name
        // beside it), seated pointing into the shown body: is any of them under its head bone? Then shrinking the head would move the view.
        RE::NiAVObject* head = g_ladder_head.node;
        RE::NiAVObject* nodes[3]{};
        if (auto pc = CreationEngineSingletonManager::GetPlayerCameraSingleton(); pc && pc->pFirstPersonModeState) {
            nodes[0] = pc->pFirstPersonModeState->cameraRoot;
            nodes[1] = pc->pFirstPersonModeState->pNiNode;
            nodes[2] = pc->pFirstPersonModeState->pNiNode2;
        }
        if (head == nullptr || nodes[0] == nullptr) {
            return false;
        }
        under = false;
        for (auto* node : nodes) {
            under = under || (node != nullptr && IsAncestorGuarded(node, head));
        }
        return true;
    }

    bool ReadRootWorld(float position[3], float forward[3])
    {
        // (fix146) For the view at the body's eyes on a ladder (LadderClimb): the third-person body's root (the character's place, at its
        // feet: always there, unlike a bone found by name; fix145's head bone was never found in its test) and the way it faces, as last
        // placed (read under a guard: the body may be loading).
        auto player = CreationEngineSingletonManager::GetPlayerRef();
        if (!player) {
            return false;
        }
        bool ok = false;
        __try {
            if (auto root = ThirdPersonRoot(player)) {
                const auto r = FromNi(root->world);
                position[0]  = r.t.x;
                position[1]  = r.t.y;
                position[2]  = r.t.z;
                forward[0]   = r.r[1].x;
                forward[1]   = r.r[1].y;
                forward[2]   = r.r[1].z;
                ok           = std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) && std::isfinite(forward[0]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    namespace
    {
        // (fix146) The game's switch of the player between first and third person (Starfield 1.16.236: SetFirstPerson's work, 0x141A41D50,
        // rcx the player, dl 1 for first person). It reads which of the two bodies is hidden (the first-person one, [player+0xDE8]+0x118
        // bit 0, at 0x141A41E28; the third-person one, its loaded 3D's +0x118, at 0x141A41E33): going to third person with the third-person
        // body already shown and the first-person one hidden, it takes that for "already in third person" and stops (0x141A41ED6 to its
        // end) without swapping the bodies or the animations to the third-person ones (0x1419FF6D0). The shown body makes exactly that
        // state in first person, so a ladder's climb (only third-person animations) was refused every time (fix145's test: the camera
        // in third person, no climb). So, just before it, the bodies as the game has them in first person (the shown body hidden, the
        // arms shown): it then does the whole switch itself. (The shown body is not put back meanwhile: it is only shown in first
        // person.)
        // (fix202) Every turn to third person, not only a ladder's. A new game stopped before character creation: the quest snaps the
        // player into the character-creation bench (a seat marked "third person only"), the game turns the player to third person for
        // its animations, the shown body made that turn do nothing, the seat was refused, and the scene waited for the player to sit
        // with the screen held black. Going to first person the game swaps the bodies itself whenever the first-person body is hidden
        // (refused only for some player states, leaving the mark set), or starts its own body-in-a-seat view in a seat, so only the
        // turn to third person needs it. Still only with the game's third-person mark clear (BodiesMixed): real third person
        // and the game's own seat view are left alone. Each call logged (capped) with its caller.
        constexpr uintptr_t kSwitchRva    = 0x1A41D50;
        constexpr uint8_t   kSwitchBytes[] = { 0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57 };
        safetyhook::MidHook g_switch_hook{};
        std::atomic<int>    g_switch_fixes{ 0 };

        // (The two bodies read under a guard: the first-person one hidden, the third-person one shown, and the game not in third person
        // ([player+0x112A] bit 8 clear: not real third person, nor the game's own body-in-a-seat view, which sets it too: the review).)
        bool BodiesMixed(RE::PlayerCharacter* player, RE::NiAVObject*& first, RE::NiAVObject*& third)
        {
            __try {
                if ((*(reinterpret_cast<const uint8_t*>(player) + 0x112A) & 8) != 0) {
                    return false;
                }
                first = FirstPersonRoot(player);
                third = ThirdPersonRoot(player);
                return first != nullptr && third != nullptr && IsAppCulled(first) && !IsAppCulled(third);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SwitchBytesMatch(uintptr_t base)
        {
            __try {
                return base != 0 && SameCode(kSwitchRva, kSwitchBytes, sizeof(kSwitchBytes));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix202) What the log tells of each call: who called (the return address), the sit state (+0xFC bits 11-12: 0 none, 1 getting
        // in, 2 sitting or lying, 3 getting up), the third-person mark (+0x112A), the game's seat-rig flag (+0x112F bit 3, its own
        // body-in-a-seat view) and which body was hidden (-1: none there). Read under a guard.
        struct SwitchFacts
        {
            uintptr_t ret{ 0 };
            uint32_t  state2{ 0 };
            uint8_t   mark{ 0 };
            uint8_t   seat_rig{ 0 };
            int       first_hidden{ -1 };
            int       third_hidden{ -1 };
        };

        bool ReadSwitchFacts(RE::PlayerCharacter* player, uintptr_t rsp, SwitchFacts& f)
        {
            __try {
                f.ret             = *reinterpret_cast<const uintptr_t*>(rsp);
                const auto* bytes = reinterpret_cast<const uint8_t*>(player);
                f.state2          = *reinterpret_cast<const uint32_t*>(bytes + 0xFC);
                f.mark            = bytes[kPlayerViewMark];
                f.seat_rig        = bytes[0x112F];
                const auto* first = FirstPersonRoot(player);
                const auto* third = ThirdPersonRoot(player);
                f.first_hidden    = first != nullptr ? (IsAppCulled(first) ? 1 : 0) : -1;
                f.third_hidden    = third != nullptr ? (IsAppCulled(third) ? 1 : 0) : -1;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void OnPlayerSwitch(safetyhook::Context& ctx)
        {
            auto* player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr || ctx.rcx != reinterpret_cast<uintptr_t>(player)) {
                return;
            }
            const bool  to_third = (ctx.rdx & 0xFF) == 0;
            const bool  shown    = g_state.active.load();
            SwitchFacts facts{};
            const bool  read  = ReadSwitchFacts(player, ctx.rsp, facts);
            RE::NiAVObject* first = nullptr;
            RE::NiAVObject* third = nullptr;
            // (fix202: any turn to third person; fix146 only a ladder's.)
            const bool fixed = to_third && BodiesMixed(player, first, third);
            if (fixed) {
                // (Noted first: the body's own update, on another thread, then leaves the bodies alone until the game has swapped them.)
                g_switch_fixed_ms.store(SteadyMs());
                SetAppCulled(third, true);
                SetAppCulled(first, false);
                g_switch_fixes.fetch_add(1);
            }
            // (Separate caps: the turns the hook acted on are never crowded out by the others.)
            static std::atomic<int> lines{ 0 };
            static std::atomic<int> fixed_lines{ 0 };
            if (fixed ? fixed_lines.fetch_add(1) < 40 : lines.fetch_add(1) < 80) {
                const char* what = !read   ? "its state could not be read"
                                   : fixed ? "the shown body up: the bodies put as in first person, so the game swaps them and its animations "
                                             "to third person itself"
                                   : !to_third
                                       ? "left to the game: going to first person it swaps the bodies itself whenever the first-person body "
                                         "is hidden (refused only for some player states, leaving the mark set), or starts its own "
                                         "body-in-a-seat view in a seat"
                                   : (facts.mark & 8) != 0
                                       ? "left as they are: the game's third-person mark already set (third person, or its own "
                                         "body-in-a-seat view)"
                                       : "the bodies as the game has them: it swaps them itself";
                const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
                spdlog::info("[Body] (fix202) the game turns you to {} person (called from rva {:x}): the shown body {}, sit state {}, the "
                             "third-person mark {} (+0x112A {:02x}), its seat view {}, the first-person body {}, the third-person body {}, the "
                             "camera in first person {}, a ladder just activated {}: {} (thread {})",
                    to_third ? "third" : "first", facts.ret > base ? facts.ret - base : 0, shown ? "up" : "off", (facts.state2 >> 11) & 3u,
                    (facts.mark >> 3) & 1, facts.mark, (facts.seat_rig >> 3) & 1,
                    facts.first_hidden < 0 ? "missing" : facts.first_hidden ? "hidden" : "shown",
                    facts.third_hidden < 0 ? "missing" : facts.third_hidden ? "hidden" : "shown", GameFlow::isInFirstPerson(),
                    LadderClimb::ActivatedRecently(), what, GetCurrentThreadId());
            }
            // (The review) Put as in first person once more just before the game reads them: the body's update on another thread may have
            // shown the body between the first time and now.
            if (fixed && (!IsAppCulled(third) || IsAppCulled(first))) {
                SetAppCulled(third, true);
                SetAppCulled(first, false);
            }
        }
    }

    void InstallSwitchHook()
    {
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (!SwitchBytesMatch(base)) {
            spdlog::warn("[Body] the game's first/third-person switch is not as in Starfield 1.16.236 (rva {:x}): left as it is", kSwitchRva);
            return;
        }
        g_switch_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kSwitchRva)), &OnPlayerSwitch);
        spdlog::info("[Body] the game's first/third-person switch (rva {:x}) {}", kSwitchRva,
            g_switch_hook ? "hooked: the bodies put as in first person before any turn to third person with the shown body up (fix202: not "
                            "only a ladder's)"
                          : "could not be hooked");
    }

    bool RightBladeLocal(glm::vec3& way)
    {
        std::scoped_lock _{ g_right_blade_mutex };
        if (std::chrono::steady_clock::now() - g_right_blade_at > std::chrono::milliseconds(200)) {
            return false;
        }
        way = g_right_blade_local;
        return true;
    }

    void NoteLeftWeaponRoot(const glm::vec3& root)
    {
        static auto vr = VR::get();
        std::scoped_lock _{ g_left_copy_mtx };
        g_left_copy_root  = root;
        g_left_copy_frame = vr->m_engine_frame_count.load();
        g_left_copy_valid = true;
    }

    bool GetHandGripFrame(bool left, HandGripFrame& out)
    {
        const auto& store = g_grip_frame[left ? 0 : 1];
        if (!store.valid.load(std::memory_order_acquire)) {
            return false;
        }
        out = store.frame;
        return true;
    }

    float ViewHeightOffset()
    {
        return g_state.active ? g_view_offset.load() : 0.0f;
    }

    namespace
    {
        // (fix189) The muzzle only if it was found this engine frame or the last (RefreshMuzzle): an older one may have been freed.
        RE::NiAVObject* FreshMuzzle()
        {
            static auto    vr     = VR::get();
            const uint32_t set_at = g_body_muzzle_frame.load(std::memory_order_acquire);
            auto* const    muzzle = g_body_muzzle.load(std::memory_order_acquire);
            const uint32_t now    = vr->m_engine_frame_count.load();
            return muzzle != nullptr && now - set_at <= 1 ? muzzle : nullptr;
        }
    }

    RE::NiAVObject* GetBodyMuzzle()
    {
        return FreshMuzzle();
    }

    bool GetBodyAimForward(float out[3])
    {
        if (!FreshMuzzle()) { // (fix189: found this frame or the last)
            return false;
        }
        out[0] = g_barrel.aim_forward.x;
        out[1] = g_barrel.aim_forward.y;
        out[2] = g_barrel.aim_forward.z;
        return true;
    }

    bool GetMuzzleInAim(float position[3], float direction[3])
    {
        if (!g_body_muzzle.load()) {
            return false;
        }
        std::scoped_lock _{ g_muzzle_mutex };
        if (!g_muzzle_in_aim_set || std::chrono::steady_clock::now() - g_muzzle_in_aim_at > std::chrono::milliseconds(300)) {
            return false;
        }
        position[0]  = g_muzzle_in_aim.x;
        position[1]  = g_muzzle_in_aim.y;
        position[2]  = g_muzzle_in_aim.z;
        direction[0] = g_barrel_in_aim.x;
        direction[1] = g_barrel_in_aim.y;
        direction[2] = g_barrel_in_aim.z;
        return true;
    }

    bool BarrelKnown()
    {
        return g_body_muzzle.load() != nullptr && g_basis.has && g_basis.weapon == g_place.node && (g_basis.from_muzzle || g_basis.fired);
    }

    bool GetBodyMuzzleWorld(float out[3])
    {
        if (!g_body_muzzle.load()) {
            return false;
        }
        std::scoped_lock _{ g_muzzle_mutex };
        if (!g_muzzle_valid) {
            return false;
        }
        out[0] = g_muzzle_world.x;
        out[1] = g_muzzle_world.y;
        out[2] = g_muzzle_world.z;
        return true;
    }

    void NotifyPlayerFired()
    {
        g_fire_sample = true;
    }

    void NotifyBeamFrame()
    {
        g_beam_sample = true;
    }

    bool GunHeldWithBothHands()
    {
        return g_state.active && g_gun_both_hands.load(std::memory_order_relaxed);
    }

    bool IsSupportHandOnGun()
    {
        return g_state.active && g_support_on_gun.load();
    }

    void DebugRoots(RE::NiAVObject*& first_person, RE::NiAVObject*& third_person)
    {
        first_person = nullptr;
        third_person = nullptr;
        auto player  = CreationEngineSingletonManager::GetPlayerRef();
        if (player == nullptr) {
            return;
        }
        first_person = FirstPersonRoot(player);
        third_person = ThirdPersonRoot(player);
    }
}
