#include "MenuPointer.h"

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "WindowsMessageHook.hpp"
#include "ConversationPanel.h"
#include "InputMode.h"
#include "UiProbe.h"
#include "InputBlocker.hpp"
#include "Haptics.h"
#include "CreationEngineConstants.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include <ModSettings.h>
#include <mods/VR.hpp>

namespace MenuPointer
{
    namespace
    {
        // (fix154) The last press sent: the hand whose trigger made it (0 left, 1 right) and when (steady ms).
        std::atomic<int>     g_last_press_hand{ -1 };
        std::atomic<int64_t> g_last_press_ms{ 0 };
        int64_t              NowMsSteady()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        using Clock = std::chrono::steady_clock;

        // Steering: a correction is sent, then the game's cursor is read until it has moved and stopped again (it shows a move a
        // frame or two after it is sent), then the next. How far the cursor goes per count sent is learned from each correction.
        constexpr float kCorrection     = 0.8f;  // the part of the distance left sent each time
        constexpr float kThere          = 2.5f;  // close enough (the game's pixels)
        constexpr int   kMaxCounts      = 150;   // per correction, each way
        constexpr int   kFirstMaxCounts = 40;    // the same before the cursor's speed is known (a menu that turns the view with the
                                                 // mouse, not a cursor, gets no more than this)
        constexpr int   kSettleQuiet    = 2;     // fresh readings without a move, after it moved: it has stopped
        constexpr int   kSettleMost     = 8;     // fresh readings after a correction at most
        constexpr auto  kSettleNoMove   = std::chrono::milliseconds(150); // and 3 readings with nothing moved: it did not follow
        constexpr int64_t kReadingAge   = 40;    // ms: a correction is sent only from a reading this new
        constexpr int   kWakeTries      = 3;     // small moves (2, 4, 8 counts) to make a menu show its cursor, each time you point
        constexpr auto  kClickWait      = std::chrono::milliseconds(250); // a click waits this long at most for the cursor to arrive
        constexpr auto  kClickHold      = std::chrono::milliseconds(70);  // the button is held down at least this long (the game
                                                                          // reads its state once a frame)
        // The game's input mode (InputMode).
        constexpr WORD  kModeKey        = VK_F13;                         // pressed to switch the game to the mouse (nothing uses it)
        constexpr auto  kKeyHold        = std::chrono::milliseconds(60);  // held this long (read once a frame, as the button)
        constexpr auto  kKeyEvery       = std::chrono::milliseconds(700); // pressed at most this often
        constexpr int   kKeyTries       = 3;                              // presses before the mode byte is written instead
        constexpr auto  kButtonsQuiet   = std::chrono::milliseconds(300); // a gamepad button let go this long before switching back
        constexpr auto  kMouseSettled   = std::chrono::milliseconds(1500); // in mouse mode this long: the presses count from 0 again
        constexpr int   kSessionPresses = 20;                             // presses at most while you point at one menu
        constexpr int   kSessionWrites  = 3;                              // and writes of the mode byte
        constexpr auto  kActive         = std::chrono::seconds(2);        // pointing counts as in use this long after the dot moved
        constexpr float kActiveMove     = 24.0f;                          // (this far, in the menu's pixels) or the trigger was pulled
        constexpr auto  kClickMost      = std::chrono::milliseconds(1200); // a click waits at most this long (a mode switch first)
        constexpr float kWheelDeadzone  = 0.35f;                          // a stick pushed further scrolls
        constexpr int   kStickNavPush   = 8000;                           // (fix146) the left stick pushed past this (of 32767) navigates a menu (fix166: 11500 before)
        constexpr float kStickNavResume = 0.08f;                          // ...until the hand's dot moves this far (of the picture) from there (fix166: 0.03)
        constexpr auto  kStickNavSettle = std::chrono::milliseconds(1000); // ...counted from this long after the last push (fix166: 0.3 s)
        constexpr auto  kStickNavPress  = std::chrono::milliseconds(80);   // (fix166) a trigger pulled meanwhile: A pressed this long
        constexpr float kZoomDeadzone   = 0.30f;                          // (fix175) the star map: the right stick pushed further up or down zooms...
        constexpr int   kZoomTriggerLeast = 96;                           // ...as RT or LT from this (of 255, well past the game's 30) to 255 at full push
        // (fix175) The star map's zoom by the right stick, counted for the log as it closes (under g_mutex).
        struct StarZoom {
            bool     shown{ false };
            uint32_t wheel_in{ 0 }, wheel_out{ 0 }; // as the mouse wheel's notches (pointing at it: FilterGamepad)
            uint32_t push_in{ 0 }, push_out{ 0 };   // as RT (in) or LT (out): each push
            int      sign_was{ 0 };
            int      logs{ 0 };
        };
        StarZoom g_star_zoom{};
        StarZoom g_chargen_zoom{}; // (fix203) character creation's, the same way (its wheel is the lists' scroll, not counted)
        // Placing (the game's cursor is where its mouse messages say the mouse is: mouse_override).
        constexpr float kPlacedNear     = 6.0f;                           // the cursor where it was put (the game's pixels)
        constexpr float kPlacedFar      = 30.0f;                          // and not there
        constexpr auto  kPlaceRetry     = std::chrono::milliseconds(120); // made known again this often while it is not there
        constexpr auto  kPlaceGiveUp    = std::chrono::milliseconds(1500); // not there this long: steered as a mouse is instead

        struct State
        {
            // The last place on the panel (kept over a frame or two without one).
            runtimes::OpenXR::PanelPointer pointer{};
            Clock::time_point              last_shown{};
            Clock::time_point              last_update{}; // (the last gamepad poll)
            bool                           trigger_was[2]{ false, false };
            bool                           trigger_owned[2]{ false, false }; // went down while pointing: not the game's until let go
            int                            was_pointing{ -1 };

            bool  active{ false };
            int   hand{ -1 };
            bool  foreground{ false };
            float target[2]{};       // where the game's cursor should be (the menu movie's own pixels)
            bool  target_clamped{ false };
            int   buffer[2]{};       // the size of the area the menu draws in

            // The menu the cursor is steered in, whether its cursor has been seen to move since then, and how far it goes each way.
            std::string       movie{};
            Clock::time_point movie_since{};
            bool              woken{ false };
            int               wake_tries{ 0 };
            Clock::time_point woke_at{};
            uint64_t          seen_seq{ 0 };
            float             last_xy[2]{};
            float             wall_lo[2]{};
            float             wall_hi[2]{};

            // Placing: the place in the game's mouse messages made the dot's (mouse_override), and the game told of it by a move of
            // one count (its mouse code takes the messages' place when the mouse moves). Off for a menu where the cursor did not go
            // where it was put: steered there as a mouse is (Steer), Windows' own cursor moving.
            bool              placing{ true };
            int               placed_client[2]{ -100000, -100000 }; // the place last made known (client pixels)
            Clock::time_point placed_at{};           // (when)
            Clock::time_point place_changed_at{};    // the place last moved more than a hand's tremor (still_anchor)
            int               still_anchor[2]{ -100000, -100000 };
            int               wiggle{ 1 };
            struct Placed
            {
                float   x{ 0.0f }, y{ 0.0f };        // the target then (the menu's pixels)
                int64_t ms{ 0 };
            };
            std::array<Placed, 16> recent{};         // the last places made known (a reading shows one made 1-15 frames before)
            int               recent_next{ 0 };
            Clock::time_point confirmed_at{};        // the cursor last found where it was put
            Clock::time_point off_since{};           // the cursor far from where it was put, the target still, since
            int               off_checks{ 0 };
            float             offset[2]{};           // learned: where the cursor lands against where it is put (the menu's pixels)
            float             last_miss[2]{};
            bool              have_miss{ false };

            // Steering.
            float             gain[2]{ 1.0f, 1.0f }; // the cursor's move per count sent (the game's pixels)
            int               learned{ 0 };          // corrections it was learned from
            int               rejected{ 0 };         // samples in a row too far from what was learned
            bool              waiting{ false };      // a correction sent, the cursor not yet stopped
            bool              moved_since{ false };
            int               sent[2]{};
            float             from[2]{};
            int               readings_since{ 0 };
            int               quiet_readings{ 0 };
            Clock::time_point sent_at{};
            float             error{ 1e9f };         // the distance left at the last reading

            // The left button, held by a trigger.
            bool              click_pending{ false };
            Clock::time_point click_deadline{};
            bool              button_down{ false };
            Clock::time_point button_down_at{};
            int               button_hand{ -1 };
            bool              release_owed{ false }; // (a release Windows did not take: sent again)
            bool              tap_only{ false };     // (fix142: a conversation's: let go after the shortest hold, the trigger held or not)
            bool              dialogue_was{ false }; // (fix142: a conversation shown at the last poll)
            uint32_t          touch_presses[2]{ 0, 0 }; // (fix143: the touch's press counts last seen)
            int32_t           touch_scroll[2]{ 0, 0 };  // (fix146: the conversation panel's drag notches last seen)
            int               touch_wheel_owed{ 0 };    // (fix146: ...still to be sent, one a poll: up above 0)
            bool              stick_nav{ false };       // (fix146: the left stick navigating a menu: the pointer aside)
            float             stick_nav_anchor[2][2]{};   // (each hand's dot at the last push)
            Clock::time_point stick_nav_pushed{};         // (the last push)
            Clock::time_point stick_nav_ended{};          // (when the pointer came back: its first click may wait longer)
            Clock::time_point stick_a_until{};            // (fix166) A pressed for the game until then: a trigger chose the stick's highlight
            int               stick_nav_total{ 0 };       // (fix166) the stick's log lines this session
            int               stick_nav_logs{ 0 };
            bool              touch_seen{ false };

