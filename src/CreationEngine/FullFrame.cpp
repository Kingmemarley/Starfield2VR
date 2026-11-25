#include "FullFrame.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "LiveWorld.h"
#include "memory/ScanHelper.h"
#include "memory/offsets.h"
#include <mods/VR.hpp>

namespace FullFrame
{
    namespace
    {
        // The render window's rectangle (0x142990090 in 1.16.236: (window, {left, top, right, bottom}); the rectangle is copied into a
        // command for the render thread, which makes the window's frame texture again when its size changed).
        constexpr const char* kWindowRectPattern =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 05 ? ? ? ? 41 B9 08 00 00 00 48 8B F2 48 8B F9 48 8D 54 24 20 48 8B 58 78";
        // The box's function (0x142499D80: the game's display settings S; with the box on, the box's rectangle sent, else the whole
        // window's; its call of the one above at +0x134).
        constexpr const char* kBoxPattern       = "48 89 5C 24 10 57 48 83 EC 40 F6 41 21 01 48 8D 51 10 48 8B F9 48 89 74 24 50 48 0F 44 D1";
        constexpr size_t      kBoxCallOffset    = 0x134;
        // In S (the display settings: the game's render settings +0x10): +0x00 the window's rectangle, +0x10 the monitor's (used when
        // the mode's bit 0 at +0x21 is set), +0x38 the render window, +0x41 the box on.
        constexpr uintptr_t kSettingsAt = 0x10;
        constexpr uintptr_t kModeAt     = 0x21;
        constexpr uintptr_t kWindowAt   = 0x38;
        constexpr uintptr_t kBoxOnAt    = 0x41;
        // (fix173) The size the box's function last worked out (its height at +0x2C, its width at +0x30: written just before it sends the
        // rectangle, 0x142499E71 and 0x142499E79; when the window needs no box, its own size from +0x24 and +0x28). With the box on and a
        // picture taller than 16:9 the box is the picture's width and ceil(width / the box's aspect, +0x44) high, centred (top (h - box) / 2):
        // 1123 of 2150 rows, rows 513 to 1636, as the saved pictures show it.
        constexpr uintptr_t kBoxHeightAt = 0x2C;
        constexpr uintptr_t kBoxWidthAt  = 0x30;

        using BoxFn = void (*)(uintptr_t settings);

