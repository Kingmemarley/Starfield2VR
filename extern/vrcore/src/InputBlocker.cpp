#include <cstring>
#include <intrin.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <spdlog/spdlog.h>

#include "InputBlocker.hpp"

InputBlocker& InputBlocker::get() {
    static InputBlocker inst{};
    return inst;
}

void InputBlocker::install() {
    if (m_installed) {
        return;
    }

    m_installed = true;

    const auto user32 = GetModuleHandleW(L"user32.dll");

    if (user32 == nullptr) {
        spdlog::error("[InputBlocker] user32.dll not loaded");
        return;
    }

    auto hook = [&](std::unique_ptr<FunctionHook>& out, const char* name, void* detour) {
        const auto target = (uintptr_t)GetProcAddress(user32, name);

        if (target == 0) {
            spdlog::error("[InputBlocker] {} not found", name);
            return;
        }

        out = std::make_unique<FunctionHook>(target, (uintptr_t)detour);

        if (!out->create()) {
            spdlog::error("[InputBlocker] Failed to hook {}", name);
            out.reset();
        }
    };

    // The game's .exe: its own calls are counted (and see the menu pointer's point).
    {
        const auto base = (uintptr_t)GetModuleHandleW(nullptr);
        const auto dos = (const IMAGE_DOS_HEADER*)base;
        if (base != 0 && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                m_game_begin = base;
                m_game_end = base + nt->OptionalHeader.SizeOfImage;
            }
        }
    }

    hook(m_set_cursor_pos_hook, "SetCursorPos", (void*)&InputBlocker::set_cursor_pos);
    hook(m_clip_cursor_hook, "ClipCursor", (void*)&InputBlocker::clip_cursor);
    hook(m_get_raw_input_buffer_hook, "GetRawInputBuffer", (void*)&InputBlocker::get_raw_input_buffer);
    hook(m_get_cursor_pos_hook, "GetCursorPos", (void*)&InputBlocker::get_cursor_pos);
    hook(m_get_physical_cursor_pos_hook, "GetPhysicalCursorPos", (void*)&InputBlocker::get_physical_cursor_pos);
    hook(m_get_raw_input_data_hook, "GetRawInputData", (void*)&InputBlocker::get_raw_input_data);

    hook_dinput();

    spdlog::info("[InputBlocker] installed");
}

void InputBlocker::hook_dinput() {
    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

    const auto dinput8 = LoadLibraryW(L"dinput8.dll");

    if (dinput8 == nullptr) {
        return;
    }

    const auto create = (DirectInput8CreateFn)GetProcAddress(dinput8, "DirectInput8Create");

    if (create == nullptr) {
        return;
    }

    IDirectInput8W* dinput{};

    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (LPVOID*)&dinput, nullptr)) || dinput == nullptr) {
        spdlog::info("[InputBlocker] Could not create IDirectInput8");
        return;
    }

    IDirectInputDevice8W* device{};

    if (FAILED(dinput->CreateDevice(GUID_SysKeyboard, &device, nullptr)) || device == nullptr) {
        dinput->Release();
        return;
    }

    // IDirectInputDevice8 vtable: 9 = GetDeviceState, 10 = GetDeviceData
    const auto vtable = *(uintptr_t**)device;
    const auto get_device_state = vtable[9];
    const auto get_device_data = vtable[10];

    device->Release();
    dinput->Release();

    m_get_device_state_hook = std::make_unique<FunctionHook>(get_device_state, (uintptr_t)&InputBlocker::dinput_get_device_state);

    if (!m_get_device_state_hook->create()) {
        m_get_device_state_hook.reset();
    }

    m_get_device_data_hook = std::make_unique<FunctionHook>(get_device_data, (uintptr_t)&InputBlocker::dinput_get_device_data);

    if (!m_get_device_data_hook->create()) {
        m_get_device_data_hook.reset();
    }
}

void InputBlocker::set_blocking(bool block) {
    m_blocking = block;

    if (block) {
        // Release any cursor lock the game has, so the menu can be used with the mouse.
        if (m_clip_cursor_hook != nullptr) {
            m_clip_cursor_hook->get_original<decltype(::ClipCursor)>()(nullptr);
        } else {
            ::ClipCursor(nullptr);
        }
    }
}