            // The game's input mode, and the key that switches it to the mouse.
            int               mode_seen{ -2 };       // the mode byte last seen (-1: not known; -2: not looked at yet)
            bool              key_down{ false };
            Clock::time_point key_down_at{};
            Clock::time_point last_key{};
            int               key_tries{ 0 };        // presses since you pointed (or it was last in mouse mode a while)
            bool              forced{ false };       // the byte written since then
            bool              key_up_owed{ false };  // (a key-up Windows did not take: sent again)
            bool              key_due{ false };      // a press (or a write) wanted: done at the end of the poll (FilterGamepad), when
            bool              key_due_known{ false }; // no gamepad button is held in its report (known: the mode is known)
            Clock::time_point mouse_since{};         // in mouse mode since
            int               session_presses{ 0 };
            int               session_writes{ 0 };
            bool              capped_logged{ false };
            Clock::time_point last_activity{};       // the pointer last in use (the dot moved, the trigger pulled)
            float             activity_anchor[2]{};
            Clock::time_point click_started{};
            Clock::time_point buttons_down_at{};     // a gamepad button (one that still reaches the game) last held
            // The gamepad's report filtered (FilterGamepad), as this poll's Update found it.
            bool              filtering{ false };
            bool              may_send_now{ false };
            bool              wheel_on{ false };
            float             wheel{ 0.0f };         // notches owed
            Clock::time_point wheel_at{};

            // For the log.
            Clock::time_point next_log{};
            Clock::time_point active_since{};
            uint32_t          corrections{ 0 };
            uint32_t          counts{ 0 };
            uint32_t          not_followed{ 0 };
            uint32_t          clicks{ 0 };
            uint32_t          clicks_early{ 0 };
            uint32_t          clicks_dropped{ 0 };
            uint32_t          not_in_front{ 0 };
            uint32_t          key_presses{ 0 };
            uint32_t          mode_writes{ 0 };
            uint32_t          to_gamepad{ 0 };
            uint32_t          to_mouse{ 0 };
            uint32_t          wheel_notches{ 0 };
            uint32_t          jumps{ 0 };
            uint32_t          placements{ 0 };
            uint32_t          placed_near{ 0 };
            uint32_t          placed_far{ 0 };
            uint64_t          rewritten_at_log{ 0 };
            std::string       last_status{ "Menu pointer: not used yet (open a menu and point at it)." };
        };

        std::mutex g_mutex{};
        State      g{};

        int64_t Ms(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

        bool InFront(HWND window)
        {
            const HWND front = GetForegroundWindow();
            return window != nullptr && front != nullptr && (front == window || GetAncestor(front, GA_ROOT) == GetAncestor(window, GA_ROOT));
        }

        // The middle of the part of the game's window on its screen (where Windows' cursor is kept: a click there lands in the
        // game's window, never in another), as Windows' absolute mouse coordinates (0..65535 over all screens). False when there is
        // no such place, or it is not the game's window there.
        bool ParkPlace(HWND window, LONG& x, LONG& y, int shift = 0)
        {
            RECT client{};
            if (window == nullptr || !GetClientRect(window, &client)) {
                return false;
            }
            POINT corners[2]{ { client.left, client.top }, { client.right, client.bottom } };
            ClientToScreen(window, &corners[0]);
            ClientToScreen(window, &corners[1]);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
                return false;
            }
            const LONG left = std::max(corners[0].x, info.rcMonitor.left), right = std::min(corners[1].x, info.rcMonitor.right);
            const LONG top = std::max(corners[0].y, info.rcMonitor.top), bottom = std::min(corners[1].y, info.rcMonitor.bottom);
            if (right - left < 4 || bottom - top < 4) {
                return false;
            }
            const POINT middle{ (left + right) / 2 + shift, (top + bottom) / 2 };
            const HWND  under = WindowFromPoint(middle);
            if (under == nullptr || GetAncestor(under, GA_ROOT) != GetAncestor(window, GA_ROOT)) {
                return false;
            }
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vw < 2 || vh < 2) {
                return false;
            }
            x = (LONG)MulDiv(middle.x - vx, 65535, vw - 1);
            y = (LONG)MulDiv(middle.y - vy, 65535, vh - 1);
            return true;
        }

