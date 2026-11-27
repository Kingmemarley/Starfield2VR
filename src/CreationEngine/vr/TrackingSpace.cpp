#include "TrackingSpace.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstring>
#include <excpt.h>
#include <format>
#include <mutex>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_interpolation.hpp>
#include <glm/gtx/quaternion.hpp>
#include <mods/VR.hpp>

#include <CreationEngine/CreationEngineConstants.h>
#include <CreationEngine/CreationEngineSingletonManager.h>
#include <CreationEngine/GrenadeHand.h>
#include <CreationEngine/GunSight.h>
#include <CreationEngine/HandActions.h>
#include <CreationEngine/TempleGestures.h>
#include <CreationEngine/LadderClimb.h>
#include <CreationEngine/LeftWeapon.h>
#include <CreationEngine/FavoritesWheel.h>
#include <CreationEngine/ConversationPanel.h>
#include <CreationEngine/BuildConsole.h>
#include <CreationEngine/Climbing.h>
#include <CreationEngine/HandGrab.h>
#include <CreationEngine/ImpactMelee.h>
#include <CreationEngine/MeleeThrow.h>
#include <CreationEngine/Physical.h>
#include <CreationEngine/PointInteract.h>
#include <CreationEngine/SeatView.h>
#include <CreationEngine/NiMath.h>
#include <RE/N/NiCamera.h>
#include <RE/P/PlayerCamera.h>
#include <RE/F/FirstPersonState.h>
#include <spdlog/spdlog.h>
#include <CreationEngine/body/BodyIK.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <ModSettings.h>

// Ported from SFVR (AetherianArtificer's fork of starfield2vr). Our framework's controller pose
// (get_transform) is the grip pose; the aim pose comes from get_aim_transform.
namespace tracking
{
    namespace
    {
        const glm::mat4 permutation_pre = {
            1, 0, 0, 0,
            0, 0, 1, 0,
            0, -1, 0, 0,
            0, 0,  0, 1
        };
        const glm::mat4 permutation_post = glm::transpose(permutation_pre);

        std::mutex g_aim_mutex;
        glm::mat4  g_aim_rotation{ 1.0f };
        std::atomic<bool> g_aiming_with_hand{ false };
        std::atomic<bool> g_pointing{ false };
        // The rotation last written into the game camera, and the engine frame it was written on.
        glm::mat4  g_applied_aim_rotation{ 1.0f };
        int        g_applied_aim_frame{ -1000 };
        // Right controller aim pose (stage space) sampled together with the aim rotation.
        glm::mat4  g_right_hand_pose{ 1.0f };

        // Grip poses (palm) of both controllers, sampled with the aim rotation.
        glm::mat4 g_grip_pose[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
        glm::mat4 g_left_aim_pose{ 1.0f };
        glm::mat4 g_head_pose{ 1.0f };

        std::mutex g_room_mutex;
        glm::mat3  g_room_rotation{ 1.0f };
        RE::NiQuaternion g_room_quaternion{ 1.0f, 0.0f, 0.0f, 0.0f }; // (fix145: as given, for the ladder's view)
        bool       g_has_room{ false };

        std::atomic<float> g_tracking_scale{ 1.0f };
        std::atomic<float> g_character_eye_height{ 1.65f }; // (fix146) the character's eyes above its feet, as last measured (metres)
        std::atomic<bool>  g_eye_height_known{ false }; // (fix185: atomic, read by climbing on the main thread)
    }

    namespace
    {
        void NoteCameraRootNode();
        void NoteFrameStart();
        void UpdateViewTie();
    }

    void Snapshot()
    {
        static auto vr = VR::get();
        NoteFrameStart();     // (fix169: when each frame starts, for the frame's move guessed by FrameStartLead)
        NoteCameraRootNode(); // (fix148: the camera root watched as the game updates it)
        UpdateViewTie();      // (fix195: whether the view is tied to the body this frame)
        const auto& settings = GameFlow::gStore.internalSettings;
        const bool controllers = vr->is_hmd_active() && vr->is_using_controllers();
        const bool weapon_out  = GameFlow::isWeaponDrawn();
        // Hand interaction: with the weapon put away (or only your fists out), the game picks what is near the right
        // hand (doors, loot, people). Not in menus, conversations, the pilot's seat or on the flat screen. (fix175: nor build mode, where the
        // game's pick from its camera chooses what modify mode works on; no grab or gloves' pull starts there either: HandGrab.)
        const bool pointing = controllers && settings.handInteract && (!weapon_out || HandGrab::HandsFree()) && GameFlow::isInFirstPerson() &&
                              !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown() && !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() &&
                              !ModSettings::showFlatScreenDisplay();
        // Aim with the right hand: always in the "Right hand" aiming mode, and while the body holds a drawn
        // weapon when "Aim with the gun" is on (the gun is in your hand, so shots follow it). Hand interaction
        // does not turn the camera: the game's interaction pick is moved to the hand directly (PointInteract).
        // (fix176) And in build mode: the game places the item along its camera's aim (the camera's parent, this aim rotation), so it goes
        // where your right hand points (after fix175: item placement aimed with the hand, not the face); your view stays your head's.
        const bool building = GameFlow::isWorkshopShown() && !GameFlow::isShowingMenu();
        const bool hand = controllers && (ModConstants::headTrackingType == ModConstants::kAimWithRightHand ||
                                          (ModConstants::aimWithGunHand && body::IsActive() && weapon_out) || building);
        const auto right_aim  = vr->get_aim_transform(vr->get_right_controller_index());
        const auto rotation   = hand ? glm::extractMatrixRotation(right_aim) : vr->get_rotation(0);
        g_pointing = pointing;
        PointInteract::Report();
        const auto left_grip  = vr->get_grip_transform(vr->get_left_controller_index());
        const auto left_aim   = vr->get_aim_transform(vr->get_left_controller_index());
        const auto right_grip = vr->get_grip_transform(vr->get_right_controller_index());
        const auto head       = vr->get_transform(0);
        {
            std::scoped_lock _{ g_aim_mutex };
            if (hand != g_aiming_with_hand.load()) {
                // At most one line a second, in case a menu flickers on and off.
                static auto last_log = std::chrono::steady_clock::time_point{};
                const auto  now      = std::chrono::steady_clock::now();
                if (now - last_log > std::chrono::seconds(1)) {
                    last_log = now;
                    spdlog::info("[Aim] aiming with the {}{}", hand ? "right controller" : "headset", hand && building ? " ((fix176) build mode: items placed where it points)" : "");
                }
            }
            g_aiming_with_hand = hand;
            g_grip_pose[0]     = left_grip;
            g_left_aim_pose    = left_aim;
            g_grip_pose[1]     = right_grip;
            g_aim_rotation     = rotation;
            g_right_hand_pose  = right_aim;
            g_head_pose        = head;
        }
        // Swings and grips are measured on the poses just stored: first the shoulder and push gestures (a grip they take is
        // then neither a grab nor a melee swing), then melee and the hand grab.
        HandActions::Update();
        TempleGestures::Update(); // (a trigger at a temple: the flashlight, the scanner)
        LadderClimb::Update(); // (fix143: a ladder climbed with the hands)
        // (fix171) The favorites wheel: before the hand grab, melee, the throw and the left weapon (it takes both grips while open, and its
        // choice for the left hand is decided by LeftWeapon::Update below).
        FavoritesWheel::Update();
        ConversationPanel::Update(); // (fix145: a conversation on its own panel in front of the person speaking)
        BuildConsole::Update();      // (fix177: build mode as a console of panels low in front of your seat)
        ImpactMelee::Update();
        HandGrab::Update();
        MeleeThrow::Update(); // (after the hand grab has read the grips)
        LeftWeapon::Update();  // (fix154: the left hand's melee weapon asked for, or let go)
        GrenadeHand::Update(); // (a grenade taken from your shoulder: its copy in your hand; the left hand's weapon's copy)
        Physical::Update();    // (fix172: people grabbed, shoved, knocked down; blocks; the probe)
        // (fix179) Climbing with the hands: after the hand grab and Physical have had the grips (a squeeze is the climb's only when they took
        // nothing with it), before the body places the hands.
        Climbing::Update();
        // A power pushed out goes where the hand pushed: the aim follows that push for a moment.
        if (glm::mat4 push_aim{ 1.0f }; controllers && HandActions::AimOverride(push_aim)) {
            std::scoped_lock _{ g_aim_mutex };
            g_aim_rotation = push_aim;
        }
        GunSight::Update(hand);
    }

    bool AimingWithHand()
    {
        return g_aiming_with_hand.load();
    }

    bool HandInteract()
    {
        return g_pointing.load();
    }

