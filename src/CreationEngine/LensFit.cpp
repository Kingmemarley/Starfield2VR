#include "LensFit.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "ShadowShare.h"
#include "StereoView.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <mods/VR.hpp>

namespace LensFit
{
    namespace
    {
        // Starfield 1.16.236 (rvas from the game's base): each hooked instruction checked by its bytes (and the next one's first).
        // (fix190) Each place and its bytes as constants of their own, for GameCode (GameCode.h): checked and found where the hooks are
        // listed (Install).
        struct Code
        {
            uintptr_t   rva;
            const char* what;
        };
        // The sun's shadow cascades' fit (0x1429A5B90): the camera's frustum {left, right, top, bottom} copied to [rsp+60..6C]; here
        // vbroadcastss xmm0, [rsp+68] (top, the up axis's scale), then [rsp+64] (right, the right axis's) at 0x1429A5D66.
        constexpr uintptr_t kCascadeFitRva     = 0x29A5D55;
        constexpr uint8_t   kCascadeFitBytes[] = { 0xC4, 0xE2, 0x79, 0x18, 0x44, 0x24, 0x68, 0xC5 };
        constexpr Code      kCascadeFit        = { kCascadeFitRva, "the shadow cascades' fit" };
        // The second shadow box (0x1429A6CC0): the frustum at [rsp+30..3C]; here vbroadcastss xmm0, [rsp+38] (top), then [rsp+34]
        // (right). (Jumped to from 0x1429A7398 and 0x1429A7466: to this instruction's start, which the hook keeps.)
        constexpr uintptr_t kShadowBoxRva     = 0x29A746C;
        constexpr uint8_t   kShadowBoxBytes[] = { 0xC4, 0xE2, 0x79, 0x18, 0x44, 0x24, 0x38, 0xC4 };
        constexpr Code      kShadowBox        = { kShadowBoxRva, "the shadow box" };
        // Ambient occlusion's constants (SAORawAORenderPass, 0x1429BADF0), all written by here: vextractps [rbp+54], xmm6, 3; rcx the
        // view's constant block (+0x1A0 the inverse projection, its off-centre terms at +0x1D0 and +0x1D4; +0x1E0 the projection).
        constexpr uintptr_t kAmbientRva     = 0x29BB4C1;
        constexpr uint8_t   kAmbientBytes[] = { 0xC4, 0xE3, 0x79, 0x17, 0x75, 0x54, 0x03, 0xC5 };
        constexpr Code      kAmbient        = { kAmbientRva, "ambient occlusion's constants" };
        // The sun's disk (SunDiskRenderPass, 0x142B0BC60): xmm0 = the frustum's right edge ([rax+rdx*8+4]); here vzeroupper and the
        // call to atanf (radians), from which the field of view the disks are sized by is made (in degrees, xmm6).
        constexpr uintptr_t kSunDiskRva     = 0x2B0C0F5;
        constexpr uint8_t   kSunDiskBytes[] = { 0xC5, 0xF8, 0x77, 0xE8, 0x21, 0x25, 0x9C, 0x00 };
        constexpr Code      kSunDisk        = { kSunDiskRva, "the sun disk's field of view" };

