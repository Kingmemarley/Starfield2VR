#include "MenuTaa.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <CreationEngine/models/GameFlow.h>
#include <ModSettings.h>
#include <mods/VR.hpp>

namespace MenuTaa
{
    namespace
    {
        // Starfield 1.16.236: in the upscaler's choice (0x140840A10), the call that asks whether a shown menu wants TAA, the store of
        // its answer, and where it is read (with the setting).
        constexpr uintptr_t kCallRva   = 0x840B67;
        constexpr uint8_t   kCall[]    = { 0xE8, 0xD4, 0x0F, 0xD0, 0x01 };             // call 0x142541B40
        constexpr uintptr_t kStoreRva  = 0x840B6C;
        constexpr uint8_t   kStore[]   = { 0x88, 0x85, 0xB0, 0x00, 0x00, 0x00 };       // mov [rbp+0xB0], al
        constexpr uintptr_t kReadRva   = 0x840DC7;
        constexpr uint8_t   kRead[]    = { 0x80, 0xBD, 0xB0, 0x00, 0x00, 0x00, 0x00 }; // cmp byte ptr [rbp+0xB0], 0
        constexpr size_t    kViewsSlot = 0x50; // (the frame's views' list on the stack there: its count first, the world's view the first)

        safetyhook::MidHook   g_hook{};
        std::atomic<uint64_t> g_changed{ 0 };  // frames the answer was made yes
        std::atomic<bool>     g_was_changing{ false };
        std::atomic<int>      g_logs{ 0 };
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool ReadCount(uintptr_t at, uint32_t& count)
        {
            __try {
                count = *reinterpret_cast<const uint32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Right after the game asked (the game's thread choosing the frame's upscaler, once a frame): its answer in al.
        void OnAnswer(safetyhook::Context& ctx)
        {
            auto& vr = VR::get();
            const auto xr = vr != nullptr ? vr->m_openxr : nullptr; // (a copy: it may be made anew on another thread)
            // (Only while a game menu is shown: the list may hold other views in other modes, fix128's review.)
            if (xr == nullptr || !xr->panel_menu_taa.load() || !ModSettings::g_internalSettings.showQuadDisplay) {
                return;
            }
            uint32_t views = 0;
            const bool asked_yes = (ctx.rax & 0xff) != 0;
            const bool more      = !asked_yes && ReadCount(ctx.rsp + kViewsSlot, views) && views > 1 && views < 64;
            if (more) {
                ctx.rax = (ctx.rax & ~0xffull) | 1;
                g_changed.fetch_add(1, std::memory_order_relaxed);
            }
            // (Logged as it starts and stops for a menu.)
            if (more != g_was_changing.exchange(more) && g_logs.fetch_add(1) < 40) {
                if (more) {
                    spdlog::info("[MenuTaa] the frame has the world and {} menu view(s) and the menu ({}) does not ask for TAA: asked for it, "
                                 "as the Data menu does (the world at full size, with anti-aliasing, not copied up from DLSS's size)",
                        views - 1, GameFlow::shownMenus());
                } else {
                    spdlog::info("[MenuTaa] no longer needed ({} frames so far)", g_changed.load());
                }
            }
        }
    }

    void Install()
    {
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (base == 0 || !SameCode(kCallRva, kCall, sizeof(kCall)) || !SameCode(kStoreRva, kStore, sizeof(kStore)) ||
            !SameCode(kReadRva, kRead, sizeof(kRead))) {
            SetStatus("Menus' 3D views with TAA: off (the game's code is not as in Starfield 1.16.236)");
            spdlog::warn("[MenuTaa] the upscaler's choice is not at rva {:x} as in Starfield 1.16.236 (another version, or another mod changed "
                         "it): menus without the Data menu's flag keep the game's own choice",
                kStoreRva);
            return;
        }
        g_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kStoreRva)), &OnAnswer);
        if (!g_hook) {
            SetStatus("Menus' 3D views with TAA: off (could not hook)");
            spdlog::warn("[MenuTaa] could not hook the upscaler's choice (rva {:x})", kStoreRva);
            return;
        }
        SetStatus("Menus' 3D views with TAA: ready");
        spdlog::info("[MenuTaa] the upscaler's choice hooked (rva {:x}): every menu with a 3D view of its own gets TAA at full size, as the "
                     "Data menu does",
            kStoreRva);
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
