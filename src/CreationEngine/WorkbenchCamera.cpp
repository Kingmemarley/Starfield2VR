#include "WorkbenchCamera.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include <mods/VR.hpp>

namespace WorkbenchCamera
{
    namespace
    {
        // The camera's "enter furniture" (Starfield 1.16.236), at its workbench branch: IsWorkbench (0x140A5E6A0) true, then
        //   mov edx, 0x14        the third-person state's index
        //   mov rcx, rbp         the player camera
        //   call SetState        (0x141E8C2C0: the old state's End, the new one's Begin)
        // then the bench's "go back on exit" flag (+0x2C7 = 1) and its blur on entry (to UIBlurManager, a count it takes back only on its
        // third-person exit, 0x141E924E8: kept in first person, the world stayed blurred for good, the review found), and on at
        // 0x141E91FE7 (rdi loaded there again): skipped to there, all three left out.
        constexpr uintptr_t kSwitchRva = 0x1E91D83;
        constexpr uint8_t   kSwitch[]  = { 0xBA, 0x14, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCD, 0xE8, 0x30, 0xA5, 0xFF, 0xFF };
        constexpr uintptr_t kAfterRva  = 0x1E91FE7;
        // The player camera: the game's singleton (its address stored at 0x1461E51F0); its current state at +0x10, its states at
        // +0x188 + index x 8 (0 first person, 20 third person, 21 furniture).
        constexpr uintptr_t kCameraRva    = 0x61E51F0;
        constexpr uintptr_t kCurrentState = 0x10;
        constexpr uintptr_t kStates       = 0x188;
        constexpr int       kStateCount   = 32;

        uintptr_t             g_base = 0;
        safetyhook::MidHook   g_hook{};
        std::atomic<uint32_t> g_skipped{ 0 }, g_left{ 0 };
        std::string           g_installed{ "not installed" };

        bool ReadPtr(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool Matches()
        {
            return SameCode(kSwitchRva, kSwitch, sizeof(kSwitch));
        }

        int StateIndexOf(uintptr_t camera)
        {
            uintptr_t current = 0;
            if (camera == 0 || !ReadPtr(camera + kCurrentState, current) || current == 0) {
                return -1;
            }
            for (int i = 0; i < kStateCount; ++i) {
                uintptr_t state = 0;
                if (!ReadPtr(camera + kStates + static_cast<uintptr_t>(i) * 8, state)) {
                    return -1;
                }
                if (state == current) {
                    return i;
                }
            }
            return -1;
        }

        // The game is about to turn to its third-person camera for a workbench: skipped while in first person (in the headset, the
        // setting on).
        void OnSwitch(safetyhook::Context& ctx)
        {
            static auto vr    = VR::get();
            const int   state = StateIndexOf(ctx.rbp);
            const bool  skip  = GameFlow::gStore.internalSettings.workbenchFirstPerson && vr != nullptr && vr->is_hmd_active() && state == 0;
            static int  told  = 0;
            if (told < 20) {
                ++told;
                spdlog::info("[Workbench] the game turns to its third-person camera for a workbench (the camera now in state {}): {}", state,
                    skip ? "skipped, first person kept" : "left as the game does it");
            }
            if (!skip) {
                g_left.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            ctx.rip = At(kAfterRva);
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (g_base == 0 || !Matches()) {
            g_installed = "the workbench camera switch is not as in Starfield 1.16.236: left as the game has it";
            spdlog::warn("[Workbench] {} (rva {:x})", g_installed, kSwitchRva);
            return;
        }
        // (fix190) Where the hook goes on to, not checked by its own code.
        if (!GameCode::Known({ kAfterRva })) {
            g_installed = "the workbench camera switch's way on is not known in this game version (GameCode): off, left as the game has it";
            spdlog::warn("[Workbench] {} (rva {:x})", g_installed, kAfterRva);
            return;
        }
        g_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kSwitchRva)), &OnSwitch);
        if (!g_hook) {
            g_installed = "could not hook the workbench camera switch: left as the game has it";
            spdlog::warn("[Workbench] {}", g_installed);
            return;
        }
        g_installed = "hooked";
        spdlog::info("[Workbench] workbenches in first person: the game's switch to its third-person camera (rva {:x}) is skipped in the "
                     "headset while in first person (setting WorkbenchFirstPerson)",
            kSwitchRva);
    }

    int CameraStateIndex()
    {
        uintptr_t       camera = 0;
        const uintptr_t at     = At(kCameraRva);
        if (g_base == 0 || at == 0 || !ReadPtr(at, camera)) { // (fix190: 0 when not known for this game version)
            return -1;
        }
        return StateIndexOf(camera);
    }

    std::string Status()
    {
        return std::format("{}; the third-person switch skipped {} times, left {} times", g_installed, g_skipped.load(), g_left.load());
    }
}
