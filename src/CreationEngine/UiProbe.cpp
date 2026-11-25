#include "UiProbe.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

#include "CreationEngineCameraManager.h"
#include "ModSettings.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "models/GameFlow.h"

namespace UiProbe
{
    namespace
    {
        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // "Interface/InventoryMenu.swf" -> "InventoryMenu.swf"
        std::string ShortUrl(const char* url)
        {
            if (url == nullptr) {
                return "?";
            }
            std::string_view v{ url };
            v = v.substr(0, std::min<size_t>(v.size(), 120));
            if (const auto slash = v.find_last_of("/\\"); slash != std::string_view::npos) {
                v = v.substr(slash + 1);
            }
            return std::string{ v };
        }

        // ---- The order of a frame's render graphs (render thread) and movie set-ups (any thread) ----

        std::mutex                                   g_mutex{};
        std::unordered_set<std::string>              g_graphs_seen{};
        std::vector<std::string>                     g_frame{};       // this frame's steps: render graphs and movie set-ups
        std::vector<std::string>                     g_frame_menus{}; // the full-screen menus set up this frame
        uint64_t                                     g_frame_number{ 0 };
        bool                                         g_play_wanted{ false };
        bool                                         g_play_captured{ false };
        int64_t                                      g_play_since_ms{ 0 };
        std::unordered_map<std::string, int>         g_menu_streak{};    // frames in a row each menu was set up
        std::unordered_set<std::string>              g_menus_captured{}; // (one frame logged for each menu)
        std::unordered_map<std::string, std::array<int32_t, 13>> g_movie_logged{}; // (its viewport's fields as last logged)
        int                                          g_movie_lines{ 0 };

        // One frame's steps in the log (lines of about 1500 characters), and whether the game drew its world in it.
        void LogFrame(const std::string& what)
        {
            const bool world      = std::find(g_frame.begin(), g_frame.end(), "Frame") != g_frame.end();
            const bool fullscreen = std::find(g_frame.begin(), g_frame.end(), "FullscreenMenu") != g_frame.end();
            spdlog::info("[UI-PROBE] one frame of {} (frame {}, {} steps): the world drawn (its \"Frame\" render graph ran): {}; the "
                         "\"FullscreenMenu\" render graph ran: {}",
                what, g_frame_number, g_frame.size(), world ? "yes" : "no", fullscreen ? "yes" : "no");
            std::string line;
            int         part = 1;
            for (size_t i = 0; i < g_frame.size(); ++i) {
                line += (line.empty() ? "" : " > ") + g_frame[i];
                if (line.size() > 1500 || i + 1 == g_frame.size()) {
                    spdlog::info("[UI-PROBE]   steps, part {}: {}", part++, line);
                    line.clear();
                }
            }
        }

        void FinishFrame()
        {
            // Each full-screen menu once, in its fifth frame in a row (open, past its first moments).
            std::unordered_map<std::string, int> streak;
            for (const auto& menu : g_frame_menus) {
                if (!streak.contains(menu)) {
                    const auto before = g_menu_streak.find(menu);
                    streak[menu]      = (before != g_menu_streak.end() ? before->second : 0) + 1;
                }
            }
            g_menu_streak = std::move(streak);
            for (const auto& [menu, frames] : g_menu_streak) {
                if (frames == 5 && g_menus_captured.size() < 24 && g_menus_captured.insert(menu).second) {
                    std::string open;
                    for (const auto& other : g_frame_menus) {
                        open += (open.empty() ? "" : ", ") + other;
                    }
                    LogFrame(fmt::format("the menu {} (menus set up in it: {})", menu, open));
                }
            }
            if (g_play_wanted) {
                LogFrame("play");
                g_play_captured = true;
                g_play_wanted   = false;
            }
        }

        // ---- Reading a movie (each call on its own, with an exception handler: only plain data in these) ----

        struct MovieFacts
        {
            float    stage_w{ 0.0f };
            float    stage_h{ 0.0f };
            uint32_t scale_mode{ 0 };
            uint32_t align{ 0 };
            float    visible[4]{};
        };

