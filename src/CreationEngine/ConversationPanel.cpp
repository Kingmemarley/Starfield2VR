#include "ConversationPanel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include <ModSettings.h>
#include "CreationEngineCameraManager.h"
#include "CreationEngineSingletonManager.h"
#include "GrenadeHand.h"
#include "MeleeThrow.h"
#include "body/SceneGraph.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"
#include <mods/VR.hpp>

namespace ConversationPanel
{
    namespace
    {
        // The conversation movie's stage (DialogueMenu.swf: 1920 x 1080, filling its viewport). Its part on the panel (stage pixels): what
        // is said and the answers (the game's list at y 763-994, Enhanced Dialogue Interface's at 763-973, its line above them), with a
        // margin; the answers' box for the touch (as fix144's).
        constexpr float kStageW = 1920.0f;
        constexpr float kStageH = 1080.0f;
        constexpr float kCropX0 = 200.0f, kCropX1 = 1720.0f, kCropY0 = 500.0f, kCropY1 = 1060.0f;
        constexpr float kTouchX0 = 300.0f, kTouchX1 = 1620.0f, kTouchY0 = 700.0f, kTouchY1 = 1000.0f;
        // Its size in the room: 1.20 m for the stage's width at 0.45 m from your eyes (fix144's, its answers' rows then about 3 cm tall with
        // Enhanced Dialogue Interface's text at 115), as large in your view at any distance (fix147: it was too close to the player: the
        // distance a setting, ConversationDistance, 0.60 m by default); this far below the line to the speaker's face (their face above it,
        // the answers within reach), never above your eyes' level nor more than 40 degrees below it.
        constexpr float kMetresPerStagePxAt045 = 1.20f / kStageW;
        float           Distance() { return std::clamp(GameFlow::gStore.internalSettings.conversationDistance, 0.35f, 1.0f); }
        float           MetresPerStagePx() { return kMetresPerStagePxAt045 * Distance() / 0.45f; }
        constexpr float kBelowDeg         = 22.0f;
        constexpr float kHighestDeg       = 0.0f;
        constexpr float kLowestDeg        = -40.0f;
        // Moved (eased over about this long) only when: a new conversation (none placed in the last second); the speaker's face moved
        // this far from where it was when placed, or is this far off the panel's way for this long; your head moved this far.
        constexpr float   kEaseS        = 0.12f;
        constexpr int64_t kNewAfterMs   = 1000;
        constexpr float   kMovedM       = 0.5f;
        constexpr float   kOffDeg       = 25.0f;
        constexpr int64_t kOffForMs     = 1000;
        constexpr float   kHeadMovedM   = 0.35f;
        // No speaker found: the person nearest where you look, within this and this far.
        constexpr float kNearestDeg = 30.0f;
        constexpr float kNearestM   = 5.0f;
        // MenuTopicManager (Starfield 1.16.236: Address Library id 938421, its pointer at 0x1461EA0C0; its fields after its three type
        // tables, from +0x20: the speaker's handle first).
        constexpr uintptr_t kTopicManagerRva = 0x61EA0C0;
        constexpr uintptr_t kSpeakerHandle   = 0x20;
        // A reference: its form type (+0x2E: 0x4B an actor), as the throw reads it.
        constexpr uintptr_t kRefFormType = 0x2E;
        constexpr uint8_t   kActorType   = 0x4B;
        // The cut refused this many times in a row: the conversation left in the picture (fix144's way). Its panel not shown this long
        // into a conversation (and not for this long since): that conversation in the picture; this many conversations so: all of them.
        // ((fix189) Both times counted only while the headset draws, and only a conversation never shown counts toward all of them.)
        constexpr int     kRefusalsToStop  = 30;
        constexpr int64_t kShownWithinMs   = 800;
        constexpr int64_t kShownGapMs      = 600;
        constexpr int     kFallbacksToStop = 3;