        safetyhook::InlineHook g_rect_hook{};
        BoxFn                  g_box_fn{ nullptr };
        uintptr_t              g_settings_var{ 0 }; // (where the game keeps its render settings' address)
        std::atomic<bool>      g_installed{ false };
        // What is wanted (decided on the main thread, steady for a moment: not a resize at each short change) and what the window
        // has (the last rectangle sent with the box on: the whole window's).
        std::atomic<bool>      g_want{ false };
        std::atomic<bool>      g_full{ false };
        std::atomic<bool>      g_pending{ false };
        std::atomic<bool>      g_box_closed{ false }; // (a menu's box closed: what was pending forgotten, steady again from the next one)
        std::atomic<bool>      g_left{ false };       // (the size could not be changed behind this menu: left as it is)
        std::chrono::steady_clock::time_point g_pending_since{};
        std::chrono::steady_clock::time_point g_last_send{};
        int                    g_tries{ 0 };
        std::atomic<int>       g_rect_logs{ 0 };
        std::atomic<int>       g_switch_logs{ 0 }; // (fix192)
        int                    g_send_logs{ 0 };
        std::atomic<uint32_t>  g_sent_whole{ 0 };
        std::atomic<uint32_t>  g_sent_box{ 0 };
        std::mutex             g_status_mtx{};
        std::string            g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        bool ReadPointer(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadU8(uintptr_t at, uint8_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint8_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadI32(uintptr_t at, int32_t& out)
        {
            __try {
                out = *reinterpret_cast<const int32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadRect(uintptr_t at, int32_t (&out)[4])
        {
            __try {
                for (int i = 0; i < 4; ++i) {
                    out[i] = reinterpret_cast<const int32_t*>(at)[i];
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallBox(uintptr_t settings)
        {
            __try {
                g_box_fn(settings);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool Settings(uintptr_t& s)
        {
            uintptr_t render_settings = 0;
            if (g_settings_var == 0 || !ReadPointer(g_settings_var, render_settings) || render_settings == 0) {
                return false;
            }
            s = render_settings + kSettingsAt;
            return true;
        }

        // Wanted now: in the headset, the setting on, and the world kept drawn behind the see-through menu.
        bool WantedNow()
        {
            auto& vr = VR::get();
            if (vr == nullptr || !vr->is_hmd_active()) {
                return false;
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            // (fix158: or about to be: the frame not made at the box's size first and again at the whole size half a second later; a
            // helper's research of the pause menu's first half second: DLSS made anew at each size, the world soft, then a jump.)
            return xr != nullptr && xr->panel_full_frame.load() && (LiveWorld::Active() || LiveWorld::Coming());
        }

        // The game sending the render window's rectangle (the main thread): with the box on and the full frame wanted, the whole
        // window's instead (what the game itself sends when no box is wanted).
        uintptr_t OnWindowRect(uintptr_t window, const int32_t* rect)
        {
            const int32_t* send = rect;
            int32_t        whole[4]{};
            uintptr_t      s = 0;
            if (window != 0 && rect != nullptr && Settings(s)) {
                uintptr_t own = 0;
                uint8_t   box = 0, mode = 0;
                if (ReadPointer(s + kWindowAt, own) && own == window && ReadU8(s + kBoxOnAt, box) && ReadU8(s + kModeAt, mode)) {
                    bool full = false;
                    if (box == 0) {
                        // (A menu with the box closed: nothing wanted until one is open again and the world is kept drawn behind it.)
                        g_box_closed.store(true);
                        g_want.store(false);
                    } else if (!g_want.load() && WantedNow()) {
                        // (fix158) The box just turned on for a see-through menu: the whole window at once (and wanted from here, so the
                        // frame is not changed back meanwhile).
                        g_want.store(true);
                        g_pending = true;
                        if (ReadRect((mode & 1) != 0 ? s + 0x10 : s, whole) && whole[2] > whole[0] && whole[3] > whole[1]) {
                            send = whole;
                            full = true;
                        }
                    } else if (g_want.load() && ReadRect((mode & 1) != 0 ? s + 0x10 : s, whole) && whole[2] > whole[0] && whole[3] > whole[1]) {
                        send = whole;
                        full = true;
                    }
                    // (fix192) Each change of the frame kept full size or not, logged on its own (the first 10 lines below were all used up
                    // at the main menu in fix191's test, so nothing showed when the frame changed size behind the menus): 40 a session.
                    if (const bool was = g_full.exchange(full); was != full && g_switch_logs.fetch_add(1) < 40) {
                        spdlog::info("[FullFrame] (fix192) the frame {} (the game sent the render window's rectangle {},{} {},{} with its 16:9 box {}; "
                                     "the menu's box then {} of the game's picture)",
                            full ? "kept full size behind the menu: the whole window's rectangle sent instead" : "no longer kept full size",
                            rect[0], rect[1], rect[2], rect[3], box != 0 ? "on" : "off",
                            full ? "the top rows" : box != 0 ? "the centred rows" : "the whole");
                    }
                    ++(full ? g_sent_whole : g_sent_box);
                    if (box != 0 && g_rect_logs.fetch_add(1) < 10) {
                        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
                        spdlog::info("[FullFrame] the game sends the render window's rectangle {},{} {},{} with the box on (from rva {:x}): {} {},{} {},{}",
                            rect[0], rect[1], rect[2], rect[3], reinterpret_cast<uintptr_t>(_ReturnAddress()) - base,
                            full ? "the whole window's sent instead," : "left as it is", send[0], send[1], send[2], send[3]);
                    }
                }
            }
            return g_rect_hook.call<uintptr_t>(window, send);
        }
    }

    void Install()
    {
        const auto rect_at = utility::scan(MemoryScan::mod, kWindowRectPattern);
        const auto box_at  = utility::scan(MemoryScan::mod, kBoxPattern);
        if (!rect_at || !box_at) {
            spdlog::warn("[FullFrame] the game's render window rectangle ({}) or its box's function ({}) not found: the world behind menus is drawn "
                         "in the menu's box, as the game does",
                rect_at ? "found" : "not found", box_at ? "found" : "not found");
            SetStatus("Full-size world behind menus: off (the game's code not found: another game version?)");
            return;
        }
        // (The box's function calls the rectangle's: both as expected.)
        int32_t   distance = 0;
        uint8_t   opcode   = 0;
        const auto call    = *box_at + kBoxCallOffset;
        if (!ReadU8(call, opcode) || opcode != 0xE8) {
            spdlog::warn("[FullFrame] the box's function is not as expected (no call at +{:#x}): left as the game does", kBoxCallOffset);
            SetStatus("Full-size world behind menus: off (the game's code is not as expected)");
            return;
        }
        std::memcpy(&distance, reinterpret_cast<const void*>(call + 1), sizeof(distance));
        if (call + 5 + static_cast<intptr_t>(distance) != *rect_at) {
            spdlog::warn("[FullFrame] the box's function calls another function than the rectangle's: left as the game does");
            SetStatus("Full-size world behind menus: off (the game's code is not as expected)");
            return;
        }
        g_settings_var = GameStore::MemoryOffsets::GlobalRenderSettings();
        if (g_settings_var == 0) {
            spdlog::warn("[FullFrame] the game's render settings not found: left as the game does");
            SetStatus("Full-size world behind menus: off (the game's settings not found)");
            return;
        }
        g_box_fn    = reinterpret_cast<BoxFn>(*box_at);
        g_rect_hook = safetyhook::create_inline(reinterpret_cast<void*>(*rect_at), reinterpret_cast<void*>(&OnWindowRect));
        if (!g_rect_hook) {
            spdlog::warn("[FullFrame] could not hook the game's render window rectangle: left as the game does");
            SetStatus("Full-size world behind menus: off (could not hook the game's code)");
            return;
        }
        g_installed = true;
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        spdlog::info("[FullFrame] the game's render window rectangle hooked (rva {:x}; the box's function at rva {:x}; the settings at rva {:x})",
            *rect_at - base, *box_at - base, g_settings_var - base);
        SetStatus("Full-size world behind menus: ready");
    }

    void OnMenuFrame()
    {
        if (!g_installed.load()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        // (A box closed since the last look: the next one starts afresh, steady for a moment before the size changes.)
        if (g_box_closed.exchange(false)) {
            g_pending       = false;
            g_pending_since = now;
            g_tries         = 0;
            g_left          = false;
        }
        uintptr_t s   = 0;
        uint8_t   box = 0;
        if (!Settings(s) || !ReadU8(s + kBoxOnAt, box) || box == 0) {
            g_pending = false; // (no box: nothing to change, settled)
            return;
        }
        const bool want_now = WantedNow();
        if (want_now != g_pending) {
            g_pending       = want_now;
            g_pending_since = now;
        }
        // (Steady for a moment: the world dropped for a few frames, as on a menu change, does not make the frame again.)
        if (g_pending != g_want.load() && now - g_pending_since >= std::chrono::milliseconds(300)) {
            g_want.store(g_pending);
        }
        const bool want = g_want.load();
        if (want == g_full.load()) {
            g_tries = 0;
            return;
        }
        // The rectangle sent again by the game's own box function (the box itself unchanged: only the rectangle, through the hook).
        // (Not again and again: every half second at most; a few tries in a row, then left.)
        if (now - g_last_send < std::chrono::milliseconds(500) || g_tries >= 6) {
            return;
        }
        ++g_tries;
        g_last_send   = now;
        const bool ok = CallBox(s);
        if (g_send_logs++ < 40) { // (fix192: 40, not 12)
            spdlog::info("[FullFrame] {} behind the open menu: the render window's rectangle sent again{}", want ? "the world kept drawn" : "the world no longer kept drawn",
                ok ? (g_full.load() == want ? "" : " (not taken: the game sent another)") : " (the game's function failed)");
        }
        if (g_tries == 6 && g_full.load() != want) {
            g_left = true;
            spdlog::warn("[FullFrame] the frame's size could not be changed behind this menu (6 tries): left as it is");
        }
        SetStatus(fmt::format("Full-size world behind menus: {} [whole {}, box {}]", g_full.load() ? "on (the frame kept full size)" : "ready",
            g_sent_whole.load(), g_sent_box.load()));
    }

    bool Active()
    {
        return g_full.load();
    }

    bool GameBox(int& width, int& height)
    {
        width = height = 0;
        uintptr_t s   = 0;
        uint8_t   box = 0;
        if (!g_installed.load() || !Settings(s) || !ReadU8(s + kBoxOnAt, box) || box == 0) {
            return false;
        }
        int32_t h = 0, w = 0;
        if (ReadI32(s + kBoxHeightAt, h) && ReadI32(s + kBoxWidthAt, w)) {
            width  = w;
            height = h;
        }
        return true;
    }

    bool Settled()
    {
        if (!g_installed.load() || g_left.load()) {
            return true;
        }
        const bool want = g_want.load();
        return want == g_full.load() && g_pending.load() == want;
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
