// (fix193) The VR controllers as an Xbox pad in the game's own list of Windows gamepads (see VirtualGamepad.h).
#include "VirtualGamepad.h"

#include <Windows.h>
#include <intrin.h>
#include <objbase.h>
#include <objidl.h>
#include <inspectable.h>
#include <hstring.h>
#include <Xinput.h>
#include <windows.gaming.input.h>

#include <safetyhook/inline_hook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "CreationEngineInputManager.h"
#include "Haptics.h"

namespace
{
    namespace wgi = ABI::Windows::Gaming::Input;
    using PadView      = ABI::Windows::Foundation::Collections::IVectorView<wgi::Gamepad*>;
    using PadHandler   = ABI::Windows::Foundation::IEventHandler<wgi::Gamepad*>;
    using ActivationFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
    using RawBufferFn  = PCWSTR(WINAPI*)(HSTRING, UINT32*);
    using CreateStrFn  = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);

    constexpr wchar_t kGamepadClass[] = L"Windows.Gaming.Input.Gamepad";
    constexpr UINT32  kGamepadClassLength = static_cast<UINT32>(std::size(kGamepadClass) - 1);

    // Whether the game gets the stand-in. Undecided until the game first asks (then Wrapped) or the mod picks ViGEmBus
    // (PassThrough); never changes after that, so the virtual pad and the ViGEm pad are never both in use. (fix195) One exception:
    // when the game's request reached the watch through another module's hook and the game then never read the virtual pad, the
    // mod may give it up for the ViGEm pad (Abandoned): the virtual pad then reads idle and the game's list is Windows' own again.
    enum State : int { kUndecided = 0, kWrapped = 1, kPassThrough = 2, kAbandoned = 3 };
    std::atomic<int> g_state{ kUndecided };

    safetyhook::InlineHook    g_hook{};
    std::atomic<ActivationFn> g_original{ nullptr };
    RawBufferFn               g_raw_buffer{ nullptr };
    CreateStrFn               g_create_string{ nullptr };
    std::atomic<bool>         g_installed{ false };
    std::string               g_install_note{ "not tried" };

    // (fix195) The game's code: its executable sections (set once, before the watch is switched on).
    struct CodeRange {
        std::uintptr_t begin;
        std::uintptr_t end;
    };
    constexpr int  kMaxCodeRanges = 8;
    CodeRange      g_code[kMaxCodeRanges]{};
    int            g_code_ranges{ 0 };
    HMODULE        g_exe_module{ nullptr };
    HMODULE        g_self{ nullptr };  // this dll
    std::wstring   g_windows_dir;      // Windows' folder and its system folder, each ending in a backslash
    std::wstring   g_system_dir;
    std::atomic<bool> g_via_hook{ false };     // (fix195) a game's request was wrapped through another module's hook in front
    std::atomic<int>  g_direct_wraps{ 0 };     // (fix195) the game's requests wrapped with the game's code as the direct caller

    // What happened, for the log (written on the game's threads, read by Describe and the first-read note).
    std::atomic<int>      g_wraps{ 0 };              // the stand-in handed to the game
    std::atomic<long>     g_first_real_result{ 1 };  // Windows' own answer to the game's first request (1: none yet)
    std::atomic<int>      g_passed_on{ 0 };          // requests from other modules, or after the mod chose ViGEmBus
    std::atomic<int>      g_real_pads{ -1 };         // real pads in Windows' list at the game's last look (all hidden from the game)
    std::atomic<int>      g_logged_pads{ -1 };       // (fix195) the count of hidden pads the log last told
    std::atomic<bool>     g_active{ false };         // Activate: the VR controllers may be read (and the log is open)
    std::atomic<bool>     g_read_logged{ false };
    std::atomic<bool>     g_rumble_logged{ false };
    std::atomic<uint64_t> g_reads{ 0 };              // the game's reads of the virtual pad (GetCurrentReading), from the game's start
    std::atomic<uint64_t> g_list_requests{ 0 };      // (fix195) the game's requests for its gamepad list (get_Gamepads)
    std::atomic<int>      g_event_failures{ 0 };     // (fix195) Windows refused a GamepadAdded/Removed registration
    std::atomic<long>     g_event_failure_result{ 0 };
    std::mutex            g_other_module_mutex;
    std::string           g_other_module;  // the first other module that asked for the Gamepad statics, with its stack's modules
    std::string           g_route_note;    // (fix195) the game's request came through another module first (found deeper in the stack)

    // (fix195) How many of the call's stack frames are taken (the watch, its direct caller, and four more).
    constexpr USHORT kStackFrames = 8;

    bool IsGamepadClass(HSTRING class_id)
    {
        if (class_id == nullptr || g_raw_buffer == nullptr) {
            return false;
        }
        UINT32       length = 0;
        const PCWSTR text   = g_raw_buffer(class_id, &length);
        return text != nullptr && length == kGamepadClassLength && std::wmemcmp(text, kGamepadClass, kGamepadClassLength) == 0;
    }

    // (fix195) An address in the game's code (one of Starfield.exe's executable sections).
    bool FromTheGame(const void* return_address)
    {
        const auto at = reinterpret_cast<std::uintptr_t>(return_address);
        for (int i = 0; i < g_code_ranges; ++i) {
            if (at >= g_code[i].begin && at < g_code[i].end) {
                return true;
            }
        }
        return false;
    }

    HMODULE ModuleHandleOf(const void* address)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)) {
            return nullptr;
        }
        return module;
    }

    bool StartsWithFolder(const wchar_t* path, const std::wstring& folder)
    {
        return !folder.empty() && _wcsnicmp(path, folder.c_str(), folder.size()) == 0;
    }

    // (fix195) A module of Windows itself (under Windows' folder or its system folder).
    bool IsSystemModule(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
            return true;  // (unknown: not taken as a hook in front)
        }
        return StartsWithFolder(path, g_windows_dir) || StartsWithFolder(path, g_system_dir);
    }

    // (fix195) The module an address is in, with the address's offset in it ("name+0x1234"), for the log.
    std::string ModuleOf(const void* address)
    {
        if (HMODULE module = ModuleHandleOf(address); module != nullptr) {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(module, path, MAX_PATH) != 0) {
                const wchar_t* name = std::wcsrchr(path, L'\\');
                std::string    narrow;
                for (const wchar_t* c = name != nullptr ? name + 1 : path; *c != L'\0'; ++c) {
                    narrow.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
                }
                return fmt::format("{}+0x{:X}", narrow, reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
            }
        }
        return fmt::format("(no module) 0x{:X}", reinterpret_cast<std::uintptr_t>(address));
    }

    // (fix195) The modules of a stack's frames, in order, for the log.
    std::string StackModules(void* const* frames, USHORT count)
    {
        std::string text;
        for (USHORT i = 0; i < count; ++i) {
            text += (i == 0 ? "" : ", ") + ModuleOf(frames[i]);
        }
        return text.empty() ? std::string{ "(no frames)" } : text;
    }

    // (fix195) The game's own call (its C++/WinRT helper, which asks combase through a pointer it looked up by name) is normally the
    // watch's direct caller. When one other module hooked the same function in front of the watch (the Steam overlay hooks
    // Windows' gamepad API, for one), that module is the direct caller and the game's call is a few frames further down. Only that
    // shape counts as the game's request: the first frame outside this dll (the direct caller) is in a module that is neither this
    // dll, nor the game, nor part of Windows, and the next frames (up to four) stay in that same module until one is in the game's
    // code. Returns that frame (frame 0 is the watch itself), or -1.
    int HookedGameFrame(void* const* frames, USHORT count)
    {
        USHORT direct = 0;
        while (direct < count && ModuleHandleOf(frames[direct]) == g_self) {
            ++direct;
        }
        if (direct >= count) {
            return -1;
        }
        const HMODULE front = ModuleHandleOf(frames[direct]);
        if (front == nullptr || front == g_self || front == g_exe_module || IsSystemModule(front)) {
            return -1;
        }
        for (USHORT k = static_cast<USHORT>(direct + 1); k < count && k <= direct + 4; ++k) {
            if (FromTheGame(frames[k])) {
                return static_cast<int>(k);
            }
            if (ModuleHandleOf(frames[k]) != front) {
                return -1;
            }
        }
        return -1;
    }

    // (The texts are made before the lock is taken; only the first of each is kept.)
    void NoteOtherModule(const void* return_address, void* const* frames, USHORT count)
    {
        {
            std::scoped_lock lock{ g_other_module_mutex };
            if (!g_other_module.empty()) {
                return;
            }
        }
        std::string text = ModuleOf(return_address) + "; its stack: " + StackModules(frames, count);
        std::scoped_lock lock{ g_other_module_mutex };
        if (g_other_module.empty()) {
            g_other_module = std::move(text);
        }
    }

    void NoteRoute(const void* return_address, int frame, void* const* frames, USHORT count)
    {
        {
            std::scoped_lock lock{ g_other_module_mutex };
            if (!g_route_note.empty()) {
                return;
            }
        }
        std::string text = fmt::format("the game's request came through a hook in {} first (the game's code found at stack frame {}, frame 0 being the "
                                       "watch; the stack: {})",
            ModuleOf(return_address), frame, StackModules(frames, count));
        std::scoped_lock lock{ g_other_module_mutex };
        if (g_route_note.empty()) {
            g_route_note = std::move(text);
        }
    }

    HRESULT MakeClassName(const wchar_t* text, HSTRING* name)
    {
        if (name == nullptr) {
            return E_POINTER;
        }
        *name = nullptr;  // an empty name, when the string function is missing
        if (g_create_string == nullptr) {
            return S_OK;
        }
        return g_create_string(text, static_cast<UINT32>(std::wcslen(text)), name);
    }

    HRESULT OneIid(const IID& iid, ULONG* count, IID** iids)
    {
        if (count == nullptr || iids == nullptr) {
            return E_POINTER;
        }
        auto* list = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
        if (list == nullptr) {
            *count = 0;
            *iids  = nullptr;
            return E_OUTOFMEMORY;
        }
        list[0] = iid;
        *count  = 1;
        *iids   = list;
        return S_OK;
    }

    std::uint8_t MotorByte(double level)
    {
        if (!(level > 0.0)) {  // also NaN
            return 0;
        }
        return static_cast<std::uint8_t>(std::lround(std::fmin(level, 1.0) * 255.0));
    }

    double Stick(SHORT value)
    {
        // XInput's -32768..32767, up and right positive; Windows' gamepad: -1..1, the same directions.
        return std::fmax(-1.0, static_cast<double>(value) / 32767.0);
    }

    wgi::GamepadButtons Buttons(WORD xinput)
    {
        struct Pair {
            WORD                xinput;
            wgi::GamepadButtons windows;
        };
        static constexpr Pair kMap[] = {
            { XINPUT_GAMEPAD_DPAD_UP, wgi::GamepadButtons_DPadUp },
            { XINPUT_GAMEPAD_DPAD_DOWN, wgi::GamepadButtons_DPadDown },
            { XINPUT_GAMEPAD_DPAD_LEFT, wgi::GamepadButtons_DPadLeft },
            { XINPUT_GAMEPAD_DPAD_RIGHT, wgi::GamepadButtons_DPadRight },
            { XINPUT_GAMEPAD_START, wgi::GamepadButtons_Menu },
            { XINPUT_GAMEPAD_BACK, wgi::GamepadButtons_View },
            { XINPUT_GAMEPAD_LEFT_THUMB, wgi::GamepadButtons_LeftThumbstick },
            { XINPUT_GAMEPAD_RIGHT_THUMB, wgi::GamepadButtons_RightThumbstick },
            { XINPUT_GAMEPAD_LEFT_SHOULDER, wgi::GamepadButtons_LeftShoulder },
            { XINPUT_GAMEPAD_RIGHT_SHOULDER, wgi::GamepadButtons_RightShoulder },
            { XINPUT_GAMEPAD_A, wgi::GamepadButtons_A },
            { XINPUT_GAMEPAD_B, wgi::GamepadButtons_B },
            { XINPUT_GAMEPAD_X, wgi::GamepadButtons_X },
            { XINPUT_GAMEPAD_Y, wgi::GamepadButtons_Y },
        };
        unsigned int out = wgi::GamepadButtons_None;
        for (const auto& pair : kMap) {
            if ((xinput & pair.xinput) != 0) {
                out |= static_cast<unsigned int>(pair.windows);
            }
        }
        return static_cast<wgi::GamepadButtons>(out);
    }

    // An object's COM identity (its IUnknown), with a reference; null for none.
    IUnknown* Identity(IUnknown* object)
    {
        if (object == nullptr) {
            return nullptr;
        }
        IUnknown* identity = nullptr;
        if (FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity)))) {
            return nullptr;
        }
        return identity;
    }

    UINT64 NowMicroseconds()
    {
        static const LONGLONG frequency = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f.QuadPart > 0 ? f.QuadPart : 1;
        }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<UINT64>((now.QuadPart / frequency) * 1000000 + (now.QuadPart % frequency) * 1000000 / frequency);
    }

    // ---- The virtual pad: one object for the whole run (never freed; its count is kept for the COM rules) ----
    class VirtualPad final : public wgi::IGamepad
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (out == nullptr) {
                return E_POINTER;
            }
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(wgi::IGamepad) || iid == __uuidof(IAgileObject)) {
                *out = static_cast<wgi::IGamepad*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1) + 1; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG left = m_refs.fetch_sub(1) - 1;
            return left;  // it lives as long as the game: never deleted
        }

        HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override { return OneIid(__uuidof(wgi::IGamepad), count, iids); }
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override { return MakeClassName(kGamepadClass, name); }
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
        {
            if (level == nullptr) {
                return E_POINTER;
            }
            *level = BaseTrust;
            return S_OK;
        }

        // (No exception may leave these methods into the game: each catches everything and still answers S_OK.)
        HRESULT STDMETHODCALLTYPE get_Vibration(wgi::GamepadVibration* value) override
        {
            if (value == nullptr) {
                return E_POINTER;
            }
            try {
                std::scoped_lock lock{ m_vibration_mutex };
                *value = m_vibration;
            } catch (...) {
                *value = {};
            }
            return S_OK;
        }

        // The game's rumble (getting hit, explosions, hard landings): the big (left) and small (right) motor, 0 to 1, passed on
        // where the ViGEm pad's rumble went (Haptics), once the mod is ready (as with the ViGEm pad, plugged in only then). The game
        // leaves the trigger motors at 0.
        HRESULT STDMETHODCALLTYPE put_Vibration(wgi::GamepadVibration value) override
        {
            try {
                {
                    std::scoped_lock lock{ m_vibration_mutex };
                    m_vibration = value;
                }
                if (!g_active.load()) {
                    return S_OK;
                }
                const std::uint8_t big_motor   = MotorByte(value.LeftMotor);
                const std::uint8_t small_motor = MotorByte(value.RightMotor);
                haptics::SetGameRumble(big_motor, small_motor);
                if ((big_motor != 0 || small_motor != 0) && !g_rumble_logged.exchange(true)) {
                    spdlog::info("[Input] (fix193) the game's first rumble on the virtual pad (big motor {:.2f}, small {:.2f}); passed on to the controllers",
                        value.LeftMotor, value.RightMotor);
                }
            } catch (...) {
            }
            return S_OK;
        }

        // The game's own gamepad poll asks for the pad's state, once a frame, on its input thread: the VR controllers are read here
        // (the same path the ViGEm pad was fed by), and handed back as Windows' gamepad reading. An idle pad until the mod is
        // ready, and while the headset's session is off (so nothing stays held: fix189's intent).
        HRESULT STDMETHODCALLTYPE GetCurrentReading(wgi::GamepadReading* value) override
        {
            if (value == nullptr) {
                return E_POINTER;
            }
            *value           = {};
            value->Timestamp = NowMicroseconds();
            g_reads.fetch_add(1);
            if (!g_active.load()) {
                return S_OK;
            }
            XUSB_REPORT report{};
            bool        live = false;
            // One reader at a time: the game reads from one thread, so this only guards against a second caller (or a call from
            // inside the read itself), which gets an idle pad instead of waiting.
            if (m_reading.test_and_set(std::memory_order_acquire)) {
                return S_OK;
            }
            try {
                live = CreationEngineInputManager::ReadVrPad(report);
            } catch (...) {
                live = false;
            }
            m_reading.clear(std::memory_order_release);
            if (!g_read_logged.exchange(true)) {
                try {
                    spdlog::info("[Input] (fix193) the game reads the virtual pad (its own gamepad poll; {} reads since the game started, the headset's session {}; "
                                 "the game's list: the virtual pad alone, {} real pad(s) hidden (fix195))",
                        g_reads.load(), live ? "on" : "off", (std::max)(0, g_real_pads.load()));
                } catch (...) {
                }
            }
            if (!live) {
                return S_OK;
            }
            value->Buttons          = Buttons(report.wButtons);
            value->LeftTrigger      = static_cast<double>(report.bLeftTrigger) / 255.0;
            value->RightTrigger     = static_cast<double>(report.bRightTrigger) / 255.0;
            value->LeftThumbstickX  = Stick(report.sThumbLX);
            value->LeftThumbstickY  = Stick(report.sThumbLY);
            value->RightThumbstickX = Stick(report.sThumbRX);
            value->RightThumbstickY = Stick(report.sThumbRY);
            return S_OK;
        }

    private:
        std::atomic<ULONG>     m_refs{ 1 };  // the mod's own reference
        std::mutex             m_vibration_mutex;
        wgi::GamepadVibration  m_vibration{};
        std::atomic_flag       m_reading{};  // set while the VR controllers are being read
    };

    VirtualPad g_pad;

    // ---- (fix195) The list of pads handed to the game: the virtual pad alone; one object for the whole run, never freed (so handing
    // it out cannot fail), its count kept for the COM rules ----
    class PadList final : public PadView
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (out == nullptr) {
                return E_POINTER;
            }
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(PadView) || iid == __uuidof(IAgileObject)) {
                *out = static_cast<PadView*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1) + 1; }
        ULONG STDMETHODCALLTYPE Release() override { return m_refs.fetch_sub(1) - 1; }  // never deleted

        HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override { return OneIid(__uuidof(PadView), count, iids); }
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override
        {
            return MakeClassName(L"Windows.Foundation.Collections.IVectorView`1<Windows.Gaming.Input.Gamepad>", name);
        }
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
        {
            if (level == nullptr) {
                return E_POINTER;
            }
            *level = BaseTrust;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetAt(unsigned index, wgi::IGamepad** item) override
        {
            if (item == nullptr) {
                return E_POINTER;
            }
            *item = nullptr;
            if (index >= 1) {
                return E_BOUNDS;
            }
            g_pad.AddRef();
            *item = &g_pad;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_Size(unsigned* size) override
        {
            if (size == nullptr) {
                return E_POINTER;
            }
            *size = 1;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE IndexOf(wgi::IGamepad* value, unsigned* index, boolean* found) override
        {
            if (index == nullptr || found == nullptr) {
                return E_POINTER;
            }
            *index = 0;
            *found = false;
            // By COM identity (the same object, whichever of its interfaces was handed in).
            IUnknown* const wanted = Identity(value);
            if (wanted == nullptr) {
                return S_OK;
            }
            IUnknown* const here = Identity(&g_pad);
            *found               = here != nullptr && here == wanted;
            if (here != nullptr) {
                here->Release();
            }
            wanted->Release();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity, wgi::IGamepad** items, unsigned* actual) override
        {
            if (actual == nullptr || (capacity > 0 && items == nullptr)) {
                return E_POINTER;
            }
            *actual = 0;
            if (start > 1) {
                return E_BOUNDS;
            }
            if (start == 0 && capacity > 0) {
                g_pad.AddRef();
                items[0] = &g_pad;
                *actual  = 1;
            }
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_refs{ 1 };  // the mod's own reference
    };

    PadList g_list;
    // ---- The Gamepad statics handed to the game: Windows' own (when Windows gave one) for the events, the virtual pad as the list ----
    class PadStatics final : public wgi::IGamepadStatics
    {
    public:
        // Holds its own reference to Windows' object (none when Windows gave none: then the list is the virtual pad alone).
        explicit PadStatics(wgi::IGamepadStatics* real) : m_real(real)
        {
            if (m_real != nullptr) {
                m_real->AddRef();
            }
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (out == nullptr) {
                return E_POINTER;
            }
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(wgi::IGamepadStatics) || iid == __uuidof(IAgileObject)) {
                *out = static_cast<wgi::IGamepadStatics*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1) + 1; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG left = m_refs.fetch_sub(1) - 1;
            if (left == 0) {
                delete this;
            }
            return left;
        }

        HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override { return OneIid(__uuidof(wgi::IGamepadStatics), count, iids); }
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override { return MakeClassName(kGamepadClass, name); }
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
        {
            if (level == nullptr) {
                return E_POINTER;
            }
            *level = BaseTrust;
            return S_OK;
        }

        // A pad plugged in or out: Windows tells the game as before. The game's handler for both events only marks its list to be
        // read again (0x9C4F70 in 1.16.236), so it then gets the virtual pad alone again. (fix195) Should Windows refuse the
        // registration, the game gets a token of the mod's own and success instead: the game turns any failure here into an
        // exception, and it needs no events to find the virtual pad (its list starts marked to be read).
        HRESULT STDMETHODCALLTYPE add_GamepadAdded(PadHandler* handler, EventRegistrationToken* token) override { return AddEvent(true, handler, token); }
        HRESULT STDMETHODCALLTYPE remove_GamepadAdded(EventRegistrationToken token) override { return RemoveEvent(true, token); }
        HRESULT STDMETHODCALLTYPE add_GamepadRemoved(PadHandler* handler, EventRegistrationToken* token) override { return AddEvent(false, handler, token); }
        HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(EventRegistrationToken token) override { return RemoveEvent(false, token); }

        // (fix195) The pads: the virtual one alone. Windows' own pads are counted (for the log) and left out. The game makes its gamepad
        // device for a list index only when the same index was its pick on its previous look as well, and it looks at every index
        // in one go with a single remembered pick (0x2300C60 in 1.16.236): with two or more pads listed, the pick moves 0, 1, 0,
        // 1... and no device is ever made, so no controller works. With the virtual pad alone that cannot happen. A real Xbox pad
        // (or another program's virtual one) is therefore hidden from the game while the mod's pad is given: the VR controllers
        // are the pad. (A DualSense, which the game reads through Sony's library first, is not in this list and still wins.)
        HRESULT STDMETHODCALLTYPE get_Gamepads(PadView** value) override
        {
            if (value == nullptr) {
                return E_POINTER;
            }
            *value = nullptr;
            g_list_requests.fetch_add(1);
            // (fix195) The virtual pad given up for the ViGEm pad (see State): the game gets Windows' own list again (should Windows
            // fail or give none, the list of the idle virtual pad instead: the game turns a failure into an exception).
            if (g_state.load() == kAbandoned && m_real != nullptr) {
                PadView* windows_list = nullptr;
                if (SUCCEEDED(m_real->get_Gamepads(&windows_list)) && windows_list != nullptr) {
                    *value = windows_list;
                    return S_OK;
                }
                g_list.AddRef();
                *value = &g_list;
                return S_OK;
            }
            int real_count = 0;
            if (m_real != nullptr) {
                PadView* real_view = nullptr;
                if (SUCCEEDED(m_real->get_Gamepads(&real_view)) && real_view != nullptr) {
                    struct Done {
                        PadView* view;
                        ~Done() { view->Release(); }
                    } done{ real_view };
                    unsigned size = 0;
                    if (FAILED(real_view->get_Size(&size))) {
                        size = 0;
                    }
                    for (unsigned i = 0; i < size; ++i) {
                        wgi::IGamepad* real_pad = nullptr;
                        if (SUCCEEDED(real_view->GetAt(i, &real_pad)) && real_pad != nullptr) {
                            if (real_pad != &g_pad) {
                                ++real_count;
                            }
                            real_pad->Release();  // hidden from the game
                        }
                    }
                }
            }
            g_real_pads.store(real_count);
            NoteHiddenPads(real_count);
            g_list.AddRef();
            *value = &g_list;
            return S_OK;
        }

    private:
        ~PadStatics()
        {
            if (m_real != nullptr) {
                m_real->Release();
            }
        }

        // (fix195) Once the log is open (the mod active), a change in the number of hidden pads is logged once; before that, Describe
        // tells it.
        static void NoteHiddenPads(int count)
        {
            if (!g_active.load() || g_logged_pads.exchange(count) == count) {
                return;
            }
            try {
                spdlog::info("[Input] (fix195) the game's gamepad list: the virtual pad alone; {} real pad(s) in Windows' list hidden from the game (the VR "
                             "controllers are the pad; the game makes no gamepad device when two or more pads are listed)",
                    count);
            } catch (...) {
            }
        }

        HRESULT AddEvent(bool added, PadHandler* handler, EventRegistrationToken* token)
        {
            if (token == nullptr) {
                return E_POINTER;
            }
            if (m_real != nullptr) {
                const HRESULT result = added ? m_real->add_GamepadAdded(handler, token) : m_real->add_GamepadRemoved(handler, token);
                if (SUCCEEDED(result)) {
                    return result;
                }
                const int failures = g_event_failures.fetch_add(1) + 1;
                g_event_failure_result.store(static_cast<long>(result));
                if (g_active.load()) {
                    try {
                        spdlog::warn("[Input] (fix195) Windows refused the game's {} registration (0x{:08X}, {} time(s)); the game was given a token of the "
                                     "mod's own and success (it needs no events to find the virtual pad)",
                            added ? "GamepadAdded" : "GamepadRemoved", static_cast<unsigned long>(result), failures);
                    } catch (...) {
                    }
                }
            }
            return NoEvents(token);
        }

        HRESULT RemoveEvent(bool added, EventRegistrationToken token)
        {
            if (TakeOwnToken(token) || m_real == nullptr) {
                return S_OK;
            }
            return added ? m_real->remove_GamepadAdded(token) : m_real->remove_GamepadRemoved(token);
        }

        // A token of the mod's own (Windows has nothing registered for it). Its values start far from small numbers, and each one is
        // kept until removed, so that a remove never mixes one up with a token Windows gave.
        HRESULT NoEvents(EventRegistrationToken* token)
        {
            if (token == nullptr) {
                return E_POINTER;
            }
            token->value = m_next_token.fetch_add(1);
            try {
                std::scoped_lock lock{ m_own_tokens_mutex };
                m_own_tokens.push_back(token->value);
            } catch (...) {
            }
            return S_OK;
        }

        bool TakeOwnToken(EventRegistrationToken token)
        {
            try {
                std::scoped_lock lock{ m_own_tokens_mutex };
                for (auto it = m_own_tokens.begin(); it != m_own_tokens.end(); ++it) {
                    if (*it == token.value) {
                        m_own_tokens.erase(it);
                        return true;
                    }
                }
            } catch (...) {
            }
            return false;
        }

        std::atomic<ULONG>    m_refs{ 1 };
        wgi::IGamepadStatics* m_real{ nullptr };
        std::atomic<__int64>  m_next_token{ 0x5F5F000000000001 };
        std::mutex            m_own_tokens_mutex;
        std::vector<__int64>  m_own_tokens;
    };

    // Windows' activation call (combase's RoGetActivationFactory), watched: the game's request for the Gamepad statics gets the
    // stand-in; every other request (other classes, other interfaces, other modules, or after the mod chose ViGEmBus) is passed
    // on unchanged.
    HRESULT WINAPI OnActivationFactory(HSTRING class_id, REFIID iid, void** factory)
    {
        const void* const caller   = _ReturnAddress();
        const auto        original = g_original.load(std::memory_order_acquire);
        if (original == nullptr) {
            if (factory != nullptr) {
                *factory = nullptr;
            }
            return E_FAIL;  // cannot happen: the watch is switched on only after this is set
        }
        const HRESULT result = original(class_id, iid, factory);
        // (No exception may leave this into the game: on any, Windows' own answer goes back as it is.)
        try {
            // (The class and interface first: the stack is only looked at for the Gamepad statics, about once a run, since the game
            // keeps the statics it gets.)
            if (factory == nullptr || iid != __uuidof(wgi::IGamepadStatics) || !IsGamepadClass(class_id)) {
                return result;
            }
            // (fix195) The game's request: the game's code is the caller, or one other module's hook sits in front of the watch
            // (see HookedGameFrame).
            const bool direct = FromTheGame(caller);
            if (!direct) {
                void*        frames[kStackFrames]{};
                const USHORT frame_count = RtlCaptureStackBackTrace(0, kStackFrames, frames, nullptr);
                const int    game_frame  = HookedGameFrame(frames, frame_count);
                if (game_frame < 0) {
                    g_passed_on.fetch_add(1);
                    NoteOtherModule(caller, frames, frame_count);
                    return result;
                }
                NoteRoute(caller, game_frame, frames, frame_count);
            }
            // (The review) COM not set up yet on the game's thread: the game sets it up and asks again (its activation code retries
            // after CoIncrementMTAUsage on this answer), so it is not answered with a stand-in alone; nothing is decided yet.
            if (result == CO_E_NOTINITIALIZED) {
                return result;
            }
            if (const int state = g_state.load(); state == kPassThrough || state == kAbandoned) {
                g_passed_on.fetch_add(1);
                return result;
            }
            auto* const real = SUCCEEDED(result) ? static_cast<wgi::IGamepadStatics*>(*factory) : nullptr;
            auto* const ours = new (std::nothrow) PadStatics(real);  // takes a reference of its own to Windows' object
            if (ours == nullptr) {
                return result;
            }
            int expected = kUndecided;
            if (!g_state.compare_exchange_strong(expected, kWrapped) && expected != kWrapped) {
                ours->Release();  // the mod chose ViGEmBus in the meantime: Windows' object goes to the game as it is
                g_passed_on.fetch_add(1);
                return result;
            }
            if (real != nullptr) {
                real->Release();  // the reference Windows gave the game: the stand-in holds its own
            }
            long none = 1;
            g_first_real_result.compare_exchange_strong(none, static_cast<long>(result));
            g_wraps.fetch_add(1);
            if (direct) {
                g_direct_wraps.fetch_add(1);
            } else {
                g_via_hook.store(true);
            }
            *factory = static_cast<wgi::IGamepadStatics*>(ours);
            return S_OK;
        } catch (...) {
            return result;
        }
    }
}

namespace VirtualGamepad
{
    bool InstallEarly()
    {
        if (g_installed.load()) {
            return true;
        }
        const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (exe == 0) {
            g_install_note = "the game's module was not found";
            return false;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
        g_exe_module    = reinterpret_cast<HMODULE>(exe);
        // (fix195) The game's code: its executable sections only.
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections && g_code_ranges < kMaxCodeRanges; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }
            const std::uintptr_t begin = exe + section->VirtualAddress;
            const std::uintptr_t size  = (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
            g_code[g_code_ranges++]    = { begin, begin + size };
        }
        if (g_code_ranges == 0) {
            g_install_note = "the game's code sections were not found";
            return false;
        }
        // (fix195) This dll, and Windows' folders (a module there is never taken as a hook in front of the watch).
        g_self = ModuleHandleOf(reinterpret_cast<const void*>(&OnActivationFactory));
        wchar_t folder[MAX_PATH]{};
        if (const UINT length = GetWindowsDirectoryW(folder, MAX_PATH); length > 0 && length < MAX_PATH) {
            g_windows_dir = folder;
            if (g_windows_dir.back() != L'\\') {
                g_windows_dir.push_back(L'\\');
            }
        }
        if (const UINT length = GetSystemDirectoryW(folder, MAX_PATH); length > 0 && length < MAX_PATH) {
            g_system_dir = folder;
            if (g_system_dir.back() != L'\\') {
                g_system_dir.push_back(L'\\');
            }
        }

        HMODULE combase = GetModuleHandleW(L"combase.dll");
        if (combase == nullptr) {
            combase = LoadLibraryW(L"combase.dll");
        }
        if (combase == nullptr) {
            g_install_note = "combase.dll was not found";
            return false;
        }
        void* const target = reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory"));
        g_raw_buffer       = reinterpret_cast<RawBufferFn>(GetProcAddress(combase, "WindowsGetStringRawBuffer"));
        g_create_string    = reinterpret_cast<CreateStrFn>(GetProcAddress(combase, "WindowsCreateString"));
        if (target == nullptr || g_raw_buffer == nullptr) {
            g_install_note = "combase.dll has no RoGetActivationFactory or WindowsGetStringRawBuffer";
            return false;
        }
        // Made switched off, the way back to Windows' code kept, then switched on: no call can come in before that is known.
        auto hook = safetyhook::InlineHook::create(target, reinterpret_cast<void*>(&OnActivationFactory), safetyhook::InlineHook::StartDisabled);
        if (!hook) {
            g_install_note = "the hook could not be made (safetyhook error " + std::to_string(static_cast<int>(hook.error().type)) + ")";
            return false;
        }
        g_hook = std::move(*hook);
        g_original.store(g_hook.original<ActivationFn>(), std::memory_order_release);
        if (!g_hook.enable()) {
            g_install_note = "the hook could not be switched on";
            g_hook         = {};
            g_original.store(nullptr);
            return false;
        }
        g_install_note = "on combase's RoGetActivationFactory since the dll started";
        g_installed.store(true);
        return true;
    }

    bool HookInstalled() { return g_installed.load(); }

    bool Wrapped() { return g_state.load() == kWrapped; }

    bool GiveUp()
    {
        int expected = kUndecided;
        if (g_state.compare_exchange_strong(expected, kPassThrough)) {
            return true;
        }
        return expected == kPassThrough;
    }

    void Activate()
    {
        g_logged_pads.store(g_real_pads.load());  // (fix195) the count the mod's own log line tells (UseVirtualPad)
        g_active.store(true);
    }

    int HiddenPads() { return g_real_pads.load(); }

    std::uint64_t Reads() { return g_reads.load(); }

    // (Only when no request of the game's own code was wrapped directly: then the game holds the stand-in itself, and the pad is
    // never given up.)
    bool ThroughHook() { return g_via_hook.load() && g_direct_wraps.load() == 0; }

    bool EventsWork() { return g_event_failures.load() == 0; }

    bool Abandon()
    {
        int expected = kWrapped;
        if (!g_state.compare_exchange_strong(expected, kAbandoned)) {
            return false;
        }
        g_active.store(false);  // idle readings, no VR read, no rumble from here on
        return true;
    }

    void Resume()
    {
        int expected = kAbandoned;
        if (g_state.compare_exchange_strong(expected, kWrapped)) {
            g_active.store(true);
        }
    }

    std::string Describe()
    {
        std::string text = "the watch on Windows' gamepad list: " + g_install_note;
        const int   wraps = g_wraps.load();
        if (wraps > 0) {
            const long first = g_first_real_result.load();
            text += "; the game asked Windows for its gamepad statics " + std::to_string(wraps) + " time(s) and got the stand-in (" +
                    (first == 1 ? std::string{ "?" } :
                     first >= 0 ? std::string{ "with Windows' own events behind it" } :
                                  fmt::format("Windows gave none (0x{:08X}): no events", static_cast<unsigned long>(first))) +
                    ")";
            if (const int hidden = g_real_pads.load(); hidden >= 0) {
                text += fmt::format("; its list: the virtual pad alone, {} real pad(s) hidden (fix195)", hidden);
            }
            text += fmt::format("; (fix195) the game read its list {} time(s) and the virtual pad {} time(s) since it started{}{}", g_list_requests.load(),
                g_reads.load(), g_via_hook.load() ? "; the stand-in went through another module's hook" : "",
                g_state.load() == kAbandoned ? "; the virtual pad given up for the ViGEm pad (it reads idle, the list is Windows' own)" : "");
        } else {
            text += "; the game has not asked for its gamepad list yet";
        }
        std::string other;
        std::string route;
        {
            std::scoped_lock lock{ g_other_module_mutex };
            other = g_other_module;
            route = g_route_note;
        }
        if (!route.empty()) {
            text += "; (fix195) " + route;
        }
        if (const int passed = g_passed_on.load(); passed > 0) {
            text += "; " + std::to_string(passed) + " request(s) passed on unchanged" + (other.empty() ? std::string{} : " (first from another module: " + other + ")");
        }
        if (const int failures = g_event_failures.load(); failures > 0) {
            text += fmt::format("; (fix195) Windows refused {} event registration(s) (last 0x{:08X}): the game got tokens of the mod's own", failures,
                static_cast<unsigned long>(g_event_failure_result.load()));
        }
        return text;
    }
}
