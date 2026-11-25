#include "MessagesBlock.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <shared_mutex>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include "CreationEngineCameraManager.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include <mods/VR.hpp>

namespace MessagesBlock
{
    namespace
    {
        constexpr float kStageW = 1920.0f;
        constexpr float kStageH = 1080.0f;
        // Their text: 18 stage units this many degrees tall (chosen: about 0.8 to 0.9); not made smaller than the game's, nor more
        // than this much larger.
        constexpr float kTextDeg  = 0.85f;
        constexpr float kMaxScale = 2.2f;
        // The block's top this many degrees below your view's middle (clear of the build console's warnings, 5 below, and the action cards).
        constexpr float kTopDeg = 8.0f;
        // Between the two columns (stage units, after scaling).
        constexpr float kColumnGap = 20.0f;
        // The parts' own sizes in the movie (HUDMessagesMenu.gfx as loaded: scratchpad buildmenu_rs\out\hudmessagesmenu.layout.txt): a
        // mission update's drawing from 59 to 440 across (and from 59 down) of the point it is placed by; a notification 390 wide, right-aligned
        // to Messages_mc's place; the tutorial tip (unnamed: it moves with the menu clip) from 695 to 1144 across.
        constexpr float kQuestLeft = 59.0f, kQuestRight = 440.0f, kQuestTop = 59.0f;
        constexpr float kMessageWidth = 390.0f;
        constexpr float kTipMiddle = 919.5f, kTipTop = -28.0f;
        constexpr float kFlyoutWidth = 447.0f;

        std::atomic<void*>   g_movie{ nullptr };
        std::atomic<void*>   g_movie_def{ nullptr }; // ((the review) its definition: another movie made at the same address has another)
        std::atomic<int>     g_vp_w{ 0 }, g_vp_h{ 0 }, g_buf_w{ 0 };
        std::atomic<int64_t> g_vp_ms{ 0 };