        // (The render side: the movie's box, as last placed.)
        struct Placed
        {
            int     buffer[2]{ 0, 0 };
            int     rect[4]{ 0, 0, 0, 0 }; // left, top, width, height (the layer's pixels)
            int64_t ms{ 0 };
        };
        std::mutex       g_placed_mtx{};
        Placed           g_placed{};
        std::atomic<int> g_refusals{ 0 };
        std::atomic<bool> g_stopped{ false };
        std::atomic<uint32_t> g_cuts{ 0 };
        // (The render side: this conversation's start, its last placement, whether it went back in the picture.)
        std::atomic<int64_t> g_conv_start_ms{ 0 };
        std::atomic<int64_t> g_last_place_ms{ 0 };
        std::atomic<bool>    g_fallback{ false };
        std::atomic<int>     g_fallbacks{ 0 };
        // (The cut's window: placed within about two frames, from the time between placements.)
        std::atomic<int64_t> g_place_gap_ms{ 30 };
        // (fix189) Whether its panel is shown, judged only while the headset draws (as the build console's, BuildConsole): fix145's check
        // counted any 0.6 s with no frame shown, so a 0.6-1 s hitch or Virtual Desktop pausing in a conversation counted as a failure, and
        // three such turned the panel off until the game restarted (the review). Its clock: from the conversation's start, started again
        // whenever the headset does not draw or no placement came for 300 ms (a hitch); the placements while it drew since then; and since
        // the panel was last seen shown.
        constexpr int64_t kHitchMs     = 300;
        constexpr int     kShownPlaces = 24;
        struct ShownCheck
        {
            int64_t clock_start_ms{ 0 };
            int     places{ 0 };
            int64_t shown_seen_ms{ 0 };   // conv_shown_ms as last seen
            int64_t unshown_since_ms{ 0 }; // drawing and not shown since (started again with the clock)
            int     places_unshown{ 0 };
        };
        std::mutex g_shown_mtx{};
        ShownCheck g_shown{};

