#include "MenuLayer.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include "ConversationPanel.h"
#include "BuildConsole.h"
#include "CreationEngineRendererModule.h"
#include "RenderPassIo.h"
#include "StereoView.h"
#include "memory/ScanHelper.h"
#include <ModSettings.h>
#include <RE/C/CreationRendererPrivate.h>
#include <mods/VR.hpp>

namespace MenuLayer
{
    namespace
    {
        using PassFn = uintptr_t (*)(void* pass, RE::CreationRendererPrivate::RenderGraphData* data, void* pass_data);

        PassFn                g_original{ nullptr };
        std::atomic<uint64_t> g_calls{ 0 };
        std::atomic<uint64_t> g_copies{ 0 };
        std::atomic<int>      g_failures{ 0 };
        std::atomic<uint64_t> g_last_signature{ 0 };
        std::atomic<int>      g_signatures_logged{ 0 };
        std::atomic<bool>     g_was_wanted{ false };
        std::atomic<bool>     g_gave_up{ false };
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        using RenderPassIo::DescOf;
        using RenderPassIo::PassIo;
        using RenderPassIo::ReadPassIo;

        // Full stereo: the interface (the HUD) laid over the left eye's picture too, as the game lays it over the right eye's.
        bool NativeWanted()
        {
            auto& vr = VR::get();
            return vr != nullptr && vr->is_hmd_active() && vr->is_native_frame(vr->m_render_frame_count.load()) && StereoView::Active();
        }

        bool Wanted()
        {
            const auto& s = ModSettings::g_internalSettings;
            if (!s.showQuadDisplay || !s.menuWorldBehind || !s.menuSeeThrough || s.forceFlatScreen) {
                return false;
            }
            auto& vr = VR::get();
            if (vr == nullptr || !vr->is_hmd_active() || !vr->get_runtime()->is_openxr()) {
                return false;
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            // (Not with darkness behind the panel: nothing would show through.)
            return xr != nullptr && xr->panel_see_through.load() && xr->panel_enabled.load() && xr->panel_backdrop.load() != 0;
        }

        // (counts: a see-through menu's copy; full stereo's alone does not turn see-through menus off, it is only logged once.)
        void Fail(const char* what, bool counts)
        {
            if (!counts) {
                static bool told = false;
                if (!told) {
                    told = true;
                    spdlog::warn("[MenuLayer] full stereo: {} (the left eye without the interface)", what);
                }
                return;
            }
            const int n = ++g_failures;
            if (n == 1 || n == 30) {
                spdlog::warn("[MenuLayer] {}{}", what, n == 30 ? " (30 times: see-through menus are off until the game restarts)" : "");
            }
            if (n >= 30) {
                g_gave_up = true;
                SetStatus(std::string{ "See-through menus: off (" } + what + ")");
            }
        }

        // (fix145) The conversation on its own panel (ConversationPanel): before the game lays its interface layer over the picture, the
        // conversation's part copied out of the layer and its box cleared there (neither eye's picture has it; full stereo's left eye takes
        // its copy of the layer after this). The layer: the texture the pass reads, an 8-bit one with alpha, the size of the picture it
        // draws into (as below).
        void CutConversation(RE::CreationRendererPrivate::RenderGraphData* data, void* pass_data)
        {
            if (!ConversationPanel::CutWanted()) {
                return;
            }
            PassIo io{};
            if (!ReadPassIo(pass_data, io)) {
                return;
            }
            D3D12_RESOURCE_DESC   descs[RenderPassIo::kMaxItems]{};
            bool                  have[RenderPassIo::kMaxItems]{};
            D3D12_RESOURCE_STATES states[RenderPassIo::kMaxItems]{};
            int                   target = -1;
            for (uint32_t i = 0; i < io.count; ++i) {
                have[i]   = io.items[i].resource != nullptr && DescOf(io.items[i].resource, descs[i]);
                states[i] = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[i].flags));
                if (have[i] && target < 0 && (states[i] & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0) {
                    target = static_cast<int>(i);
                }
            }
            int layer = -1;
            for (uint32_t i = 0; i < io.count; ++i) {
                if (!have[i] || static_cast<int>(i) == target ||
                    (states[i] & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) == 0) {
                    continue;
                }
                if (target >= 0 && (descs[i].Width != descs[target].Width || descs[i].Height != descs[target].Height)) {
                    continue;
                }
                const auto f = descs[i].Format;
                if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    layer = static_cast<int>(i);
                    break;
                }
            }
            // (Its state known: an untracked one is not where the pass says, fix124; the review.)
            if (layer < 0 || !io.items[layer].tracked) {
                ConversationPanel::NoteCut(-1);
                return;
            }
            D3D12_BOX copy{}, clear{};
            if (!ConversationPanel::CutBoxes(static_cast<uint32_t>(descs[layer].Width), descs[layer].Height, copy, clear)) {
                return;
            }
            auto* context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(data->getCommandList());
            if (context == nullptr || context->pID3D12CommandList == nullptr || context->pID3D12CommandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                return;
            }
            // (Once a frame for a layer: a second pass over the same layer would copy the box already cleared, the review.)
            static ID3D12Resource* cut_layer = nullptr;
            static uint32_t        cut_frame = 0;
            const uint32_t         frame     = VR::get()->m_render_frame_count.load();
            if (cut_layer == io.items[layer].resource && cut_frame == frame) {
                return;
            }
            cut_layer = io.items[layer].resource;
            cut_frame = frame;
            ConversationPanel::NoteCut(
                VR::get()->m_d3d12.record_conversation(context->pID3D12CommandList, io.items[layer].resource, states[layer], copy, clear));
        }