        std::mutex g_mtx{};
        Plan       g_applied_plan{};
        bool       g_applied{ false };
        int64_t    g_applied_ms{ 0 };

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool DefOf(void* movie, void*& def)
        {
            __try {
                def = reinterpret_cast<RE::Scaleform::GFx::MovieImpl*>(movie)->GetMovieDef();
                return def != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool UrlOfDef(void* def, const char*& url)
        {
            __try {
                url = reinterpret_cast<RE::Scaleform::GFx::MovieDef*>(def)->GetFileURL();
                return url != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    void Install()
    {
        spdlog::info("[Messages] (fix178) the game's messages in one place below your view's middle, bigger (setting MessagesBlock): notifications "
                     "and mission updates in two columns from {:.0f} degrees below, their text about {:.2f} degrees",
            kTopDeg, kTextDeg);
    }

    void NoteViewport(void* movie, const Scaleform::Gfx::Viewport* viewport)
    {
        if (movie == nullptr || viewport == nullptr) {
            return;
        }
        // (The messages' movie: known by its address and its definition (the review: another movie made where it was has another one); a
        // new pair looked at by its file.)
        void* def = nullptr;
        if (!DefOf(movie, def)) {
            return;
        }
        if (movie != g_movie.load(std::memory_order_relaxed) || def != g_movie_def.load(std::memory_order_relaxed)) {
            const char* url = nullptr;
            if (!UrlOfDef(def, url) || url == nullptr) {
                return;
            }
            const uint32_t hash = djb2Hash(url);
            if (hash != "Interface/HUDMessagesMenu.gfx"_DJB) {
                if (movie == g_movie.load(std::memory_order_relaxed)) {
                    g_movie     = nullptr; // (that address another movie's now)
                    g_movie_def = nullptr;
                }
                return;
            }
            g_movie     = movie;
            g_movie_def = def;
        }
        g_vp_w  = viewport->width;
        g_vp_h  = viewport->height;
        g_buf_w = viewport->bufferWidth;
        g_vp_ms = NowMs();
    }

    bool Wanted()
    {
        static auto vr = VR::get();
        if (!GameFlow::gStore.internalSettings.messagesBlock || vr == nullptr || !vr->is_hmd_active() || ModSettings::showFlatScreenDisplay()) {
            return false;
        }
        const int w = g_vp_w.load(), h = g_vp_h.load();
        // (Its box lately, taller than 4:3: the HUD's in play, or the build console's, the same shape. A menu's 16:9 box: not.)
        return NowMs() - g_vp_ms.load() < 500 && w > 0 && h > 0 && static_cast<float>(h) > 0.75f * static_cast<float>(w);
    }

    Plan Current()
    {
        static auto vr = VR::get();
        Plan        p{};
        const int   w = g_vp_w.load(), h = g_vp_h.load();
        const auto& bb = vr->get_backbuffer_size();
        if (w <= 0 || h <= 0 || bb[0] == 0) {
            return p;
        }
        // The HUD's own size in view: its box's share of the eye picture's width, across the eye's view (tangents), per stage unit.
        Vector4f f{};
        {
            std::shared_lock _{ vr->get_runtime()->eyes_mtx };
            f = vr->get_runtime()->eye_frustums[1];
        }
        const float across = f[1] > f[0] ? f[1] - f[0] : 2.75f;
        const float share  = std::clamp(static_cast<float>(w) / static_cast<float>(bb[0]), 0.1f, 1.0f);
        p.deg_per_unit     = glm::degrees(std::atan(share * across / kStageW));
        if (!(p.deg_per_unit > 0.005f)) {
            return p;
        }
        p.scale = std::clamp(kTextDeg / (18.0f * p.deg_per_unit), 1.0f, kMaxScale);
        const float s = p.scale;
        // The two columns side by side, centred: the mission updates' (pinned left at the margin) and the notifications' (pinned right).
        p.safe_x       = std::max(0.0f, (kStageW - kColumnGap - (kQuestRight + kMessageWidth) * s) * 0.5f);
        p.left_col[0]  = p.safe_x + kQuestLeft * s;
        p.left_col[1]  = p.safe_x + kQuestRight * s;
        p.right_col[1] = kStageW - p.safe_x;
        p.right_col[0] = p.right_col[1] - kMessageWidth * s;
        // (Shown whole in its box, as wide as the stage: the part of the stage shown runs below 1080 in a box taller than 16:9.)
        p.shown_bottom = kStageH * 0.5f + 0.5f * kStageW * static_cast<float>(h) / static_cast<float>(w);
        p.shown_bottom = std::max(p.shown_bottom, kStageH);
        // (The review: at a very small HUD scale 8 degrees is far down the stage: never lower than 300 units above the box's bottom.)
        p.top = std::min(kStageH * 0.5f + kTopDeg / p.deg_per_unit, p.shown_bottom - 300.0f);
        // The menu clip: its 0,0 (where the mission updates start) so their drawing's top is at the block's top; across, so the tutorial tip
        // (moved with it) sits over the notifications' column.
        p.origin[1] = p.top - kQuestTop * s;
        p.origin[0] = (p.right_col[0] + p.right_col[1]) * 0.5f - kTipMiddle * s;
        // The block for the build console's crop: from the tutorial tip's top to above the subtitles (pinned at the bottom middle, up to about
        // 230 units above the box's bottom), across both columns and the flyout.
        p.region[0] = std::max(0.0f, p.left_col[0] - 12.0f);
        p.region[1] = std::max(0.0f, p.origin[1] + kTipTop * s - 12.0f);
        p.region[2] = std::min(kStageW, std::max(p.right_col[1], p.right_col[0] + kFlyoutWidth * s) + 12.0f);
        p.region[3] = std::min(p.shown_bottom, std::max(p.region[1] + 50.0f, p.shown_bottom - 240.0f));
        p.valid     = true;
        return p;
    }

    void NoteApplied(bool applied, const Plan& plan)
    {
        std::scoped_lock _{ g_mtx };
        g_applied = applied;
        if (applied) {
            g_applied_plan = plan;
            g_applied_ms   = NowMs();
        }
    }

    bool Applied(Plan& plan)
    {
        std::scoped_lock _{ g_mtx };
        plan = g_applied_plan;
        return g_applied && plan.valid && NowMs() - g_applied_ms < 2000;
    }

    std::string Status()
    {
        Plan p{};
        const bool on = Applied(p);
        return on ? std::format("Messages below your view's middle: arranged ({:.2f} times larger, their text {:.2f} degrees, from {:.0f} degrees below)",
                        p.scale, 18.0f * p.scale * p.deg_per_unit, kTopDeg)
                  : std::format("Messages below your view's middle: {}", GameFlow::gStore.internalSettings.messagesBlock ? "on (not arranged now)" : "off");
    }
}