void InputBlocker::set_cursor_override(bool on, POINT screen) {
    m_cursor_x = (int32_t)screen.x;
    m_cursor_y = (int32_t)screen.y;
    m_cursor_override = on;
}

BOOL InputBlocker::real_get_cursor_pos(POINT* point) const {
    if (m_get_cursor_pos_hook != nullptr) {
        return m_get_cursor_pos_hook->get_original<decltype(InputBlocker::get_cursor_pos)>()(point);
    }
    return ::GetCursorPos(point);
}

BOOL InputBlocker::real_set_cursor_pos(int x, int y) const {
    if (m_set_cursor_pos_hook != nullptr) {
        return m_set_cursor_pos_hook->get_original<decltype(InputBlocker::set_cursor_pos)>()(x, y);
    }
    return ::SetCursorPos(x, y);
}

InputBlocker::Counts InputBlocker::take_counts() {
    Counts out{};
    out.get_cursor_pos = m_counts.get_cursor_pos.exchange(0);
    out.get_physical_cursor_pos = m_counts.get_physical_cursor_pos.exchange(0);
    out.set_cursor_pos = m_counts.set_cursor_pos.exchange(0);
    out.clip_cursor = m_counts.clip_cursor.exchange(0);
    out.raw_buffer_calls = m_counts.raw_buffer_calls.exchange(0);
    out.raw_buffer_mouse = m_counts.raw_buffer_mouse.exchange(0);
    out.raw_data_mouse = m_counts.raw_data_mouse.exchange(0);
    out.dinput_reads = m_counts.dinput_reads.exchange(0);
    out.get_cursor_pos_caller = m_counts.get_cursor_pos_caller.load();
    out.set_cursor_pos_caller = m_counts.set_cursor_pos_caller.load();
    out.raw_caller = m_counts.raw_caller.load();
    out.set_cursor_pos_last[0] = m_counts.set_x.load();
    out.set_cursor_pos_last[1] = m_counts.set_y.load();
    return out;
}

BOOL WINAPI InputBlocker::get_cursor_pos(LPPOINT point) {
    auto& self = get();
    void* const caller = _ReturnAddress();
    const auto original = self.m_get_cursor_pos_hook->get_original<decltype(InputBlocker::get_cursor_pos)>();

    if (!self.from_game(caller)) {
        return original(point);
    }
    self.m_counts.get_cursor_pos.fetch_add(1, std::memory_order_relaxed);
    self.m_counts.get_cursor_pos_caller.store(self.game_rva(caller), std::memory_order_relaxed);
    if (self.m_cursor_override.load() && point != nullptr) {
        point->x = self.m_cursor_x.load();
        point->y = self.m_cursor_y.load();
        return TRUE;
    }
    return original(point);
}

BOOL WINAPI InputBlocker::get_physical_cursor_pos(LPPOINT point) {
    auto& self = get();
    void* const caller = _ReturnAddress();
    const auto original = self.m_get_physical_cursor_pos_hook->get_original<decltype(InputBlocker::get_physical_cursor_pos)>();

    if (!self.from_game(caller)) {
        return original(point);
    }
    self.m_counts.get_physical_cursor_pos.fetch_add(1, std::memory_order_relaxed);
    if (self.m_cursor_override.load() && point != nullptr) {
        point->x = self.m_cursor_x.load();
        point->y = self.m_cursor_y.load();
        return TRUE;
    }
    return original(point);
}

UINT WINAPI InputBlocker::get_raw_input_data(HRAWINPUT raw, UINT command, LPVOID data, PUINT size, UINT header_size) {
    auto& self = get();
    void* const caller = _ReturnAddress();
    const auto original = self.m_get_raw_input_data_hook->get_original<decltype(InputBlocker::get_raw_input_data)>();
    const auto result = original(raw, command, data, size, header_size);

    if (self.from_game(caller) && command == RID_INPUT && data != nullptr && result != (UINT)-1 && result >= sizeof(RAWINPUTHEADER) &&
        ((const RAWINPUTHEADER*)data)->dwType == RIM_TYPEMOUSE) {
        self.m_counts.raw_data_mouse.fetch_add(1, std::memory_order_relaxed);
        self.m_counts.raw_caller.store(self.game_rva(caller), std::memory_order_relaxed);
    }
    return result;
}