    bool StageToWorld(const glm::vec3& stage_point, const glm::vec3& stage_direction, glm::vec3& world_point, glm::vec3& world_direction,
        float* world_scale)
    {
        auto root   = CreationEngineSingletonManager::GetSceneGraphRoot();
        auto camera = root ? root->worldCamera : nullptr;
        const auto room = RoomRotation();
        if (!camera || !camera->parent || !room) {
            return false;
        }
        // Same mapping as the shown body's hands: stage space around the recentre point, turned with the
        // room and placed at the camera's parent.
        const auto&     anchor = camera->parent->world;
        const float     scale  = TrackingScale() * anchor.scale;
        const glm::vec3 offset = *room * ToHavokVector(stage_point - StandingOriginPosition()) * scale;
        const glm::vec3 place  = CameraRootPlace(camera->parent); // (fix147: this frame's)
        // (fix199) Plus the view's move to the seated body's eyes (SeatView; zero unless seated or just up).
        world_point            = glm::vec3{ place.x, place.y, place.z + body::ViewHeightOffset() } + SeatView::WorldShift() + offset;
        world_direction        = *room * ToHavokVector(stage_direction);
        const float length     = glm::length(world_direction);
        if (length > 1e-6f) {
            world_direction /= length;
        }
        if (world_scale) {
            *world_scale = scale;
        }
        return std::isfinite(world_point.x) && std::isfinite(world_point.y) && std::isfinite(world_point.z) && std::isfinite(world_direction.x) &&
               std::isfinite(world_direction.y) && std::isfinite(world_direction.z) && std::isfinite(scale);
    }

    namespace
    {
        // (fix148) The camera root as the game last worked out its world place (AfterNodeUpdate), and your character's place then; how far
        // the body's place was from where the root went next (for the log).
        constexpr uintptr_t           kRefLocation = 0x8c; // TESObjectREFR::data.location (as HandGrab reads it)
        std::atomic<const void*>      g_watched_root{ nullptr };
        std::mutex                    g_root_place_mutex;
        glm::vec3                     g_root_written{ 0.0f };
        glm::vec3                     g_you_at_write{ 0.0f };
        bool                          g_root_written_valid{ false };
        // (fix157) The root's place before its last, and when each was written: its move over the last frame, for what is placed before
        // your character has moved this frame (the left hand's weapon's copy).
        glm::vec3                     g_root_before{ 0.0f };
        bool                          g_root_before_valid{ false };
        std::chrono::steady_clock::time_point g_root_written_at{}, g_root_before_at{};
        glm::vec3                     g_predicted{ 0.0f };
        glm::vec3                     g_stale{ 0.0f };
        bool                          g_predicted_valid{ false };
        double                        g_err_new{ 0.0 }, g_err_old{ 0.0 };
        float                         g_err_new_max{ 0.0f }, g_err_old_max{ 0.0f }, g_moved_max{ 0.0f };
        int                           g_err_n{ 0 }, g_mismatch{ 0 };
        auto                          g_err_at = std::chrono::steady_clock::now();
        int                           g_err_logs{ 0 };
        // (fix185) While climbing ties the view to you (Climbing::ViewTie): the place given to the body (CameraRootPlace) against where
        // the root then went, counted apart from the frames not climbing (the lead's own numbers above).
        glm::vec3                     g_tied_given{ 0.0f };
        bool                          g_tied_valid{ false };
        double                        g_tied_err{ 0.0 };
        float                         g_tied_err_max{ 0.0f };
        int                           g_tied_n{ 0 };
        std::atomic<int64_t>          g_root_seen_ms{ 0 }; // (the root last through the hook)
        // (fix169) The frame's move as guessed at its start (FrameStartLead: the left saber's copy, a grenade), three ways: last frame's
        // move as it was; scaled by this frame's time against the last (the frames' starts, Snapshot); scaled by this frame's time against
        // the last move's (the root's writes). Each checked against where the root really went (AfterNodeUpdate); the closest on average
        // used. The test with fix168 (a helper's research): at a run the frames' lengths jump by whole headset refreshes, so "as last frame"
        // put the saber 5-6 cm off your drawn hand at each jump and up to 15-20 cm on a slow frame (and a frame's move behind, 15-35 cm, when
        // a frame took over 70 ms: no guess at all then).
        constexpr int                 kGuesses = 3;
        constexpr const char*         kGuessNames[kGuesses] = { "as last frame", "scaled by the frames' starts", "scaled by the root's writes" };
        std::chrono::steady_clock::time_point g_start_at[3]{}; // (this frame's start, the last's, the one before)
        std::chrono::steady_clock::time_point g_guess_for{};   // (the frame start the guesses were made for)
        glm::vec3                     g_guess[kGuesses]{};
        glm::vec3                     g_guess_base{ 0.0f };    // (the root's place they were made from)
        bool                          g_guess_ok{ false };     // (made, not yet checked)
        float                         g_guess_avg[kGuesses]{}; // (each one's error, a running average over about 20 frames)
        bool                          g_guess_avg_ok{ false };
        int                           g_guess_use{ 1 };        // (the frames' starts first: the game moves you by the time between them)
        double                        g_guess_sum[kGuesses]{};
        float                         g_guess_max[kGuesses]{};
        int                           g_guess_n{ 0 }, g_guess_used[kGuesses]{};
        int                           g_guess_logs{ 0 }; // (its own count: the older line keeps its 60; the review)
        // (fix195) "Tie the view to the body" (BeforeNodeUpdate). Under g_root_place_mutex unless atomic.
        // - g_tie_weight: how much the root is tied now (0..1), eased over kTieEase in Snapshot (main thread); g_tie_wanted its target.
        // - g_root_game: the game's own place for the root at its last update, before the tie moved it. The next frame's place is worked
        //   out from it (LeadPlace), so the game's eye height (crouching, the weapon drawn, a landing) still comes through, a frame
        //   later; from the tied place itself the tie would keep its own height for good.
        // - g_last_write_tied / g_root_written_frame: the root's last update was tied, and the engine frame it was in (once the root is
        //   put this frame, CameraRootPlace gives its place as put: what the view is drawn from).
        // - g_given: the place given to the body this frame (CameraRootPlace before the root's update), which the root is then put at.
        // - g_pass_*: from BeforeNodeUpdate to AfterNodeUpdate within one update of the root; g_tie_local: the local place written this
        //   frame (a second update of the camera tree in a frame gets the same).
        constexpr float               kTieEase = 0.15f; // seconds to tie or let go
        // (fix195, the reviews) A guess over kTieMove off the game's own root is taken for a spike (a jet pack's start, a landing, the
        // game's collision pushing you: it would put the whole view there for a frame and back the next); the usual miss is 0.5-1.5 cm.
        // Over kTieFar (a jump, a load) the game's place is kept as it is. (BeforeNodeUpdate.)
        constexpr float               kTieMove = 0.05f;
        constexpr float               kTieFar  = 0.5f;
        std::atomic<float>            g_tie_weight{ 0.0f };
        std::atomic<bool>             g_tie_wanted{ false };
        glm::vec3                     g_root_game{ 0.0f };
        bool                          g_root_game_ok{ false };
        bool                          g_last_write_tied{ false };
        uint32_t                      g_root_written_frame{ 0 };
        glm::vec3                     g_given{ 0.0f };
        uint32_t                      g_given_frame{ 0 };
        bool                          g_given_ok{ false };
        const void*                   g_pass_obj{ nullptr };
        glm::vec3                     g_pass_game{ 0.0f };
        uint32_t                      g_pass_frame{ 0 };
        RE::NiPoint3                  g_tie_local{};
        uint32_t                      g_tie_local_frame{ 0 };
        bool                          g_tie_local_ok{ false };
        std::chrono::steady_clock::time_point g_tie_local_at{}; // (when it was written: a repeat only within kTieRepeatShare of a frame)
        // (fix195, the second review) The view's offset off the game's own root (game world), as last put, and the frame it was put in:
        // its change is held to kTieStep a frame, and a miss over kTieMove (a spike of the guess) halves it instead, so the view never
        // jumps by the guess's misses (the last log: your character's place jumped up to 0.4 m in one frame while the game's root moved 6 cm).
        constexpr float               kTieStep        = 0.01f;
        constexpr float               kTieRepeatShare = 0.4f;
        glm::vec3                     g_tie_o{ 0.0f };
        uint32_t                      g_tie_o_frame{ 0 };
        bool                          g_tie_o_ok{ false };
        // (fix195) For the log, every 10 s while tied: root updates tied, how far the view was moved off the game's own root, its offset's
        // change frame to frame, how many were not put all the way (a spike over kTieMove, or the change held to kTieStep), refused (over
        // kTieFar) or not worked out ahead, passes before the game had put the root that frame (left alone), second updates soon after the
        // first and later ones, and how far the place the body was given this frame was from the one the root was put at (about 0: the
        // same place, worked out again with your character's place then).
        int                           g_tie_n{ 0 }, g_tie_refused{ 0 }, g_tie_clamped{ 0 }, g_tie_untouched{ 0 }, g_tie_repeats{ 0 };
        int                           g_tie_body_n{ 0 }, g_tie_unled{ 0 }, g_tie_steps_n{ 0 }, g_tie_reput_n{ 0 }, g_tie_late{ 0 };
        double                        g_tie_off_sum{ 0.0 }, g_tie_step_sum{ 0.0 }, g_tie_reput_sum{ 0.0 };
        float                         g_tie_off_max{ 0.0f }, g_tie_body_max{ 0.0f }, g_tie_step_max{ 0.0f }, g_tie_reput_max{ 0.0f };
        int                           g_tie_logs{ 0 };

