#include <algorithm>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "utility/Thread.hpp"

#include "WindowsMessageHook.hpp"

using namespace std;

static WindowsMessageHook* g_windows_message_hook{ nullptr };
std::recursive_mutex g_proc_mutex{};

LRESULT WINAPI window_proc(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    std::lock_guard _{ g_proc_mutex };

    if (g_windows_message_hook == nullptr) {
        return 0;
    }

    // Call our onMessage callback.
    auto& on_message = g_windows_message_hook->on_message;

    if (on_message) {
        // If it returns false we don't call the original window procedure.
        if (!on_message(wnd, message, w_param, l_param)) {
            return DefWindowProc(wnd, message, w_param, l_param);
        }
    }

    // The game's mouse place (mouse_override): the place in its mouse messages (client coordinates) made the pointer's.
    if (mouse_override::active.load(std::memory_order_relaxed)) {
        switch (message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK: {
            const uint64_t place = mouse_override::place.load(std::memory_order_acquire);
            const auto x = (int16_t)std::clamp((int32_t)(uint32_t)place, -32768, 32767);
            const auto y = (int16_t)std::clamp((int32_t)(uint32_t)(place >> 32), -32768, 32767);
            l_param = MAKELPARAM((WORD)x, (WORD)y);
            mouse_override::rewritten.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        default:
            break;
        }
    }

    // Call the original message procedure.
    return CallWindowProc(g_windows_message_hook->get_original(), wnd, message, w_param, l_param);
}

WindowsMessageHook::WindowsMessageHook(HWND wnd)
    : m_wnd{ wnd },
    m_original_proc{ nullptr }
{
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Initializing WindowsMessageHook");

    utility::ThreadSuspender suspender{};

    g_windows_message_hook = this;

    // Save the original window procedure.
    m_original_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

    // Set it to our "hook" procedure.
    SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)&window_proc);

    spdlog::info("Hooked Windows message handler");
}

WindowsMessageHook::~WindowsMessageHook() {
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Destroying WindowsMessageHook");

    utility::ThreadSuspender suspender{};

    remove();
    g_windows_message_hook = nullptr;
}

bool WindowsMessageHook::remove() {
    // Don't attempt to restore invalid original window procedures.
    if (m_original_proc == nullptr || m_wnd == nullptr) {
        return true;
    }

    // Restore the original window procedure.
    auto current_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

    // lets not try to restore the original window procedure if it's not ours.
    if (current_proc == &window_proc) {
        SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)m_original_proc);
    }

    // Invalidate this message hook.
    m_wnd = nullptr;
    m_original_proc = nullptr;

    return true;
}

bool WindowsMessageHook::is_hook_intact() {
    if (!m_wnd) {
        return false;
    }

    return GetWindowLongPtr(m_wnd, GWLP_WNDPROC) == (LONG_PTR)&window_proc;
}
