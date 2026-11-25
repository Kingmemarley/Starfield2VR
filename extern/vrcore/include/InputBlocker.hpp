// vrcore - keeps mouse/keyboard input away from the game while the menu is open.
// Hooks the cursor functions (so the game can't lock or recenter the cursor), raw input
// buffering, and DirectInput device state/data reads.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <Windows.h>

#include "utility/FunctionHook.hpp"

class InputBlocker {
public:
    static InputBlocker& get();

    void install();

    void set_blocking(bool block);
    bool is_blocking() const { return m_blocking.load(); }

    // The menu pointer (the game side): while on, the game's own calls (from its .exe) to GetCursorPos and
    // GetPhysicalCursorPos see this point (screen coordinates) instead of where the real cursor is.
    void set_cursor_override(bool on, POINT screen = {});
    bool cursor_override() const { return m_cursor_override.load(); }
    // The real ones, past the hooks (for the pointer's own use).
    BOOL real_get_cursor_pos(POINT* point) const;
    BOOL real_set_cursor_pos(int x, int y) const;

    // What the game itself calls (from its .exe), counted for the menu pointer's log: how its menu cursor follows the mouse.
    struct Counts {
        uint32_t get_cursor_pos{0};
        uint32_t get_physical_cursor_pos{0};
        uint32_t set_cursor_pos{0};
        uint32_t clip_cursor{0};
        uint32_t raw_buffer_calls{0};
        uint32_t raw_buffer_mouse{0};  // mouse records it got from GetRawInputBuffer
        uint32_t raw_data_mouse{0};    // mouse records it got from GetRawInputData
        uint32_t dinput_reads{0};
        uintptr_t get_cursor_pos_caller{0}; // (the last caller's rva in the game)
        uintptr_t set_cursor_pos_caller{0};
        uintptr_t raw_caller{0};
        int set_cursor_pos_last[2]{};
    };
    // The counts since the last call (and starts counting again).
    Counts take_counts();

private:
    InputBlocker() = default;

    static BOOL WINAPI set_cursor_pos(int x, int y);
    static BOOL WINAPI clip_cursor(const RECT* rect);
    static BOOL WINAPI get_cursor_pos(LPPOINT point);
    static BOOL WINAPI get_physical_cursor_pos(LPPOINT point);
    static UINT WINAPI get_raw_input_data(HRAWINPUT raw, UINT command, LPVOID data, PUINT size, UINT header_size);
    static UINT WINAPI get_raw_input_buffer(PRAWINPUT data, PUINT size, UINT header_size);
    static HRESULT WINAPI dinput_get_device_state(void* device, DWORD size, LPVOID data);
    static HRESULT WINAPI dinput_get_device_data(void* device, DWORD object_size, void* data, LPDWORD in_out, DWORD flags);

    void hook_dinput();

    std::atomic<bool> m_blocking{false};
    bool m_installed{false};

    std::atomic<bool> m_cursor_override{false};
    std::atomic<int32_t> m_cursor_x{0};
    std::atomic<int32_t> m_cursor_y{0};

    // The game's .exe (its calls are the ones counted and given the pointer's point).
    uintptr_t m_game_begin{0};
    uintptr_t m_game_end{0};
    bool from_game(void* return_address) const {
        const auto at = (uintptr_t)return_address;
        return at >= m_game_begin && at < m_game_end;
    }
    uintptr_t game_rva(void* return_address) const { return (uintptr_t)return_address - m_game_begin; }

    struct AtomicCounts {
        std::atomic<uint32_t> get_cursor_pos{0}, get_physical_cursor_pos{0}, set_cursor_pos{0}, clip_cursor{0}, raw_buffer_calls{0},
            raw_buffer_mouse{0}, raw_data_mouse{0}, dinput_reads{0};
        std::atomic<uintptr_t> get_cursor_pos_caller{0}, set_cursor_pos_caller{0}, raw_caller{0};
        std::atomic<int> set_x{0}, set_y{0};
    } m_counts{};

    std::unique_ptr<FunctionHook> m_set_cursor_pos_hook{};
    std::unique_ptr<FunctionHook> m_clip_cursor_hook{};
    std::unique_ptr<FunctionHook> m_get_raw_input_buffer_hook{};
    std::unique_ptr<FunctionHook> m_get_cursor_pos_hook{};
    std::unique_ptr<FunctionHook> m_get_physical_cursor_pos_hook{};
    std::unique_ptr<FunctionHook> m_get_raw_input_data_hook{};
    std::unique_ptr<FunctionHook> m_get_device_state_hook{};
    std::unique_ptr<FunctionHook> m_get_device_data_hook{};
};