BOOL WINAPI InputBlocker::set_cursor_pos(int x, int y) {
    auto& self = get();
    void* const caller = _ReturnAddress();

    if (self.from_game(caller)) {
        self.m_counts.set_cursor_pos.fetch_add(1, std::memory_order_relaxed);
        self.m_counts.set_cursor_pos_caller.store(self.game_rva(caller), std::memory_order_relaxed);
        self.m_counts.set_x.store(x, std::memory_order_relaxed);
        self.m_counts.set_y.store(y, std::memory_order_relaxed);
    }

    if (self.m_blocking) {
        return TRUE; // the game recenters the cursor every frame; ignore that while the menu is open
    }

    return self.m_set_cursor_pos_hook->get_original<decltype(InputBlocker::set_cursor_pos)>()(x, y);
}

BOOL WINAPI InputBlocker::clip_cursor(const RECT* rect) {
    auto& self = get();
    if (self.from_game(_ReturnAddress())) {
        self.m_counts.clip_cursor.fetch_add(1, std::memory_order_relaxed);
    }
    const auto original = self.m_clip_cursor_hook->get_original<decltype(InputBlocker::clip_cursor)>();

    if (self.m_blocking) {
        return original(nullptr);
    }

    return original(rect);
}

UINT WINAPI InputBlocker::get_raw_input_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    auto& self = get();
    void* const caller = _ReturnAddress();
    const auto original = self.m_get_raw_input_buffer_hook->get_original<decltype(InputBlocker::get_raw_input_buffer)>();
    const auto result = original(data, size, header_size);

    if (self.m_blocking && data != nullptr && result != (UINT)-1) {
        return 0; // drained, but the game sees nothing
    }

    // (Counted: how many of the records the game gets are the mouse's.)
    if (data != nullptr && result != (UINT)-1 && self.from_game(caller)) {
        self.m_counts.raw_buffer_calls.fetch_add(1, std::memory_order_relaxed);
        uint32_t mouse = 0;
        PRAWINPUT record = data;
        for (UINT i = 0; i < result && i < 4096; ++i) {
            if (record->header.dwType == RIM_TYPEMOUSE) {
                ++mouse;
            }
            record = (PRAWINPUT)(((uintptr_t)record + record->header.dwSize + 7) & ~(uintptr_t)7); // (NEXTRAWINPUTBLOCK, 64-bit)
        }
        if (mouse > 0) {
            self.m_counts.raw_buffer_mouse.fetch_add(mouse, std::memory_order_relaxed);
            self.m_counts.raw_caller.store(self.game_rva(caller), std::memory_order_relaxed);
        }
    }

    return result;
}

HRESULT WINAPI InputBlocker::dinput_get_device_state(void* device, DWORD size, LPVOID data) {
    auto& self = get();
    if (self.from_game(_ReturnAddress())) {
        self.m_counts.dinput_reads.fetch_add(1, std::memory_order_relaxed);
    }
    const auto original = self.m_get_device_state_hook->get_original<decltype(InputBlocker::dinput_get_device_state)>();
    const auto result = original(device, size, data);

    if (self.m_blocking && SUCCEEDED(result) && data != nullptr) {
        std::memset(data, 0, size);
    }

    return result;
}

HRESULT WINAPI InputBlocker::dinput_get_device_data(void* device, DWORD object_size, void* data, LPDWORD in_out, DWORD flags) {
    auto& self = get();
    if (self.from_game(_ReturnAddress())) {
        self.m_counts.dinput_reads.fetch_add(1, std::memory_order_relaxed);
    }
    const auto original = self.m_get_device_data_hook->get_original<decltype(InputBlocker::dinput_get_device_data)>();
    const auto result = original(device, object_size, data, in_out, flags);

    if (self.m_blocking && SUCCEEDED(result) && in_out != nullptr) {
        *in_out = 0;
    }

    return result;
}