        uintptr_t                g_base = 0;
        safetyhook::MidHook      g_cascade_hook{}, g_box_hook{}, g_ambient_hook{}, g_sun_hook{};
        std::atomic<uint32_t>    g_cascades{ 0 }, g_boxes{ 0 }, g_ambients{ 0 }, g_suns{ 0 };
        std::atomic<uint32_t>    g_off_centre{ 0 }; // (of the fits: how many had an off-centre frustum, for the log)
        std::atomic<uint32_t>    g_widened{ 0 };    // (fix140: widened for both eyes, full stereo with the shadows shared)
        std::string              g_installed{ "not installed" };

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool ReadFloats(uintptr_t at, float* out, size_t count)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), count * sizeof(float));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool Finite(float v) { return std::isfinite(v); }

        // {left, right, top, bottom} at [at]: right and top made the half-extents that cover the whole frustum (the largest of each
        // pair's sizes), so the fit, which spans +-right and +-top, covers an off-centre frustum. The same numbers for a centred one.
        void Cover(uintptr_t at)
        {
            float f[4]{};
            std::memcpy(f, reinterpret_cast<const void*>(at), sizeof(f));
            if (!Finite(f[0]) || !Finite(f[1]) || !Finite(f[2]) || !Finite(f[3])) {
                return;
            }
            const float right = std::max(std::abs(f[0]), std::abs(f[1]));
            const float top   = std::max(std::abs(f[2]), std::abs(f[3]));
            if (right != f[1] || top != f[2]) {
                g_off_centre.fetch_add(1, std::memory_order_relaxed);
            }
            std::memcpy(reinterpret_cast<void*>(at + 4), &right, 4);
            std::memcpy(reinterpret_cast<void*>(at + 8), &top, 4);
        }

        void OnCascadeFit(safetyhook::Context& ctx)
        {
            g_cascades.fetch_add(1, std::memory_order_relaxed);
            Cover(ctx.rsp + 0x60);
            // (fix140) Full stereo with the right eye given the left eye's shadows (ShadowShare): each cascade spans +-right*z across
            // at the depth z (the axes scaled by right and top here, used by the split function 0x142AB9F00 for each cascade), and the
            // right eye's view reaches the eyes' distance further right: right widened by ipd / z0 (z0 the first cascade's far depth,
            // [rbx+0] plus xmm11, as the split function makes it: fix140's review), which covers it at every cascade's far end.
            auto& vr = VR::get();
            if (ShadowShare::On() && vr != nullptr && vr->is_native_frame(vr->m_render_frame_count.load())) {
                float start = 0.0f;
                std::memcpy(&start, reinterpret_cast<const void*>(ctx.rbx), 4);
                const float d    = start + ctx.xmm11.f32[0];
                float       ipd  = StereoView::EyesApart();
                ipd              = ipd > 0.01f && ipd < 0.2f ? ipd : 0.07f;
                float right      = 0.0f;
                std::memcpy(&right, reinterpret_cast<const void*>(ctx.rsp + 0x64), 4);
                if (std::isfinite(d) && d >= 1.0f && std::isfinite(right) && right > 0.0f) {
                    right += ipd / d;
                    std::memcpy(reinterpret_cast<void*>(ctx.rsp + 0x64), &right, 4);
                    g_widened.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        void OnShadowBox(safetyhook::Context& ctx)
        {
            g_boxes.fetch_add(1, std::memory_order_relaxed);
            Cover(ctx.rsp + 0x30);
        }

        // Ambient occlusion rebuilds a pixel's place as (its screen place times a scale, plus an offset) times its depth: the offsets
        // ([rbp+44] across, [rbp+4C] up) made from the inverse projection's scale alone, as for a centred frustum. Its off-centre terms
        // added (zero for a centred one), and its "one over the right edge" ([rbp-54], the radius's scale across) the projection's
        // 2 / (right - left), the same for a centred one.
        void OnAmbient(safetyhook::Context& ctx)
        {
            g_ambients.fetch_add(1, std::memory_order_relaxed);
            float off[2]{}, scale = 0.0f;
            if (!ReadFloats(ctx.rcx + 0x1D0, off, 2) || !ReadFloats(ctx.rcx + 0x1E0, &scale, 1) || !Finite(off[0]) || !Finite(off[1]) ||
                !Finite(scale) || !(scale > 0.0f) || std::abs(off[0]) > 4.0f || std::abs(off[1]) > 4.0f) {
                return;
            }
            auto* across = reinterpret_cast<float*>(ctx.rbp + 0x44);
            auto* up     = reinterpret_cast<float*>(ctx.rbp + 0x4C);
            auto* radius = reinterpret_cast<float*>(ctx.rbp - 0x54);
            *across += off[0];
            *up += off[1];
            *radius = scale;
        }

        // The disks' field of view from the frustum's half-width ((right - left) / 2) rather than its right edge: the same in both
        // eyes, and the game's for a centred frustum.
        void OnSunDisk(safetyhook::Context& ctx)
        {
            g_suns.fetch_add(1, std::memory_order_relaxed);
            float f[2]{};
            if (!ReadFloats(ctx.rax + ctx.rdx * 8, f, 2) || !Finite(f[0]) || !Finite(f[1]) || !(f[1] > f[0])) {
                return;
            }
            ctx.xmm0.f32[0] = (f[1] - f[0]) * 0.5f;
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        struct Site
        {
            const Code*               code;
            bool                      same; // (fix190) its bytes as in Starfield 1.16.236, and its address, for this game version
            uintptr_t                 at;
            safetyhook::MidHook*      hook;
            safetyhook::MidHookFn     fn;
        };
        const Site sites[] = {
            { &kCascadeFit, SameCode(kCascadeFitRva, kCascadeFitBytes, sizeof(kCascadeFitBytes)), At(kCascadeFitRva), &g_cascade_hook, &OnCascadeFit },
            { &kShadowBox, SameCode(kShadowBoxRva, kShadowBoxBytes, sizeof(kShadowBoxBytes)), At(kShadowBoxRva), &g_box_hook, &OnShadowBox },
            { &kAmbient, SameCode(kAmbientRva, kAmbientBytes, sizeof(kAmbientBytes)), At(kAmbientRva), &g_ambient_hook, &OnAmbient },
            { &kSunDisk, SameCode(kSunDiskRva, kSunDiskBytes, sizeof(kSunDiskBytes)), At(kSunDiskRva), &g_sun_hook, &OnSunDisk },
        };
        std::string done;
        for (const auto& site : sites) {
            if (g_base == 0 || !site.same) {
                spdlog::warn("[LensFit] {} is not at rva {:x} as in Starfield 1.16.236: left as the game has it", site.code->what, site.code->rva);
                continue;
            }
            *site.hook = safetyhook::create_mid(reinterpret_cast<void*>(site.at), site.fn);
            if (!*site.hook) {
                spdlog::warn("[LensFit] could not hook {}: left as the game has it", site.code->what);
                continue;
            }
            done += done.empty() ? site.code->what : std::string{ ", " } + site.code->what;
        }
        g_installed = done.empty() ? std::string{ "none hooked" } : done;
        spdlog::info("[LensFit] each eye's own off-centre lens: hooked {} (each the game's own numbers for a centred frustum)", g_installed);
    }

    std::string Status()
    {
        const uint32_t cascades = g_cascades.exchange(0), boxes = g_boxes.exchange(0), ambients = g_ambients.exchange(0), suns = g_suns.exchange(0);
        const uint32_t off      = g_off_centre.exchange(0), widened = g_widened.exchange(0);
        return std::format("hooked: {}; since last: shadow fits {} (off-centre {}, widened for both eyes {}), shadow boxes {}, ambient occlusion {}, "
                           "sun disks {}", g_installed,
            cascades, off, widened, boxes, ambients, suns);
    }
}
