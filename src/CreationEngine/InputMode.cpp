#include "InputMode.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>
#include <utility/Scan.hpp>

#include "memory/GameCode.h"
#include "memory/ScanHelper.h"

namespace InputMode
{
    namespace
    {
        constexpr uintptr_t kKnownGlobalRva = 0x5fe1a50; // (1.16.236)
        constexpr uintptr_t kModeOffset     = 0x60;

        std::mutex               g_mutex{};
        std::atomic<uintptr_t>   g_global{ 0 };  // the global holding the manager's pointer (checked)
        std::atomic<bool>        g_checked{ false };
        std::atomic<bool>        g_given_up{ false };
        std::chrono::steady_clock::time_point g_next_try{};
        int                      g_tries{ 0 };
        std::string              g_description{ "not looked for yet" };

        bool Readable(uintptr_t address, size_t size)
        {
            if (address < 0x10000) {
                return false;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
                return false;
            }
            const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            return address + size <= end;
        }

        bool InModule(uintptr_t address)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            return address >= base && address < base + MemoryScan::mod_size;
        }

        // The global a `mov reg,[rip+x]` at `at` reads (the displacement at +3, the instruction 7 bytes long).
        uintptr_t RipTarget(uintptr_t at)
        {
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + 3), sizeof(rel));
            return at + 7 + static_cast<intptr_t>(rel);
        }

        // The manager the global points at, when it is one (its type checked by RTTI).
        uintptr_t ManagerAt(uintptr_t global)
        {
            if (global == 0 || !InModule(global) || !Readable(global, 8)) {
                return 0;
            }
            const uintptr_t object = *reinterpret_cast<const uintptr_t*>(global);
            if (object == 0 || !Readable(object, kModeOffset + 1)) {
                return 0;
            }
            const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(object);
            if (!InModule(vtable) || !Readable(vtable - 8, 16)) {
                return 0;
            }
            return utility::rtti::derives_from(reinterpret_cast<const void*>(object), "class BSInputDeviceManager") ? object : 0;
        }
    }

    bool Find()
    {
        if (g_checked.load()) {
            return true;
        }
        if (g_given_up.load()) {
            return false;
        }
        std::scoped_lock _{ g_mutex };
        if (g_checked.load()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < g_next_try) {
            return false;
        }
        g_next_try = now + std::chrono::seconds(3);
        ++g_tries;

        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        // The places the game's code reads it: the cursor-in-window check (189f914: mov rcx,[global]; cmp byte [rcx+60h],1;
        // jbe) and the window procedure (1883baa: mov rax,[global]; test rax,rax; je; cmp byte [flag],0; jne; cmp byte [rax+60h],1; ja).
        uintptr_t from_clip = 0, from_window = 0;
        if (const auto at = utility::scan(MemoryScan::mod, "48 8B 0D ? ? ? ? 80 79 60 01 0F 86")) {
            from_clip = RipTarget(*at);
        }
        if (const auto at = utility::scan(MemoryScan::mod, "48 8B 05 ? ? ? ? 48 85 C0 74 ? 80 3D ? ? ? ? 00 75 ? 80 78 60 01 77")) {
            from_window = RipTarget(*at);
        }
        const uintptr_t known = GameCode::At(kKnownGlobalRva); // (fix190) 0 when not known for this game version: only the scans then
        uintptr_t       chosen = 0;
        for (const uintptr_t candidate : { from_clip, from_window, known }) {
            if (candidate != 0 && ManagerAt(candidate) != 0) {
                chosen = candidate;
                break;
            }
        }
        const auto rva = [&](uintptr_t a) { return a != 0 ? fmt::format("{:x}", a - base) : std::string{ "not found" }; };
        if (chosen == 0) {
            const bool made_yet = (from_clip != 0 && Readable(from_clip, 8) && *reinterpret_cast<const uintptr_t*>(from_clip) != 0) ||
                                  (Readable(known, 8) && *reinterpret_cast<const uintptr_t*>(known) != 0);
            g_description = fmt::format("the game's input manager not found (its code: {} and {}; the fixed place {}{})", rva(from_clip), rva(from_window),
                rva(known), made_yet ? ", holding something else" : ", empty so far");
            if (g_tries >= 20 || (made_yet && g_tries >= 3)) {
                g_given_up = true;
                spdlog::warn("[InputMode] {}: the menu pointer presses a key to switch to the mouse and cannot check it", g_description);
            }
            return false;
        }
        const uintptr_t manager = ManagerAt(chosen);
        if (manager == 0) {
            return false; // (gone between the two looks: looked for again)
        }
        g_global  = chosen;
        g_checked = true;
        g_description = fmt::format("the game's input manager at {:x} (its pointer at rva {}; the code found it at {} and {}; the fixed place is {}), "
                                    "mode byte now {:#x}",
            manager, rva(chosen), rva(from_clip), rva(from_window), rva(known), *reinterpret_cast<const volatile uint8_t*>(manager + kModeOffset));
        spdlog::info("[InputMode] {}", g_description);
        return true;
    }

    int Raw()
    {
        // (Only once found: the search itself runs on its own thread, see Find.)
        if (!g_checked.load()) {
            return -1;
        }
        const uintptr_t global = g_global.load();
        const uintptr_t object = *reinterpret_cast<const uintptr_t*>(global);
        if (object == 0 || object < 0x10000) {
            return -1;
        }
        return *reinterpret_cast<const volatile uint8_t*>(object + kModeOffset);
    }

    bool IsMouse()
    {
        const int raw = Raw();
        return raw > 1;
    }

    bool IsGamepad()
    {
        const int raw = Raw();
        return raw == 0 || raw == 1;
    }

    bool ForceMouse()
    {
        if (!g_checked.load()) {
            return false;
        }
        // (Checked again before writing: the manager the global points at now, of its type.)
        const uintptr_t object = ManagerAt(g_global.load());
        if (object == 0) {
            return false;
        }
        *reinterpret_cast<volatile uint8_t*>(object + kModeOffset) = 0xFF;
        return true;
    }

    std::string Describe()
    {
        std::scoped_lock _{ g_mutex };
        return g_description;
    }
}
