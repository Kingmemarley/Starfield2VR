#include "LadderClimb.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include "CreationEngineSingletonManager.h"
#include "NiMath.h"
#include "Haptics.h"
#include "WorkbenchCamera.h"
#include "body/BodyIK.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"
#include <mods/VR.hpp>
#include <RE/N/NiCamera.h>
#include <RE/N/NiQuaternion.h>

namespace LadderClimb
{
    namespace
    {
        // The player's activate (Starfield 1.16.236), its ladder branch: the reference is a ladder (0x140B37290), the player not in third
        // person already ([player+0x112A] bit 8 clear), then
        //   mov edx, 0x14                    the third-person state's index
        //   mov rcx, [PlayerCamera]          (0x1461E51F0)
        //   call SetState                    (0x141E8C2C0)
        // and on at 0x141A390D1 (the activation queued, the climb tried in it). (fix145) Only noted now, the switch left to the game: its
        // climb is the third-person body's (fix144's test: with the switch skipped, 20 tries and no climb).
        constexpr uintptr_t kSwitchRva = 0x1A390C0;
        constexpr uint8_t   kSwitch[]  = { 0xBA, 0x14, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0x24, 0xC1, 0x7A, 0x04, 0xE8, 0xEF, 0x31, 0x45, 0x00 };
        // (fix145) The third-person camera state (ThirdPersonState: its type table at 0x144D09BB8). Each frame the player camera puts its
        // root (the view's parent) where its current state says: the state's function 13 where it looks (0x141E9C5C0: its quaternion at
        // +0x170), its function 14 where it is (0x141E9C5D0: its point at +0x190). Both answered here while the view is at the head.
        constexpr uintptr_t kThirdPersonTableRva = 0x4D09BB8;
        constexpr int       kTurnSlot            = 13;
        constexpr int       kPlaceSlot           = 14;
        constexpr uintptr_t kTurnRva             = 0x1E9C5C0;
        constexpr uint8_t   kTurnBytes[]         = { 0xC5, 0xF8, 0x10, 0x81, 0x70, 0x01, 0x00, 0x00, 0xC5, 0xF8, 0x11, 0x02, 0xC3 };
        constexpr uintptr_t kPlaceRva            = 0x1E9C5D0;
        constexpr uint8_t   kPlaceBytes[]        = { 0xC5, 0xFB, 0x10, 0x81, 0x90, 0x01, 0x00, 0x00, 0xC5, 0xFB, 0x11, 0x02,
                                                     0x8B, 0x81, 0x98, 0x01, 0x00, 0x00, 0x89, 0x42, 0x08, 0xC3 };
        // (fix145) The camera put back to the state it was in before (0x141E91BD0, as the game does as a climb ends, 0x141A34247: only
        // from the third-person state with one remembered: SetState keeps the one it leaves). For a climb that did not start: the game
        // would leave the camera in third person for good.
        constexpr uintptr_t kRestoreRva    = 0x1E91BD0;
        constexpr uint8_t   kRestoreBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0xB8, 0x00, 0x00, 0x00, 0x00 };
        // The player camera (its address stored at 0x1461E51F0): its states at +0x188 + index x 8 (20 the third-person one).
        // (fix147) The third-person state's update (its function 12, 0x141E97F40) works its camera's place out itself (an orbit about the
        // body) and does not ask its function 14 for it (fix146's test: ladders worked but they were not in first person): it writes the
        // camera root's turn (from its function 13's quaternion: the room's, HookedTurn) and place (r15+0x70..0x7B, 0x141E98E1B; r15 the
        // root, [[state+0x48]+8]), then has the root update itself (its function 73, NiNode's 0x142BD6510), which first works out its own
        // world place (function 79, 0x142BD9330, the world update CreationEngineCameraManager hooks for every node) and then its
        // children's, the world camera among them.
        // (fix148) fix147 wrote the root's place again inside that update (a hook at 0x141E98E2B), and the code there was not as in the
        // game's file when the mod started (its test: "not as in Starfield 1.16.236", the file's bytes the expected ones: something had
        // changed it in memory first). Now the place is written as the root works out its world place (BeforeNodeUpdate, from the world
        // update hook already there), the root known from the turn this frame: nothing hooked in the third-person update's code. Its bytes
        // there are logged (for whoever changed them).
        constexpr uintptr_t kStateRoot        = 0x48; // [[state+0x48]+8]: the camera root the third-person update writes
        constexpr int64_t   kRootFreshUs      = 100'000;
        constexpr uintptr_t kRootWritesRva    = 0x1E98E1B;
        constexpr uint8_t   kRootWritesBytes[] = { 0xC4, 0xC1, 0x7B, 0x11, 0x47, 0x70, 0x8B, 0x85, 0xB8, 0x00, 0x00, 0x00,
                                                   0x41, 0x89, 0x47, 0x78, 0xC5, 0xFA, 0x10, 0x85, 0xBC, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kCameraRva   = 0x61E51F0;
        constexpr uintptr_t kStates      = 0x188;
        constexpr int       kThirdPerson = 20;
        // (fix145) From the ladder's activation: the view at the head this long while the climb is to start (the game turns to third person
        // just before), and the camera put back if none has started by then (and if one ended and left it in third person).
        constexpr int64_t kStartUs = 2'500'000; // (the review: the activation is queued, the mount can take a moment)
        // (fix146) The eyes from the body's root (its feet, the character's place: fix145's head bone was never found): your character's eye
        // height up (as measured in first person, tracking::CharacterEyeHeight: 1.67 m in fix145's test) and this far ahead along its facing;
        // the place eased (this time constant) so the climb's steps do not jolt the view.
        constexpr float kEyeAhead  = 0.08f;
        constexpr float kEyeEaseS  = 0.05f;
        // (fix148) The eyes from the head bone (C_Head, at the top of the neck): this far up and ahead along the body's facing (the face is
        // hidden, so a little ahead rather than inside the head).
        constexpr float kHeadToEyesUp    = 0.08f;
        constexpr float kHeadToEyesAhead = 0.11f;
        // The hands' pull: a squeezed grip (past this) moving down against the head faster than this climbs up, up climbs down; the
        // command held this long after the hand stops (between pulls), so the climb does not stutter.
        constexpr float   kGripOn      = 0.7f;
        constexpr float   kPullSpeed   = 0.15f; // m/s
        constexpr int64_t kHoldUs      = 180'000;

        using TurnFn  = void (*)(void* state, float* quaternion_out);
        using PlaceFn = void (*)(void* state, float* point_out);
        using RestoreFn = void (*)(uintptr_t camera);

        uintptr_t             g_base = 0;
        safetyhook::MidHook   g_hook{};
        // (fix148) The camera root the third-person update writes this frame (from its turn, HookedTurn), while the view is at the eyes.
        std::atomic<void*>    g_root_target{ nullptr };
        std::atomic<void*>    g_root_state{ nullptr };
        std::atomic<int64_t>  g_root_target_us{ 0 };
        std::atomic<uint32_t> g_root_writes{ 0 };
        std::atomic<uint32_t> g_root_seen{ 0 };      // (the root through the node update hook while wanted)
        std::atomic<uint32_t> g_root_published{ 0 }; // (frames the turn noted it)
        std::atomic<int>      g_root_far_logs{ 0 };
        int                   g_turn_logs{ 0 };
        std::string           g_installed{ "not installed" };
        std::string           g_view_installed{ "not installed" };
        TurnFn                g_turn_original{ nullptr };
        PlaceFn               g_place_original{ nullptr };
        RestoreFn             g_restore{ nullptr };
        std::atomic<uint32_t> g_activations{ 0 };
        std::atomic<int64_t>  g_pending_us{ 0 };   // (fix145) the ladder activated then (0: not waiting for a climb)
        std::atomic<int64_t>  g_ladder_us{ 0 };    // (fix145) the camera last in third person for a ladder then (0: not)
        std::atomic<uint32_t> g_view_frames{ 0 };
        std::atomic<uint32_t> g_restores{ 0 };
        std::atomic<int>      g_stick{ 0 };
        bool                  g_was_on{ false };
        bool                  g_was_view{ false };
        int64_t               g_on_us{ 0 };
        int64_t               g_command_until{ 0 };
        int                   g_command{ 0 };
        float                 g_last_rel[2]{ 0.0f, 0.0f };
        int64_t               g_last_us{ 0 };
        bool                  g_have_last{ false };
        uint32_t              g_climbs{ 0 }, g_pulls{ 0 };
        int                   g_logs{ 0 };
        // (The present place's easing: the game's thread, the camera's update.)
        glm::vec3             g_eye{ 0.0f };
        bool                  g_eye_valid{ false };
        int64_t               g_eye_us{ 0 };
        int                   g_eye_logs{ 0 };
        bool                  g_eye_from_head{ false };
        int                   g_eye_source_logs{ 0 };
        std::atomic<bool>     g_head_wanted{ false }; // (fix148) the head bone wanted this frame (BodyIK reads it then)

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool BytesAre(uintptr_t at, const uint8_t* bytes, size_t n)
        {
            __try {
                return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadBytes(uintptr_t at, uint8_t* out, size_t n)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadPtr(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        uintptr_t Camera()
        {
            uintptr_t camera = 0;
            return g_base != 0 && ReadPtr(At(kCameraRva), camera) ? camera : 0;
        }

        // The view at the head wanted for this state now: the player camera's third-person state, for a ladder (climbing, or activated a
        // moment ago), in the headset (setting LadderFirstPerson).
        bool ViewWantedFor(const void* state)
        {
            static auto vr = VR::get();
            if (!GameFlow::gStore.internalSettings.ladderFirstPerson || vr == nullptr || !vr->is_hmd_active() || ModSettings::cameraFlat() ||
                ModSettings::g_debug.disableHeadTracking) {
                return false;
            }
            const uintptr_t camera = Camera();
            uintptr_t       third  = 0;
            if (camera == 0 || !ReadPtr(camera + kStates + kThirdPerson * 8, third) || third == 0 || third != reinterpret_cast<uintptr_t>(state)) {
                return false;
            }
            // (Until the camera is put back or the climb is over: never a frame of the game's orbit view between, the review.)
            return GameFlow::isOnLadder() || g_ladder_us.load() != 0;
        }

        // Where the eyes are now: the body's head bone moved to the eyes, eased; false without the body's head (the game's own place
        // then).
        bool EyeAtHead(glm::vec3& eye)
        {
            float feet[3]{}, forward[3]{};
            if (!body::ReadRootWorld(feet, forward)) {
                static int told = 0;
                if (told++ < 3) {
                    spdlog::warn("[Ladder] the body's place could not be read: the view left where the game puts it");
                }
                return false;
            }
            glm::vec3 f{ forward[0], forward[1], 0.0f };
            f = glm::length(f) > 1e-3f ? glm::normalize(f) : glm::vec3{ 0.0f };
            const glm::vec3 estimate = glm::vec3{ feet[0], feet[1], feet[2] } + glm::vec3{ 0.0f, 0.0f, tracking::CharacterEyeHeight() } + f * kEyeAhead;
            // (fix148) The body's head bone as the climb's animation put it this frame (BodyIK, from the skeleton's update): fix148's test had
            // the view inside the body, the estimate from the feet half a metre below the game's camera while the animation lifts and leans
            // the body. The eyes a little above and ahead of it (its face hidden). The estimate only without it.
            glm::vec3 want = estimate;
            float     head[3]{};
            const bool from_head = body::LadderHeadWorld(head);
            if (from_head) {
                want = glm::vec3{ head[0], head[1], head[2] } + glm::vec3{ 0.0f, 0.0f, kHeadToEyesUp } + f * kHeadToEyesAhead;
            }
            if (from_head != g_eye_from_head || g_eye_source_logs == 0) {
                if (g_eye_source_logs < 12) {
                    ++g_eye_source_logs;
                    spdlog::info("[Ladder] the eyes from {}: {:.2f},{:.2f},{:.2f} (the estimate from the feet {:.2f},{:.2f},{:.2f}, {:.2f} m apart)",
                        from_head ? "the body's head bone (C_Head)" : "the feet and your eye height (no head bone this frame)", want.x, want.y, want.z,
                        estimate.x, estimate.y, estimate.z, glm::length(want - estimate));
                }
                g_eye_from_head = from_head;
            }
            if (!std::isfinite(want.x) || !std::isfinite(want.y) || !std::isfinite(want.z)) {
                return false;
            }
            const int64_t now = NowUs();
            if (!g_eye_valid || glm::length(want - g_eye) > 1.0f) {
                g_eye = want;
            } else {
                const float dt = std::clamp(static_cast<float>(now - g_eye_us) * 1e-6f, 0.0f, 0.1f);
                g_eye += (want - g_eye) * (1.0f - std::exp(-dt / kEyeEaseS));
            }
            g_eye_us    = now;
            g_eye_valid = true;
            eye         = g_eye;
            return true;
        }

        // ThirdPersonState's function 13 (where it looks): while the view is at the head, the room's turn as it was in first person (the
        // headset's own turn is put on the view as in play): the view does not swing as the climb turns the body to the ladder.
        void HookedTurn(void* state, float* quaternion_out)
        {
            g_turn_original(state, quaternion_out);
            if (!ViewWantedFor(state)) {
                g_root_target.store(nullptr, std::memory_order_relaxed);
                return;
            }
            g_view_frames.fetch_add(1, std::memory_order_relaxed); // (the room held: counted for the first-person camera's return)
            if (const auto room = tracking::RoomQuaternion()) {
                auto* q = reinterpret_cast<RE::NiQuaternion*>(quaternion_out);
                *q      = *room;
            }
            // (fix148) The root it writes next (and then has update itself): its place put at the eyes then (BeforeNodeUpdate).
            uintptr_t holder = 0, root = 0;
            if (ReadPtr(reinterpret_cast<uintptr_t>(state) + kStateRoot, holder) && holder != 0 && ReadPtr(holder + 8, root) && root != 0) {
                g_root_state.store(state, std::memory_order_relaxed);
                g_root_target_us.store(NowUs(), std::memory_order_relaxed);
                g_root_target.store(reinterpret_cast<void*>(root), std::memory_order_release);
                // (A second of frames with it noted and it never through the node update hook: told once; the review.)
                static bool told = false;
                if (g_root_published.fetch_add(1) == 60 && g_root_seen.load() == 0 && !told) {
                    told = true;
                    spdlog::warn("[Ladder] the camera root ({:#x}) never came through the node update hook in 60 frames: the view left where the "
                                 "game puts it",
                        root);
                }
            }
        }

        // ThirdPersonState's function 14 (where it is): while the view is at the head, the eyes of the body (the headset's own move is put
        // on the view as in play).
        void HookedPlace(void* state, float* point_out)
        {
            g_place_original(state, point_out);
            if (!ViewWantedFor(state)) {
                g_eye_valid = false;
                return;
            }
            glm::vec3 eye{};
            if (!EyeAtHead(eye)) {
                return;
            }
            // (Far from the game's own camera place (10 m): not trusted, the game's kept; the review.)
            if (glm::length(eye - glm::vec3{ point_out[0], point_out[1], point_out[2] }) > 10.0f) {
                return;
            }
            if (g_eye_logs < 12) {
                ++g_eye_logs;
                spdlog::info("[Ladder] the view at the body's head: the third-person camera's place {:.2f},{:.2f},{:.2f} -> the eyes {:.2f},{:.2f},{:.2f} "
                             "({:.2f} m moved)",
                    point_out[0], point_out[1], point_out[2], eye.x, eye.y, eye.z,
                    glm::length(eye - glm::vec3{ point_out[0], point_out[1], point_out[2] }));
            }
            point_out[0] = eye.x;
            point_out[1] = eye.y;
            point_out[2] = eye.z;
        }

        // (fix148) The camera root works out its world place (the world update hook, before the game's): while the view is at the eyes, its
        // place is the body's eyes (its turn already the room's: HookedTurn; the headset's pose goes on top, as in play). Its children, the
        // world camera among them, follow it.
        void PlaceRoot(RE::NiAVObject* root)
        {
            // (The root is the player camera's own, first person's too: only while the camera is in its third-person state; the review.)
            if (NowUs() - g_root_target_us.load(std::memory_order_relaxed) > kRootFreshUs ||
                WorkbenchCamera::CameraStateIndex() != kThirdPerson || !ViewWantedFor(g_root_state.load(std::memory_order_relaxed))) {
                g_root_target.store(nullptr, std::memory_order_relaxed);
                g_eye_valid = false; // (the next climb's view starts at its eyes, not eased from this one's; the review)
                return;
            }
            g_root_seen.fetch_add(1, std::memory_order_relaxed);
            glm::vec3 eye{};
            if (!EyeAtHead(eye)) {
                return;
            }
            // (Its local place in its parent's, as the update reads it: the parent's turn, place and scale taken off.)
            glm::vec3 local = eye;
            if (const auto* parent = root->parent; parent != nullptr) {
                const float scale = parent->world.scale > 1e-4f ? parent->world.scale : 1.0f;
                local = glm::transpose(NiMath::to_glm(parent->world.rotate)) * (eye - NiMath::to_glm(parent->world.translate)) / scale;
            }
            const glm::vec3 was = NiMath::to_glm(root->local.translate);
            if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || glm::length(local - was) > 10.0f) {
                // (Far from the game's own place: not trusted. Told the first few times: the view then stays where the game puts it.)
                if (g_root_far_logs.fetch_add(1) < 5) {
                    spdlog::warn("[Ladder] the body's eyes {:.2f},{:.2f},{:.2f} too far from the camera root's place {:.2f},{:.2f},{:.2f}: the "
                                 "game's kept",
                        local.x, local.y, local.z, was.x, was.y, was.z);
                }
                return;
            }
            if (g_root_writes.fetch_add(1) < 12) {
                auto       scene        = CreationEngineSingletonManager::GetSceneGraphRoot();
                const bool camera_child = scene && scene->worldCamera != nullptr && scene->worldCamera->parent == root;
                spdlog::info("[Ladder] the view at the body's eyes: the camera root's place {:.2f},{:.2f},{:.2f} -> {:.2f},{:.2f},{:.2f} ({:.2f} m "
                             "moved; the world camera {})",
                    was.x, was.y, was.z, local.x, local.y, local.z, glm::length(local - was),
                    camera_child ? "is its child" : "is not its child: the view may not follow");
            }
            root->local.translate.x = local.x;
            root->local.translate.y = local.y;
            root->local.translate.z = local.z;
            // (fix148) Its turn too: with a target ([state+0x200], a ladder's) the update makes its own, looking at it from above
            // (0x141E987A9: a look-at basis), and the turn HookedTurn hands it is not used (fix148's test: the view looked down). The room's turn,
            // level as in first person (the headset's turn goes on top), in its parent's axes.
            // (The glm matrix the hands are placed with, RoomRotation: the same convention as the rest of the mod whichever way NiMath reads
            // the game's matrices; the review.)
            if (const auto room = tracking::RoomRotation()) {
                glm::mat3 turn = *room;
                if (const auto* parent = root->parent; parent != nullptr) {
                    turn = glm::transpose(NiMath::to_glm(parent->world.rotate)) * turn;
                }
                const bool finite = std::isfinite(turn[0][0]) && std::isfinite(turn[1][1]) && std::isfinite(turn[2][2]);
                if (finite) {
                    if (g_turn_logs < 3) {
                        ++g_turn_logs;
                        const glm::mat3 was_turn = NiMath::to_glm(root->local.rotate);
                        const float     apart    = glm::degrees(std::acos(std::clamp((glm::dot(was_turn[0], turn[0]) + glm::dot(was_turn[1], turn[1]) +
                                                                                     glm::dot(was_turn[2], turn[2]) - 1.0f) * 0.5f, -1.0f, 1.0f)));
                        spdlog::info("[Ladder] the camera root's turn the room's (level): {:.1f} degrees from the game's own", apart);
                    }
                    NiMath::from_glm(turn, root->local.rotate);
                }
            }
        }

        // (fix150) The view's turn on a ladder (after fix149 the ladder's view was not reset as a climb started, so
        // whichever way you looked while climbing it stayed stuck that way). The climb turns the body to face the ladder (39 to
        // 151 degrees in fix149's test), and the room's turn was held as it was in first person: the view stayed turned from the ladder,
        // and the right stick went to the game's third-person camera, which the view does not use. Now, as the view goes to the head, the
        // room turns with the body to face the ladder (eased, for kFaceS: looking straight ahead in your seat then faces it), and then
        // holds (your head looks around); the right stick turns it (GameFlow::pendingLadderYaw, snap or smooth as on foot), which also
        // ends the facing. The room's turn as the ladder's view began kept, for first person's return (no jump: the camera manager).
        constexpr int64_t kFaceUs   = 1'500'000;
        constexpr float   kFaceEaseS = 0.10f;
        std::mutex        g_room_start_mtx;
        RE::NiQuaternion  g_room_start{};
        bool              g_room_start_valid{ false };
        bool              g_turn_was_at_head{ false };
        int64_t           g_face_until_us{ 0 };
        int64_t           g_turn_last_us{ 0 };
        float             g_faced{ 0.0f };      // (turned to face the ladder this time, for the log)
        float             g_stick_turned{ 0.0f };
        int               g_face_logs{ 0 };

        void TurnRoom(bool at_head, int64_t now)
        {
            if (at_head && !g_turn_was_at_head) {
                const auto room = tracking::RoomQuaternion();
                {
                    std::scoped_lock _{ g_room_start_mtx };
                    g_room_start_valid = room.has_value();
                    if (room) {
                        g_room_start = *room;
                    }
                }
                g_face_until_us = now + kFaceUs;
                g_turn_last_us  = now;
                g_faced         = 0.0f;
                g_stick_turned  = 0.0f;
                GameFlow::pendingLadderYaw.store(0.0f); // (nothing from before the ladder)
            }
            if (!at_head) {
                if (g_turn_was_at_head && g_face_logs < 20 && (std::abs(g_faced) > 1e-3f || std::abs(g_stick_turned) > 1e-3f)) {
                    ++g_face_logs;
                    spdlog::info("[Ladder] (fix150) on the ladder the view turned {:.1f} degrees to face it, and {:.1f} by the right stick",
                        glm::degrees(g_faced), glm::degrees(g_stick_turned));
                }
                g_turn_was_at_head = false;
                return;
            }
            g_turn_was_at_head = true;
            const auto room = tracking::RoomRotation();
            if (!room) {
                return;
            }
            const float dt  = std::clamp(static_cast<float>(now - g_turn_last_us) * 1e-6f, 0.0f, 0.1f);
            g_turn_last_us  = now;
            float turn      = 0.0f; // (about the world's up, counterclockwise seen from above)
            // The right stick's (its turns are to the right: clockwise). It ends the facing: the turn is yours from then on.
            if (const float stick = GameFlow::pendingLadderYaw.exchange(0.0f); stick != 0.0f && std::isfinite(stick)) {
                turn -= stick;
                g_stick_turned += stick;
                g_face_until_us = 0;
            }
            // Facing the ladder: the room's forward (where straight ahead in your seat looks: its second axis, as StageToWorld maps the
            // room's ahead) eased to the body's.
            float feet[3]{}, forward[3]{};
            if (now < g_face_until_us && body::ReadRootWorld(feet, forward)) {
                const glm::vec2 body_way{ forward[0], forward[1] };
                const glm::vec2 room_way{ (*room)[1].x, (*room)[1].y };
                if (glm::length(body_way) > 0.1f && glm::length(room_way) > 0.1f) {
                    const float delta = std::atan2(room_way.x * body_way.y - room_way.y * body_way.x, glm::dot(room_way, body_way));
                    if (std::isfinite(delta) && std::abs(delta) > glm::radians(0.3f)) {
                        const float step = delta * (1.0f - std::exp(-dt / kFaceEaseS));
                        turn += step;
                        g_faced += step;
                    }
                }
            }
            if (turn == 0.0f) {
                return;
            }
            const float     c = std::cos(turn), s = std::sin(turn);
            const glm::mat3 about_up{ glm::vec3{ c, s, 0.0f }, glm::vec3{ -s, c, 0.0f }, glm::vec3{ 0.0f, 0.0f, 1.0f } };
            const glm::quat q = glm::normalize(glm::quat_cast(about_up * *room));
            if (!std::isfinite(q.w) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
                return;
            }
            tracking::SetRoomRotation(RE::NiQuaternion{ q.w, q.x, q.y, q.z });
        }

        std::string HexAt(uintptr_t at, size_t n)
        {
            uint8_t bytes[32]{};
            n = std::min(n, sizeof(bytes));
            if (!ReadBytes(at, bytes, n)) {
                return "(unreadable)";
            }
            std::string s{};
            for (size_t i = 0; i < n; ++i) {
                s += std::format("{:02X} ", bytes[i]);
            }
            return s;
        }

        // A ladder activated: the game is about to turn to its third-person camera; noted (the view goes to the body's head with it).
        void OnSwitch(safetyhook::Context& ctx)
        {
            (void)ctx;
            static auto vr    = VR::get();
            const int   state = WorkbenchCamera::CameraStateIndex();
            const bool  view  = GameFlow::gStore.internalSettings.ladderFirstPerson && vr != nullptr && vr->is_hmd_active();
            if (view) {
                const int64_t now = NowUs();
                g_pending_us      = now;
                g_ladder_us       = now;
            }
            if (g_activations.fetch_add(1) < 20) {
                spdlog::info("[Ladder] a ladder activated: the game turns to its third-person camera for the climb (the camera now in state {}){}", state,
                    view ? "; the view goes to the body's head" : "");
            }
        }

        // A function slot of a type table changed to ours, the game's own there checked first.
        bool Patch(uintptr_t table, int slot, uintptr_t expected, const uint8_t* bytes, size_t n, void* ours, uintptr_t& original)
        {
            auto*     at = reinterpret_cast<uintptr_t*>(table) + slot;
            uintptr_t fn = 0;
            if (!ReadPtr(reinterpret_cast<uintptr_t>(at), fn) || fn != expected || !BytesAre(fn, bytes, n)) {
                return false;
            }
            DWORD old = 0;
            if (!VirtualProtect(at, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                return false;
            }
            original = fn;
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(at), static_cast<LONG64>(reinterpret_cast<uintptr_t>(ours)));
            VirtualProtect(at, sizeof(uintptr_t), old, &old);
            return true;
        }

        void InstallView()
        {
            // (fix190) The player camera and the third-person state's type table, read without a check of their own: known in this game
            // version first (every use of the camera below comes after this).
            if (!GameCode::Known({ kCameraRva, kThirdPersonTableRva })) {
                g_view_installed = "the player camera or its third-person state is not known in this game version (GameCode): off, ladders left in third person";
                spdlog::warn("[Ladder] {}", g_view_installed);
                return;
            }
            const uintptr_t table = At(kThirdPersonTableRva);
            if (!SameCode(kRestoreRva, kRestoreBytes, sizeof(kRestoreBytes))) {
                g_view_installed = "the camera's put-back is not as in Starfield 1.16.236: ladders left in third person";
                spdlog::warn("[Ladder] {}", g_view_installed);
                return;
            }
            uintptr_t turn = 0, place = 0;
            // (Both slots checked before either is changed: Patch checks its own again.)
            uintptr_t now_turn = 0, now_place = 0;
            if (!ReadPtr(table + kTurnSlot * 8, now_turn) || now_turn != At(kTurnRva) || !SameCode(kTurnRva, kTurnBytes, sizeof(kTurnBytes)) ||
                !ReadPtr(table + kPlaceSlot * 8, now_place) || now_place != At(kPlaceRva) || !SameCode(kPlaceRva, kPlaceBytes, sizeof(kPlaceBytes))) {
                g_view_installed = "the third-person camera's type table is not as in Starfield 1.16.236: ladders left in third person";
                spdlog::warn("[Ladder] {}", g_view_installed);
                return;
            }
            // (Each original set before its slot is changed: a call in between would find none, the review. It is the one checked there.)
            g_turn_original = reinterpret_cast<TurnFn>(At(kTurnRva));
            if (!Patch(table, kTurnSlot, At(kTurnRva), kTurnBytes, sizeof(kTurnBytes), reinterpret_cast<void*>(&HookedTurn), turn)) {
                g_turn_original = nullptr;
                g_view_installed = "the third-person camera's turn could not be hooked: ladders left in third person";
                spdlog::warn("[Ladder] {}", g_view_installed);
                return;
            }
            g_place_original = reinterpret_cast<PlaceFn>(At(kPlaceRva));
            if (!Patch(table, kPlaceSlot, At(kPlaceRva), kPlaceBytes, sizeof(kPlaceBytes), reinterpret_cast<void*>(&HookedPlace), place)) {
                g_place_original = nullptr;
                g_view_installed = "the third-person camera's place could not be hooked: ladders left in third person (its turn kept as the game's)";
                spdlog::warn("[Ladder] {}", g_view_installed);
                // (The turn alone does nothing wrong, but the view would be the game's orbit camera turned as in first person: put back.)
                auto*     at  = reinterpret_cast<uintptr_t*>(table) + kTurnSlot;
                DWORD     old = 0;
                if (VirtualProtect(at, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(at), static_cast<LONG64>(turn));
                    VirtualProtect(at, sizeof(uintptr_t), old, &old);
                }
                return;
            }
            g_restore        = reinterpret_cast<RestoreFn>(At(kRestoreRva));
            g_view_installed = "hooked";
            spdlog::info("[Ladder] on a ladder the view is at the body's head: the third-person camera's turn and place (type table rva {:x}, functions "
                         "{} and {}) answered while climbing; a climb that does not start puts the camera back (rva {:x})",
                kThirdPersonTableRva, kTurnSlot, kPlaceSlot, kRestoreRva);
            // (fix148) Its update's own write of the camera root (it does not ask function 14): the place put as the root works out its
            // world place (BeforeNodeUpdate); the code of that write only logged, as found, when it differs from the file (fix147's check
            // failed there).
            // (fix190: only when the place is known in this game version: its bytes are read for the log. fix147 also compared a
            // "quaternion-to-matrix" at 0x1440002E0, but that is in the game's data (.rdata), not its code, so it never matched and the
            // line said "differs" at every start: no longer compared.)
            const bool known       = GameCode::Known({ kRootWritesRva });
            const bool writes_same = SameCode(kRootWritesRva, kRootWritesBytes, sizeof(kRootWritesBytes));
            if (known && !writes_same) {
                spdlog::info("[Ladder] the third-person update's code differs from the game's file (changed in memory: another mod, or a hook): its "
                             "root write (rva {:x}): {} (not needed now)",
                    kRootWritesRva, HexAt(At(kRootWritesRva), sizeof(kRootWritesBytes)));
            }
            spdlog::info("[Ladder] on a ladder the camera root put at the body's eyes as it works out its world place (the node update hook), turned "
                         "as the room");
        }
    }

    void BeforeNodeUpdate(RE::NiAVObject* obj)
    {
        if (obj != nullptr && obj == g_root_target.load(std::memory_order_acquire)) {
            PlaceRoot(obj);
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (g_base == 0 || !SameCode(kSwitchRva, kSwitch, sizeof(kSwitch))) {
            g_installed = "the ladder's camera switch is not as in Starfield 1.16.236: left as the game has it";
            spdlog::warn("[Ladder] {} (rva {:x})", g_installed, kSwitchRva);
            return;
        }
        g_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kSwitchRva)), &OnSwitch);
        if (!g_hook) {
            g_installed = "could not hook the ladder's camera switch: left as the game has it";
            spdlog::warn("[Ladder] {}", g_installed);
            return;
        }
        g_installed = "hooked";
        spdlog::info("[Ladder] ladders: the game's switch to its third-person camera (rva {:x}) noted, the climb left to the game; climbing with "
                     "the hands (setting LadderFirstPerson)",
            kSwitchRva);
        InstallView();
    }

