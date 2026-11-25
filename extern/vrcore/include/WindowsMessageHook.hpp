#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <Windows.h>

#define RE_TOGGLE_CURSOR WM_APP + 1

// The game's mouse place, given by the window's mouse messages (the menu pointer, the game side). Starfield puts its menu cursor
// where its mouse-move messages say the mouse is (its window procedure keeps their place for the mouse device), so while `active`
// the place in each mouse message passed to the game's window procedure is this one (client pixels; it may lie outside the
// screen: the game's window can be taller than it). Messages the mod's own menu takes are not changed.
namespace mouse_override {
inline std::atomic<bool> active{false};
inline std::atomic<uint64_t> place{0};     // x in the low 32 bits, y in the high (both as int32: one value, never half old)
inline std::atomic<uint64_t> rewritten{0}; // (messages changed so, for the log)
inline void set(int32_t x, int32_t y) {
    place.store((uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), std::memory_order_release);
}
}

// This type of hook replaces a windows message procedure so that it can intercept
// messages sent to the window.
class WindowsMessageHook {
public:
    std::function<bool(HWND, UINT, WPARAM, LPARAM)> on_message;

    WindowsMessageHook() = delete;
    WindowsMessageHook(const WindowsMessageHook& other) = delete;
    WindowsMessageHook(WindowsMessageHook&& other) = delete;
    WindowsMessageHook(HWND wnd);
    virtual ~WindowsMessageHook();

    // This gets called automatically by the destructor but you can call it
    // explicitly if you need to remove the message hook for some reason.
    bool remove();

    auto is_valid() const {
        return m_original_proc != nullptr;
    }

    auto get_original() const {
        return m_original_proc;
    }

    inline void window_toggle_cursor(bool show) {
        ::PostMessage(m_wnd, RE_TOGGLE_CURSOR, show, 1);
    }

    WindowsMessageHook& operator=(const WindowsMessageHook& other) = delete;
    WindowsMessageHook& operator=(const WindowsMessageHook&& other) = delete;

    bool is_hook_intact();

private:
    HWND m_wnd;
    WNDPROC m_original_proc;
};