        // The game reads the mouse as raw input and moves its menu cursor by how far the mouse moved (rva 22fd440 adds each
        // relative move to its mouse device and ignores absolute ones; nothing in it asks where Windows' cursor is). A move sent
        // with SendInput arrives the same way. In the same batch (so in this order), Windows' cursor is put back in the middle of
        // the game's window with an absolute move, which the game ignores.
        void SendMove(int dx, int dy, HWND window, bool park = true, int park_shift = 0)
        {
            INPUT inputs[2]{};
            inputs[0].type       = INPUT_MOUSE;
            inputs[0].mi.dx      = dx;
            inputs[0].mi.dy      = dy;
            inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE;
            UINT count           = 1;
            LONG x = 0, y = 0;
            if (park && ParkPlace(window, x, y, park_shift)) {
                inputs[1].type       = INPUT_MOUSE;
                inputs[1].mi.dx      = x;
                inputs[1].mi.dy      = y;
                inputs[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                count                = 2;
            }
            SendInput(count, inputs, sizeof(INPUT));
        }

        // The left button down, with Windows' cursor first put in the middle of the game's window (same batch); not sent when that
        // is not the game's window.
        bool SendPress(HWND window, bool park = true)
        {
            if (!park) {
                // (Windows' cursor where it is: in the game's window, which the game keeps it in while in mouse mode.)
                POINT at{};
                const HWND under = GetCursorPos(&at) ? WindowFromPoint(at) : nullptr;
                if (under == nullptr || GetAncestor(under, GA_ROOT) != GetAncestor(window, GA_ROOT)) {
                    return false;
                }
                INPUT input{};
                input.type       = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                return SendInput(1, &input, sizeof(INPUT)) == 1;
            }
            LONG x = 0, y = 0;
            if (!ParkPlace(window, x, y)) {
                return false;
            }
            INPUT inputs[2]{};
            inputs[0].type       = INPUT_MOUSE;
            inputs[0].mi.dx      = x;
            inputs[0].mi.dy      = y;
            inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[1].type       = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            return SendInput(2, inputs, sizeof(INPUT)) == 2;
        }

        bool SendRelease()
        {
            INPUT input{};
            input.type       = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            return SendInput(1, &input, sizeof(input)) == 1;
        }

        // The mode key (F13) down or up. The game reads the keyboard as raw input by its key code (rva 22d9fc0 keeps each key's
        // state, 0..255, for the keyboard device), and a keyboard press switches it to the mouse and keyboard.
        bool SendKey(bool down)
        {
            INPUT input{};
            input.type       = INPUT_KEYBOARD;
            input.ki.wVk     = kModeKey;
            input.ki.wScan   = static_cast<WORD>(MapVirtualKeyW(kModeKey, MAPVK_VK_TO_VSC));
            input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            return SendInput(1, &input, sizeof(input)) == 1;
        }

        void KeyUp()
        {
            if (g.key_down || g.key_up_owed) {
                g.key_up_owed = !SendKey(false);
                g.key_down    = false;
            }
        }

        // One notch of the mouse wheel (the game keeps the last wheel record of a frame: one a poll at most).
        void SendWheel(int delta)
        {
            INPUT input{};
            input.type         = INPUT_MOUSE;
            input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(delta);
            SendInput(1, &input, sizeof(input));
        }

        const char* ModeName(int raw) { return raw < 0 ? "not known" : raw > 1 ? "mouse and keyboard" : "gamepad"; }

        void Release()
        {
            if (g.button_down || g.release_owed) {
                g.release_owed = !SendRelease();
            }
            g.button_down   = false;
            g.click_pending = false;
            g.button_hand   = -1;
        }

        void ResetSteering()
        {
            g.waiting        = false;
            g.moved_since    = false;
            g.readings_since = 0;
            g.quiet_readings = 0;
            g.seen_seq       = 0;
            g.error          = 1e9f;
        }

        void Stop()
        {
            g.stick_nav      = false; // (fix146: no menu: the pointer not left aside for the next)
            g.stick_nav_logs = 0; // (fix166: each menu logged anew: 12 lines a menu, 300 a session, StickLog)
            if (g.active) {
                UiProbe::WantGameMouse(false);
                g.active = false;
            }
            KeyUp();
            mouse_override::active = false;
            g.placing          = true;
            g.placed_client[0] = g.placed_client[1] = -100000;
            g.off_since        = {};
            g.off_checks       = 0;
            g.movie.clear();
            g.woken      = false;
            g.wake_tries = 0;
            g.woke_at    = {};
            g.mode_seen       = -2;
            g.key_tries       = 0;
            g.forced          = false;
            g.key_due         = false;
            g.filtering       = false;
            g.session_presses = 0;
            g.session_writes  = 0;
            g.capped_logged   = false;
            ResetSteering();
        }

        const char* HandName(int hand) { return hand == 0 ? "left" : "right"; }

        void Log(bool ending)
        {
            const auto  counts = InputBlocker::get().take_counts();
            const auto  mouse  = UiProbe::LastGameMouse(g.movie);
            const auto  now    = Clock::now();
            const float span   = std::chrono::duration<float>(now - g.active_since).count();
            const int raw = InputMode::Raw();
            spdlog::info("[MenuPointer] {:.1f} s{}: {} hand; the game's input mode {} ({}); {}; aim {:.0f},{:.0f}{}, {:.1f} away; corrections {} ({} counts; "
                         "not followed {}; cursor jumps {}), cursor speed {:.2f},{:.2f} per count (from {}); clicks {} ({} before the cursor arrived, {} "
                         "dropped); mode key presses {}, mode writes {}, switched to the gamepad {} and back to the mouse {} times; wheel notches {}; {}; polls "
                         "with the game's window behind {}; the game read {} mouse records (raw input) and set the cursor {} times",
                span, ending ? " (stopped)" : "", HandName(g.hand), ModeName(raw), raw >= 0 ? fmt::format("{:#x}", raw) : std::string{ "-" },
                mouse.read ? fmt::format("the game's cursor at {:.0f},{:.0f} in {} ({}x{}, buttons {}{})", mouse.x, mouse.y, mouse.movie, mouse.buffer_w,
                                 mouse.buffer_h, mouse.buttons, g.woken ? "" : ", not seen to move yet")
                           : std::string{ "the game's cursor not read (no menu movie read)" },
                g.target[0], g.target[1], g.target_clamped ? " (at the menu's edge)" : "", g.error < 1e8f ? g.error : -1.0f, g.corrections, g.counts,
                g.not_followed, g.jumps, g.gain[0], g.gain[1], g.learned, g.clicks, g.clicks_early, g.clicks_dropped, g.key_presses, g.mode_writes,
                g.to_gamepad, g.to_mouse, g.wheel_notches,
                g.placing ? fmt::format("placing the cursor: {} places made known, then found there {} and far off {} times, {} mouse messages given the place",
                                g.placements, g.placed_near, g.placed_far, mouse_override::rewritten.load() - g.rewritten_at_log)
                          : std::string{ "steering the cursor as a mouse (placing it did not work in this menu)" },
                g.not_in_front, counts.raw_data_mouse + counts.raw_buffer_mouse, counts.set_cursor_pos);
            g.corrections    = 0;
            g.counts         = 0;
            g.not_followed   = 0;
            g.clicks         = 0;
            g.clicks_early   = 0;
            g.clicks_dropped = 0;
            g.not_in_front   = 0;
            g.key_presses    = 0;
            g.mode_writes    = 0;
            g.to_gamepad     = 0;
            g.to_mouse       = 0;
            g.wheel_notches  = 0;
            g.jumps          = 0;
            g.placements     = 0;
            g.placed_near    = 0;
            g.placed_far     = 0;
            g.rewritten_at_log = mouse_override::rewritten.load();
            g.active_since   = now;
            g.next_log       = now + std::chrono::seconds(3);
        }

        // One poll's reading of the game's cursor: once the last correction has played out, learn from it; then send the next.
        void Steer(const UiProbe::GameMouse& m, Clock::time_point now, bool may_send, HWND window)
        {
            g.error = std::hypot(g.target[0] - m.x, g.target[1] - m.y);
            const bool fresh_reading = m.seq != g.seen_seq;
            const bool moved         = m.x != g.last_xy[0] || m.y != g.last_xy[1];
            g.seen_seq   = m.seq;
            g.last_xy[0] = m.x;
            g.last_xy[1] = m.y;
            if (g.waiting) {
                if (fresh_reading) {
                    ++g.readings_since;
                    if (moved) {
                        g.moved_since    = true;
                        g.quiet_readings = 0;
                    } else if (g.moved_since) {
                        ++g.quiet_readings;
                    }
                }
                const bool stopped  = g.moved_since && g.quiet_readings >= kSettleQuiet;
                const bool no_move  = !g.moved_since && now - g.sent_at >= kSettleNoMove && g.readings_since >= 3;
                const bool too_long = g.readings_since >= kSettleMost || now - g.sent_at > std::chrono::milliseconds(600);
                if (!stopped && !no_move && !too_long) {
                    return;
                }
                g.waiting = false;
                // A jump (the game put its cursor in the middle, as it does when it switches to the mouse; the last test showed
                // one taken for a speed of 11.9 pixels a count): not learned from, nor taken for not following.
                {
                    const float moved    = std::hypot(m.x - g.from[0], m.y - g.from[1]);
                    const float expected = std::hypot(static_cast<float>(g.sent[0]) * (g.learned > 0 ? g.gain[0] : 1.0f),
                        static_cast<float>(g.sent[1]) * (g.learned > 0 ? g.gain[1] : 1.0f));
                    const bool  middle   = std::abs(m.x - static_cast<float>(g.buffer[0]) * 0.5f) <= 3.0f &&
                                        std::abs(m.y - static_cast<float>(g.buffer[1]) * 0.5f) <= 3.0f;
                    if (middle && moved > std::max(40.0f, 3.0f * expected)) {
                        ++g.jumps;
                        // (A click waiting: it waits for the cursor from here, not where it was.)
                        if (g.click_pending) {
                            g.click_deadline = std::min(now + kClickWait, g.click_started + kClickMost);
                        }
                        return;
                    }
                }
                // Followed: it went the way sent, a good part of the way expected (or, before its speed is known, at least a pixel).
                bool followed = false, any_sent = false;
                for (int a = 0; a < 2; ++a) {
                    if (std::abs(g.sent[a]) < 2) {
                        continue;
                    }
                    any_sent         = true;
                    const float went = ((a == 0 ? m.x : m.y) - g.from[a]) * (g.sent[a] > 0 ? 1.0f : -1.0f);
                    const float need = g.learned > 0 ? 0.25f * std::abs((float)g.sent[a]) * g.gain[a] : 1.0f;
                    if (went >= need) {
                        followed = true;
                    }
                }
                if (any_sent && !followed) {
                    ++g.not_followed;
                    // Against the edge of where it can go (near the menu's edge): that is its edge, the aim stops there.
                    bool wall = false;
                    for (int a = 0; a < 2; ++a) {
                        const float at = a == 0 ? m.x : m.y;
                        if (g.sent[a] <= -2 && at < 40.0f) {
                            g.wall_lo[a] = std::max(g.wall_lo[a], at);
                            wall         = true;
                        } else if (g.sent[a] >= 2 && at > (float)g.buffer[a] - 41.0f) {
                            g.wall_hi[a] = std::min(g.wall_hi[a], at);
                            wall         = true;
                        }
                    }
                    if (!wall) {
                        // It does not follow: no more corrections until it is seen to move again, after now (small moves only,
                        // within this time's few).
                        g.woken   = false;
                        g.woke_at = now;
                    }
                    return;
                }
                // Learned only from a correction that played out, away from the menu's edges (where it stops short).
                const bool at_edge = m.x < 2.0f || m.y < 2.0f || m.x > (float)g.buffer[0] - 3.0f || m.y > (float)g.buffer[1] - 3.0f;
                if (stopped && !at_edge && !g.target_clamped) {
                    for (int a = 0; a < 2; ++a) {
                        if (std::abs(g.sent[a]) < 4) {
                            continue;
                        }
                        const float ratio = ((a == 0 ? m.x : m.y) - g.from[a]) / (float)g.sent[a];
                        const bool  first = g.learned == 0;
                        // (A first one past 20 pixels a count is no mouse speed a menu has: a jump the check above missed.)
                        if (first ? (ratio > 0.05f && ratio < 20.0f) : (ratio > g.gain[a] * 0.2f && ratio < g.gain[a] * 5.0f)) {
                            g.gain[a] = first ? ratio : std::clamp(g.gain[a] * 0.4f + ratio * 0.6f, 0.02f, 50.0f);
                            if (first) {
                                g.gain[1 - a] = ratio; // (the other way alike until it is measured)
                            }
                            ++g.learned;
                            g.rejected = 0;
                        } else if (!first && ++g.rejected >= 3) {
                            // (What was learned first was wrong: learned again from the start.)
                            g.learned  = 0;
                            g.rejected = 0;
                            g.gain[0] = g.gain[1] = 1.0f;
                            spdlog::info("[MenuPointer] the cursor's speed is learned again (three moves far from what was learned)");
                        }
                    }
                }
            }
            // A correction only from a reading of this very moment (an old one, from a menu that has just closed, could turn your view).
            const int64_t age = Ms(now) - m.at_ms;
            if (!may_send || !fresh_reading || age > kReadingAge) {
                return;
            }
            const float error[2]{ g.target[0] - m.x, g.target[1] - m.y };
            int         counts[2]{};
            bool        any = false;
            for (int a = 0; a < 2; ++a) {
                // (Close enough: within the threshold, or within most of what one count moves it.)
                if (std::abs(error[a]) <= std::max(kThere, 0.6f * g.gain[a])) {
                    continue;
                }
                // (Once its speed is known: up to 150 counts, or as many as go 600 of its pixels for a slow cursor, 1500 at most.)
                const int most = g.learned > 0 ? std::clamp((int)(600.0f / g.gain[a]), kMaxCounts, 1500) : kFirstMaxCounts;
                counts[a]      = std::clamp((int)std::lround(error[a] * kCorrection / g.gain[a]), -most, most);
                if (counts[a] == 0) {
                    counts[a] = error[a] > 0.0f ? 1 : -1;
                }
                any = true;
            }
            if (!any) {
                return;
            }
            SendMove(counts[0], counts[1], window, false); // (as a mouse moves: Windows' cursor too, which the game follows)
            g.waiting        = true;
            g.moved_since    = false;
            g.sent[0]        = counts[0];
            g.sent[1]        = counts[1];
            g.from[0]        = m.x;
            g.from[1]        = m.y;
            g.readings_since = 0;
            g.quiet_readings = 0;
            g.sent_at        = now;
            ++g.corrections;
            g.counts += (uint32_t)(std::abs(counts[0]) + std::abs(counts[1]));
        }
    }

    namespace
    {
        // One poll's placing: the game puts its menu cursor where its mouse messages say the mouse is (its window procedure,
        // 1883450, keeps a mouse-move message's place for the mouse device, +0x8c/+0x90; the device's poll, 22fd200, looks at that
        // first; the last test's cursor went exactly where Windows' cursor was kept: the middle of the menu, 919,516, or of the
        // window's part on the screen in the Data menu, 955,8). So the place in its mouse messages is made the dot's
        // (mouse_override: client pixels, which may be below the screen's edge, where Windows' cursor cannot go: the game's window is
        // taller than the screen), and a move of one count (with Windows' cursor kept in the window) makes a message and a move for
        // it to take the place from. The cursor is read back: where it was put, it is there (`woken`); if it does not go there in
        // a menu, that menu is steered as a mouse moves instead (Steer).
        void Place(const UiProbe::GameMouse& m, Clock::time_point now, bool may_send, HWND window, float W, float H)
        {
            RECT client{};
            if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0 || W <= 0.0f || H <= 0.0f) {
                return;
            }
            // The target in the picture's pixels (the menu's box centred in it; less the offset learned), then in the window's (its
            // client area: the picture's size, or scaled to it).
            const float px = g.target[0] - g.offset[0] + (W - (float)m.buffer_w) * 0.5f;
            const float py = g.target[1] - g.offset[1] + (H - (float)m.buffer_h) * 0.5f;
            const int   cx = (int)std::lround(px * (float)client.right / W);
            const int   cy = (int)std::lround(py * (float)client.bottom / H);
            mouse_override::set(cx, cy);
            mouse_override::active = true;

            g.waiting              = false;
            const bool reading_new = m.seq != g.seen_seq;
            g.seen_seq             = m.seq;
            g.error                = Ms(now) - m.at_ms <= 250 ? std::hypot(g.target[0] - m.x, g.target[1] - m.y) : 1e9f;