        // (fix177) Build mode as a console (BuildConsole): before the game lays its interface layer over the picture, the build menu's and its
        // notifications' parts copied out of the layer into the console's picture and their boxes cleared there (as the conversation's
        // cut above, the same layer found the same way).
        void CutConsole(RE::CreationRendererPrivate::RenderGraphData* data, void* pass_data)
        {
            if (!BuildConsole::CutWanted()) {
                return;
            }
            PassIo io{};
            if (!ReadPassIo(pass_data, io)) {
                return;
            }
            D3D12_RESOURCE_DESC   descs[RenderPassIo::kMaxItems]{};
            bool                  have[RenderPassIo::kMaxItems]{};
            D3D12_RESOURCE_STATES states[RenderPassIo::kMaxItems]{};
            int                   target = -1;
            for (uint32_t i = 0; i < io.count; ++i) {
                have[i]   = io.items[i].resource != nullptr && DescOf(io.items[i].resource, descs[i]);
                states[i] = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[i].flags));
                if (have[i] && target < 0 && (states[i] & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0) {
                    target = static_cast<int>(i);
                }
            }
            int layer = -1;
            for (uint32_t i = 0; i < io.count; ++i) {
                if (!have[i] || static_cast<int>(i) == target ||
                    (states[i] & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) == 0) {
                    continue;
                }
                if (target >= 0 && (descs[i].Width != descs[target].Width || descs[i].Height != descs[target].Height)) {
                    continue;
                }
                const auto f = descs[i].Format;
                if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    layer = static_cast<int>(i);
                    break;
                }
            }
            // (Its state known: an untracked one is not where the pass says, fix124.)
            if (layer < 0 || !io.items[layer].tracked) {
                BuildConsole::NoteCut(-1);
                return;
            }
            vrmod::D3D12Component::ConsoleCut cut{};
            if (!BuildConsole::CutBoxes(static_cast<uint32_t>(descs[layer].Width), descs[layer].Height, cut)) {
                return;
            }
            auto* context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(data->getCommandList());
            if (context == nullptr || context->pID3D12CommandList == nullptr || context->pID3D12CommandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                return;
            }
            // (Once a frame for a layer: a second pass over the same layer would copy the boxes already cleared.)
            static ID3D12Resource* cut_layer = nullptr;
            static uint32_t        cut_frame = 0;
            const uint32_t         frame     = VR::get()->m_render_frame_count.load();
            if (cut_layer == io.items[layer].resource && cut_frame == frame) {
                return;
            }
            cut_layer = io.items[layer].resource;
            cut_frame = frame;
            BuildConsole::NoteCut(VR::get()->m_d3d12.record_console(context->pID3D12CommandList, io.items[layer].resource, states[layer], cut));
        }

        uintptr_t Hooked(void* pass, RE::CreationRendererPrivate::RenderGraphData* data, void* pass_data)
        {
            if (data != nullptr && pass_data != nullptr) {
                CutConversation(data, pass_data);
                CutConsole(data, pass_data); // (fix177)
            }
            const auto result = g_original(pass, data, pass_data);
            ++g_calls;
            const bool menu_wanted = Wanted();
            const bool wanted      = !g_gave_up.load() && data != nullptr && pass_data != nullptr && (menu_wanted || NativeWanted());
            // (No see-through menu any more: its last layer is not shown again, a moment late.)
            if (g_was_wanted.exchange(wanted) && !wanted) {
                if (auto& vr = VR::get(); vr != nullptr) {
                    vr->m_d3d12.drop_menu_layer();
                }
            }
            if (!wanted) {
                return result;
            }
            PassIo io{};
            if (!ReadPassIo(pass_data, io)) {
                Fail("the game's interface pass: its textures could not be read", menu_wanted);
                return result;
            }
            // The texture it draws into (a render target), and the one it reads (a shader resource) of that size: the layer.
            D3D12_RESOURCE_DESC   descs[RenderPassIo::kMaxItems]{};
            bool                  have[RenderPassIo::kMaxItems]{};
            D3D12_RESOURCE_STATES states[RenderPassIo::kMaxItems]{};
            int                   target = -1;
            for (uint32_t i = 0; i < io.count; ++i) {
                have[i]   = io.items[i].resource != nullptr && DescOf(io.items[i].resource, descs[i]);
                states[i] = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[i].flags));
                if (have[i] && target < 0 && (states[i] & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0) {
                    target = static_cast<int>(i);
                }
            }
            // (Of those that could be it, an 8-bit one with alpha first: what the interface is drawn on, the picture under it is not.)
            int layer = -1, candidates = 0;
            for (uint32_t i = 0; i < io.count; ++i) {
                if (!have[i] || static_cast<int>(i) == target ||
                    (states[i] & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) == 0) {
                    continue;
                }
                if (target >= 0 && (descs[i].Width != descs[target].Width || descs[i].Height != descs[target].Height)) {
                    continue;
                }
                ++candidates;
                const auto f       = descs[i].Format;
                const bool eight   = f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                                   f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                const bool chosen8 = layer >= 0 && (descs[layer].Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || descs[layer].Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                                       descs[layer].Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || descs[layer].Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                                                       descs[layer].Format == DXGI_FORMAT_B8G8R8A8_UNORM || descs[layer].Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                if (layer < 0 || (eight && !chosen8)) {
                    layer = static_cast<int>(i);
                }
            }
            // (Each different arrangement of its textures logged, the first few.)
            const uint64_t signature = (static_cast<uint64_t>(io.count) << 56) ^ (static_cast<uint64_t>(target + 1) << 48) ^ (static_cast<uint64_t>(layer + 1) << 40) ^
                                       (static_cast<uint64_t>(candidates) << 32) ^ (target >= 0 ? (descs[target].Width << 16) ^ descs[target].Height : 0);
            // (fix201) And every change in the 90 s after a loading screen ends (at most 200 more a session): which texture the left eye's and
            // the panel's interface comes from then.
            static std::atomic<int> s_diag_logged{ 0 };
            const int64_t           diag_since = VR::get()->diag_ms_since_load();
            if (g_last_signature.exchange(signature) != signature &&
                (g_signatures_logged.fetch_add(1) < 8 || (diag_since >= 0 && s_diag_logged.fetch_add(1) < 200))) {
                if (diag_since >= 0) {
                    spdlog::info("[MenuLayer] (fix201) {:.1f} s after loading screen {} ended (for {}):", diag_since / 1000.0, VR::get()->m_diag_loads.load(),
                        menu_wanted ? "a see-through menu" : "full stereo's left eye");
                }
                std::string list;
                for (uint32_t i = 0; i < io.count; ++i) {
                    list += fmt::format("; [{}] key {:x} index {} flags {:#x} (state {:#x}): {}", i, io.items[i].key, io.items[i].graph_index, io.items[i].flags,
                        (uint32_t)states[i],
                        have[i] ? fmt::format("{}x{} format {} flags {:#x}", (uint64_t)descs[i].Width, descs[i].Height, (int)descs[i].Format,
                                      (uint32_t)descs[i].Flags)
                                : std::string{ io.items[i].resource != nullptr ? "not a texture" : "no texture found" });
                }
                spdlog::info("[MenuLayer] the game's interface pass with a see-through menu: {} textures{}; it draws into [{}], the interface "
                             "layer is [{}] ({} could be)",
                    io.count, list, target, layer, candidates);
            }
            if (layer < 0) {
                Fail("the game's interface pass: no texture it reads the size of its picture (the interface layer)", menu_wanted);
                return result;
            }
            auto* context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(data->getCommandList());
            if (context == nullptr || context->pID3D12CommandList == nullptr) {
                Fail("the game's interface pass: no command list", menu_wanted);
                return result;
            }
            const int recorded = VR::get()->m_d3d12.record_menu_layer(context->pID3D12CommandList, io.items[layer].resource, states[layer]);
            if (recorded == 0) {
                return result; // (not this time)
            }
            if (recorded > 0) {
                if (g_copies++ == 0) {
                    spdlog::info("[MenuLayer] the interface layer copied for a see-through menu (first time)");
                    SetStatus(fmt::format("See-through menus: working (the interface layer {}x{})", (uint64_t)descs[layer].Width, descs[layer].Height));
                }
                g_failures = 0;
            } else {
                Fail("the game's interface pass: its layer could not be copied", menu_wanted);
            }
            return result;
        }
    }

    void Install()
    {
        // (The type table of the object itself, offset 0 in it: its complete object locator, just before the table, says so.)
        std::optional<uintptr_t> vtable{};
        for (const uintptr_t table : utility::rtti::find_vtables(MemoryScan::mod, ".?AVScaleformCompositeRenderPass@CreationRendererPrivate@@")) {
            const uintptr_t locator = *reinterpret_cast<const uintptr_t*>(table - 8);
            if (locator != 0 && *reinterpret_cast<const uint32_t*>(locator + 4) == 0) {
                vtable = table;
                break;
            }
        }
        if (!vtable) {
            spdlog::warn("[MenuLayer] the game's interface pass (ScaleformCompositeRenderPass) not found: menus are not see-through");
            SetStatus("See-through menus: off (the game's interface pass not found)");
            return;
        }
        auto*           slot = reinterpret_cast<uintptr_t*>(*vtable) + 7;
        const uintptr_t fn   = *slot;
        const auto      base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (fn < base || fn >= base + MemoryScan::mod_size) {
            spdlog::warn("[MenuLayer] the game's interface pass: its function 7 ({:x}) is not in the game: menus are not see-through", fn);
            SetStatus("See-through menus: off (the game's interface pass is not as expected)");
            return;
        }
        // (Its type table's entry replaced, as Luma does: only this pass, whatever else shares the function.)
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
            spdlog::warn("[MenuLayer] the game's interface pass: its type table could not be changed: menus are not see-through");
            SetStatus("See-through menus: off (could not hook the game's interface pass)");
            return;
        }
        g_original = reinterpret_cast<PassFn>(fn);
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(reinterpret_cast<uintptr_t>(&Hooked)));
        VirtualProtect(slot, sizeof(uintptr_t), old, &old);
        spdlog::info("[MenuLayer] the game's interface pass hooked (ScaleformCompositeRenderPass: type table rva {:x}, function 7 rva {:x})",
            *vtable - base, fn - base);
        SetStatus("See-through menus: ready (open a menu)");
    }

    bool SeeThroughWanted()
    {
        return !g_gave_up.load() && g_original != nullptr && Wanted();
    }

    bool SeeThroughPossible()
    {
        if (g_gave_up.load() || g_original == nullptr || ModSettings::g_internalSettings.forceFlatScreen) {
            return false;
        }
        auto& vr = VR::get();
        if (vr == nullptr || !vr->is_hmd_active() || !vr->get_runtime()->is_openxr()) {
            return false;
        }
        const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
        return xr != nullptr && xr->panel_see_through.load() && xr->panel_enabled.load() && xr->panel_backdrop.load() != 0;
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return fmt::format("{} [passes {}, copies {}]", g_status, g_calls.load(), g_copies.load());
    }
}