        uint32_t EngineFrameNow()
        {
            static auto vr = VR::get();
            return vr != nullptr ? vr->m_engine_frame_count.load() : 0u;
        }

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Your character's place (its reference's: what its character controller moves), read under a guard.
        bool YouAtSafe(float out[3])
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (player == 0) {
                return false;
            }
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(player + kRefLocation), 3 * sizeof(float));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }

        bool YouAt(glm::vec3& out)
        {
            float at[3]{};
            if (!YouAtSafe(at)) {
                return false;
            }
            out = glm::vec3{ at[0], at[1], at[2] };
            return true;
        }

        // (Each frame's start, Snapshot) The node to watch: the world camera's parent. Told if, in first person, it has not come
        // through the node update hook for 5 s (the body then placed as before fix148; the review).
        // (fix189) The 5 s counted only in play: not at the main menu, on a loading screen or just after, nor behind a menu (the game may
        // not update the camera's tree there: the warning came at the main menu, and, told only once, a real case later was never told).
        // Told again after the root has come through again (10 times in all).
        void NoteCameraRootNode()
        {
            auto  scene  = CreationEngineSingletonManager::GetSceneGraphRoot();
            auto* camera = scene ? scene->worldCamera : nullptr;
            g_watched_root.store(camera != nullptr ? camera->parent : nullptr, std::memory_order_release);
            static int64_t since_ms = 0;
            static bool    told     = false;
            static int     told_n   = 0; // (fix189)
            const int64_t  now      = NowMs();
            const int64_t  seen_ago = now - g_root_seen_ms.load(std::memory_order_relaxed);
            if (told && seen_ago < 1000) {
                told = false;
                spdlog::info("[Tracking] (fix189) the camera root comes through the node update hook again");
            }
            if (camera == nullptr || camera->parent == nullptr || !GameFlow::isInFirstPerson() || !GameFlow::worldBehindMenu() ||
                GameFlow::isShowingMenu()) {
                since_ms = 0;
                return;
            }
            if (since_ms == 0) {
                since_ms = now;
            }
            if (!told && told_n < 10 && now - since_ms > 5000 && seen_ago > 5000) {
                told = true;
                ++told_n;
                spdlog::warn("[Tracking] the camera root has not come through the node update hook for 5 s in first person: the body placed at "
                             "its own place (the frame before's), as before fix148");
            }
        }

        // (fix195) Each frame's start (Snapshot, the main thread): whether the view is tied to the body this frame ("Tie the view to the
        // body", BeforeNodeUpdate), eased in and out over kTieEase; at once off on a loading screen. Told when it starts and stops (40 times).
        void UpdateViewTie()
        {
            static auto vr = VR::get();
            const auto& s  = GameFlow::gStore.internalSettings;
            const bool  loading = GameFlow::isLoadingScreenShown();
            // (The second review) Not while climbing either (Climbing ties the view itself), so after a climb this tie eases in again.
            glm::vec3   climb_place{};
            const bool  climbing = Climbing::Active() || Climbing::ViewTie(climb_place) > 0.0f;
            // On a ladder, at the ladder's view, or out of first person: off at once (the review: not eased out there).
            const bool  at_once = loading || !GameFlow::isInFirstPerson() || GameFlow::isOnLadder() || LadderClimb::ViewAtHead();
            const bool  want = s.tieViewToBody && vr != nullptr && vr->is_hmd_active() && !ModSettings::cameraFlat() && body::IsActive() &&
                              !at_once && !GameFlow::isShowingMenu() && !LadderClimb::ActivatedRecently() && !GameFlow::isImmovable() &&
                              !GameFlow::isControlledByAI() && !GameFlow::isWorkshopShown() && !GameFlow::isPilotingShip() &&
                              !GameFlow::isInDialogue() && !climbing;
            static auto last = std::chrono::steady_clock::now();
            const auto  now  = std::chrono::steady_clock::now();
            const float dt   = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
            last             = now;
            float w          = g_tie_weight.load(std::memory_order_relaxed);
            w                = at_once ? 0.0f : std::clamp(w + (want ? dt : -dt) / kTieEase, 0.0f, 1.0f);
            g_tie_weight.store(w, std::memory_order_relaxed);
            static int told = 0;
            if (want != g_tie_wanted.exchange(want) && told < 40) {
                ++told;
                spdlog::info("[Tracking] (fix195) the view {} the body ({})", want ? "tied to" : "no longer tied to",
                    want                          ? "first person on foot with the body shown: the camera root put where the body is placed"
                    : !s.tieViewToBody            ? "Tie the view to the body is off"
                    : !body::IsActive()           ? "the body is not shown"
                    : GameFlow::isShowingMenu()   ? "a menu"
                    : loading                     ? "a loading screen"
                    : !GameFlow::isInFirstPerson() ? "not first person"
                    : climbing                    ? "climbing (its own tie)"
                                                  : "a ladder, a seat, a conversation, build mode or the pilot's seat");
            }
        }

        // (fix169) Each frame's start (Snapshot, the main thread): the last three kept.
        void NoteFrameStart()
        {
            std::scoped_lock _{ g_root_place_mutex };
            g_start_at[2] = g_start_at[1];
            g_start_at[1] = g_start_at[0];
            g_start_at[0] = std::chrono::steady_clock::now();
        }

        // (fix169, under g_root_place_mutex) The root written anew at `root`: the guesses made from the place before it, checked.
        void CheckGuesses(const glm::vec3& root)
        {
            if (!g_guess_ok || !g_root_written_valid || glm::length(g_guess_base - g_root_written) > 1e-5f) {
                g_guess_ok = false;
                return;
            }
            g_guess_ok          = false;
            const glm::vec3 moved = root - g_root_written;
            if (!(glm::length(moved) < 1.0f)) {
                return; // (a jump: not a frame's move)
            }
            for (int k = 0; k < kGuesses; ++k) {
                const float err = glm::length(moved - g_guess[k]);
                if (!std::isfinite(err)) {
                    return;
                }
                g_guess_avg[k] = g_guess_avg_ok ? g_guess_avg[k] + (err - g_guess_avg[k]) * 0.05f : err;
                g_guess_sum[k] += err;
                g_guess_max[k] = std::max(g_guess_max[k], err);
            }
            g_guess_avg_ok = true;
            ++g_guess_n;
            // (Another one used only when clearly closer: 10%, so it does not flip each frame.)
            for (int k = 0; k < kGuesses; ++k) {
                if (g_guess_avg[k] < g_guess_avg[g_guess_use] * 0.9f) {
                    g_guess_use = k;
                }
            }
        }
    }

    void AfterNodeUpdate(const RE::NiAVObject* obj)
    {
        if (obj == nullptr || obj != g_watched_root.load(std::memory_order_acquire)) {
            return;
        }
        const glm::vec3 root{ obj->world.translate.x, obj->world.translate.y, obj->world.translate.z };
        glm::vec3       you{};
        const bool      have_you = YouAt(you);
        g_root_seen_ms.store(NowMs(), std::memory_order_relaxed);
        std::scoped_lock _{ g_root_place_mutex };
        // (fix195) Tied in this update (BeforeNodeUpdate put it): the game's own place kept apart, the next frame is worked out from it.
        // Noted before the check below: a tied place the same as the last one (standing still) may still carry a new place of the game's.
        // (BeforeNodeUpdate and this run in the same node update, so the pass is matched by its node alone (the review: the engine frame
        // can move on between the two when the camera tree updates on another thread than the frame's start); the frame is the one
        // BeforeNodeUpdate wrote in.)
        const uint32_t frame_now = EngineFrameNow();
        const bool     tied_pass = g_pass_obj == obj;
        const uint32_t pass_frame = tied_pass ? g_pass_frame : frame_now;
        const glm::vec3 game_place = tied_pass ? g_pass_game : root; // (the game's own place this update)
        if (tied_pass) {
            g_root_game          = g_pass_game;
            g_root_game_ok       = true;
            g_last_write_tied    = true;
            g_root_written_frame = pass_frame;
            g_pass_obj           = nullptr;
        }
        // (The camera tree is at times updated twice a frame: a pass that leaves the root where it was keeps your place from its last
        // move, or a pass after your character has moved but before the camera puts the root would take your move back out; the review.)
        if (g_root_written_valid && glm::length(root - g_root_written) < 1e-5f) {
            return;
        }
        // (The log: where the root went, against where the body was put before it, and where it would have been without this. (fix195)
        // While the view is tied to the body: where the game itself put the root, not where the tie did (that is the body's place).)
        if (g_predicted_valid) {
            const float err_new = glm::length(game_place - g_predicted);
            const float err_old = glm::length(game_place - g_stale);
            if (std::isfinite(err_new) && std::isfinite(err_old) && err_old < 2.0f) {
                g_err_new += err_new;
                g_err_old += err_old;
                g_err_new_max = std::max(g_err_new_max, err_new);
                g_err_old_max = std::max(g_err_old_max, err_old);
                ++g_err_n;
            }
            g_predicted_valid = false;
        }
        // (fix185) Tied: the place the body was given against where the root went (both Climbing::ViewTie's: about 0 unless the frame's
        // asked move changed between, or the tie eased meanwhile).
        if (g_tied_valid) {
            const float err = glm::length(root - g_tied_given);
            if (std::isfinite(err) && err < 2.0f) {
                g_tied_err += err;
                g_tied_err_max = std::max(g_tied_err_max, err);
                ++g_tied_n;
            }
            g_tied_valid = false;
        }
        CheckGuesses(root); // (fix169)
        g_root_before       = g_root_written;
        g_root_before_valid = g_root_written_valid;
        g_root_before_at    = g_root_written_at;
        g_root_written_at   = std::chrono::steady_clock::now();
        g_root_written       = root;
        g_you_at_write       = you;
        g_root_written_valid = have_you && std::isfinite(root.x) && std::isfinite(root.y) && std::isfinite(root.z);
        // (fix195) Not tied in this update: the root's place is the game's own.
        if (!tied_pass) {
            g_last_write_tied    = false;
            g_root_game          = root;
            g_root_game_ok       = g_root_written_valid;
            g_root_written_frame = frame_now;
        }
        if (const auto now = std::chrono::steady_clock::now(); now - g_err_at > std::chrono::seconds(10)) {
            // (fix195) The view tied to the body, the last 10 s.
            if ((g_tie_n > 0 || g_tie_refused > 0) && g_tie_logs < 60) {
                ++g_tie_logs;
                spdlog::info("[Tracking] (fix195) the view tied to the body, the last 10 s or so: {} camera root updates put where the body was "
                             "placed; the view {:.3f} m off the game's own camera on average (at most {:.3f}); its offset's change frame to frame "
                             "{:.4f} on average (at most {:.4f}, held to {:.2f}); {} not all the way to the body's place (a spike over {:.2f} m, or "
                             "the change held), {} refused (over {:.1f} m: the game's kept), {} not worked out ahead (the game's kept); {} passes "
                             "before the game had put the root that frame (left alone); {} second updates soon after the first (given the same; "
                             "the game's root moved {:.4f} m on average between, at most {:.4f}, in {}), {} later ones worked out anew; the body's "
                             "place that frame {:.4f} m at most from the root's ({} frames)",
                    g_tie_n, g_tie_n > 0 ? g_tie_off_sum / g_tie_n : 0.0, g_tie_off_max, g_tie_steps_n > 0 ? g_tie_step_sum / g_tie_steps_n : 0.0,
                    g_tie_step_max, kTieStep, g_tie_clamped, kTieMove, g_tie_refused, kTieFar, g_tie_unled, g_tie_untouched, g_tie_repeats,
                    g_tie_reput_n > 0 ? g_tie_reput_sum / g_tie_reput_n : 0.0, g_tie_reput_max, g_tie_reput_n, g_tie_late, g_tie_body_max,
                    g_tie_body_n);
            }
            g_tie_n = g_tie_refused = g_tie_clamped = g_tie_untouched = g_tie_repeats = g_tie_body_n = 0;
            g_tie_unled = g_tie_steps_n = g_tie_reput_n = g_tie_late = 0;
            g_tie_off_sum = g_tie_step_sum = g_tie_reput_sum = 0.0;
            g_tie_off_max = g_tie_body_max = g_tie_step_max = g_tie_reput_max = 0.0f;
            if (g_err_logs < 60 && ((g_err_n > 0 && g_err_old_max > 0.002f) || g_mismatch > 0 || g_tied_n > 0)) {
                ++g_err_logs;
                spdlog::info("[Tracking] the body placed ahead by your own move since the camera's last (up to {:.3f} m): where the camera then went "
                             "was {:.3f} m from it on average (at most {:.3f}); {:.3f} m (at most {:.3f}) without (fix147's lag), {} frames{} (fix185: "
                             "frames not climbing){}",
                    g_moved_max, g_err_n > 0 ? g_err_new / g_err_n : 0.0, g_err_new_max, g_err_n > 0 ? g_err_old / g_err_n : 0.0, g_err_old_max, g_err_n,
                    g_mismatch > 0 ? std::format("; {} times the root had moved outside its update (left as it was)", g_mismatch) : std::string{},
                    g_tied_n > 0 ? std::format("; (fix185) while climbing (the view tied to you, no lead): the body's place {:.3f} m from where the "
                                               "camera then went on average (at most {:.3f}), {} frames",
                                       g_tied_err / g_tied_n, g_tied_err_max, g_tied_n)
                                 : std::string{});
            }
            g_err_new = g_err_old = 0.0;
            g_err_new_max = g_err_old_max = g_moved_max = 0.0f;
            g_err_n = g_mismatch = 0;
            g_tied_err     = 0.0;
            g_tied_err_max = 0.0f;
            g_tied_n       = 0;
            g_err_at = now;
            // (fix169) The frame's move as guessed at its start, against where the root went (only while you moved).
            if (g_guess_n > 0 && g_guess_logs < 60 && g_guess_max[0] > 0.005f) {
                ++g_guess_logs;
                spdlog::info("[Tracking] (fix169) the frame's move guessed at its start (the left saber's copy, a grenade), against where the root "
                             "went: {} {:.3f} m off on average (at most {:.3f}), {} {:.3f} ({:.3f}), {} {:.3f} ({:.3f}); used: {} {} frames, {} {}, "
                             "{} {}; now {}; {} frames",
                    kGuessNames[0], g_guess_sum[0] / g_guess_n, g_guess_max[0], kGuessNames[1], g_guess_sum[1] / g_guess_n, g_guess_max[1],
                    kGuessNames[2], g_guess_sum[2] / g_guess_n, g_guess_max[2], kGuessNames[0], g_guess_used[0], kGuessNames[1], g_guess_used[1],
                    kGuessNames[2], g_guess_used[2], kGuessNames[g_guess_use], g_guess_n);
            }
            for (int k = 0; k < kGuesses; ++k) {
                g_guess_sum[k]  = 0.0;
                g_guess_max[k]  = 0.0f;
                g_guess_used[k] = 0;
            }
            g_guess_n = 0;
        }
    }

    namespace
    {
        // (fix147, fix148) The body (and what the hands hold) is placed before the player camera puts its root for the frame (the stereo
        // screenshot's trace: the first-person skeleton updated, then the camera's turn read), so the root's own place is the frame
        // before's: the body, its hands and its gun drawn behind by your speed times a frame (after fix146 the laser still moved off the gun
        // while moving, the laser then on your hand). fix147 moved it on by the first-person camera node's move, but that node is put by
        // the same camera update, later: it had not moved either (its test: the gun still 0.185 m from the laser's start, running). Your
        // character's place has: the character controller moves it before the skeleton is updated. So the root's place, as the game last
        // worked it out (AfterNodeUpdate), is moved on by how far your character has moved since then: where the camera will put it. Not
        // when the root has moved since outside its update, beyond 2 m, or out of first person. Logged every 10 s: how far the camera then
        // went from where the body was put, and from where it would have been without this. (fix185: `tied`, the view partly tied to you
        // by climbing: worked out as before but not counted in those numbers.)
        // (fix195) `led`: set true when the place was worked out ahead (not just the root's place as it is). `at_root`: asked at the root's
        // own update (BeforeNodeUpdate), so a later camera pass in the same frame is worked out from the game's place then, not handed the
        // place already put.
        glm::vec3 LeadPlace(const glm::vec3& at, bool tied, bool* led = nullptr, bool at_root = false)
        {
            if (led != nullptr) {
                *led = false;
            }
            glm::vec3 you{};
            if (!YouAt(you)) {
                return at;
            }
            std::scoped_lock _{ g_root_place_mutex };
            if (!g_root_written_valid) {
                return at;
            }
            if (glm::length(at - g_root_written) > 1e-4f) {
                if (!tied) {
                    ++g_mismatch;
                }
                return at;
            }
            // (fix195) The root's last update was tied to the body: once it has been put this frame, its place as put (the view is drawn
            // from it); before, from the game's own place then (g_root_game), not the tied one.
            glm::vec3 base = at;
            if (g_last_write_tied && g_root_game_ok) {
                if (g_root_written_frame == EngineFrameNow() && !at_root) {
                    return at;
                }
                base = g_root_game;
            }
            glm::vec3 moved = you - g_you_at_write;
            if (!std::isfinite(moved.x) || !std::isfinite(moved.y) || !std::isfinite(moved.z) || glm::length(moved) >= 2.0f) {
                return at;
            }
            if (glm::length(moved) > 1e-4f && !tied && !at_root) {
                g_moved_max       = std::max(g_moved_max, glm::length(moved));
                g_predicted       = base + moved;
                g_stale           = at;
                g_predicted_valid = true;
            }
            if (led != nullptr) {
                *led = true;
            }
            return base + moved;
        }

        // (fix195) The place given to the body this frame (before the root's update), for BeforeNodeUpdate to put the root at.
        void NoteGiven(const glm::vec3& place)
        {
            if (!(g_tie_weight.load(std::memory_order_relaxed) > 0.0f)) {
                return;
            }
            const uint32_t   frame = EngineFrameNow();
            std::scoped_lock _{ g_root_place_mutex };
            if (g_root_written_frame == frame && g_root_written_valid) {
                return; // (the root is put already this frame: nothing to give it)
            }
            g_given       = place;
            g_given_frame = frame;
            g_given_ok    = std::isfinite(place.x) && std::isfinite(place.y) && std::isfinite(place.z);
        }
    }

    glm::vec3 CameraRootPlace(const RE::NiAVObject* root)
    {
        const glm::vec3 at{ root->world.translate.x, root->world.translate.y, root->world.translate.z };
        if (!GameFlow::isInFirstPerson() || root != g_watched_root.load(std::memory_order_acquire)) {
            return at;
        }
        // (fix185) While climbing the camera root is put where Climbing ties it (your character's place as asked this frame plus where the
        // game had your eyes), before its world update (Climbing::BeforeNodeUpdate): the same place here, with no lead, so the view, the
        // body, the hands and climbing's rays from your eyes share one place (fix184's test: the lead 4-7 cm off the view while climbing,
        // worse than none). Easing in or out: the lead and the tie blended by the same weight as the root is.
        glm::vec3   tied{ 0.0f };
        const float w = Climbing::ViewTie(tied);
        if (!(w > 0.0f)) {
            const glm::vec3 lead = LeadPlace(at, false);
            NoteGiven(lead); // (fix195: the root is then put here, while the view is tied to the body)
            return lead;
        }
        const glm::vec3 lead  = w >= 1.0f ? tied : LeadPlace(at, true);
        const glm::vec3 place = lead + (tied - lead) * std::min(w, 1.0f);
        {
            std::scoped_lock _{ g_root_place_mutex };
            g_tied_given = place;
            g_tied_valid = true;
        }
        return place;
    }

    const RE::NiAVObject* WatchedRoot()
    {
        return static_cast<const RE::NiAVObject*>(g_watched_root.load(std::memory_order_acquire));
    }

    bool LastRootWrite(glm::vec3& root, glm::vec3& you)
    {
        std::scoped_lock _{ g_root_place_mutex };
        if (!g_root_written_valid) {
            return false;
        }
        // (fix195) The game's own place, not where the view's tie to the body put it.
        root = g_last_write_tied && g_root_game_ok ? g_root_game : g_root_written;
        you  = g_you_at_write;
        return true;
    }

    namespace
    {
        // (fix195) The camera root's own place (its parent's frame), its world place as last put, and its parent's world transform, read
        // under a guard; and its local place written under one (as Climbing's tie does).
        bool ReadRootGuarded(const RE::NiAVObject* obj, RE::NiPoint3& local, RE::NiPoint3& world, RE::NiTransform& parent, bool& has_parent)
        {
            __try {
                local      = obj->local.translate;
                world      = obj->world.translate;
                has_parent = obj->parent != nullptr;
                if (has_parent) {
                    std::memcpy(&parent, &obj->parent->world, sizeof(parent));
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool WriteRootLocalGuarded(RE::NiAVObject* obj, const RE::NiPoint3& local)
        {
            __try {
                obj->local.translate = local;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool FiniteVec(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    }

    // (fix195) "Tie the view to the body". The body (and the gun in its hand) is placed before the game puts the camera root for the
    // frame, at where the root is expected to go (CameraRootPlace: the game's last place for it moved on by your character's move
    // since). The view hangs from the root, so any miss of that guess moved the gun against the view: 0.5-1.5 cm on average while
    // moving, and up to 4-36 cm in single frames, by the log of the last test. Here, just before the root's world update (the game
    // has just put its local place), the root is put at that same place instead: the view and the gun then come from one place, every
    // frame. The game's own camera motion (its eye height as you crouch, the weapon drawn, its head bob and landing dips) still comes
    // through, a frame later: the next guess starts from the game's own place (AfterNodeUpdate). Only in first person on foot, in play,
    // with the body shown (Snapshot sets the weight, eased over kTieEase); never while climbing (Climbing ties the root itself), on a
    // ladder, in a seat, a menu, build mode, a conversation, the pilot's seat or near a loading screen.
    // The view's offset off the game's own place changes by at most kTieStep a frame (the second review: a spike of the guess, your
    // character's place jumping while the game's camera moved little, would move the whole view for a frame and back the next); an
    // offset over kTieMove is taken for such a spike and the last offset halved instead. In those frames the gun is off the view by the
    // rest, as before fix195. A place over kTieFar from the game's (a jump, a load), or one not worked out ahead, is not used at all.
    void BeforeNodeUpdate(RE::NiAVObject* obj)
    {
        if (obj == nullptr || obj != g_watched_root.load(std::memory_order_acquire)) {
            return;
        }
        const auto forget_offset = [] {
            std::scoped_lock _{ g_root_place_mutex };
            g_tie_o_ok = false;
        };
        const float w = std::min(g_tie_weight.load(std::memory_order_relaxed), 1.0f);
        // (The second review) Out of first person, on a ladder or at the ladder's view: never, from this update on (the weight is dropped at
        // the next frame's start, Snapshot).
        if (!(w > 0.0f) || !GameFlow::isInFirstPerson() || GameFlow::isOnLadder() || LadderClimb::ViewAtHead()) {
            forget_offset();
            return;
        }
        if (glm::vec3 climb{}; Climbing::ViewTie(climb) > 0.0f) {
            forget_offset();
            return; // (Climbing's BeforeNodeUpdate, just before, has put it)
        }
        RE::NiPoint3    local_now{}, world_now{};
        RE::NiTransform parent{};
        bool            has_parent = false;
        if (!ReadRootGuarded(obj, local_now, world_now, parent, has_parent)) {
            return;
        }
        const uint32_t  frame = EngineFrameNow();
        const auto      now   = std::chrono::steady_clock::now();
        const glm::mat3 turn  = has_parent ? NiMath::to_glm(parent.rotate) : glm::mat3{ 1.0f };
        const glm::vec3 base  = has_parent ? NiMath::to_glm(parent.translate) : glm::vec3{ 0.0f };
        const float     scale = has_parent && parent.scale > 1e-4f ? parent.scale : 1.0f;
        const auto      game_of = [&](const RE::NiPoint3& local) { return base + turn * (NiMath::to_glm(local) * scale); };
        {
            std::scoped_lock _{ g_root_place_mutex };
            const bool as_left = g_tie_local_ok && std::memcmp(&local_now, &g_tie_local, sizeof(local_now)) == 0;
            // (The review) A pass of the camera tree before the game has put the root this frame: it is as the mod left it, so it is not
            // the game's own place; left alone (the root stays where it was, as AfterNodeUpdate then sees).
            if (as_left && g_tie_local_frame != frame) {
                ++g_tie_untouched;
                return;
            }
            // A second update of the camera tree in the same frame, soon after the first (within kTieRepeatShare of a frame: the second
            // review; a later one, with the frame's count not moved on, is worked out anew below): the place written the first time,
            // again; the game's own place taken again if the game has put the root again since (how far it moved, for the log).
            if (g_tie_local_ok && g_tie_local_frame == frame) {
                const float gap    = std::chrono::duration<float>(g_start_at[0] - g_start_at[1]).count();
                const float since  = std::chrono::duration<float>(now - g_tie_local_at).count();
                const bool  recent = !(gap > 0.002f && gap < 0.5f) || since < gap * kTieRepeatShare;
                if (recent) {
                    if (!as_left) {
                        const glm::vec3 game = game_of(local_now);
                        if (FiniteVec(game)) {
                            const float moved = glm::length(game - g_pass_game);
                            if (std::isfinite(moved)) {
                                g_tie_reput_sum += moved;
                                g_tie_reput_max = std::max(g_tie_reput_max, moved);
                                ++g_tie_reput_n;
                            }
                            g_pass_game = game;
                        }
                    }
                    if (WriteRootLocalGuarded(obj, g_tie_local)) {
                        g_pass_obj   = obj;
                        g_pass_frame = frame;
                        ++g_tie_repeats;
                    }
                    return;
                }
                ++g_tie_late;
            }
        }
        const glm::vec3 game = game_of(local_now);
        // Where the body was placed, worked out again now with your character's place now (the review: a place given at the frame's start,
        // before your character moved, would be a frame's move behind); against the last place given this frame, for the log.
        bool      led   = false;
        glm::vec3 place = LeadPlace(NiMath::to_glm(world_now), false, &led, true);
        float     body_off = -1.0f;
        {
            std::scoped_lock _{ g_root_place_mutex };
            if (g_given_ok && g_given_frame == frame) {
                body_off = glm::length(g_given - place);
            }
        }
        // (The second review) Not worked out ahead (no root place of the game's to go on yet, your place unread, a root moved outside its
        // update): the game's own place, as it is.
        if (!led) {
            std::scoped_lock _{ g_root_place_mutex };
            ++g_tie_unled;
            g_tie_o_ok = false;
            return;
        }
        const glm::vec3 wanted = place - game;
        const float     miss   = glm::length(wanted);
        if (!FiniteVec(game) || !FiniteVec(place) || !(miss < kTieFar)) {
            std::scoped_lock _{ g_root_place_mutex };
            ++g_tie_refused;
            g_tie_o_ok = false;
            return;
        }
        glm::vec3 offset{ 0.0f }, last{ 0.0f };
        bool      have_last = false;
        {
            std::scoped_lock _{ g_root_place_mutex };
            have_last = g_tie_o_ok && (g_tie_o_frame == frame || g_tie_o_frame + 1 == frame);
            last      = have_last ? g_tie_o : glm::vec3{ 0.0f };
        }
        const bool spike = miss > kTieMove;
        if (spike) {
            offset = last * 0.5f;
        } else {
            glm::vec3   step = wanted - last;
            const float n    = glm::length(step);
            if (n > kTieStep) {
                step *= kTieStep / n;
            }
            offset = last + step;
        }
        const glm::vec3 placed = game + offset * w;
        const glm::vec3 local  = glm::transpose(turn) * (placed - base) / scale;
        if (!FiniteVec(local)) {
            return;
        }
        const RE::NiPoint3 written{ local.x, local.y, local.z };
        if (!WriteRootLocalGuarded(obj, written)) {
            return;
        }
        std::scoped_lock _{ g_root_place_mutex };
        g_pass_obj        = obj;
        g_pass_game       = game;
        g_pass_frame      = frame;
        g_tie_local       = written;
        g_tie_local_frame = frame;
        g_tie_local_ok    = true;
        g_tie_local_at    = now;
        g_tie_o           = offset;
        g_tie_o_frame     = frame;
        g_tie_o_ok        = true;
        ++g_tie_n;
        if (spike || glm::length(offset - wanted) > 1e-4f) {
            ++g_tie_clamped; // (not all the way to the body's place this frame)
        }
        if (have_last) {
            const float step = glm::length(offset - last);
            g_tie_step_sum += step;
            g_tie_step_max = std::max(g_tie_step_max, step);
            ++g_tie_steps_n;
        }
        if (body_off >= 0.0f && std::isfinite(body_off)) {
            ++g_tie_body_n;
            g_tie_body_max = std::max(g_tie_body_max, body_off);
        }
        const float off = glm::length(offset);
        g_tie_off_sum += off;
        g_tie_off_max = std::max(g_tie_off_max, off);
    }

    namespace
    {
        // (fix169) The lead worked out once a frame (for its start, g_start_at[0]) and given the same to every caller (GrenadeHand's Target,
        // FrameStartRoot), with the place it was worked out from: the root the copy was placed with is the one the body's left hand is
        // told (worked out twice, the two could differ by a whole step; the review).
        std::chrono::steady_clock::time_point g_lead_for{};
        glm::vec3                             g_lead{ 0.0f }, g_lead_place{ 0.0f };
        bool                                  g_lead_place_ok{ false };
    }

    glm::vec3 LeadNow(glm::vec3& place_out, bool& have_place)
    {
        // (fix157) What is placed at the frame's start (tracking::Snapshot: the left hand's weapon's copy, moved by the game's SetPosition at
        // once) is placed before your character moves this frame: CameraRootPlace then gives about last frame's place, and the copy was
        // drawn behind your hand by your speed times a frame (the lightsaber did not follow the hand correctly while moving, as
        // the laser sight at first; a helper's research: 12 cm at a run). The root's move over its last frame is taken as this frame's:
        // where the camera will put it, less where CameraRootPlace says it is now (the move already made). Zero: not in first person,
        // the last two places not a frame apart, or a jump.
        have_place       = false;
        const auto* root = static_cast<const RE::NiAVObject*>(g_watched_root.load(std::memory_order_acquire));
        if (root == nullptr) {
            return glm::vec3{ 0.0f };
        }
        const glm::vec3 place = CameraRootPlace(root);
        place_out             = place;
        have_place            = true;
        if (!GameFlow::isInFirstPerson()) {
            return glm::vec3{ 0.0f };
        }
        // (fix185) While climbing ties the view to you, CameraRootPlace gives the tied place: until this frame's move is asked (the left
        // hand's copies are placed before Climbing runs) the frame before's, so the frame before's step on top (the review); after, none.
        if (glm::vec3 tied{}; Climbing::ViewTie(tied) > 0.0f) {
            return Climbing::ViewTieLead();
        }
        const glm::vec3 at{ root->world.translate.x, root->world.translate.y, root->world.translate.z };
        std::scoped_lock _{ g_root_place_mutex };
        // (Not when the root has not moved for more than a frame: standing still, the last step would stay in force; nor when it is not
        // where it was last written (moved outside its update: CameraRootPlace then gives it as it is). The review.)
        // (fix169) "More than a frame" is now this frame's own length and a half (at least 70 ms, as before), and the last move may have
        // taken up to 250 ms: a slow frame at a run (the 24-29 fps of full stereo) had no guess at all, the saber a whole frame's move behind.
        using namespace std::chrono;
        const auto now        = steady_clock::now();
        const auto frame_gap  = g_start_at[0] - g_start_at[1];
        const auto prev_gap   = g_start_at[1] - g_start_at[2];
        const auto write_gap  = g_root_written_at - g_root_before_at;
        const bool starts_ok  = g_start_at[2] != steady_clock::time_point{} && frame_gap > milliseconds(4) && prev_gap > milliseconds(4) &&
                                frame_gap < milliseconds(250) && prev_gap < milliseconds(250);
        const auto still_after = (std::max)(duration_cast<steady_clock::duration>(milliseconds(70)), starts_ok ? frame_gap + frame_gap / 2 : steady_clock::duration{});
        if (!g_root_written_valid || !g_root_before_valid || write_gap > milliseconds(250) || now - g_root_written_at > still_after ||
            glm::length(at - g_root_written) > 1e-4f) {
            return glm::vec3{ 0.0f };
        }
        const glm::vec3 step = g_root_written - g_root_before;
        if (!(glm::length(step) < 1.0f)) {
            return glm::vec3{ 0.0f };
        }
        // (The three guesses made once a frame, from this root's place: checked when the root is written next.)
        if (g_guess_for != g_start_at[0] || glm::length(g_guess_base - g_root_written) > 1e-5f) {
            const auto ratio = [](steady_clock::duration a, steady_clock::duration b) {
                const double r = b.count() > 0 ? static_cast<double>(a.count()) / static_cast<double>(b.count()) : 1.0;
                return static_cast<float>(std::clamp(std::isfinite(r) ? r : 1.0, 0.33, 3.0));
            };
            g_guess[0]   = step;
            g_guess[1]   = step * (starts_ok ? ratio(frame_gap, prev_gap) : 1.0f);
            g_guess[2]   = step * (starts_ok && write_gap > milliseconds(4) ? ratio(frame_gap, write_gap) : 1.0f);
            g_guess_base = g_root_written;
            g_guess_for  = g_start_at[0];
            g_guess_ok   = true;
            ++g_guess_used[g_guess_use];
        }
        const glm::vec3 lead = g_root_written + g_guess[g_guess_use] - place;
        if (!(glm::length(lead) < 0.5f) || !std::isfinite(lead.x) || !std::isfinite(lead.y) || !std::isfinite(lead.z)) {
            return glm::vec3{ 0.0f };
        }
        return lead;
    }

    glm::vec3 FrameStartLead()
    {
        {
            std::scoped_lock _{ g_root_place_mutex };
            if (g_lead_for != std::chrono::steady_clock::time_point{} && g_lead_for == g_start_at[0]) {
                return g_lead; // (this frame's, as first worked out)
            }
        }
        glm::vec3       place{ 0.0f };
        bool            have_place = false;
        const glm::vec3 lead       = LeadNow(place, have_place);
        std::scoped_lock _{ g_root_place_mutex };
        g_lead_for      = g_start_at[0];
        g_lead          = lead;
        g_lead_place    = place;
        g_lead_place_ok = have_place;
        return lead;
    }

    bool FrameStartRoot(glm::vec3& out)
    {
        // (fix169) Where the camera root is taken to be for what is placed at the frame's start (CameraRootPlace and FrameStartLead, as
        // GrenadeHand's copies are placed: the same numbers, worked out once a frame): the body's left hand is put there too while it holds
        // the left weapon (body::NoteLeftWeaponRoot).
        FrameStartLead();
        std::scoped_lock _{ g_root_place_mutex };
        if (!g_lead_place_ok) {
            return false;
        }
        out = g_lead_place + g_lead;
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    bool HandRayWorld(glm::vec3& origin, glm::vec3& direction, bool left)
    {
        const glm::mat4 pose    = AimPose(left);
        const glm::vec3 forward = -glm::normalize(glm::vec3{ pose[2] });
        const glm::vec3 start   = glm::vec3{ pose[3] } + forward * 0.02f; // just in front of the controller
        return StageToWorld(start, forward, origin, direction);
    }

    bool GloveRayStage(int line, bool left, glm::vec3& origin, glm::vec3& direction)
    {
        const glm::mat4 aim     = AimPose(left);
        const glm::vec3 forward = glm::vec3{ aim[2] };
        if (!(glm::length(forward) > 1e-4f)) {
            return false;
        }
        const glm::vec3 pointing = -glm::normalize(forward);
        switch (line) {
        case 1: {
            // Out of the palm: the grip pose's +x leaves the palm of a left hand and the back of a right one (OpenXR), of the
            // physical controller (swapped controllers: the game's right hand is the physical left).
            static auto     vr        = VR::get();
            const glm::mat4 grip      = GripPose(left);
            const glm::vec3 x{ grip[0] };
            const bool      left_pose = (left ? vr->get_left_controller_index() : vr->get_right_controller_index()) == 1;
            if (!(glm::length(x) > 1e-4f)) {
                return false;
            }
            direction = glm::normalize(x) * (left_pose ? 1.0f : -1.0f);
            origin    = glm::vec3{ grip[3] } + direction * 0.03f; // the palm's surface
            break;
        }
        case 2: {
            // Sighting along the hand: from the eyes through the controller, starting at it.
            const glm::vec3 eyes{ HeadPose()[3] };
            const glm::vec3 hand{ aim[3] };
            const glm::vec3 d = hand - eyes;
            if (!(glm::length(d) > 0.05f)) {
                return false;
            }
            direction = glm::normalize(d);
            origin    = hand;
            break;
        }
        default:
            direction = pointing;
            origin    = glm::vec3{ aim[3] } + pointing * 0.02f; // just in front of the controller
            break;
        }
        return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) && std::isfinite(direction.x) &&
               std::isfinite(direction.y) && std::isfinite(direction.z);
    }

    bool GloveRayWorld(int line, bool left, glm::vec3& origin, glm::vec3& direction)
    {
        glm::vec3 o{}, d{};
        return GloveRayStage(line, left, o, d) && StageToWorld(o, d, origin, direction);
    }

    glm::mat4 GripPose(bool left)
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_grip_pose[left ? 0 : 1];
    }

    glm::mat4 RightHandPose()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_right_hand_pose;
    }

    glm::mat4 AimPose(bool left)
    {
        std::scoped_lock _{ g_aim_mutex };
        return left ? g_left_aim_pose : g_right_hand_pose;
    }

    glm::mat4 HeadPose()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_head_pose;
    }

    bool WorldToStage(const glm::vec3& world_point, glm::vec3& stage_point)
    {
        auto root   = CreationEngineSingletonManager::GetSceneGraphRoot();
        auto camera = root ? root->worldCamera : nullptr;
        const auto room = RoomRotation();
        if (!camera || !camera->parent || !room) {
            return false;
        }
        // StageToWorld: world = anchor + room * havok(stage - origin) * scale; so stage = origin + unhavok(room^T * (world - anchor) / scale).
        const auto& anchor = camera->parent->world;
        const float scale  = TrackingScale() * anchor.scale;
        if (!std::isfinite(scale) || scale < 1e-6f) {
            return false;
        }
        const glm::vec3 place = CameraRootPlace(camera->parent); // (fix147: this frame's)
        const glm::vec3 from = glm::vec3{ place.x, place.y, place.z + body::ViewHeightOffset() } + SeatView::WorldShift(); // (fix199)
        const glm::vec3 h = glm::transpose(*room) * (world_point - from) / scale;
        stage_point       = StandingOriginPosition() + glm::vec3{ h.x, h.z, -h.y };
        return std::isfinite(stage_point.x) && std::isfinite(stage_point.y) && std::isfinite(stage_point.z);
    }

    bool WorldToStageDirection(const glm::vec3& world_direction, glm::vec3& stage_direction)
    {
        const auto room = RoomRotation();
        if (!room) {
            return false;
        }
        // StageToWorld: world = room * havok(stage), havok(x, y, z) = (x, -z, y); so stage = (h.x, h.z, -h.y) of room^T * world.
        const glm::vec3 h = glm::transpose(*room) * world_direction;
        stage_direction   = glm::vec3{ h.x, h.z, -h.y };
        const float length = glm::length(stage_direction);
        if (!std::isfinite(length) || length < 1e-6f) {
            return false;
        }
        stage_direction /= length;
        return true;
    }

    namespace
    {
        // (fix195) The room's turn carried to the body's time (RoomRotationNow). Under g_room_mutex.
        // - g_room_heading_*: your character's heading the room was last worked out from (NoteRoomHeading), and the frame it was set in.
        // - g_room_guess: for the frame being placed, before the hook sets its room: 0 the frame before's room, 1 and 2 that room turned
        //   by your heading's change since one way and the other (which way the game's camera turns with the heading is measured, not
        //   assumed). Checked once a frame against the room the hook then sets, while it turns; the closest used, once clearly closer
        //   than the frame before's alone (half its miss, after 20 turning frames), else the frame before's (as before fix195).
        constexpr uintptr_t kRefHeading = 0x88; // TESObjectREFR::data.angle.z (the snap turn writes it)
        float     g_room_heading_ref{ 0.0f };
        bool      g_room_heading_ok{ false };
        uint32_t  g_room_set_frame{ 0 };
        glm::mat3 g_room_guess[3]{ glm::mat3{ 1.0f }, glm::mat3{ 1.0f }, glm::mat3{ 1.0f } };
        uint32_t  g_room_guess_frame{ 0 };
        float     g_room_guess_d{ 0.0f };    // the heading change the guesses were turned by (radians)
        uint32_t  g_room_checked_frame{ 0 };
        // (The review) Carried on only from the frame just before's room (a frame the hook skipped, a seat, a ladder, the flat screen: not),
        // and only for a stick turn's frame (under kRoomCarryMaxDeg): a snap turn is left to the next frame as before, since the body's
        // own place and its tracked joints still use the frame before's room, and the hands alone would stretch the arms for a frame.
        // A frame counts as turning in the check by its heading change (over kRoomTurningDeg), not by the rooms' difference.
        constexpr float kRoomCarryMaxDeg = 15.0f;
        constexpr float kRoomTurningDeg  = 0.1f;
        float     g_room_err_avg[3]{};
        int       g_room_turning{ 0 }; // turning frames checked, in all (the choice waits for 20)
        int       g_room_use{ 0 };
        double    g_room_err_sum[3]{};
        float     g_room_err_max[3]{};
        int       g_room_n{ 0 }, g_room_used[3]{};
        int       g_room_logs{ 0 };
        auto      g_room_log_at = std::chrono::steady_clock::now();
        constexpr const char* kRoomGuessNames[3] = { "the frame before's turn", "turned on against your heading's change",
            "turned on with your heading's change" };

        bool HeadingSafe(float& out)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (player == 0) {
                return false;
            }
            __try {
                std::memcpy(&out, reinterpret_cast<const void*>(player + kRefHeading), sizeof(out));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            return std::isfinite(out);
        }

        float WrapPi(float a)
        {
            const float two_pi = glm::two_pi<float>();
            a = std::fmod(a + glm::pi<float>(), two_pi);
            return (a < 0.0f ? a + two_pi : a) - glm::pi<float>();
        }

        // The turn between two rotations, degrees (from the quaternion of their difference: exact near 0, unlike acos of the trace).
        float TurnDeg(const glm::mat3& a, const glm::mat3& b)
        {
            const glm::quat q = glm::normalize(glm::quat_cast(a * glm::transpose(b)));
            return glm::degrees(2.0f * std::atan2(glm::length(glm::vec3{ q.x, q.y, q.z }), std::abs(q.w)));
        }

        // (Under g_room_mutex, as the hook sets this frame's room) The guesses made for this frame, checked; told every 10 s while turning.
        void CheckRoomGuesses(const glm::mat3& room, uint32_t frame)
        {
            if (g_room_guess_frame != frame || g_room_checked_frame == frame || !g_has_room) {
                return;
            }
            g_room_checked_frame = frame;
            float err[3]{};
            for (int k = 0; k < 3; ++k) {
                err[k] = TurnDeg(g_room_guess[k], room);
                if (!std::isfinite(err[k])) {
                    return;
                }
            }
            if (!(std::abs(g_room_guess_d) > glm::radians(kRoomTurningDeg)) || err[0] > 100.0f) {
                return; // (your heading did not change: the guesses are the same; or a jump, a load)
            }
            for (int k = 0; k < 3; ++k) {
                g_room_err_avg[k] = g_room_turning > 0 ? g_room_err_avg[k] + (err[k] - g_room_err_avg[k]) * 0.05f : err[k];
                g_room_err_sum[k] += err[k];
                g_room_err_max[k] = std::max(g_room_err_max[k], err[k]);
            }
            ++g_room_turning;
            ++g_room_n;
            ++g_room_used[g_room_use];
            if (g_room_turning >= 20) {
                int best = 1;
                if (g_room_err_avg[2] < g_room_err_avg[1]) {
                    best = 2;
                }
                if (g_room_use == 0 && g_room_err_avg[best] < 0.5f * g_room_err_avg[0]) {
                    g_room_use = best;
                } else if (g_room_use != 0 && g_room_err_avg[g_room_use] > 0.9f * g_room_err_avg[0]) {
                    g_room_use = 0;
                } else if (g_room_use != 0 && best != g_room_use && g_room_err_avg[best] < 0.9f * g_room_err_avg[g_room_use]) {
                    g_room_use = best;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - g_room_log_at > std::chrono::seconds(10)) {
                g_room_log_at = now;
                if (g_room_n > 0 && g_room_logs < 60) {
                    ++g_room_logs;
                    spdlog::info("[Tracking] (fix195) the room's turn while you turned ({} frames, the last 10 s): the body's hands placed with {} "
                                 "would be {:.2f} degrees off the view on average (at most {:.2f}); {} {:.2f} ({:.2f}); {} {:.2f} ({:.2f}); used: {} {} "
                                 "frames, {} {}, {} {}; now {}",
                        g_room_n, kRoomGuessNames[0], g_room_err_sum[0] / g_room_n, g_room_err_max[0], kRoomGuessNames[1], g_room_err_sum[1] / g_room_n,
                        g_room_err_max[1], kRoomGuessNames[2], g_room_err_sum[2] / g_room_n, g_room_err_max[2], kRoomGuessNames[0], g_room_used[0],
                        kRoomGuessNames[1], g_room_used[1], kRoomGuessNames[2], g_room_used[2], kRoomGuessNames[g_room_use]);
                }
                for (int k = 0; k < 3; ++k) {
                    g_room_err_sum[k] = 0.0;
                    g_room_err_max[k] = 0.0f;
                    g_room_used[k]    = 0;
                }
                g_room_n = 0;
            }
        }
    }

    void SetRoomRotation(const RE::NiQuaternion& q)
    {
        const glm::mat3  room  = glm::mat3_cast(glm::normalize(glm::quat{ q.w, q.x, q.y, q.z }));
        const uint32_t   frame = EngineFrameNow();
        std::scoped_lock _{ g_room_mutex };
        CheckRoomGuesses(room, frame); // (fix195)
        g_room_rotation = room;
        g_room_quaternion = q;
        g_has_room      = true;
        g_room_set_frame  = frame;
        g_room_heading_ok = false; // (fix195: until NoteRoomHeading; the ladder's view sets the room with no heading)
    }

    void NoteRoomHeading(float heading, bool carry)
    {
        std::scoped_lock _{ g_room_mutex };
        g_room_heading_ref = heading;
        g_room_heading_ok  = carry && std::isfinite(heading);
    }

    std::optional<glm::mat3> RoomRotationNow()
    {
        float          heading = 0.0f;
        const bool     have    = HeadingSafe(heading);
        const uint32_t frame   = EngineFrameNow();
        std::scoped_lock _{ g_room_mutex };
        if (!g_has_room) {
            return std::nullopt;
        }
        if (g_room_set_frame == frame || g_room_set_frame + 1 != frame || !g_room_heading_ok || !have) {
            return g_room_rotation; // (set already this frame, not set the frame just before, or nothing to carry it on with)
        }
        const float d = WrapPi(heading - g_room_heading_ref);
        if (!std::isfinite(d) || std::abs(d) > glm::radians(kRoomCarryMaxDeg)) {
            return g_room_rotation; // (a snap turn, or a jump: the frame before's, as before fix195)
        }
        const glm::vec3 up{ 0.0f, 0.0f, 1.0f };
        g_room_guess[0]    = g_room_rotation;
        g_room_guess[1]    = glm::mat3_cast(glm::angleAxis(-d, up)) * g_room_rotation;
        g_room_guess[2]    = glm::mat3_cast(glm::angleAxis(d, up)) * g_room_rotation;
        g_room_guess_frame = frame;
        g_room_guess_d     = d;
        return g_room_guess[g_room_use];
    }

    std::optional<glm::mat3> RoomRotation()
    {
        std::scoped_lock _{ g_room_mutex };
        return g_has_room ? std::optional<glm::mat3>{ g_room_rotation } : std::nullopt;
    }

    std::optional<RE::NiQuaternion> RoomQuaternion()
    {
        std::scoped_lock _{ g_room_mutex };
        return g_has_room ? std::optional<RE::NiQuaternion>{ g_room_quaternion } : std::nullopt;
    }

    // Our framework keeps the recentre point itself (not its inverse) as the standing origin.
    glm::vec3 StandingOriginPosition()
    {
        static auto vr = VR::get();
        return glm::vec3{ vr->get_standing_origin() };
    }

    float TrackingScale() { return GameFlow::gStore.internalSettings.matchBodyHeight ? g_tracking_scale.load() : 1.0f; }

    // Re-measured after each recenter from the eye height above the floor (OpenXR stage space).
    void UpdateTrackingScale(float character_eye_height)
    {
        // (fix146: kept for the view at the body's eyes on a ladder: standing only. A height as high as the kept one (within 1 cm, or higher) is
        // taken at once; a lower one only after it has stayed within 3 cm for 3 s (a crouch, its way down, a step, are not; the review).)
        {
            static float   lower_seen  = 0.0f;
            static int64_t lower_since = 0;
            const int64_t  now_ms      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (character_eye_height >= 1.3f && character_eye_height <= 2.5f) {
                if (character_eye_height >= g_character_eye_height.load() - 0.01f || !g_eye_height_known) {
                    g_character_eye_height = character_eye_height;
                    g_eye_height_known     = true;
                    lower_since            = 0;
                } else if (lower_since == 0 || std::abs(character_eye_height - lower_seen) > 0.03f) {
                    lower_seen  = character_eye_height;
                    lower_since = now_ms;
                } else if (now_ms - lower_since > 3000) {
                    g_character_eye_height = character_eye_height;
                    lower_since            = 0;
                }
            }
        }
        static glm::vec3 measured_origin{ -1000.0f };
        const auto       origin = StandingOriginPosition();
        if (glm::length(origin - measured_origin) < 1e-4f || character_eye_height < 0.5f || character_eye_height > 3.0f) {
            return;
        }
        static auto vr   = VR::get();
        const float real = vr->get_floor_eye_height();
        if (real < 0.0f) {
            return;  // floor height not known yet
        }
        // (fix189) Standing (your real height scales the hands and head): from your character's standing eye height kept above (a crouch
        // refused), not this frame's: a save loaded while sneaking measured about 1.0 m against your 1.7 m, and the scale clamped at 0.7
        // (hands and head moving 30% short) until the next recenter. So it waits until that height is known and this frame's is it (your
        // character standing, within 1 cm; it is then the same number); after 10 s still crouched, the kept one; never known (a character
        // too small or too large for it to be kept), this frame's, as before. Seated (not scaled) as before, at once.
        const bool       scaled     = real >= 1.3f && real <= 2.2f;
        static glm::vec3 wait_for{ -1000.0f };
        static int64_t   wait_since = 0;
        static bool      waited     = false;
        const int64_t    now_ms     = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (glm::length(origin - wait_for) >= 1e-4f) {
            wait_for   = origin;
            wait_since = now_ms;
            waited     = false;
        }
        const bool standing = g_eye_height_known.load() && character_eye_height >= g_character_eye_height.load() - 0.01f;
        if (scaled && !standing && now_ms - wait_since < 10000) {
            waited = true;
            return;
        }
        const bool  known     = g_eye_height_known.load();
        const float character = scaled && known ? g_character_eye_height.load() : character_eye_height;
        measured_origin       = origin;
        const float scale     = scaled ? std::clamp(character / real, 0.7f, 1.4f) : 1.0f;
        g_tracking_scale      = scale;
        spdlog::info("[Body] eye height: real {:.3f} m, character {:.3f} m -> tracking scale {:.3f}{}{}", real, character, scale,
            real < 1.3f ? " (seated, not scaled)" : "",
            waited ? std::format(" ((fix189) after {:.1f} s waiting for your character's standing height{})", (now_ms - wait_since) / 1000.0,
                         standing ? "" : known ? ": still crouched, the kept one taken" : ": not known, this frame's taken")
                   : std::string{});
    }

    float CharacterEyeHeight() { return g_character_eye_height.load(); }

    bool CharacterEyeHeightKnown() { return g_eye_height_known.load(); }

    glm::mat4 ToHavokSpace(const glm::mat4& mat)
    {
        return permutation_pre * mat * permutation_post;
    }

    glm::vec3 ToHavokVector(const glm::vec3& v)
    {
        return glm::vec3{ ToHavokSpace(glm::translate(glm::mat4{ 1.0f }, v))[3] };
    }

    glm::mat4 AppliedAimRotation()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_applied_aim_rotation;
    }

    glm::mat4 AimRotation()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_aim_rotation;
    }

    void RecordAppliedAim(const glm::mat4& rotation, int frame)
    {
        std::scoped_lock _{ g_aim_mutex };
        g_applied_aim_rotation = rotation;
        g_applied_aim_frame    = frame;
    }

    glm::mat4 CameraParentAimRotation(int frame)
    {
        std::scoped_lock _{ g_aim_mutex };
        return frame - g_applied_aim_frame <= 2 ? g_aim_rotation : g_applied_aim_rotation;
    }
}