            // A new reading against the places made known 1 to 15 frames (16-300 ms) before it, and the place now: found at one of
            // them, the cursor goes where it is put.
            if (reading_new && g.placed_client[0] > -100000) {
                float best = 1e9f;
                for (const auto& r : g.recent) {
                    const int64_t age = m.at_ms - r.ms;
                    if (r.ms != 0 && age >= 16 && age <= 300) {
                        best = std::min(best, std::hypot(r.x - m.x, r.y - m.y));
                    }
                }
                if (m.at_ms - Ms(g.placed_at) >= 16) {
                    best = std::min(best, std::hypot(g.target[0] - m.x, g.target[1] - m.y));
                }
                if (best <= kPlacedNear) {
                    ++g.placed_near;
                    g.woken        = true;
                    g.confirmed_at = now;
                    g.off_since    = {};
                    g.off_checks   = 0;
                    g.have_miss    = false;
                } else if (best < 1e8f && best > kPlacedFar) {
                    ++g.placed_far;
                    // Judged only with the target still a moment and away from the menu's edges (a cursor stops short there).
                    const bool still = now - g.place_changed_at > std::chrono::milliseconds(150);
                    const bool edge  = g.target_clamped || g.target[0] < 40.0f || g.target[1] < 40.0f || g.target[0] > (float)m.buffer_w - 41.0f ||
                                      g.target[1] > (float)m.buffer_h - 41.0f;
                    if (still && !edge) {
                        // The same miss twice: an offset between where it is put and where it lands (the window's frame, ...): taken
                        // off from now on.
                        const float dx = m.x - g.target[0], dy = m.y - g.target[1];
                        if (g.have_miss && std::abs(dx - g.last_miss[0]) <= 4.0f && std::abs(dy - g.last_miss[1]) <= 4.0f &&
                            std::hypot(g.offset[0] + dx, g.offset[1] + dy) <= 400.0f) {
                            g.offset[0] += dx;
                            g.offset[1] += dy;
                            g.have_miss        = false;
                            g.placed_client[0] = g.placed_client[1] = -100000; // (made known again)
                            spdlog::info("[MenuPointer] the cursor lands {:.0f},{:.0f} from where it is put in {}: that is taken off (offset now {:.0f},{:.0f})",
                                dx, dy, m.movie, g.offset[0], g.offset[1]);
                        } else {
                            g.last_miss[0] = dx;
                            g.last_miss[1] = dy;
                            g.have_miss    = true;
                        }
                        if (g.off_since == Clock::time_point{}) {
                            g.off_since = now;
                        }
                        ++g.off_checks;
                        if (now - g.off_since > kPlaceGiveUp && g.off_checks >= 6) {
                            g.placing              = false;
                            mouse_override::active = false;
                            g.woken                = false;
                            g.wake_tries           = 0;
                            g.movie_since          = now;
                            spdlog::info("[MenuPointer] placing the cursor did not work in {}: put at {:.0f},{:.0f} (window {},{}, offset {:.0f},{:.0f}), it "
                                         "stayed at {:.0f},{:.0f}; steering it as a mouse moves instead (placing is tried again when you point again)",
                                m.movie, g.target[0], g.target[1], cx, cy, g.offset[0], g.offset[1], m.x, m.y);
                            return;
                        }
                    }
                } else if (best < 1e8f) {
                    g.off_since  = {};
                    g.off_checks = 0;
                }
            }