        bool ReadMovieFacts(void* movie, MovieFacts& out)
        {
            __try {
                auto* m   = reinterpret_cast<RE::Scaleform::GFx::MovieImpl*>(movie);
                auto* def = m->GetMovieDef();
                if (def != nullptr) {
                    out.stage_w = def->GetWidth();
                    out.stage_h = def->GetHeight();
                }
                out.scale_mode     = m->GetViewScaleMode();
                out.align          = m->GetViewAlignment();
                const auto visible = m->GetVisibleFrameRect();
                out.visible[0]     = visible.x;
                out.visible[1]     = visible.y;
                out.visible[2]     = visible.z;
                out.visible[3]     = visible.w;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadCode(uintptr_t at, uint8_t* out, size_t n)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        using GetMouseStateFn = void (*)(void* movie, uint32_t mouse, float* x, float* y, uint32_t* buttons);
        bool CallGetMouseState(uintptr_t fn, void* movie, float& x, float& y, uint32_t& buttons)
        {
            __try {
                reinterpret_cast<GetMouseStateFn>(fn)(movie, 0, &x, &y, &buttons);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The movie's GetMouseState (Scaleform 4's Movie, type table slot 0x24, after SetViewport at 0xc, which the mod hooks):
        // used only if its code looks like it (a short function that first checks the mouse index (cmp edx, n) and writes floats
        // through the pointers it is given (movss [r8/r9], ...)).
        constexpr size_t     kGetMouseStateSlot = 0x24;
        std::atomic<int>     g_mouse_fn_state{ 0 }; // 0 not checked, 1 checked and used, -1 not the expected function
        std::atomic<uintptr_t> g_mouse_fn{ 0 };
        std::atomic<bool>    g_want_mouse{ false };
        std::atomic<int64_t> g_workshop_ms{ 0 }; // when build mode's movie was last set up
        std::mutex           g_mouse_mutex{};
        // The latest reading of each full-screen menu movie (g_mouse_mutex).
        std::unordered_map<std::string, GameMouse> g_mice{};

        uintptr_t GameRva(uintptr_t at)
        {
            return at - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        void CheckMouseFunction(void* movie)
        {
            int expected = 0;
            uintptr_t table = 0;
            if (!ReadCode(reinterpret_cast<uintptr_t>(movie), reinterpret_cast<uint8_t*>(&table), sizeof(table)) || table == 0) {
                return;
            }
            uintptr_t fn = 0;
            if (!ReadCode(table + kGetMouseStateSlot * 8, reinterpret_cast<uint8_t*>(&fn), sizeof(fn)) || fn == 0) {
                return;
            }
            uint8_t code[0x80]{};
            if (!ReadCode(fn, code, sizeof(code))) {
                return;
            }
            bool index_check = false, float_store = false, returns = false;
            // (cmp edx, n; or cmp edx, [rcx + the movie's mouse count]: 3b 51 d8 / 3b 91 d32, as in 1.16.236)
            for (size_t i = 0; i + 3 <= 12; ++i) {
                if ((code[i] == 0x83 && code[i + 1] == 0xFA && code[i + 2] >= 1 && code[i + 2] <= 16) ||
                    (code[i] == 0x3B && (code[i + 1] == 0x51 || code[i + 1] == 0x91))) {
                    index_check = true;
                }
            }
            for (size_t i = 0; i + 4 <= 0x60; ++i) {
                if (code[i] == 0xF3 && code[i + 1] == 0x41 && code[i + 2] == 0x0F && code[i + 3] == 0x11) {
                    float_store = true;
                }
            }
            for (size_t i = 0; i < 0x80; ++i) {
                if (code[i] == 0xC3) {
                    returns = true;
                }
            }
            std::string bytes;
            for (size_t i = 0; i < 48; ++i) {
                bytes += fmt::format("{:02x}", code[i]);
            }
            const bool ok = index_check && float_store && returns;
            if (g_mouse_fn_state.compare_exchange_strong(expected, ok ? 1 : -1)) {
                g_mouse_fn = fn;
                spdlog::info("[UI-PROBE] the movies' mouse state function (type table slot {:#x}, rva {:x}) {}: {}", kGetMouseStateSlot, GameRva(fn),
                    ok ? "looks as expected, read while the menu pointer is in use" : "does not look as expected, not read", bytes);
            }
        }
    }

    void OnRenderGraph(const char* name, bool before)
    {
        if (!before || name == nullptr) {
            return;
        }
        std::string n{ name, strnlen(name, 80) };
        std::scoped_lock _{ g_mutex };
        if (n == "CRBeginFrame") {
            FinishFrame();
            ++g_frame_number;
            g_frame.clear();
            g_frame_menus.clear();
            // One frame of play, 20 s into the game.
            const int64_t now = NowMs();
            if (!ModSettings::g_internalSettings.showQuadDisplay && GameFlow::worldBehindMenu()) {
                if (g_play_since_ms == 0) {
                    g_play_since_ms = now;
                }
                g_play_wanted = !g_play_captured && now - g_play_since_ms > 20000;
            }
        }
        if (g_graphs_seen.size() < 400 && g_graphs_seen.insert(n).second) {
            spdlog::info("[UI-PROBE] render graph first seen: {} (frame {}, thread {})", n, g_frame_number, GetCurrentThreadId());
        }
        if (g_frame.size() < 400) {
            g_frame.push_back(std::move(n));
        }
    }

    void OnMovieViewport(void* movie, const char* url, const void* viewport)
    {
        if (movie == nullptr || viewport == nullptr) {
            return;
        }
        const auto*       vp   = static_cast<const Scaleform::Gfx::Viewport*>(viewport);
        const std::string name = ShortUrl(url);
        const auto        tid  = GetCurrentThreadId();

        // Where in the frame (and on which thread) each movie is set up.
        {
            std::scoped_lock _{ g_mutex };
            if (g_frame.size() < 400) {
                g_frame.push_back(fmt::format("[movie {} on thread {}]", name, tid));
            }
            if (url != nullptr) {
                const auto hash = djb2Hash(url);
                if (GameFlow::isFullscreenMenuHash(hash) && hash != "Interface/CursorMenu.swf"_DJB && hash != "Interface/CursorMenu_LRG.swf"_DJB &&
                    std::find(g_frame_menus.begin(), g_frame_menus.end(), name) == g_frame_menus.end()) {
                    g_frame_menus.push_back(name);
                }
            }
            // Its viewport, the first time and when it changes (a few hundred lines at most).
            const std::array<int32_t, 13> fields{ vp->bufferWidth, vp->bufferHeight, vp->left, vp->top, vp->width, vp->height, vp->scissorLeft,
                vp->scissorTop, vp->scissorWidth, vp->scissorHeight, static_cast<int32_t>(vp->flags), static_cast<int32_t>(vp->scale * 1000.0f),
                static_cast<int32_t>(vp->aspectRatio * 1000.0f) };
            const auto known = g_movie_logged.find(name);
            if ((known == g_movie_logged.end() || known->second != fields) && g_movie_lines < 300) {
                const bool first = known == g_movie_logged.end();
                g_movie_logged[name] = fields;
                ++g_movie_lines;
                const auto text = fmt::format("buffer {}x{}, rect {},{} {}x{}, scissor {},{} {}x{}, flags {:#x}{}{}, scale {:.2f}, aspect {:.2f}",
                    vp->bufferWidth, vp->bufferHeight, vp->left, vp->top, vp->width, vp->height, vp->scissorLeft, vp->scissorTop, vp->scissorWidth,
                    vp->scissorHeight, vp->flags, (vp->flags & 1u) != 0 ? " (draws into a texture of its own)" : "",
                    (vp->flags & 2u) != 0 ? " (alpha composite)" : "", vp->scale, vp->aspectRatio);
                MovieFacts facts{};
                if (first && ReadMovieFacts(movie, facts)) {
                    spdlog::info("[UI-PROBE] movie {} (thread {}): viewport {}; stage {:.0f}x{:.0f}, scale mode {}, align {}, part of the stage shown "
                                 "{:.0f},{:.0f} to {:.0f},{:.0f}",
                        name, tid, text, facts.stage_w, facts.stage_h, facts.scale_mode, facts.align, facts.visible[0], facts.visible[1], facts.visible[2],
                        facts.visible[3]);
                } else {
                    spdlog::info("[UI-PROBE] movie {} (thread {}): viewport {}", name, tid, text);
                }
            }
        }

        // The game's mouse place in it: each frame while the menu pointer is in use, in the menus (and their pop-ups), with the size
        // of the area the movie draws in (its coordinates). Not the HUD's movies, the fader's, the cursor's own or the loading
        // screen's; nor build mode's (WorkshopMenu: the mouse turns the view there, it has no cursor to steer).
        if (!g_want_mouse.load() || url == nullptr) {
            return;
        }
        {
            const auto hash = djb2Hash(url);
            if (name.find("WorkshopMenu") != std::string::npos) {
                g_workshop_ms = NowMs(); // (build mode showing: nothing is steered, see LastGameMouse)
                return;
            }
            // (fix142: a conversation's own movie is read: its responses are pointed at, MenuPointer.)
            const bool conversation = hash == "Interface/DialogueMenu.swf"_DJB || hash == "Interface/DialogueMenu_LRG.swf"_DJB;
            if ((GameFlow::isHudMovieHash(hash) && !conversation) || name.find("HUD") != std::string::npos || name.find("Hud") != std::string::npos ||
                name.find("Fader") != std::string::npos || name.find("CursorMenu") != std::string::npos || name.find("LoadingMenu") != std::string::npos) {
                return;
            }
        }
        if (g_mouse_fn_state.load() == 0) {
            CheckMouseFunction(movie);
        }
        if (g_mouse_fn_state.load() != 1) {
            return;
        }
        // (The same function for every movie: one type.)
        uintptr_t table = 0, fn = 0;
        if (!ReadCode(reinterpret_cast<uintptr_t>(movie), reinterpret_cast<uint8_t*>(&table), sizeof(table)) ||
            !ReadCode(table + kGetMouseStateSlot * 8, reinterpret_cast<uint8_t*>(&fn), sizeof(fn)) || fn != g_mouse_fn.load()) {
            return;
        }
        // (fix163) Not a menu's square offscreen picture (the slate draws one of 1024x1024 each frame besides its own): the pointer's
        // movie size would flip between the two.
        if (vp->bufferWidth == vp->bufferHeight && vp->bufferWidth < 1280) { // (a small square only: the review)
            return;
        }
        float    x = 0.0f, y = 0.0f;
        uint32_t buttons = 0;
        if (CallGetMouseState(fn, movie, x, y, buttons) && std::isfinite(x) && std::isfinite(y)) {
            const int64_t now = NowMs();
            std::scoped_lock _{ g_mouse_mutex };
            auto& m = g_mice[name];
            // (Only a move counts: a menu's first reading may be an old place, the menu shows no cursor until the mouse moves. A
            // reading after a pause, a menu opened again, is a first reading.)
            if (m.read && now - m.at_ms > 300) {
                m.read       = false;
                m.changed_ms = 0;
            }
            if (m.read && (m.x != x || m.y != y)) {
                m.changed_ms = now;
            }
            m.read     = true;
            m.x        = x;
            m.y        = y;
            m.buttons  = buttons;
            m.movie    = name;
            m.at_ms    = now;
            m.buffer_w = vp->bufferWidth;
            m.buffer_h = vp->bufferHeight;
            ++m.seq;
        }
    }

    GameMouse LastGameMouse(const std::string& prefer)
    {
        // The menu showing now (read in the last moments) whose mouse moved last (of two at once, the one the game moves its cursor
        // in); the one asked for while it is showing, unless another one's moved clearly later.
        const int64_t    now = NowMs();
        if (now - g_workshop_ms.load() < 300) {
            return GameMouse{}; // (build mode: the mouse turns the view there; no other movie is steered meanwhile)
        }
        std::scoped_lock _{ g_mouse_mutex };
        const GameMouse* best      = nullptr;
        const GameMouse* preferred = nullptr;
        for (const auto& [name, m] : g_mice) {
            if (!m.read || now - m.at_ms > 300) {
                continue;
            }
            if (!prefer.empty() && name == prefer) {
                preferred = &m;
            }
            if (best == nullptr || m.changed_ms > best->changed_ms || (m.changed_ms == best->changed_ms && m.at_ms > best->at_ms)) {
                best = &m;
            }
        }
        if (preferred != nullptr && best != nullptr && best->changed_ms <= preferred->changed_ms + 50) {
            best = preferred;
        }
        return best != nullptr ? *best : GameMouse{};
    }

    void WantGameMouse(bool on)
    {
        if (on && !g_want_mouse.load()) {
            std::scoped_lock _{ g_mouse_mutex };
            g_mice.clear(); // (each time anew: no old places)
        }
        g_want_mouse = on;
    }
}