        // (The game's thread: where the panel is.)
        struct Panel
        {
            bool      placed{ false };
            glm::vec3 at{ 0.0f };          // where it is now (eased toward target)
            glm::quat turn{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 target_at{ 0.0f };
            glm::quat target_turn{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 head_at{ 0.0f };     // your eyes when it was placed
            glm::vec3 face_at{ 0.0f };     // the speaker's face when it was placed (stage)
            bool      had_face{ false };
            int64_t   off_since{ 0 };
            int64_t   last_ms{ 0 };
            int64_t   last_us{ 0 };
            uint32_t  speaker{ 0 };        // the person it is placed for (their handle; 0: none)
            int       moves{ 0 };
            float     room_yaw{ 0.0f };    // (fix173) the room's turn at the last update (radians, about the world's up)
            bool      have_room_yaw{ false };
        };
        Panel g_panel{};
        int   g_place_logs{ 0 };
        int   g_speaker_logs{ 0 };
        // (fix173) The room's turn with the panel turned along, for the log (once a second while it turns).
        float   g_room_turned{ 0.0f };
        int64_t g_room_turn_logged{ 0 };
        int     g_room_turn_logs{ 0 };

        // (fix173) The room's turn about the world's up (radians, counterclockwise seen from above): the way its forward (its second axis, as
        // StageToWorld maps the room's ahead) points, level. False: no room yet, or its forward straight up or down.
        bool RoomYaw(float& yaw)
        {
            const auto room = tracking::RoomRotation();
            if (!room) {
                return false;
            }
            const glm::vec2 ahead{ (*room)[1].x, (*room)[1].y };
            if (!(glm::length(ahead) > 0.1f)) {
                return false;
            }
            yaw = std::atan2(ahead.y, ahead.x);
            return std::isfinite(yaw);
        }

        // (fix173) The panel turned about the room's middle (the stage's up through where it was recentred, about which the room turns:
        // StageToWorld), by `angle` (radians, counterclockwise seen from above, in the stage's own axes).
        void TurnPanel(Panel& pl, float angle)
        {
            const glm::vec3 middle = tracking::StandingOriginPosition();
            const glm::quat r      = glm::angleAxis(angle, glm::vec3{ 0.0f, 1.0f, 0.0f });
            pl.at                  = middle + r * (pl.at - middle);
            pl.target_at           = middle + r * (pl.target_at - middle);
            pl.face_at             = middle + r * (pl.face_at - middle);
            pl.turn                = glm::normalize(r * pl.turn);
            pl.target_turn         = glm::normalize(r * pl.target_turn);
        }

        uintptr_t              g_base{ 0 };
        std::vector<uintptr_t> g_topic_tables{};
        std::string            g_topic_status{ "not looked for" };
        bool                   g_topic_checked{ false };
        bool                   g_topic_ok{ false };

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The cut wanted while the conversation was placed within about two frames (the review: not for long after it closed, the HUD there
        // cleared and shown on the panel).
        int64_t CutWindowMs() { return std::clamp<int64_t>(g_place_gap_ms.load() * 5 / 2, 50, 150); }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        // (fix190) A game place by its 1.16.236 rva, for this build's game version (GameCode.h); 0 when not known (SafeRead then fails).
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // (Not with a full-screen menu up, the menu panel's: the conversation is not shown then, and its box would be cleared out of that
        // menu, the review; as MenuPointer's conversation.)
        bool Wanted()
        {
            static auto vr = VR::get();
            if (!GameFlow::gStore.internalSettings.dialoguePanel || !GameFlow::gStore.internalSettings.dialogueTouch || g_stopped.load() || vr == nullptr ||
                !vr->is_hmd_active() || !vr->get_runtime()->is_openxr() || ModSettings::showFlatScreenDisplay() ||
                ModSettings::g_internalSettings.showQuadDisplay) {
                return false;
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            return xr != nullptr && !xr->panel_mode;
        }

        // The speaker's handle (0: none), the conversation manager checked by its type table the first time.
        uint32_t SpeakerHandle()
        {
            uintptr_t manager = 0;
            if (g_base == 0 || !SafeRead(&manager, At(kTopicManagerRva), 8) || manager == 0) {
                return 0;
            }
            if (!g_topic_checked) {
                g_topic_checked = true;
                uintptr_t table = 0;
                g_topic_ok = SafeRead(&table, manager, 8) && std::find(g_topic_tables.begin(), g_topic_tables.end(), table) != g_topic_tables.end();
                g_topic_status = g_topic_ok ? "the conversation manager found (its speaker)"
                                            : std::format("the conversation manager not as in Starfield 1.16.236 (its type table rva {:x}): the person "
                                                          "nearest your view instead",
                                                  table - g_base);
                spdlog::info("[Dialogue] {}", g_topic_status);
            }
            uint32_t handle = 0;
            return g_topic_ok && SafeRead(&handle, manager + kSpeakerHandle, 4) ? handle : 0;
        }

        // A node below (or at) a root, its parents read under a guard.
        bool IsUnder(RE::NiAVObject* node, RE::NiAVObject* root)
        {
            for (int i = 0; i < 40 && node != nullptr; ++i) {
                if (node == root) {
                    return true;
                }
                node = body::ParentOf(node);
            }
            return false;
        }

        // A node's world place (game world), read under a guard.
        bool WorldPlace(RE::NiAVObject* node, glm::vec3& out)
        {
            __try {
                const auto& t = node->world.translate;
                out           = glm::vec3{ t.x, t.y, t.z };
                return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A person's face (game world): their head bone (C_Head, a little up to the eyes), else their middle and a bit; false: not a
        // person, or the player.
        bool FaceOf(uint32_t handle, glm::vec3& face)
        {
            static uintptr_t       cached_root{ 0 };
            static RE::NiAVObject* cached_head{ nullptr };
            static int64_t         cached_none_ms{ 0 }; // (fix189) when cached_root was last searched and had no C_Head (0: it had one)
            auto*                  player = CreationEngineSingletonManager::GetPlayerRef();
            bool                   ok     = false;
            GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t root) {
                uint8_t type = 0;
                if (ref == 0 || ref == reinterpret_cast<uintptr_t>(player) || root == 0 || !SafeRead(&type, ref + kRefFormType, 1) || type != kActorType) {
                    return;
                }
                // (Found again for another person, or when it is no longer under their 3D: a skeleton made anew, the review.)
                // (fix189) A speaker with no C_Head (a robot, a creature): their whole skeleton searched again only once a second, not two or
                // three times a frame (the review); another person (another root) is searched at once.
                const int64_t now       = NowMs();
                const bool    same_root = root == cached_root;
                const bool    none_seen = same_root && cached_head == nullptr && cached_none_ms != 0 && now - cached_none_ms < 1000;
                if (!none_seen && (!same_root || !IsUnder(cached_head, reinterpret_cast<RE::NiAVObject*>(root)))) {
                    cached_root    = root;
                    cached_head    = body::FindDescendant(reinterpret_cast<RE::NiAVObject*>(root), "C_Head");
                    cached_none_ms = cached_head == nullptr ? now : 0;
                }
                glm::vec3 at{};
                if (cached_head != nullptr && WorldPlace(cached_head, at)) {
                    face = at + glm::vec3{ 0.0f, 0.0f, 0.08f };
                    ok   = true;
                    return;
                }
                float bound[4]{};
                if (SafeRead(bound, root + 0x100, sizeof(bound)) && std::isfinite(bound[0]) && bound[3] > 0.1f && bound[3] < 5.0f) {
                    face = glm::vec3{ bound[0], bound[1], bound[2] + 0.6f * bound[3] };
                    ok   = true;
                }
            });
            return ok;
        }

        // The person spoken with (their handle): the speaker, else the one placed for, else the person nearest where you look.
        uint32_t Speaker(const glm::vec3& head_world, const glm::vec3& look_world)
        {
            const uint32_t speaker = SpeakerHandle();
            glm::vec3      face{};
            if (speaker != 0 && FaceOf(speaker, face)) {
                return speaker;
            }
            if (g_panel.speaker != 0 && FaceOf(g_panel.speaker, face)) {
                return g_panel.speaker;
            }
            uint32_t best      = 0;
            float    best_cos  = std::cos(glm::radians(kNearestDeg));
            MeleeThrow::PeopleNearby([&](uint32_t handle, uintptr_t, const glm::vec3& middle) {
                const glm::vec3 to = middle - head_world;
                const float     d  = glm::length(to);
                if (d < 0.2f || d > kNearestM) {
                    return;
                }
                const float c = glm::dot(to / d, look_world);
                if (c > best_cos) {
                    best_cos = c;
                    best     = handle;
                }
            });
            return best;
        }

        // The panel's place for your eyes (stage) and a face (stage; or none: ahead, level): Distance() on the way to the face, kBelowDeg
        // below it (within kLowestDeg..kHighestDeg of level), upright, facing your eyes.
        void Aim(const glm::vec3& eyes, const glm::vec3& head_forward, const glm::vec3* face, glm::vec3& at, glm::quat& turn)
        {
            glm::vec3 way = face != nullptr ? *face - eyes : head_forward;
            glm::vec3 flat{ way.x, 0.0f, way.z };
            if (glm::length(flat) < 1e-3f) {
                flat = glm::vec3{ head_forward.x, 0.0f, head_forward.z };
            }
            flat               = glm::length(flat) > 1e-3f ? glm::normalize(flat) : glm::vec3{ 0.0f, 0.0f, -1.0f };
            const float up_deg = face != nullptr ? glm::degrees(std::atan2(way.y, glm::length(glm::vec2{ way.x, way.z }))) : 0.0f;
            const float pitch  = glm::radians(std::clamp(up_deg - kBelowDeg, kLowestDeg, kHighestDeg));
            at                 = eyes + (flat * std::cos(pitch) + glm::vec3{ 0.0f, 1.0f, 0.0f } * std::sin(pitch)) * Distance();
            const glm::vec3 z  = glm::normalize(eyes - at);
            glm::vec3       x  = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, z);
            x                  = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 y  = glm::cross(z, x);
            turn               = glm::normalize(glm::quat_cast(glm::mat3{ x, y, z }));
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (g_base != 0) {
            g_topic_tables = utility::rtti::find_vtables(MemoryScan::mod, ".?AVMenuTopicManager@@");
        }
        g_topic_status = g_topic_tables.empty() ? "the conversation manager's type not found: the person nearest your view instead"
                                                : std::format("the conversation manager's type found ({} tables)", g_topic_tables.size());
        spdlog::info("[Dialogue] conversations on a panel of their own in front of the person speaking (setting DialoguePanel); {}", g_topic_status);
    }

    bool PlaceMovie(Scaleform::Gfx::Viewport* viewport, int buffer_w, int buffer_h)
    {
        static auto vr = VR::get();
        if (viewport == nullptr || buffer_w <= 0 || buffer_h <= 0 || !Wanted()) {
            return false;
        }
        // (The review) Not shown on its panel a moment into the conversation (any failure on the way: the cut, its image, the runtime):
        // back in the picture for the rest of it (fix144's way); three conversations so, for the session. (fix189) Only a conversation whose
        // panel was never shown counts (0.8 s and two dozen placements in while the headset drew: the runtime refusing it, its image never
        // made); one shown that stops being shown for 0.6 s and two dozen placements while the headset draws goes back in the picture too,
        // not counted (the next conversation tries the panel again). A hitch (no placement for 0.3 s) or the headset not drawing
        // (shouldRender false: Virtual Desktop paused, the headset taken off) starts the clock again: none of these is a failure.
        const int64_t now      = NowMs();
        const int64_t previous = g_last_place_ms.exchange(now);
        if (now - previous > kNewAfterMs) {
            g_conv_start_ms = now;
            g_fallback      = false;
        } else if (now > previous) {
            g_place_gap_ms = (g_place_gap_ms.load() * 3 + (now - previous)) / 4;
        }
        if (g_fallback.load()) {
            return false;
        }
        if (const auto xr = vr->m_openxr; xr != nullptr) {
            const bool    drawing  = xr->frame_state.shouldRender == XR_TRUE;
            const int64_t start    = g_conv_start_ms.load();
            const int64_t shown_ms = xr->conv_shown_ms.load();
            bool          give_up  = false;
            bool          counted  = false;
            int64_t       drew_ms  = 0;
            int           places   = 0;
            {
                std::scoped_lock _{ g_shown_mtx };
                auto& s = g_shown;
                if (!drawing || now - previous > kHitchMs) {
                    s.clock_start_ms   = now; // (a new conversation too: its gap is over a second)
                    s.unshown_since_ms = now;
                    s.places           = 0;
                    s.places_unshown   = 0;
                    s.shown_seen_ms    = shown_ms;
                } else {
                    ++s.places;
                    if (shown_ms != s.shown_seen_ms) {
                        s.shown_seen_ms    = shown_ms;
                        s.unshown_since_ms = now;
                        s.places_unshown   = 0;
                    } else {
                        ++s.places_unshown;
                    }
                }
                drew_ms = now - s.clock_start_ms;
                places  = s.places;
                if (drawing && shown_ms < start && drew_ms > kShownWithinMs && s.places > kShownPlaces) {
                    give_up = counted = true;
                } else if (drawing && shown_ms >= start && now - s.unshown_since_ms > kShownGapMs && s.places_unshown > kShownPlaces) {
                    give_up = true;
                }
            }
            if (give_up) {
                g_fallback  = true;
                const int n = counted ? ++g_fallbacks : g_fallbacks.load();
                spdlog::warn("[Dialogue] the conversation's panel {} (cut {} times so far, {} placements while the headset drew): this conversation in "
                             "the picture instead{}",
                    counted ? std::format("never shown {} ms into the conversation ({} ms while the headset drew)", now - start, drew_ms)
                            : std::string{ "shown, then not for 0.6 s while the headset drew (fix189: not counted toward stopping)" },
                    g_cuts.load(), places, counted && n >= kFallbacksToStop ? "; from now on for every conversation" : "");
                if (counted && n >= kFallbacksToStop) {
                    g_stopped = true;
                }
                return false;
            }
        }
        // Its 16:9 box, as wide as the layer (as tall at most), in the middle.
        int width  = buffer_w;
        int height = static_cast<int>(std::lround(static_cast<float>(buffer_w) * kStageH / kStageW));
        if (height > buffer_h) {
            height = buffer_h;
            width  = static_cast<int>(std::lround(static_cast<float>(buffer_h) * kStageW / kStageH));
        }
        const int left = (buffer_w - width) / 2;
        const int top  = (buffer_h - height) / 2;
        viewport->left   = left;
        viewport->top    = top;
        viewport->width  = width;
        viewport->height = height;
        {
            std::scoped_lock _{ g_placed_mtx };
            g_placed.buffer[0] = buffer_w;
            g_placed.buffer[1] = buffer_h;
            g_placed.rect[0]   = left;
            g_placed.rect[1]   = top;
            g_placed.rect[2]   = width;
            g_placed.rect[3]   = height;
            g_placed.ms        = NowMs();
        }
        // The answers' box for the touch (over the picture, 0..1).
        if (const auto xr = vr->m_openxr; xr != nullptr) {
            const float s   = static_cast<float>(width) / kStageW;
            const float W   = static_cast<float>(buffer_w);
            const float H   = static_cast<float>(buffer_h);
            auto&       box = xr->hud_touch_box;
            box[0]          = (left + kTouchX0 * s) / W;
            box[1]          = (top + kTouchY0 * s) / H;
            box[2]          = (left + kTouchX1 * s) / W;
            box[3]          = (top + kTouchY1 * s) / H;
        }
        return true;
    }

    bool CutWanted()
    {
        int64_t ms = 0;
        {
            std::scoped_lock _{ g_placed_mtx };
            ms = g_placed.ms;
        }
        return ms != 0 && NowMs() - ms <= CutWindowMs() && Wanted();
    }

    bool CutBoxes(uint32_t layer_w, uint32_t layer_h, D3D12_BOX& copy, D3D12_BOX& clear)
    {
        Placed p{};
        {
            std::scoped_lock _{ g_placed_mtx };
            p = g_placed;
        }
        if (p.ms == 0 || NowMs() - p.ms > CutWindowMs() || !Wanted()) {
            return false;
        }
        if (static_cast<uint32_t>(p.buffer[0]) != layer_w || static_cast<uint32_t>(p.buffer[1]) != layer_h) {
            static int told = 0;
            if (told++ < 5) {
                spdlog::info("[Dialogue] the interface layer is {}x{}, the conversation was placed for {}x{}: not cut this time", layer_w, layer_h,
                    p.buffer[0], p.buffer[1]);
            }
            return false;
        }
        const float s     = static_cast<float>(p.rect[2]) / kStageW;
        const auto  clamp = [](float v, uint32_t hi) { return static_cast<UINT>(std::clamp(std::lround(v), 0L, static_cast<long>(hi))); };
        copy.left   = clamp(p.rect[0] + kCropX0 * s, layer_w);
        copy.right  = clamp(p.rect[0] + kCropX1 * s, layer_w);
        copy.top    = clamp(p.rect[1] + kCropY0 * s, layer_h);
        copy.bottom = clamp(p.rect[1] + kCropY1 * s, layer_h);
        copy.front  = 0;
        copy.back   = 1;
        clear.left   = clamp(static_cast<float>(p.rect[0]), layer_w);
        clear.right  = clamp(static_cast<float>(p.rect[0] + p.rect[2]), layer_w);
        clear.top    = clamp(static_cast<float>(p.rect[1]), layer_h);
        clear.bottom = clamp(static_cast<float>(p.rect[1] + p.rect[3]), layer_h);
        clear.front  = 0;
        clear.back   = 1;
        return copy.right > copy.left && copy.bottom > copy.top && clear.right > clear.left && clear.bottom > clear.top;
    }

    bool MovieBox(int picture_w, int picture_h, int& width, int& height)
    {
        Placed p{};
        {
            std::scoped_lock _{ g_placed_mtx };
            p = g_placed;
        }
        // (Placed lately for a picture of that size, and centred in it as the pointer takes a menu's box to be.)
        if (p.ms == 0 || NowMs() - p.ms > 300 || g_fallback.load() || p.buffer[0] != picture_w || p.buffer[1] != picture_h || p.rect[2] <= 0 ||
            p.rect[3] <= 0 || std::abs(2 * p.rect[0] + p.rect[2] - picture_w) > 1 || std::abs(2 * p.rect[1] + p.rect[3] - picture_h) > 1) {
            return false;
        }
        width  = p.rect[2];
        height = p.rect[3];
        return true;
    }

    void NoteCut(int result)
    {
        if (result > 0) {
            g_refusals = 0;
            g_cuts.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (result < 0 && ++g_refusals >= kRefusalsToStop && !g_stopped.exchange(true)) {
            spdlog::warn("[Dialogue] the conversation could not be cut out of the interface layer {} times: it stays in the picture (fix144's way) "
                         "until the game restarts",
                kRefusalsToStop);
        }
    }

    void Update()
    {
        static auto   vr  = VR::get();
        const int64_t now = NowMs();
        Placed        p{};
        {
            std::scoped_lock _{ g_placed_mtx };
            p = g_placed;
        }
        const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
        if (!Wanted() || xr == nullptr || p.ms == 0 || now - p.ms > 200 || p.buffer[0] <= 0 || p.buffer[1] <= 0 || p.rect[2] <= 0) {
            return;
        }
        // Your eyes and the way your head faces (stage, y up).
        const glm::mat4 head    = tracking::HeadPose();
        const glm::vec3 eyes{ head[3] };
        const glm::vec3 forward = -glm::normalize(glm::vec3{ head[2] });
        if (!std::isfinite(eyes.x) || !std::isfinite(forward.x)) {
            return;
        }
        // The person spoken with, and their face (stage).
        glm::vec3 eyes_world{}, look_world{};
        uint32_t  speaker  = 0;
        glm::vec3 face{};
        bool      has_face = false;
        {
            glm::vec3 look_dir{};
            if (tracking::StageToWorld(eyes, forward, eyes_world, look_dir)) {
                look_world = glm::length(look_dir) > 1e-4f ? glm::normalize(look_dir) : glm::vec3{ 0.0f, 1.0f, 0.0f };
                speaker    = Speaker(eyes_world, look_world);
                glm::vec3 face_world{};
                has_face = speaker != 0 && FaceOf(speaker, face_world) && tracking::WorldToStage(face_world, face);
            }
        }
        if (speaker != 0 && speaker != g_panel.speaker && g_speaker_logs < 20) {
            ++g_speaker_logs;
            spdlog::info("[Dialogue] speaking with {:#x} ({}){}", speaker, speaker == SpeakerHandle() ? "the speaker" : "the person nearest your view",
                has_face ? std::format(", their face {:.2f} m from your eyes", glm::length(face - eyes)) : std::string{ ", their face not found" });
        }
        auto&         pl  = g_panel;
        const int64_t nus = NowUs();
        const float   dt  = pl.last_us != 0 ? std::clamp(static_cast<float>(nus - pl.last_us) * 1e-6f, 0.0f, 0.1f) : 0.0f;
        pl.last_us        = nus;
        // Placed anew, or moved: (new) a new conversation; (moved) the face far from where it was or off the panel's way a while, your head
        // moved.
        const bool fresh  = !pl.placed || now - pl.last_ms > kNewAfterMs;
        // (fix173) The room turned since the last update (the right stick turns you in a conversation now: the camera hook): the panel turned
        // with it about the room's middle, so it stays where it was in the world, in front of the person (fixed in the room, it would slide off
        // them as the world turns round you; it is meant to stay locked in front of the npc, fix145). Its face and place as placed turn
        // too, so "the person moved" and "off it a while" still compare like with like; your head's place does not (that is your own move).
        // (The room's turn is put by the camera hook later in the frame: the panel follows it a frame behind, under 2 degrees while turning.)
        {
            float      room_yaw  = 0.0f;
            const bool have_room = RoomYaw(room_yaw);
            if (!fresh && have_room && pl.have_room_yaw) {
                const float two_pi = 2.0f * glm::pi<float>();
                float       d      = std::fmod(room_yaw - pl.room_yaw + 3.0f * glm::pi<float>(), two_pi);
                d                  = (d < 0.0f ? d + two_pi : d) - glm::pi<float>();
                if (std::isfinite(d) && std::abs(d) > 1e-5f) {
                    TurnPanel(pl, -d); // (the room turned left, counterclockwise: what stays in the world turns right in the room)
                    g_room_turned += d;
                }
            }
            pl.room_yaw      = room_yaw;
            pl.have_room_yaw = have_room;
            if (std::abs(g_room_turned) > 1e-4f && now - g_room_turn_logged >= 1000 && g_room_turn_logs < 30) {
                ++g_room_turn_logs;
                g_room_turn_logged = now;
                spdlog::info("[Dialogue] (fix173) the room turned {:.1f} degrees in the conversation: its panel turned with it, kept in front of the person",
                    -glm::degrees(g_room_turned)); // (as the stick turns: positive to the right)
                g_room_turned = 0.0f;
            }
        }
        bool       move   = false;
        const char* why   = "";
        if (!fresh) {
            if (has_face && pl.had_face && glm::length(face - pl.face_at) > kMovedM) {
                move = true;
                why  = "the person moved";
            } else if (has_face != pl.had_face && has_face) {
                move = true;
                why  = "the person found";
            } else if (glm::length(eyes - pl.head_at) > kHeadMovedM) {
                move = true;
                why  = "your head moved";
            } else if (has_face) {
                const glm::vec3 to_face  = face - eyes;
                const glm::vec3 to_panel = pl.target_at - eyes;
                const glm::vec2 a{ to_face.x, to_face.z }, b{ to_panel.x, to_panel.z };
                const float     off = glm::length(a) > 1e-3f && glm::length(b) > 1e-3f
                                          ? glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)))
                                          : 0.0f;
                if (off > kOffDeg) {
                    if (pl.off_since == 0) {
                        pl.off_since = now;
                    } else if (now - pl.off_since > kOffForMs) {
                        move = true;
                        why  = "the person was off it a while";
                    }
                } else {
                    pl.off_since = 0;
                }
            }
        }
        if (fresh || move) {
            Aim(eyes, forward, has_face ? &face : nullptr, pl.target_at, pl.target_turn);
            pl.head_at   = eyes;
            pl.face_at   = face;
            pl.had_face  = has_face;
            pl.speaker   = speaker;
            pl.off_since = 0;
            if (fresh) {
                pl.at   = pl.target_at;
                pl.turn = pl.target_turn;
            }
            ++pl.moves;
            if (g_place_logs < 30) {
                ++g_place_logs;
                const glm::vec3 d = pl.target_at - eyes;
                spdlog::info("[Dialogue] the conversation's panel {} {:.2f} m from your eyes, {:.0f} degrees below level{} ({})", fresh ? "put" : "moved",
                    glm::length(d), -glm::degrees(std::asin(std::clamp(d.y / std::max(glm::length(d), 1e-4f), -1.0f, 1.0f))),
                    has_face ? std::format(", on the way to the person's face ({:.2f} m away)", glm::length(face - eyes)) : std::string{ ", ahead (no one found)" },
                    fresh ? "a new conversation" : why);
            }
        }
        pl.placed  = true;
        pl.last_ms = now;
        // Eased toward its place (put at once when new).
        const float k = 1.0f - std::exp(-dt / kEaseS);
        pl.at += (pl.target_at - pl.at) * k;
        pl.turn = glm::normalize(glm::slerp(pl.turn, pl.target_turn, k));
        // Given to the headset: its pose, size, and the part of the picture it shows.
        const float s  = static_cast<float>(p.rect[2]) / kStageW;
        const float W  = static_cast<float>(p.buffer[0]);
        const float H  = static_cast<float>(p.buffer[1]);
        runtimes::OpenXR::Conversation c{};
        c.pose.orientation = { pl.turn.x, pl.turn.y, pl.turn.z, pl.turn.w };
        c.pose.position    = { pl.at.x, pl.at.y, pl.at.z };
        c.size[0]          = (kCropX1 - kCropX0) * MetresPerStagePx();
        c.size[1]          = (kCropY1 - kCropY0) * MetresPerStagePx();
        c.crop[0]          = (p.rect[0] + kCropX0 * s) / W;
        c.crop[1]          = (p.rect[1] + kCropY0 * s) / H;
        c.crop[2]          = (p.rect[0] + kCropX1 * s) / W;
        c.crop[3]          = (p.rect[1] + kCropY1 * s) / H;
        c.ms               = now;
        xr->set_conversation(c);
    }

    std::string Status()
    {
        return std::format("Conversations on their own panel: {}; {}; cut {} times", g_stopped.load() ? "stopped (the cut was refused)" : "on",
            g_topic_status, g_cuts.load());
    }
}