            // Made known when the place changed, or again a moment later while the cursor has not been found there: a move of
            // one count (raw input: the game's mouse moved) with Windows' cursor kept in the window, a pixel to one side or the
            // other each time (so it moves, and Windows sends a mouse-move message), and a mouse-move message of its own (the
            // window hook gives both the place).
            // (fix163: in a conversation only a move of more than 6 pixels is sent again: each is a mouse move to the game, whose conversation
            // camera turned with them, about 37 a second from a hand's tremor; a helper's research.)
            const bool moved_place = GameFlow::isDialogueDrawnNow()
                                         ? std::hypot((float)(cx - g.placed_client[0]), (float)(cy - g.placed_client[1])) > 6.0f
                                         : (cx != g.placed_client[0] || cy != g.placed_client[1]);
            // (Moved more than a hand's tremor: the target is not still; the cursor's misses counted from here.)
            const bool moved_far = std::hypot((float)(cx - g.still_anchor[0]), (float)(cy - g.still_anchor[1])) > 8.0f;
            if (moved_far) {
                g.still_anchor[0]  = cx;
                g.still_anchor[1]  = cy;
                g.place_changed_at = now;
                g.off_since        = {};
                g.off_checks       = 0;
            }
            const bool not_there = now - g.confirmed_at > std::chrono::milliseconds(300) && now - g.placed_at > kPlaceRetry;
            if (may_send && (moved_place || not_there)) {
                SendMove(g.wiggle, 0, window, true, g.wiggle > 0 ? 1 : 0);
                PostMessageW(window, WM_MOUSEMOVE, g.button_down ? MK_LBUTTON : 0, MAKELPARAM((WORD)(int16_t)cx, (WORD)(int16_t)cy));
                g.wiggle           = -g.wiggle;
                g.placed_client[0] = cx;
                g.placed_client[1] = cy;
                g.placed_at        = now;
                g.recent[g.recent_next] = { g.target[0], g.target[1], Ms(now) };
                g.recent_next           = (g.recent_next + 1) % (int)g.recent.size();
                ++g.placements;
            }
        }
    }

    int Update(bool left_trigger, bool right_trigger, bool allowed)
    {
        auto& vr = VR::get();
        if (vr == nullptr || vr->m_openxr == nullptr || !vr->get_runtime()->is_openxr()) {
            return 0;
        }
        const auto xr  = vr->m_openxr; // (a copy: it may be made anew on another thread)
        const auto now = Clock::now();
        bool       trigger[2]{ left_trigger, right_trigger }; // (fix143: a touch's press counts as a pull, below)
        const bool real_trigger[2]{ left_trigger, right_trigger }; // (fix166: the triggers alone, without a touch's press)

        std::scoped_lock _{ g_mutex };
        g.last_update = now;
        if (g.release_owed) {
            Release();
        }
        if ((g.key_down && now - g.key_down_at >= kKeyHold) || g.key_up_owed) {
            KeyUp();
        }
        // (fix142) A conversation (no panel): its responses pointed at, the interface drawn over the world (vrcore: hud_pointer). Which
        // eye's picture the interface's places are in, and how far the other eye's is moved (its depth): mono, one picture, no depth;
        // full stereo, the right eye's (the game's own layer; the left eye's copy moved by the HUD's shift); the eyes taking turns,
        // the eye whose interface is not moved (the dominant eye's is: CreationEngineCameraManager).
        // (Drawn in this frame or the last: as it closes, play follows at once, where a mouse move turns you and a press attacks.)
        const auto& settings = ModSettings::g_internalSettings;
        const bool  dialogue = allowed && GameFlow::isDialogueDrawnNow() && !settings.showQuadDisplay && !settings.forceFlatScreen &&
                              GameFlow::gStore.internalSettings.dialoguePointer;
        xr->hud_pointer_wanted = dialogue;
        xr->hud_pointer_ms     = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        // The conversation closed (no panel up): the button let go and the pointer stopped at once, its last place no longer shown.
        if (g.dialogue_was && !dialogue && !xr->get_panel_pointer().shown) {
            if (g.button_down || g.click_pending) {
                Release();
            }
            if (g.active) {
                Log(true);
                Stop();
            }
            g.last_shown = {};
        }
        g.dialogue_was = dialogue;
        if (dialogue) {
            int   eye      = -1;
            float shift_px = 0.0f;
            if (!vr->is_mono()) {
                if (vr->is_native()) {
                    eye      = 1;
                    shift_px = (float)vr->m_native_hud_shift.load();
                } else {
                    // (While a hand points at it, not moved in either eye: GameFlow::dialoguePointing.)
                    eye      = ModConstants::dominantEye == 1 ? 0 : 1;
                    shift_px = GameFlow::dialoguePointing.load() ? 0.0f : (float)GameFlow::gStore.hudSettings.perspective;
                }
            }
            float shift = 0.0f;
            if (eye >= 0 && shift_px > 0.0f) {
                Vector4f f{};
                {
                    std::shared_lock _{ vr->get_runtime()->eyes_mtx };
                    f = vr->get_runtime()->eye_frustums[eye];
                }
                const auto size = vr->get_backbuffer_size();
                if (size[0] > 0 && f[1] > f[0]) {
                    shift = shift_px * (f[1] - f[0]) / (float)size[0];
                }
            }
            // (fix144) To be touched: fixed in the room, its depth from the eye the mouse is placed for, as the other eye's shift that depth
            // gives (vrcore takes the depth back from it). Not in mono: one picture, no depth to show (the dot far off; the touch's plane
            // within reach by itself), fix144's review.
            if (GameFlow::gStore.internalSettings.dialogueTouch && !vr->is_mono()) {
                const float depth = GameFlow::dialogueDepthM.load();
                if (depth > 0.1f) {
                    shift = xr->ipd_m.load() / depth;
                }
            }
            xr->hud_pointer_eye   = eye;
            xr->hud_pointer_shift = shift;
        }
        // (fix143) The answers touched: each physical hand's finger off the trigger (pointing) told to vrcore; a hand at the answers
        // takes the pointer (its place the finger's), and each new press of it is a pull of its trigger (a tap, below).
        // (fix173) Only with the press's own setting (DialogueTouchPress, off by default: pressing conversation answers
        // physically with the hand is optional): off, no finger is told to vrcore, so it neither hovers, drags nor presses (a 5 mm push through the answers'
        // plane); the panel (DialogueTouch, DialoguePanel), the beam and the trigger stay as they are.
        const bool touching = dialogue && GameFlow::gStore.internalSettings.dialogueTouch && GameFlow::gStore.internalSettings.dialogueTouchPress;
        xr->hud_touch_wanted = touching;
        for (int h = 0; h < 2; ++h) {
            xr->hud_touch_finger[h] = touching && !vr->get_finger_state(h == 0).trigger_touch;
        }
        // (When the frame it came with was made: frames that stopped coming leave an old one, which is no panel.)
        bool touch_used = false;
        const auto p = xr->get_panel_pointer();
        if (p.shown) {
            g.pointer    = p;
            g.last_shown = p.at;
        } else if (dialogue) {
            if (const auto q = xr->get_hud_pointer(); q.shown) {
                g.pointer    = q;
                g.last_shown = q.at;
                if (touching) {
                    touch_used = true;
                    const auto t = xr->get_hud_touch();
                    // (The hand nearer through the answers' plane, if both are there; fix146: the one that just tapped, its place held for the
                    // click, the review.)
                    const bool tapped[2]{ g.touch_seen && t.presses[0] != g.touch_presses[0] && t.hover[0], g.touch_seen && t.presses[1] != g.touch_presses[1] && t.hover[1] };
                    const int  finger = tapped[0] ? 0 : tapped[1] ? 1 : t.hover[0] && t.hover[1] ? (t.gap[0] <= t.gap[1] ? 0 : 1) : t.hover[0] ? 0 : t.hover[1] ? 1 : -1;
                    // (fix146) A finger through the panel, or a new notch: the pointer in use (a press held still, a drag, keeps it so: the review).
                    if (finger >= 0 && t.gap[finger] < 0.0f) {
                        g.last_activity = now;
                    }
                    if (finger >= 0) {
                        g.pointer.hit[finger] = true;
                        g.pointer.u[finger]   = t.u[finger];
                        g.pointer.v[finger]   = t.v[finger];
                        g.pointer.hit[1 - finger] = false;
                        xr->panel_preferred_hand  = finger;
                    }
                    // (fix146) A drag on the conversation's panel: its new notches owed to the game as the mouse wheel (FilterGamepad).
                    for (int h = 0; h < 2; ++h) {
                        if (t.scroll[h] != g.touch_scroll[h]) {
                            if (g.touch_seen) {
                                g.touch_wheel_owed = std::clamp(g.touch_wheel_owed + (t.scroll[h] - g.touch_scroll[h]), -8, 8);
                                g.last_activity    = now;
                            }
                            g.touch_scroll[h] = t.scroll[h];
                        }
                    }
                    for (int h = 0; h < 2; ++h) {
                        if (t.presses[h] != g.touch_presses[h]) {
                            // (The first count seen is where it stands, no press.)
                            if (g.touch_seen) {
                                trigger[h]       = true;
                                g.trigger_was[h] = false;
                                haptics::MeleePulse(h == 0, 0.6f, 0.025f);
                            }
                            g.touch_presses[h] = t.presses[h];
                        }
                    }
                    g.touch_seen = true;
                }
            }
        }
        // (Not touching just now: its press counts taken as they stand, so a press as a conversation ended is no click in the next one:
        // fix143's review.)
        if (!touch_used) {
            const auto t       = xr->get_hud_touch();
            g.touch_presses[0] = t.presses[0];
            g.touch_presses[1] = t.presses[1];
            g.touch_scroll[0]  = t.scroll[0];
            g.touch_scroll[1]  = t.scroll[1];
            g.touch_wheel_owed = 0;
        }
        const bool shown  = now - g.last_shown < std::chrono::milliseconds(150);
        // (Mouse moves and clicks only with a panel this very moment: one sent as a menu closes would turn your view, or shoot.)
        const bool fresh  = now - g.last_shown < std::chrono::milliseconds(60);
        const HWND window = g_framework->get_window();
        const bool can    = allowed && shown && xr->panel_pointer_enabled.load() && window != nullptr && IsWindow(window);
        g.foreground      = InFront(window);

        // A trigger pulled while that hand points at the panel makes it the pointing hand (its beam shows).
        for (int h = 0; h < 2; ++h) {
            if (can && trigger[h] && !g.trigger_was[h] && g.pointer.hit[h]) {
                xr->panel_preferred_hand = h;
            }
        }
        const int preferred = std::clamp(xr->panel_preferred_hand.load(), 0, 1);
        // (fix146) The left stick navigating (FilterGamepad): the pointer aside until either hand moves its dot clearly from where it was at the
        // last push (not within 0.3 s of one), a trigger is pulled (a touch's press too), or the menu is gone. Its mode key presses counted
        // afresh then (each return costs some: the review).
        if (g.stick_nav) {
            // (fix166) A trigger pulled while the stick navigates chooses what the stick highlighted, as A does (equipped per
            // hand for melee, nothing for the left hand if not a melee weapon): the right trigger in any menu; the left only in the
            // inventory and the favorites, where the left hand's choice is made (LeftWeapon's hooks: a melee weapon to the left hand, nothing
            // else); elsewhere the left trigger does nothing. The pointer stays aside: before, a pull brought it back and clicked where the
            // hand pointed, not on the stick's choice.
            // (A touch's press, or the left trigger in a conversation, brings the pointer back as before: the touched answer is chosen, and the
            // left trigger is never dead there; the review.)
            bool back_by_press = false;
            for (int h = 0; h < 2; ++h) {
                if (trigger[h] && !g.trigger_was[h] && shown) {
                    if (!real_trigger[h] || (h == 0 && GameFlow::isDialogueDrawnNow())) {
                        back_by_press = true;
                        continue;
                    }
                    const bool choose = h == 1 || GameFlow::isInventoryShown() || GameFlow::isFavoritesShown();
                    if (choose) {
                        g.stick_a_until = now + kStickNavPress;
                        // (Noted as the pointer's press is: the inventory's and favorites' hooks (LeftWeapon) tell the left hand's choice by it.)
                        g_last_press_hand.store(h, std::memory_order_relaxed);
                        g_last_press_ms.store(NowMsSteady(), std::memory_order_release);
                    }
                    if (g.stick_nav_logs++ < 12 && g.stick_nav_total++ < 300) {
                        spdlog::info("[MenuPointer] (fix166) the {} trigger while the stick navigates {}", h == 0 ? "left" : "right",
                            choose ? "chooses the stick's highlight (A)" : "does nothing here (the left hand chooses only in the inventory and favorites)");
                    }
                }
            }
            // (fix166) Back only after a clear move of the hand (8% of the picture, about 5 degrees; 3% before, which a still hand drifted past
            // in moments: the pointer took the menu back and its highlight jumped to the hand's dot), counted from a second after the last push.
            bool       moved  = false;
            float      most   = 0.0f;
            if (now - g.stick_nav_pushed > kStickNavSettle) {
                for (int h = 0; h < 2; ++h) {
                    if (g.pointer.hit[h]) {
                        const float d = std::hypot(g.pointer.u[h] - g.stick_nav_anchor[h][0], g.pointer.v[h] - g.stick_nav_anchor[h][1]);
                        most          = std::max(most, d);
                        moved         = moved || d > kStickNavResume;
                    }
                }
            }
            if (moved || back_by_press || !shown || !GameFlow::gStore.internalSettings.menuStickNavigate) {
                g.stick_nav       = false;
                g.stick_nav_ended = now;
                g.key_tries       = 0;
                g.session_presses = 0;
                g.session_writes  = 0;
                if (g.stick_nav_logs++ < 12 && g.stick_nav_total++ < 300) {
                    spdlog::info("[MenuPointer] the pointer back ({}; in {})", moved ? std::format("your hand moved {:.0f}% of the picture", most * 100.0f) : back_by_press ? std::string{ "a touch, or the left trigger in a conversation" } : std::string{ "no menu" },
                        GameFlow::shownMenus());
                }
            }
        }
        const int pointing  = !can || g.stick_nav ? -1 : g.pointer.hit[preferred] ? preferred : (g.pointer.hit[1 - preferred] ? 1 - preferred : -1);
        // Pointing again, or a fresh pull of the pointing hand's trigger: a few small moves again to find the cursor (and the mode
        // key again, if the game is in gamepad mode).
        if ((pointing >= 0 && g.was_pointing < 0) || (pointing >= 0 && trigger[pointing] && !g.trigger_was[pointing])) {
            g.wake_tries    = 0;
            g.key_tries     = 0;
            g.forced        = false;
            g.last_activity = now;
            if (!g.placing) {
                g.placing          = true; // (placing tried again)
                g.off_since        = {};
                g.off_checks       = 0;
                g.placed_client[0] = g.placed_client[1] = -100000;
            }
        }
        g.was_pointing = pointing;
        GameFlow::dialoguePointing.store(dialogue && pointing >= 0);

        // The game's cursor in the menu showing (read while you point at it; none read: a menu without a cursor to steer, such as
        // build mode: then the pointer only shows, and the trigger stays the gamepad's).
        UiProbe::WantGameMouse(pointing >= 0 || g.active);
        auto       m        = pointing >= 0 ? UiProbe::LastGameMouse(g.movie) : UiProbe::GameMouse{};
        // (fix146) The conversation on its own panel: its movie is in a box of the picture (ConversationPanel, centred), and the game gives
        // its cursor from that box's corner (fix145's test: the cursor on the touched answer read 513 px above it, the box's top, so the
        // presses were dropped as not arrived): the box is the menu's area here, as a full-screen menu's 16:9 box is.
        if (int box_w = 0, box_h = 0; m.read && m.movie.find("DialogueMenu") != std::string::npos &&
                                                ConversationPanel::MovieBox((int)vr->get_backbuffer_size()[0], (int)vr->get_backbuffer_size()[1], box_w, box_h)) {
            m.buffer_w = box_w;
            m.buffer_h = box_h;
        }
        const bool steering = pointing >= 0 && m.read && m.buffer_w > 0 && m.buffer_h > 0;
        const int  hand     = steering ? pointing : -1;
        const bool may_send = fresh && g.foreground;
        // (FilterGamepad, at the end of this poll: only while the pointer is in use, so a hand resting with its beam on the panel
        // leaves the sticks and shoulder buttons to the game.)
        g.key_due      = false;
        g.may_send_now = may_send;

        if (steering) {
            if (!g.active) {
                g.active = true;
                InputBlocker::get().take_counts(); // (counted from here)
                g.active_since = now;
                g.next_log     = now + std::chrono::seconds(3);
                spdlog::info("[MenuPointer] pointing at the menu with the {} hand", HandName(hand));
            }
            g.hand = hand;
            if (!g.foreground) {
                ++g.not_in_front;
            }

            // Another menu (a pop-up, a sub-menu): its own cursor place and edges, seen to move again before it is steered.
            // (fix146: or the same menu in another box: the conversation's panel and the picture, the review.)
            if (m.movie != g.movie || m.buffer_w != g.buffer[0] || m.buffer_h != g.buffer[1]) {
                g.movie       = m.movie;
                g.movie_since = now;
                g.woken       = false;
                g.wake_tries  = 0;
                g.placing     = true; // (each menu: placing first, its own offset)
                g.off_since   = {};
                g.off_checks  = 0;
                g.offset[0] = g.offset[1] = 0.0f;
                g.have_miss   = false;
                g.recent      = {};
                g.placed_client[0] = g.placed_client[1] = -100000;
                ResetSteering();
                g.wall_lo[0] = g.wall_lo[1] = 0.0f;
                g.wall_hi[0]                = (float)m.buffer_w - 1.0f;
                g.wall_hi[1]                = (float)m.buffer_h - 1.0f;
            }
            g.buffer[0] = m.buffer_w;
            g.buffer[1] = m.buffer_h;

            // Where the game's cursor should be: the place on the picture, in the menu movie's own pixels (the area it draws in,
            // centred in the picture: the game's 16:9 box), within where its cursor can go.
            const auto  size = vr->get_backbuffer_size();
            const float W = (float)size[0], H = (float)size[1];
            const float x = g.pointer.u[hand] * W - (W - (float)m.buffer_w) * 0.5f;
            const float y = g.pointer.v[hand] * H - (H - (float)m.buffer_h) * 0.5f;
            g.target[0]      = std::clamp(x, std::max(0.0f, g.wall_lo[0]), std::min((float)m.buffer_w - 1.0f, g.wall_hi[0]));
            g.target[1]      = std::clamp(y, std::max(0.0f, g.wall_lo[1]), std::min((float)m.buffer_h - 1.0f, g.wall_hi[1]));
            g.target_clamped = g.target[0] != x || g.target[1] != y;
            if (std::hypot(g.target[0] - g.activity_anchor[0], g.target[1] - g.activity_anchor[1]) > kActiveMove) {
                g.activity_anchor[0] = g.target[0];
                g.activity_anchor[1] = g.target[1];
                g.last_activity      = now;
            }
            const bool in_use = now - g.last_activity < kActive;

            // The game's input mode. In gamepad mode its cursor is hidden and does not follow the mouse: F13 (a key nothing uses) is
            // pressed for a moment, as a keyboard press switches the game back to the mouse and keyboard (once the gamepad's buttons
            // have been let go a moment: one held would switch it back at once); after three presses that did not, the mode byte is
            // written. Back in mouse mode, its cursor may have been put elsewhere: seen to move again before it is steered.
            const int  raw          = InputMode::Raw();
            const bool gamepad_mode = raw == 0 || raw == 1;
            if (raw != g.mode_seen) {
                if (g.mode_seen != -2 && raw >= 0) {
                    spdlog::info("[MenuPointer] the game's input mode is now {} ({:#x})", ModeName(raw), raw);
                    ++(gamepad_mode ? g.to_gamepad : g.to_mouse);
                }
                if (raw > 1 && (g.mode_seen == 0 || g.mode_seen == 1)) {
                    g.movie_since = now;
                    g.woken       = false;
                    g.wake_tries  = 0;
                    ResetSteering();
                    // (A click waiting: it waits for the cursor from here.)
                    if (g.click_pending) {
                        g.click_deadline = std::min(now + kClickWait, g.click_started + kClickMost);
                    }
                }
                if (raw > 1) {
                    g.mouse_since = now;
                }
                g.mode_seen = raw;
            }
            // (Back to counting from nothing only once the game has stayed in mouse mode a while: not when it went straight back.)
            if (raw > 1 && now - g.mouse_since > kMouseSettled) {
                g.key_tries = 0;
                g.forced    = false;
            }
            if (gamepad_mode) {
                g.woken = false;
                g.error = 1e9f;
                // (A click waiting: it waits for the switch, a while.)
                if (g.click_pending) {
                    g.click_deadline = std::max(g.click_deadline, std::min(now + kClickWait, g.click_started + kClickMost));
                }
            }
            // The press (or the write) is done at the end of this poll, when no gamepad button is held in its report.
            const bool may_press = in_use && may_send && !g.key_down && !g.key_up_owed && now - g.buttons_down_at > kButtonsQuiet;
            if (may_press && gamepad_mode && now - g.last_key > kKeyEvery && (g.key_tries < kKeyTries || !g.forced)) {
                g.key_due       = true;
                g.key_due_known = true;
            } else if (may_press && raw < 0 && !g.woken && g.key_tries < kKeyTries && now - g.last_key > std::chrono::milliseconds(1000)) {
                // (The mode not known: pressed now and then while the cursor has not been seen to move.)
                g.key_due       = true;
                g.key_due_known = false;
            }

            // The menu's cursor seen to move since it came up (its first place may be an old one: a menu shows no cursor until the
            // mouse moves); until then, a small move now and then (2, 4, 8 counts), away from the nearer edge.
            if (!gamepad_mode && !g.placing && !g.woken && m.changed_ms > 0 && m.changed_ms >= Ms(g.movie_since) && m.changed_ms >= Ms(g.woke_at)) {
                g.woken = true;
                ResetSteering();
            }
            if (gamepad_mode) {
                // (Nothing placed or steered: the cursor is hidden until the game is back in mouse mode.)
                mouse_override::active = false;
                g.placed_client[0] = g.placed_client[1] = -100000;
            } else if (g.placing) {
                // (Only while the pointer is in use, or a click is under way: a hand resting with its beam on the panel leaves a
                // real mouse to the game.)
                if (in_use || g.click_pending || g.button_down) {
                    Place(m, now, may_send, window, W, H);
                }
            } else if (!g.woken) {
                g.error = 1e9f;
                const bool new_reading = Ms(now) - m.at_ms <= kReadingAge;
                if (may_send && new_reading && g.wake_tries < kWakeTries && now - g.woke_at > std::chrono::milliseconds(300)) {
                    const int size_of = 2 << g.wake_tries;
                    SendMove(m.x < (float)m.buffer_w * 0.5f ? size_of : -size_of, 0, window, false);
                    g.woke_at = now;
                    ++g.wake_tries;
                }
            } else {
                Steer(m, now, may_send, window);
            }
        }

        g.filtering = steering && now - g.last_activity < kActive;
        // The game's mouse messages given the last place while the pointer is in use or a click is under way (a moment without
        // the beam on the panel keeps it: a click then lands there); else they are its own again, for a real mouse.
        {
            const bool mouse_mode = g.mode_seen != 0 && g.mode_seen != 1;
            if (!(g.active && g.placing && mouse_mode && (now - g.last_activity < kActive || g.click_pending || g.button_down))) {
                mouse_override::active = false;
            }
        }

        // The button: a fresh pull of the pointing hand's trigger presses it once the game's cursor is under the dot (or after a
        // moment); letting go of that trigger lets it go (held: a drag, the cursor still following). No panel this very moment,
        // the mod's menu opening, or the game's window going behind, lets it go at once (a press never reaches the game outside a
        // menu).
        if ((g.button_down || g.click_pending) && (!allowed || !g.foreground || !fresh)) {
            Release();
        }
        if (g.active && hand >= 0 && !g.button_down && !g.click_pending && trigger[hand] && !g.trigger_was[hand]) {
            g.click_pending  = true;
            g.tap_only       = dialogue; // (a conversation: a tap, never held into play as it closes)
            g.click_started  = now;
            // (fix146: right after the left stick navigated, the game in gamepad mode: the cursor has to come back first, a longer wait.)
            g.click_deadline = now + (now - g.stick_nav_ended < std::chrono::seconds(1) ? kClickMost : kClickWait);
            g.button_hand    = hand;
        }
        if (g.click_pending) {
            // (Placing: the cursor found where it was put a moment ago, the press itself bringing the place; steering: under the dot.)
            const bool arrived = g.placing ? g.woken && now - g.confirmed_at < std::chrono::milliseconds(300)
                                           : g.woken && !g.waiting && g.error <= kThere * 2.0f;
            // (A quick tap, let go before the cursor arrived: it still waits for it; the button is then let go after the
            // shortest hold.)
            if (arrived || now >= g.click_deadline) {
                // (Not with no panel this very moment, nor with the cursor never seen to follow: it would click somewhere unknown.)
                if (!fresh || !can || !g.foreground || !g.woken || !SendPress(window, g.placing)) {
                    g.click_pending = false;
                    g.button_hand   = -1;
                    ++g.clicks_dropped;
                } else {
                    g.button_down    = true;
                    g.button_down_at = now;
                    g.click_pending  = false;
                    ++g.clicks;
                    // (fix154) Which hand's trigger pressed it, and when (LeftWeapon: the left trigger's equip in the inventory).
                    g_last_press_hand.store(g.button_hand, std::memory_order_relaxed);
                    g_last_press_ms.store(NowMsSteady(), std::memory_order_release);
                    if (!arrived) {
                        ++g.clicks_early;
                    }
                }
            }
        }
        if (g.button_down && (!trigger[g.button_hand] || g.tap_only) && now - g.button_down_at >= kClickHold) {
            Release();
        }
        if (!g.button_down && !g.click_pending && g.active && !can) {
            // The menu has gone, or the mod's menu is open.
            Log(true);
            Stop();
        }

        // The pointing hand's trigger (while the pointer is in use in this menu), or a trigger that went down then, is not the
        // gamepad's.
        int consumed = 0;
        for (int h = 0; h < 2; ++h) {
            const bool mine = g.active && h == pointing;
            if (trigger[h] && (mine || ((g.button_down || g.click_pending) && h == g.button_hand))) {
                g.trigger_owned[h] = true;
            } else if (!trigger[h]) {
                g.trigger_owned[h] = false;
            }
            if (g.trigger_owned[h] || mine) {
                consumed |= 1 << h;
            }
            g.trigger_was[h] = trigger[h];
        }

        if (g.active && now >= g.next_log) {
            Log(false);
        }
        g.last_status = !xr->panel_pointer_enabled.load() ? "Menu pointer: off."
                        : g.active ? fmt::format("Menu pointer: the {} hand points{}; the game's input mode {}; its cursor {} (cursor speed {:.2f} per count).",
                                         HandName(g.hand), g.foreground ? "" : " (the game's window is not in front: nothing is sent)",
                                         ModeName(g.mode_seen < -1 ? -1 : g.mode_seen),
                                         g.error < 1e8f ? fmt::format("{:.0f} away", g.error) : std::string{ "not seen to move yet" }, g.gain[0])
                                   : "Menu pointer: ready (point at a menu with a controller; the trigger clicks).";
        return consumed;
    }

    void FilterGamepad(_XINPUT_GAMEPAD* pad, float left_y, float right_y)
    {
        if (pad == nullptr) {
            return;
        }
        std::scoped_lock _{ g_mutex };
        const auto     now        = Clock::now();
        constexpr WORD kShoulders = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
        // (fix146) The left stick (or the D-pad: the left grip's) pushed with a menu or a conversation shown: the game's own navigation (the
        // highlight moved, as on a gamepad). The pointer steps aside (Update: not pointing) until your hand moves clearly or a trigger is
        // pulled, so it does not take the highlight back to where its cursor is; the stick and the D-pad reach the game meanwhile, the
        // triggers and shoulder buttons not (a thumb resting on a thumbrest is a shoulder button: the menu's next tab; the review).
        {
            constexpr WORD kDpad   = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
            // (fix163: half way in a conversation: each push switched the game to the gamepad and back, its cursor put back in the middle.)
            const int      push   = GameFlow::isDialogueDrawnNow() ? 16384 : kStickNavPush;
            const bool     pushed = std::abs((int)pad->sThumbLX) > push || std::abs((int)pad->sThumbLY) > push || (pad->wButtons & kDpad) != 0;
            // (fix175) Never in build mode with no menu over it (no panel there since fix175; its left stick walks, as the game means): each push
            // turned this on before and kept LB, RB, LT and RT from the game, the right trigger made A (build). A panel just gone is no menu.
            const bool     building = GameFlow::isWorkshopShown() && !GameFlow::isShowingMenu();
            if (pushed && !building && GameFlow::gStore.internalSettings.menuStickNavigate &&
                (g.filtering || g.stick_nav || now - g.last_shown < std::chrono::milliseconds(150))) {
                if (!g.stick_nav) {
                    g.stick_nav = true;
                    if (g.stick_nav_logs++ < 12 && g.stick_nav_total++ < 300) {
                        spdlog::info("[MenuPointer] the left stick navigates the menu: the pointer steps aside until your hand moves or a trigger is pulled");
                    }
                }
                // (Each push: where each hand points taken anew, so slow drift over a long stretch does not count as a move; the review.)
                for (int h = 0; h < 2; ++h) {
                    g.stick_nav_anchor[h][0] = g.pointer.u[h];
                    g.stick_nav_anchor[h][1] = g.pointer.v[h];
                }
                g.stick_nav_pushed = now;
                g.key_due          = false; // (no mode key into a stick's navigation: the game would flip back and forth; the review)
            }
            if (g.stick_nav) {
                pad->bLeftTrigger = pad->bRightTrigger = 0;
                pad->wButtons &= ~kShoulders;
                if (now < g.stick_a_until) {
                    pad->wButtons |= XINPUT_GAMEPAD_A; // (fix166: a trigger chose the stick's highlight)
                }
                g.filtering = false; // (this report: the sticks to the game, the wheel not)
                g.wheel_on  = false;
                return;
            }
        }
        // (A button that still reaches the game: the game is in gamepad mode until it is let go; then the mode key.)
        const bool buttons_now = (pad->wButtons & ~(g.filtering ? kShoulders : 0)) != 0;
        if (buttons_now) {
            g.buttons_down_at = now;
        }
        // The mode key (Update found it due): pressed, or after three presses the mode byte written; not with a gamepad button held
        // in this very report (it would switch the game straight back), and at most a few times while you point at one menu.
        if (g.key_due) {
            g.key_due = false;
            if (!buttons_now) {
                if (g.key_tries < kKeyTries) {
                    if (g.session_presses < kSessionPresses) {
                        if (SendKey(true)) {
                            g.key_down    = true;
                            g.key_down_at = now;
                            ++g.key_presses;
                        }
                        ++g.session_presses;
                    } else if (!g.capped_logged) {
                        g.capped_logged = true;
                        spdlog::info("[MenuPointer] the mode key was pressed {} times in this menu: no more until you point at another", kSessionPresses);
                    }
                    g.last_key = now;
                    ++g.key_tries;
                } else if (g.key_due_known && !g.forced && g.session_writes < kSessionWrites) {
                    g.forced   = true;
                    g.last_key = now;
                    ++g.session_writes;
                    if (InputMode::ForceMouse()) {
                        ++g.mode_writes;
                        spdlog::info("[MenuPointer] three presses of the mode key did not switch the game to the mouse: its mode byte written");
                    }
                }
            }
        }
        if (!g.filtering) {
            g.wheel_on         = false;
            g.touch_wheel_owed = 0;
            return;
        }
        // While you point at the menu: no sticks, triggers or shoulder buttons for the game (a resting thumb or finger, the grip
        // that aims, the thumbrest's shoulder button: each would switch it to gamepad mode and hide its cursor).
        pad->sThumbLX = pad->sThumbLY = pad->sThumbRX = pad->sThumbRY = 0;
        pad->bLeftTrigger = pad->bRightTrigger = 0;
        pad->wButtons &= ~kShoulders;

        // (fix146) A finger's drag on the conversation's panel: its notches, one a poll (the game keeps one wheel record a frame).
        if (g.touch_wheel_owed != 0) {
            if (g.may_send_now) {
                const int sign = g.touch_wheel_owed > 0 ? 1 : -1;
                SendWheel(sign * WHEEL_DELTA);
                g.touch_wheel_owed -= sign;
                ++g.wheel_notches;
            }
            g.wheel_on = false;
            return;
        }
        // A stick pushed up or down scrolls (the mouse wheel: up away from you), faster the further it is pushed: a notch at once,
        // then 3 to 15 a second. (fix146: the right stick only, with the left navigating; the left too with that off.)
        const bool  left_scrolls = !GameFlow::gStore.internalSettings.menuStickNavigate;
        const float y            = left_scrolls && std::abs(left_y) >= std::abs(right_y) ? left_y : right_y;
        const float push = std::abs(y);
        if (push < kWheelDeadzone || !g.may_send_now) {
            g.wheel_on = false;
            return;
        }
        if (!g.wheel_on) {
            g.wheel_on = true;
            g.wheel    = 1.0f;
            g.wheel_at = now;
        }
        const float dt = std::clamp(std::chrono::duration<float>(now - g.wheel_at).count(), 0.0f, 0.1f);
        g.wheel_at     = now;
        g.wheel += (3.0f + 12.0f * (push - kWheelDeadzone) / (1.0f - kWheelDeadzone)) * dt;
        if (g.wheel >= 1.0f) {
            g.wheel = std::min(g.wheel - 1.0f, 1.0f);
            SendWheel(y > 0.0f ? WHEEL_DELTA : -WHEEL_DELTA);
            ++g.wheel_notches;
            if (GameFlow::isStarMapShown()) { // (fix175: the star map's zoom, for its log)
                ++(y > 0.0f ? g_star_zoom.wheel_in : g_star_zoom.wheel_out);
            }
        }
    }

    // (fix203) The right stick's zoom in one place (the star map, character creation), its own count for the log (below; under g_mutex).
    static void ZoomByStick(_XINPUT_GAMEPAD* pad, StarZoom& z, bool shown, bool chargen);

    void StarMapZoom(_XINPUT_GAMEPAD* pad)
    {
        // (fix175) The star map zoomed with the right stick. The game zooms its star map with the triggers (RT in, LT out) or the mouse wheel,
        // and turns it with the right stick (a helper's research of the exe's control map; eip.gg's menu controls agree). Pointing at it, the
        // right stick's up and down are the wheel already (FilterGamepad); with the left stick moving the game's own cursor, or navigating,
        // nothing zoomed: LT had no button there (the right grip is RB, the left trigger nothing) and RT only with the beam off the panel. Now
        // its push up is RT and down is LT, as far as the stick is pushed (a gamepad's triggers: the game stays in gamepad mode), only a push
        // mostly up or down; its left and right stay the game's turn, the up and down never reach the game there (no tilt by the stick).
        // Only while the star map is drawn this moment and is the menu shown (never into play, where RT fires).
        // (fix203) Character creation the same way: the game zooms its character with LT and RT (the menu's own button handler: RT brings it
        // nearer, the value starting at its farthest) and turns it with the right stick's left and right only (its up and down unused there).
        // Pointing, the triggers are the clicks and the stick's up and down the lists' wheel; navigating with the left stick, the triggers
        // choose; so no zoom at all before. Now, while you do not point, the right stick's up is RT (nearer) and down LT (farther).
        if (pad == nullptr) {
            return;
        }
        std::scoped_lock _{ g_mutex };
        ZoomByStick(pad, g_star_zoom, GameFlow::isStarMapShown(150) && GameFlow::isShowingMenu(), false);
        ZoomByStick(pad, g_chargen_zoom, GameFlow::isChargenShown(100) && GameFlow::isShowingMenu(), true); // (fix203: never into play)
    }

    static void ZoomByStick(_XINPUT_GAMEPAD* pad, StarZoom& z, bool shown, bool chargen)
    {
        // (Under g_mutex.)
        if (!shown) {
            if (z.shown && (z.wheel_in + z.wheel_out + z.push_in + z.push_out) > 0 && z.logs < 30) {
                ++z.logs;
                if (chargen) {
                    spdlog::info("[MenuPointer] (fix203) character creation closed: the right stick zoomed in {} and out {} times (as RT and LT, "
                                 "while not pointing)",
                        z.push_in, z.push_out);
                } else {
                    spdlog::info("[MenuPointer] (fix175) star map closed: the right stick zoomed in {} and out {} times (as the mouse wheel {} and {} "
                                 "notches while pointing at it; as RT and LT {} and {} pushes otherwise)",
                        z.wheel_in + z.push_in, z.wheel_out + z.push_out, z.wheel_in, z.wheel_out, z.push_in, z.push_out);
                }
            }
            if (z.shown) {
                const int logs = z.logs;
                z              = StarZoom{};
                z.logs         = logs;
            }
            return;
        }
        z.shown = true;
        if (g.filtering) { // (this report was the pointer's: the wheel, above)
            z.sign_was = 0;
            return;
        }
        const float x    = static_cast<float>(pad->sThumbRX) / 32767.0f;
        const float y    = static_cast<float>(pad->sThumbRY) / 32767.0f;
        const float push = std::abs(y);
        pad->sThumbRY    = 0;
        if (push < kZoomDeadzone || push < std::abs(x)) {
            z.sign_was = 0;
            return;
        }
        const int   sign   = y > 0.0f ? 1 : -1;
        const float amount = std::clamp((push - kZoomDeadzone) / (1.0f - kZoomDeadzone), 0.0f, 1.0f);
        const auto  value  = static_cast<BYTE>(std::lround(kZoomTriggerLeast + (255.0f - kZoomTriggerLeast) * amount));
        BYTE&       lane   = sign > 0 ? pad->bRightTrigger : pad->bLeftTrigger;
        lane               = std::max(lane, value);
        if (sign != z.sign_was) {
            ++(sign > 0 ? z.push_in : z.push_out);
        }
        z.sign_was = sign;
    }

    void StickSideways(_XINPUT_GAMEPAD* pad, float left_x)
    {
        // (fix147) The left stick's left and right in trading's inventory go to the other trader, yourself or your ship, as LB does: the left
        // stick's left or right is LB or RB for 80 ms. (fix166) Only a quick flick (past 0.6 and back inside 0.3 within 0.35 s), sent as it comes
        // back: a push held longer reaches the game as it is (the left stick is needed to move through every menu screen: the stick's left
        // and right were always kept from the game, its lists' sideways moves and the quantity slider with them). Read from the stick it holds
        // back (the game's left: the physical right with the hands swapped; the review).
        // (fix169) The left stick navigates every menu; it could not go left and right in the missions menu (a
        // helper's research of each menu's own code and of the game's control map, rebuilt from the exe): in a menu the game turns the stick
        // into arrow keys, and several menus move sideways only on the D-pad's left and right ("Left"/"Right") or on LB and RB, which the
        // stick never makes (and while it navigates, FilterGamepad keeps LB and RB from the game). Per menu now (GameFlow::stickSidewaysMenu):
        //  - the missions' categories and the ship crew's tabs: the stick's left or right is the D-pad's, held until the stick comes back
        //    (one tab a push: the game moves it as the button is let go);
        //  - the skills' and research's tabs, the ship menu's ships (as chosen): a quick flick is LB or RB; a push held longer
        //    reaches the game as it is (the ship menu's power, the skills' cursor);
        //  - the almanac: a flick is LB or RB (its pages), a push held longer the D-pad's left or right (its categories);
        //  - trading and containers: a flick either way is LB (the next inventory: the trader or container, you, your ship); never RB (in a
        //    container "take or store all resources"; in trading only the amount slider's jump).
        // Everywhere else the stick's left and right are the game's, as before (the stick already moves sideways there).
        (void)left_x;
        static bool              out{ false };
        static bool              held{ false };   // (held past a flick's time)
        static bool              centre{ false }; // (the stick to come back to the middle before the next push counts)
        static Clock::time_point out_since{};
        static int               out_dir{ 0 };
        static int               out_kind{ GameFlow::kSidewaysNone };
        static WORD              button{ 0 };
        static WORD              dpad{ 0 }; // (the D-pad's left or right, pressed for a moment: dpad_until)
        static Clock::time_point until{}, dpad_until{};
        static int               logs{ 0 };
        constexpr auto           kFlick = std::chrono::milliseconds(350);
        constexpr auto           kPress = std::chrono::milliseconds(90); // (a button's press: down and up again, as a tap)
        if (pad == nullptr) {
            return;
        }
        std::scoped_lock _{ g_mutex };
        const auto  now  = Clock::now();
        const auto& s    = GameFlow::gStore.internalSettings;
        const int   kind = GameFlow::stickSidewaysMenu();
        const bool  trade = kind == GameFlow::kSidewaysTrading || kind == GameFlow::kSidewaysContainer;
        // (A menu showing now too: these menus count for 300 ms after they were last drawn, and a push then would be LB or RB in play; the
        // review.)
        if (kind == GameFlow::kSidewaysNone || !(trade ? s.tradeStickTabs : s.menuStickNavigate) || !GameFlow::isShowingMenu()) {
            out    = false;
            held   = false;
            centre = std::abs(static_cast<int>(pad->sThumbLX)) > 9800; // (a push begun outside these menus does not count in one; the review)
            button = 0;
            dpad   = 0;
            return;
        }
        static constexpr const char* kNames[GameFlow::kSidewaysKinds] = { "", "the missions or ship crew menu", "the skills or research menu",
            "the almanac", "trading", "a container", "the ship menu" };
        // (Not with the left grip's D-pad left or right: that is the D-pad. Pressed during a push, the push is given up, not taken for a
        // flick; the review.)
        const bool real_dpad = (pad->wButtons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) != 0;
        const int  x         = static_cast<int>(pad->sThumbLX);
        const int  y         = static_cast<int>(pad->sThumbLY);
        // (Only while the stick navigates, or nothing is kept from the game: else a button would put the game in gamepad mode under the
        // pointer, its cursor hidden; the review.)
        const bool sent    = g.stick_nav || !g.filtering;
        const WORD dpad_of = out_dir < 0 ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_RIGHT;
        const auto told    = [&](const char* verb, const char* how) {
            if (logs++ < 20) {
                spdlog::info("[MenuPointer] (fix169) {}: the left stick {} {} is {}{}", kNames[out_kind], verb, out_dir < 0 ? "left" : "right", how,
                    sent ? "" : " (not sent: the pointer in use)");
            }
        };
        if (out && (kind != out_kind || real_dpad)) {
            out    = false; // (another menu meanwhile, or the real D-pad: this push not counted, the next after the stick comes back)
            centre = true;
        }
        if (centre && std::abs(x) < 9800 && !real_dpad) {
            centre = false;
        }
        // (A push mostly sideways: a list scrolled with the thumb a little off up or down is not a tab; the review.)
        if (!out && !centre && !real_dpad && std::abs(x) > 19600 && std::abs(x) > (std::abs(y) * 3) / 2) {
            out       = true;
            held      = false;
            out_since = now;
            out_dir   = x < 0 ? -1 : 1;
            out_kind  = kind;
            // (A press of the D-pad, down and up within 90 ms, once a push: the game moves the tab once whether it acts as the button goes
            // down or as it comes up; the review.)
            if (kind == GameFlow::kSidewaysDpad) {
                if (sent) {
                    dpad       = out_dir < 0 ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_RIGHT;
                    dpad_until = now + kPress;
                }
                told("pushed", out_dir < 0 ? "the D-pad's left (the tab before)" : "the D-pad's right (the next tab)");
            }
        }
        if (out && !held && now - out_since > kFlick) {
            held = true;
            if (kind == GameFlow::kSidewaysAlmanac) {
                if (sent) {
                    dpad       = dpad_of;
                    dpad_until = now + kPress;
                }
                told("held", out_dir < 0 ? "the D-pad's left (the category before)" : "the D-pad's right (the next category)");
            }
        }
        if (out && std::abs(x) < 9800) {
            out = false;
            if (!held && kind != GameFlow::kSidewaysDpad) {
                const WORD shoulder = trade || out_dir < 0 ? XINPUT_GAMEPAD_LEFT_SHOULDER : XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (sent) {
                    button = shoulder;
                    until  = now + kPress;
                }
                told("flicked", trade ? "LB (the next inventory)" : shoulder == XINPUT_GAMEPAD_LEFT_SHOULDER ? "LB (the one before)" : "RB (the next)");
            }
        }
        // Its left and right kept from the game while the D-pad makes the move, and while a flick is still being told from a held push; held
        // longer elsewhere, they reach it.
        if (out && (kind == GameFlow::kSidewaysDpad || kind == GameFlow::kSidewaysAlmanac || !held)) {
            pad->sThumbLX = 0;
        }
        if (dpad != 0 && now < dpad_until) {
            pad->wButtons |= dpad;
        } else {
            dpad = 0;
        }
        if (button != 0 && now < until) {
            pad->wButtons |= button;
        } else {
            button = 0;
        }
    }

    void Watchdog()
    {
        std::scoped_lock _{ g_mutex };
        // The game stopped asking for the gamepad (it lost the front, paused, ...), or its window went behind, with the button held:
        // it is let go (and a release Windows did not take is sent again).
        const bool stalled = Clock::now() - g.last_update > std::chrono::milliseconds(250);
        if (g.release_owed || ((g.button_down || g.click_pending) && (stalled || !InFront(g_framework->get_window())))) {
            Release();
        }
        if ((g.key_down && (stalled || Clock::now() - g.key_down_at >= kKeyHold * 3)) || g.key_up_owed) {
            KeyUp();
        }
        if (g.active && stalled) {
            Log(true);
            Stop();
        }
        // (fix142: the polls stopped: no longer pointing at a conversation, its movie drawn as usual.)
        if (stalled) {
            GameFlow::dialoguePointing.store(false);
        }
    }

    void HoldModeKey()
    {
        // (fix173) As a face button that reaches the game: it switches the game to gamepad mode while held, so the mode key waits until it has
        // been let go a while (kButtonsQuiet), and not in this very poll (a key with it would switch the game straight back; the review).
        std::scoped_lock _{ g_mutex };
        g.buttons_down_at = Clock::now();
        g.key_due         = false;
    }

    bool LastPress(int& hand, int64_t& at_ms)
    {
        at_ms = g_last_press_ms.load(std::memory_order_acquire);
        hand  = g_last_press_hand.load(std::memory_order_relaxed);
        return at_ms != 0 && hand >= 0;
    }

    std::string Status()
    {
        std::scoped_lock _{ g_mutex };
        return g.last_status;
    }
}