    bool ViewAtHead()
    {
        if (g_place_original == nullptr) {
            return false;
        }
        const uintptr_t camera = Camera();
        uintptr_t       third  = 0;
        return camera != 0 && ReadPtr(camera + kStates + kThirdPerson * 8, third) && third != 0 && ViewWantedFor(reinterpret_cast<const void*>(third)) &&
               WorkbenchCamera::CameraStateIndex() == kThirdPerson;
    }

    uint32_t ViewFrames() { return g_view_frames.load(); }

    bool HeadWanted() { return g_head_wanted.load(std::memory_order_relaxed); }

    bool RoomAtLadderStart(RE::NiQuaternion& out)
    {
        std::scoped_lock _{ g_room_start_mtx };
        if (!g_room_start_valid) {
            return false;
        }
        out = g_room_start;
        return true;
    }

    bool ActivatedRecently()
    {
        const int64_t pending = g_pending_us.load();
        return pending != 0 && NowUs() - pending < 1'500'000;
    }

    void Update()
    {
        static auto   vr  = VR::get();
        const int64_t now = NowUs();
        // (fix148) The head bone wanted while the view is at the head (read by BodyIK as the skeleton updates: one flag for every model).
        const bool at_head = ViewAtHead();
        g_head_wanted.store(at_head, std::memory_order_relaxed);
        TurnRoom(at_head, now);
        const bool    on  = GameFlow::isOnLadder();
        const int     state = WorkbenchCamera::CameraStateIndex();
        if (on != g_was_on) {
            g_was_on = on;
            const int64_t pending = g_pending_us.load();
            if (on) {
                g_on_us = now;
                ++g_climbs;
                g_pending_us = 0; // (started)
            }
            if (g_logs < 40) {
                ++g_logs;
                spdlog::info("[Ladder] {} (the camera in state {}{})", on ? "climbing a ladder" : "off the ladder", state,
                    on ? (pending != 0 ? std::format("; {:.2f} s after the ladder was activated", (now - pending) * 1e-6) : std::string{})
                       : std::format("; {:.1f} s on it, {} pulls", (now - g_on_us) * 1e-6, g_pulls));
            }
            g_pulls     = 0;
            g_command   = 0;
            g_have_last = false;
        }
        if (on) {
            g_ladder_us = now;
        } else if (g_pending_us.load() == 0 && state != kThirdPerson && state >= 0) {
            g_ladder_us = 0; // (a climb over and the camera back: done)
        }
        // (fix145) The view at the head begun or ended (for the log).
        const bool view = ViewAtHead();
        if (view != g_was_view) {
            g_was_view = view;
            if (g_logs < 40) {
                ++g_logs;
                spdlog::info("[Ladder] the view {} the body's head (the camera in state {}; {} frames placed there so far)", view ? "at" : "back from", state,
                    g_view_frames.load());
            }
        }
        // (fix145) No climb 2.5 s after the activation (or one ended and left the camera in third person): the camera put back to first
        // person as the game does at a climb's end.
        if (const int64_t since = g_ladder_us.load(); since != 0 && !on && now - since > kStartUs) {
            if (state == kThirdPerson && g_restore != nullptr && GameFlow::gStore.internalSettings.ladderFirstPerson) {
                if (const uintptr_t pc = Camera(); pc != 0) {
                    const bool started = g_pending_us.load() == 0;
                    g_restore(pc);
                    const int after = WorkbenchCamera::CameraStateIndex();
                    if (g_restores.fetch_add(1) < 20) {
                        spdlog::info("[Ladder] {}: the camera put back as the game does at a climb's end (now in state {})",
                            started ? "the climb ended with the camera left in third person" : "no climb 2.5 s after the ladder was activated",
                            after);
                    }
                }
            }
            g_ladder_us  = 0;
            g_pending_us = 0;
        }
        if (!on || !GameFlow::gStore.internalSettings.ladderFirstPerson || vr == nullptr || !vr->is_hmd_active() || !vr->is_using_controllers()) {
            g_stick = 0;
            return;
        }
        // Each hand against the head, up and down (stage space: y up): a squeezed grip moving down climbs up, up climbs down.
        const glm::mat4 head = tracking::HeadPose();
        const float     dt   = g_have_last ? static_cast<float>(now - g_last_us) * 1e-6f : 0.0f;
        int             want      = 0;
        int             pull_hand = 0;
        float           fastest   = 0.0f;
        for (int hand = 0; hand < 2; ++hand) {
            const float rel = tracking::GripPose(hand == 0)[3].y - head[3].y;
            if (g_have_last && dt > 0.002f && dt < 0.2f && std::isfinite(rel)) {
                const int   index    = hand == 0 ? vr->get_left_controller_index() : vr->get_right_controller_index();
                const bool  gripping = vr->get_finger_state(index == 1).grip > kGripOn;
                const float speed    = (rel - g_last_rel[hand]) / dt;
                if (gripping && std::abs(speed) > kPullSpeed && std::abs(speed) > std::abs(fastest)) {
                    fastest   = speed;
                    want      = speed < 0.0f ? 1 : -1;
                    pull_hand = hand;
                }
            }
            g_last_rel[hand] = rel;
        }
        g_last_us   = now;
        g_have_last = true;
        if (want != 0) {
            if (want != g_command) {
                ++g_pulls;
                haptics::MeleePulse(pull_hand == 0, 0.2f, 0.03f);
            }
            g_command       = want;
            g_command_until = now + kHoldUs;
        } else if (now > g_command_until) {
            g_command = 0;
        }
        g_stick = g_command;
    }

    int StickY() { return g_stick.load(); }

    std::string Status()
    {
        return std::format("{}; the view at the head: {}; ladders activated {}, climbs {}, the camera put back {} times", g_installed, g_view_installed,
            g_activations.load(), g_climbs, g_restores.load());
    }
}
