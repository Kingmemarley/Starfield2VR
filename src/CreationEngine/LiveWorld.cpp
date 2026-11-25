#include "LiveWorld.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "CreationEngineRendererModule.h"
#include "DlssViews.h"
#include "FullFrame.h"
#include "MenuLayer.h"
#include "StereoView.h"
#include "EyeShare.h"
#include "GpuTimer.h"
#include "RenderPassIo.h"
#include "RttiNames.h"
#include "ViewHistory.h"
#include "memory/ScanHelper.h"
#include <CreationEngine/models/GameFlow.h>
#include <ModSettings.h>
#include <RE/C/CreationRendererPrivate.h>
#include <mods/VR.hpp>

namespace LiveWorld
{
    namespace
    {
        using GraphData = RE::CreationRendererPrivate::RenderGraphData;
        using StepFn    = uintptr_t (*)(void* pass, GraphData* data, void* pass_data);

        // The game's frame code (Starfield 1.16.236, the function that also waits on the renderer in Main::Swap; on the game's main
        // thread, its "this", Main, in r13): the world is drawn this frame unless a shown menu has the full-screen flag ("dil": one
        // found) and the loading-screen exception does not hold ("r15b": a loading screen is up, Main+0x44b, and the place is ready):
        //   40 84 FF           test dil, dil
        //   74 05              je   draw          (no such menu: drawn)
        //   45 84 FF           test r15b, r15b    <- watched here (only reached with such a menu shown): r15b set when the world is
        //   74 06              je   not_drawn        to be kept drawn behind a see-through menu, and no loading screen is up (as the
        //   41 0F B6 CC  draw: movzx ecx, r12b       loading screen's exception does; r15 is not used after this)
        //   EB 02              jmp  store
        //   32 C9  not_drawn:  xor cl, cl
        //   48 8B 05 ...       mov rax, [the game's views]
        //   88 88 E1 04 00 00  mov [rax+0x4e1], cl  (the world drawn this frame)
        constexpr const char* kCheckPattern   = "40 84 FF 74 05 45 84 FF 74 06 41 0F B6 CC EB 02 32 C9 48 8B 05 ? ? ? ? 88 88 E1 04 00 00";
        constexpr size_t      kWatchOffset    = 5;    // the "test r15b, r15b" (5 bytes with its jump: nothing jumps into them)
        constexpr size_t      kViewsLoad      = 0x12; // the load of the game's views (its address relative to the next instruction: +3)
        constexpr uintptr_t   kMainLoading    = 0x44b; // Main: a loading screen is up (set and cleared by the game's LoadScreenRenderingEventHandler)
        constexpr const char* kMainCoded      = ".?AVMain@@";
        // The game's views (what the frame's views are chosen from, the same function's caller): the world's view (+0x3d0, or +0x3d4
        // when +0x4e0 is set), and whether the world is drawn this frame (+0x4e1).
        constexpr uintptr_t kWorldView      = 0x3d0;
        constexpr uintptr_t kWorldViewOther = 0x3d4;
        constexpr uintptr_t kWorldViewWhich = 0x4e0;
        constexpr uintptr_t kWorldDrawn     = 0x4e1;
        // A view's part of the frame (RenderViewSubGraph, made for each of the frame's views, its view's handle at +0xd28).
        constexpr uintptr_t   kSubGraphView  = 0xd28;
        constexpr const char* kSubGraphCoded = ".?AVRenderViewSubGraph@CreationRendererPrivate@@";
        constexpr const char* kGraphCoded    = ".?AVRenderGraph@CreationRendererPrivate@@";
        constexpr const char* kStepCoded     = ".?AVRenderPass@CreationRendererPrivate@@";
        constexpr const char* kTaaCoded      = ".?AVTemporalAA_idTech7RenderPass@CreationRendererPrivate@@";

        safetyhook::MidHook   g_check_hook{};
        uintptr_t             g_views_global{ 0 };
        std::atomic<bool>     g_installed{ false };
        std::atomic<bool>     g_capture{ false };  // a see-through menu wants it: the views' pictures copied (the camera is the headset's)
        std::atomic<bool>     g_draw{ false };     // ...for a few frames already: the world kept drawn (its frames drawn with that camera)
        std::atomic<uintptr_t> g_main{ 0 };        // (the game's Main, as checked once by its type)
        std::atomic<uint64_t> g_forced{ 0 };       // frames the world was kept drawn
        int                   g_wanted_frames{ 0 }; // (render thread) frames wanted in a row
        int                   g_unwanted_frames{ 0 }; // (render thread) frames not wanted in a row
        int                   g_switches{ 0 };
        uint32_t              g_frames_on{ 0 };    // (render thread) frames drawn with it on, this time
        bool                  g_drawn_logged{ false };

        // The kinds of step watched: their type table and its function 7 as it was (a table looked up by the type table's address on
        // every step of every frame: open addressing, entries only added, read without a lock).
        struct Hooked
        {
            std::atomic<uintptr_t> table{ 0 };
            uintptr_t              original{ 0 };
            std::atomic<bool>      live{ false }; // (its function 7 is StepHook: its steps are watched)
        };
        constexpr size_t             kHookSlots = 4096;
        std::array<Hooked, kHookSlots> g_hooked{};
        std::atomic<int>             g_hooked_count{ 0 };
        std::mutex                   g_hook_mtx{};
        std::atomic<uintptr_t>       g_taa_table{ 0 }; // the anti-aliasing step's kind (its history is kept in the view's persistent pictures)
        bool                         g_history_wanted{ false };
        std::shared_ptr<const ViewHistory::Places> g_places{};
        std::vector<std::pair<void*, uint32_t>>    g_places_views{};
        uint32_t                                   g_places_world{ 0xffffffff };
        uint32_t                                   g_places_age{ 0 };
        uint32_t                                   g_places_frame{ 0 }; // (the frame the steps were last walked in)

        size_t HookSlot(uintptr_t table)
        {
            return static_cast<size_t>((table >> 3) * 0x9E3779B97F4A7C15ull >> 52) & (kHookSlots - 1);
        }

        // The steps watched this frame and whose picture each is: 0 the world's, 1..3 one of the menu's 3D pictures. Of each kind, the
        // pictures its steps drew in the last frame, in order (the last one is its finished picture: from then on only that one is
        // copied).
        struct Role
        {
            int kind{ -1 };
            int order{ 0 }; // its place among its view's last steps (the order they are drawn in)
            bool operator==(const Role&) const = default;
        };
        struct Output
        {
            int      order{ -1 };
            uint32_t item{ 0 };
            void*    pass{ nullptr };
            uint32_t width{ 0 };  // (fix147, for the log: its picture's size)
            uint32_t height{ 0 };
        };
        // (The roles read on every step without a lock: the current map, kept alive a few frames after it is replaced.)
        using RoleMap = std::unordered_map<void*, Role>;
        std::mutex                                    g_roles_mtx{}; // (g_seen, g_final)
        std::shared_ptr<const RoleMap>                g_roles{};     // (render thread)
        std::array<std::shared_ptr<const RoleMap>, 4> g_roles_keep{};
        int                                           g_roles_next{ 0 };
        std::atomic<const RoleMap*>                   g_roles_now{ nullptr };

        void PublishRoles(std::shared_ptr<const RoleMap> roles)
        {
            g_roles                     = roles;
            g_roles_keep[g_roles_next]  = roles;
            g_roles_next                = (g_roles_next + 1) % static_cast<int>(g_roles_keep.size());
            g_roles_now.store(roles.get(), std::memory_order_release);
        }
        std::array<Output, 4>                   g_seen{};  // of each kind, the latest (by its steps' order) copyable picture drawn this frame
        std::array<Output, 4>                   g_final{}; // ...and last frame's: from then on only it is copied
        // The frame's views as last found (their parts of the frame and handles), and the world's view's handle then.
        std::vector<std::pair<void*, uint32_t>> g_views{};
        uint32_t                                g_world_seen{ 0xffffffff };
        int                                     g_views_logged{ 0 };
        uint32_t                                g_not_kept_logged{ 0 }; // (this menu: the reasons logged for not keeping the views' histories)

        std::atomic<uint64_t> g_world_copies{ 0 };
        std::atomic<uint64_t> g_picture_copies{ 0 };
        std::atomic<uint64_t> g_depth_copies{ 0 }; // (the menu's 3D pictures' depth)

        // (fix147) Full stereo with no see-through menu wanting the world: the world's view's pictures are the left eye's alone, and only
        // those of the eye image's size are taken (the game's finished picture, the size of the right eye's). The view's last steps
        // draw two of the colours copied here: HDRComposite's, 1996x2148, then the sharpening's (ContrastAdaptiveSharpening) finished
        // one, 1996x2150. Taking HDRComposite's when the finished step was to be found again (every picture of that frame copied) made
        // the copy's slot that size, and the finished pictures of that frame's parity were then refused for half a second: no left
        // picture on every other frame (fix146's log: "not there" on a quarter of the frames). Set as each frame starts (CRBeginFrame).
        std::atomic<bool> g_stereo_left{ false };
        // (fix147, the review) When a picture of the eye image's size was last seen for it, and since when full stereo has been on (ms): none
        // for a second (no sharpening step, another upscaler: only HDRComposite's 2148 picture), the old way again (any size), logged.
        std::atomic<int64_t> g_left_eye_sized_ms{ 0 };
        std::atomic<int64_t> g_stereo_left_since_ms{ 0 };
        std::atomic<bool>    g_left_any_size_told{ false };
        int64_t SteadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        // (fix147, for the log) The left eye's copies in the last 10 s: made, refused by the copy, pictures of another size not taken
        // (the last one's size and step); and each kind's finished step chosen anew (the first 20 times with why, then counted).
        std::atomic<uint64_t> g_left_copies{ 0 };
        std::atomic<uint64_t> g_left_refused{ 0 };
        std::atomic<uint64_t> g_left_other_size{ 0 };
        std::atomic<uint32_t> g_left_other_w{ 0 };
        std::atomic<uint32_t> g_left_other_h{ 0 };
        char                  g_left_other_step[64]{}; // (g_roles_mtx)
        int64_t               g_left_stats_ms{ 0 };    // (render thread, Watch)
        int                   g_final_logs{ 0 };       // (g_roles_mtx)
        uint64_t              g_final_changes{ 0 };    // (g_roles_mtx)
        uint64_t              g_steps_changed{ 0 };    // (render thread, Watch)
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool InGame(uintptr_t at)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            return at >= base && at < base + MemoryScan::mod_size;
        }

        bool ReadPointer(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadU32(uintptr_t at, uint32_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadU8(uintptr_t at, uint8_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint8_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A step's name (RenderPass: a C string at +8), at most 63 characters.
        bool ReadName(const void* pass, char (&out)[64])
        {
            out[0] = 0;
            __try {
                const char* name = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(pass) + 8);
                if (name == nullptr) {
                    return false;
                }
                size_t i = 0;
                for (; i < 63 && name[i] != 0; ++i) {
                    out[i] = (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
                }
                out[i] = 0;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out[0] = 0;
                return false;
            }
        }

        // The world's view's handle now, and whether the game draws the world this frame (false: the game's views not there yet).
        bool WorldView(uint32_t& handle, bool& drawn)
        {
            uintptr_t views = 0;
            uint8_t   which = 0, is_drawn = 0;
            if (g_views_global == 0 || !ReadPointer(g_views_global, views) || views == 0 || !ReadU8(views + kWorldViewWhich, which) ||
                !ReadU32(views + (which != 0 ? kWorldViewOther : kWorldView), handle) || !ReadU8(views + kWorldDrawn, is_drawn)) {
                return false;
            }
            drawn = is_drawn != 0;
            return (handle & 0xffffff) != 0xffffff;
        }

        // A graph's steps (a small array of 0x150-byte containers at +0x228, each step at +0x20; each a step or a graph of its own).
        void Children(const void* graph, std::vector<void*>& out)
        {
            out.clear();
            const uintptr_t g     = reinterpret_cast<uintptr_t>(graph);
            uint32_t        count = 0, bits = 0;
            if (!ReadU32(g + 0x228, count) || !ReadU32(g + 0x230, bits) || count > 512) {
                return;
            }
            uintptr_t data = g + 0x238;
            if (static_cast<int32_t>(bits) >= 0 && !ReadPointer(g + 0x238, data)) {
                return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uintptr_t pass = 0, table = 0;
                if (ReadPointer(data + static_cast<uintptr_t>(i) * 0x150 + 0x20, pass) && pass != 0 && ReadPointer(pass, table) && InGame(table)) {
                    out.push_back(reinterpret_cast<void*>(pass));
                }
            }
        }

        // The frame's views' parts (the "Frame" graph's, at most two graphs down: Frame, the main graph, its views), in order.
        void FindViews(const void* graph, int depth, std::vector<std::pair<void*, uint32_t>>& out)
        {
            std::vector<void*> children{};
            Children(graph, children);
            for (void* child : children) {
                if (RttiNames::HasBase(child, kSubGraphCoded)) {
                    uint32_t handle = 0xffffff;
                    ReadU32(reinterpret_cast<uintptr_t>(child) + kSubGraphView, handle);
                    if (out.size() < 16) {
                        out.emplace_back(child, handle);
                    }
                } else if (depth < 2 && RttiNames::HasBase(child, kGraphCoded)) {
                    FindViews(child, depth + 1, out);
                }
            }
        }

        // A graph's steps, its graphs' in their place (not the ones that only declare a picture: "Produce...").
        void Leaves(const void* graph, int depth, std::vector<void*>& out)
        {
            std::vector<void*> children{};
            Children(graph, children);
            for (void* child : children) {
                if (RttiNames::HasBase(child, kGraphCoded)) {
                    if (depth < 4) {
                        Leaves(child, depth + 1, out);
                    }
                } else if (RttiNames::HasBase(child, kStepCoded)) {
                    char name[64]{};
                    ReadName(child, name);
                    if (std::strncmp(name, "Produce", 7) != 0 && out.size() < 64) {
                        out.push_back(child);
                    }
                }
            }
        }

        // A view's last steps: those of its "PostUpscaleSubGraph" (its picture made full size and sharpened: the view's finished
        // picture is drawn there), or else its last few steps.
        std::vector<void*> LastSteps(const void* view)
        {
            std::vector<void*> children{};
            Children(view, children);
            std::vector<void*> steps{};
            for (void* child : children) {
                char name[64]{};
                if (ReadName(child, name) && std::strcmp(name, "PostUpscaleSubGraph") == 0 && RttiNames::HasBase(child, kGraphCoded)) {
                    Leaves(child, 0, steps);
                    return steps;
                }
            }
            Leaves(view, 0, steps);
            if (steps.size() > 6) {
                steps.erase(steps.begin(), steps.end() - 6);
            }
            return steps;
        }

        uintptr_t StepHook(void* pass, GraphData* data, void* pass_data);

        Hooked* EntryOf(uintptr_t table)
        {
            for (size_t i = HookSlot(table), probes = 0; probes < kHookSlots; i = (i + 1) & (kHookSlots - 1), ++probes) {
                const uintptr_t t = g_hooked[i].table.load(std::memory_order_acquire);
                if (t == table) {
                    return &g_hooked[i];
                }
                if (t == 0) {
                    return nullptr;
                }
            }
            return nullptr;
        }

        StepFn OriginalOf(uintptr_t table)
        {
            const Hooked* entry = EntryOf(table);
            return entry != nullptr ? reinterpret_cast<StepFn>(entry->original) : nullptr;
        }

        // Its kind's function 7 through StepHook (once; whatever was there is called after, the game's or another hook's). (fix156) By its
        // type table too (EyeShare's kinds, hooked at start: step null).
        bool HookTable(uintptr_t table, void* step);

        bool Hook(void* step)
        {
            uintptr_t table = 0;
            if (!ReadPointer(reinterpret_cast<uintptr_t>(step), table) || !InGame(table)) {
                return false;
            }
            return HookTable(table, step);
        }

        bool HookTable(uintptr_t table, void* step)
        {
            if (!InGame(table)) {
                return false;
            }
            // (Watched: its function 7 is StepHook. An entry whose function 7 could not be changed stays, unwatched.)
            if (const Hooked* entry = EntryOf(table); entry != nullptr) {
                return entry->live.load(std::memory_order_acquire);
            }
            std::scoped_lock _{ g_hook_mtx };
            if (const Hooked* entry = EntryOf(table); entry != nullptr) {
                return entry->live.load(std::memory_order_acquire);
            }
            const uintptr_t slot     = table + 7 * sizeof(uintptr_t);
            uintptr_t       original = 0;
            if (!ReadPointer(slot, original) || original == 0 || original == reinterpret_cast<uintptr_t>(&StepHook)) {
                return false;
            }
            if (g_hooked_count.load(std::memory_order_relaxed) >= static_cast<int>(kHookSlots / 2)) {
                return false;
            }
            size_t at = HookSlot(table);
            while (g_hooked[at].table.load(std::memory_order_relaxed) != 0) {
                at = (at + 1) & (kHookSlots - 1);
            }
            g_hooked[at].original = original;
            g_hooked[at].table.store(table, std::memory_order_release);
            g_hooked_count.fetch_add(1, std::memory_order_release);
            if (step != nullptr && RttiNames::HasBase(step, kTaaCoded)) {
                g_taa_table.store(table, std::memory_order_release);
            }
            auto* p   = reinterpret_cast<uintptr_t*>(slot);
            DWORD old = 0;
            if (!VirtualProtect(p, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                return false;
            }
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p), static_cast<LONG64>(reinterpret_cast<uintptr_t>(&StepHook)));
            VirtualProtect(p, sizeof(uintptr_t), old, &old);
            g_hooked[at].live.store(true, std::memory_order_release);
            return true;
        }

        ID3D12GraphicsCommandList* CommandListOf(GraphData* data)
        {
            __try {
                auto* context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(data->getCommandList());
                return context != nullptr ? context->pID3D12CommandList : nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        ID3D12GraphicsCommandList* ListOfData(void* data)
        {
            return CommandListOf(static_cast<GraphData*>(data));
        }

        // A picture of the kind the views' last steps finish in, which can be copied (as record_live_picture checks: a plain 2D colour
        // picture of one level, 8 or 10 bits, as large as a view's picture, on the main kind of command list). (desc: what it is.)
        bool Copyable(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc)
        {
            desc = D3D12_RESOURCE_DESC{};
            if (list == nullptr || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !RenderPassIo::DescOf(resource, desc)) {
                return false;
            }
            if (desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.Width < 640 || desc.Height < 360) {
                return false;
            }
            switch (desc.Format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return true;
            default:
                return false;
            }
        }

        // A depth picture (one a depth can be drawn into), of a view's size.
        bool DepthPicture(ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc)
        {
            __try {
                desc = resource->GetDesc();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
                   desc.Width >= 640 && desc.Height >= 360 && desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
        }

        // A watched step done: the pictures it drew (a render target or one written by a compute step), copied right after it. The
        // view's finished picture is the one its latest step drew (by their order in the view): found in the first frame (all copied
        // then, each later one over the earlier ones on the GPU), only it from the next frame on.
        void Capture(void* pass, GraphData* data, void* pass_data)
        {
            Role role{};
            if (const RoleMap* roles = g_roles_now.load(std::memory_order_acquire); roles != nullptr) {
                if (const auto it = roles->find(pass); it != roles->end()) {
                    role = it->second;
                }
            }
            if (role.kind < 0 || data == nullptr || pass_data == nullptr) {
                return;
            }
            RenderPassIo::PassIo io{};
            if (!RenderPassIo::ReadPassIo(pass_data, io)) {
                return;
            }
            auto* list = CommandListOf(data);
            auto& vr   = VR::get();
            if (list == nullptr || vr == nullptr) {
                return;
            }
            // (The frame whose drawing the game started last: Reflex's "render submit start", before its first step; as the per-eye
            // TAA history reads it while drawing.)
            const uint32_t frame = vr->m_render_frame_count.load();
            // (fix147, fix148) Full stereo's left eye: pictures near the eye image's size (the game's picture, the right eye's, is that size)
            // are its finished picture, or copied at all.
            const bool stereo_on   = role.kind == 0 && g_stereo_left.load(std::memory_order_acquire);
            const int64_t now_ms   = SteadyMs();
            // (fix148) No picture skipped for its size any more: in space the sharpening step is a new object every frame in full stereo, never
            // known in time to be copied, and HDRComposite's (two rows short) was the only one that came: fix147's test, "copied 0 times",
            // full stereo stopping itself again and again. Every picture of about the eye's size goes into one copy made at the eye's size
            // (record_live_picture): no copy made again for a size, none turned away.
            const bool stereo_left = stereo_on;
            (void)g_left_any_size_told;
            (void)g_stereo_left_since_ms;
            uint32_t   eye_w = 0, eye_h = 0;
            // (fix162: the eye image's size known for the world's pictures behind a menu too: see below.)
            if (role.kind == 0) {
                const auto& size = vr->m_d3d12.get_backbuffer_size();
                eye_w            = size[0];
                eye_h            = size[1];
            }
            // (Its first 8 textures, as before more were read: the same finished picture chosen.)
            for (uint32_t i = 0; i < io.count && i < 8; ++i) {
                if (io.items[i].resource == nullptr) {
                    continue;
                }
                const auto state = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[i].flags));
                D3D12_RESOURCE_DESC desc{};
                if ((state & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) == 0 ||
                    !Copyable(list, io.items[i].resource, desc)) {
                    continue;
                }
                const auto width = static_cast<uint32_t>(desc.Width);
                // (An eye-sized one seen, even with any size taken: the size rule back once there is one.)
                if (stereo_on && width == eye_w && desc.Height == eye_h) {
                    g_left_eye_sized_ms.store(now_ms, std::memory_order_relaxed);
                    g_left_any_size_told.store(false, std::memory_order_relaxed);
                }
                if (stereo_on && (width != eye_w || desc.Height != eye_h)) {
                    // (Counted, for the log: a picture of another size than the eye's.)
                    g_left_other_size.fetch_add(1, std::memory_order_relaxed);
                    g_left_other_w.store(width, std::memory_order_relaxed);
                    g_left_other_h.store(desc.Height, std::memory_order_relaxed);
                    std::scoped_lock _{ g_roles_mtx };
                    if (g_left_other_step[0] == 0) {
                        ReadName(pass, g_left_other_step);
                    }
                }
                // (fix148) Far from the eye's size (over 64 pixels, as the copy refuses): neither the finished picture nor copied, so it
                // cannot become the finished one and leave none (the review).
                if (stereo_on && (std::abs(static_cast<int64_t>(width) - eye_w) > 64 || std::abs(static_cast<int64_t>(desc.Height) - eye_h) > 64)) {
                    continue;
                }
                bool copy = true;
                {
                    std::scoped_lock _{ g_roles_mtx };
                    auto& seen = g_seen[role.kind];
                    if (role.order > seen.order || (role.order == seen.order && i >= seen.item)) {
                        seen = Output{ role.order, i, pass, width, desc.Height };
                    }
                    const auto& final = g_final[role.kind];
                    copy = final.pass == nullptr || (final.pass == pass && final.item == i);
                }
                if (copy) {
                    // (fix162) The world behind a menu (mono) as full stereo's left eye since fix148: a picture near the eye image's size goes into one
                    // copy made at the eye's size. Each was copied at its own size, HDRComposite's (two rows short) first, and the finished one
                    // then refused for half a second (the copy not made again sooner): the world behind the menu, and the menu drawn into it, held as
                    // they were for the first half second of every opening (a menu took a second to open; a helper's research).
                    const bool near_eye = role.kind == 0 && eye_w != 0 && eye_h != 0 && std::abs(static_cast<int64_t>(width) - eye_w) <= 64 &&
                                          std::abs(static_cast<int64_t>(desc.Height) - eye_h) <= 64;
                    if (vr->m_d3d12.record_live_picture(list, io.items[i].resource, state, role.kind, frame, stereo_left || near_eye) > 0) {
                        ++(role.kind == 0 ? g_world_copies : g_picture_copies);
                        if (stereo_left) {
                            g_left_copies.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (stereo_left) {
                        g_left_refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            // A menu's 3D picture's depth: the step that copies it into the previous depth (UpdatePreviousDepthBuffer) has it in the
            // copy-source state, and it stays so after the step: copied from as it is (the panel cuts the 3D picture out by it).
            // (Only while the panel cuts them out by depth.)
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            if (role.kind >= 1 && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && xr != nullptr && xr->panel_previews.load() &&
                xr->panel_depth_cutout.load()) {
                for (uint32_t i = 0; i < io.count; ++i) {
                    const auto& it = io.items[i];
                    D3D12_RESOURCE_DESC desc{};
                    if (it.found != RenderPassIo::kFound || !it.tracked ||
                        RE::CreationRendererPrivate::RenderPassItem::getDXGIState(it.flags) != D3D12_RESOURCE_STATE_COPY_SOURCE ||
                        !DepthPicture(it.resource, desc)) {
                        continue;
                    }
                    if (vr->m_d3d12.record_live_depth(list, it.resource, role.kind, frame) > 0) {
                        ++g_depth_copies;
                    }
                    break;
                }
            }
        }

        uintptr_t StepHook(void* pass, GraphData* data, void* pass_data)
        {
            const uintptr_t table    = *reinterpret_cast<const uintptr_t*>(pass);
            const StepFn    original = OriginalOf(table);
            // (The world's own last-frame pictures: which of the step's textures it attaches itself as it runs, read before it runs.)
            uint32_t empty_before = 0;
            if (g_capture.load(std::memory_order_acquire) && data != nullptr && ViewHistory::Busy()) {
                try {
                    empty_before = ViewHistory::EmptyBefore(pass, pass_data);
                } catch (...) {
                }
            }
            // (fix156) Full stereo's right eye: a step whose result is the left eye's own (the same textures, written the same) not run
            // again (EyeShare); the game's barriers around it still made. Its function 7's result is not used by the game's runner.
            if (EyeShare::Skip(table, data, pass_data)) {
                return 0;
            }
            // (fix140) A frame being timed: the step's own GPU work between two timestamps on its list (GpuTimer).
            const int       timed  = GpuTimer::Timing() ? GpuTimer::Begin(data) : -1;
            const uintptr_t result = original != nullptr ? original(pass, data, pass_data) : 0;
            if (timed >= 0) {
                GpuTimer::End(timed, pass, data);
            }
            EyeShare::After(table, data, pass_data);
            if (g_capture.load(std::memory_order_acquire)) {
                try {
                    Capture(pass, data, pass_data);
                    // (The world's own last-frame pictures: watched, or saved and put back, right after the step.)
                    if (ViewHistory::Busy() && data != nullptr) {
                        ViewHistory::AfterStep(pass, data, pass_data, table == g_taa_table.load(std::memory_order_relaxed), &ListOfData, empty_before);
                    }
                } catch (...) {
                }
            }
            return result;
        }

        // All of the frame's steps in their order (the "Frame" graph walked, graphs in their place), each as the world's view's, one of
        // the menu's views', or the frame's own, with its view's place among the frame's views; their kinds hooked. (For ViewHistory:
        // which pictures are each view's own history.)
        void WalkAll(const void* graph, int depth, uint8_t group, uint8_t view_at, const std::vector<std::pair<void*, uint32_t>>& views,
            uint32_t world, ViewHistory::Places& out, int& order, int& walked, int& failed)
        {
            std::vector<void*> children{};
            Children(graph, children);
            for (void* child : children) {
                if (walked >= 8000) {
                    out.complete = false;
                    return;
                }
                uint8_t child_group = group;
                uint8_t child_view  = view_at;
                bool    is_view     = false;
                for (size_t v = 0; v < views.size(); ++v) {
                    const auto& [view, handle] = views[v];
                    if (view == child) {
                        is_view     = true;
                        child_group = handle == world ? ViewHistory::kWorldStep : ViewHistory::kMenuStep;
                        child_view  = static_cast<uint8_t>(std::min<size_t>(v, 0xfe));
                        // (The views, in their places, in the signature: about half of the views' own steps are new objects each frame
                        // (fix110's log), so the steps themselves are not.)
                        out.signature = (out.signature ^ reinterpret_cast<uintptr_t>(child) ^ (static_cast<uint64_t>(handle) << 32) ^
                                            (static_cast<uint64_t>(child_group) << 60)) * 0x100000001B3ull;
                        break;
                    }
                }
                if (is_view || RttiNames::HasBase(child, kGraphCoded)) {
                    if (depth < 10) {
                        WalkAll(child, depth + 1, child_group, child_view, views, world, out, order, walked, failed);
                    } else {
                        out.complete = false;
                    }
                    continue;
                }
                if (!RttiNames::HasBase(child, kStepCoded)) {
                    continue;
                }
                ++walked;
                // (A step found in two places of the frame: never a place to save or put back.)
                if (const auto it = out.steps.find(child); it != out.steps.end()) {
                    it->second.shared = true;
                    continue;
                }
                ViewHistory::StepPlace place{};
                place.order = order++;
                place.group = child_group;
                place.view  = child_group == ViewHistory::kFrameStep ? 0xff : child_view;
                char name[64]{};
                ReadName(child, name);
                place.name = name;
                // (Its kind not watched: its uses not seen.)
                place.blind = !Hook(child);
                failed += place.blind ? 1 : 0;
                ++out.counts[child_group];
                // (The frame's own steps outside the views in a signature of their own: they stay the same from frame to frame.)
                if (child_group == ViewHistory::kFrameStep) {
                    out.frame_signature = (out.frame_signature ^ reinterpret_cast<uintptr_t>(child)) * 0x100000001B3ull;
                }
                out.steps.emplace(child, std::move(place));
            }
        }

        // One of the frame's own graphs about to run ("CREndFrame" after the frame, "CRBeginFrame" before the next one: which 0 and 1),
        // its steps as they are now (ViewHistory: their uses at the frame's end, in that order).
        void WalkEdge(const void* graph, int which)
        {
            if (!ViewHistory::Learning()) {
                return;
            }
            ViewHistory::Places found{};
            int                 order = (which + 1) * 1000000, walked = 0, failed = 0;
            WalkAll(graph, 0, ViewHistory::kFrameStep, 0xff, {}, 0xffffffff, found, order, walked, failed);
            ViewHistory::EdgeSteps(which, std::move(found.steps), found.complete);
        }

        // This frame's views: whose picture each is, and their last steps watched (found each frame: cheap, a few dozen steps).
        void Watch(const void* frame_graph)
        {
            std::vector<std::pair<void*, uint32_t>> views{};
            FindViews(frame_graph, 0, views);
            uint32_t world = 0xffffffff;
            bool     drawn = false;
            if (!WorldView(world, drawn)) {
                world = 0xffffffff;
            }
            const bool changed = views != g_views || world != g_world_seen;
            g_views            = views;
            g_world_seen       = world;
            // The world's view (by its handle), and the menu's: their 3D pictures, in order; with two or more, the last one is left
            // out (the Data menu's views: you, the planet, then its blue room, which has the other two in it).
            int              world_at = -1;
            std::vector<int> menu{};
            for (int i = 0; i < static_cast<int>(views.size()); ++i) {
                if (world_at < 0 && views[i].second == world) {
                    world_at = i;
                } else if (!StereoView::IsEyeView(views[i].second)) { // (full stereo's right eye: not a menu's picture)
                    menu.push_back(i);
                }
            }
            const size_t pictures = std::min<size_t>(menu.size() >= 2 ? menu.size() - 1 : menu.size(), 3);
            std::unordered_map<void*, Role> roles{};
            std::string                     text{};
            int                             failed = 0;
            const bool                      log    = changed && g_views_logged < 6;
            const auto watch = [&](int at, int kind) {
                const auto steps = LastSteps(views[at].first);
                for (size_t n = 0; n < steps.size(); ++n) {
                    roles[steps[n]] = Role{ kind, static_cast<int>(n) };
                    failed += Hook(steps[n]) ? 0 : 1;
                }
                if (log) {
                    std::string names{};
                    for (void* step : steps) {
                        char name[64]{};
                        ReadName(step, name);
                        names += (names.empty() ? "" : ", ") + std::string{ name };
                    }
                    text += fmt::format("; [{}] {} ({} last steps: {})", views[at].second & 0xffffff,
                        kind == 0 ? std::string{ "the world" } : fmt::format("the menu's 3D picture {}", kind), steps.size(), names);
                }
            };
            if (world_at >= 0) {
                watch(world_at, 0);
            }
            for (size_t k = 0; k < pictures; ++k) {
                watch(menu[k], static_cast<int>(k) + 1);
            }
            {
                const bool same_steps = g_roles != nullptr && roles == *g_roles;
                // (fix147, for the log: how the watched steps changed, when they did.)
                const bool had_roles  = g_roles != nullptr;
                size_t     new_steps  = 0;
                if (!same_steps && had_roles) {
                    for (const auto& entry : roles) {
                        new_steps += g_roles->contains(entry.first) ? 0 : 1;
                    }
                    ++g_steps_changed;
                }
                const size_t step_count = roles.size();
                PublishRoles(std::make_shared<const RoleMap>(std::move(roles)));
                std::scoped_lock _{ g_roles_mtx };
                // Each kind's finished picture: the last one its steps drew in the last frame (found again when its steps change).
                for (size_t k = 0; k < g_seen.size(); ++k) {
                    const Output before = g_final[k];
                    g_final[k]          = same_steps ? g_seen[k] : Output{};
                    g_seen[k]           = Output{};
                    // (fix147) Each change of it logged with why, the first 20 times (then counted, in the 10 s line): while it is
                    // not known (cleared), every picture of its kind is copied, each over the one before.
                    const Output& after = g_final[k];
                    if (before.pass == after.pass && before.item == after.item) {
                        continue;
                    }
                    ++g_final_changes;
                    if (g_final_logs >= 20) {
                        continue;
                    }
                    ++g_final_logs;
                    const std::string what = k == 0 ? std::string{ "the world's" } : fmt::format("the menu's 3D picture {}'s", k);
                    if (after.pass != nullptr) {
                        // (The step is this frame's too: its steps are the same objects as the frame before's.)
                        char name[64]{};
                        ReadName(after.pass, name);
                        spdlog::info("[LiveWorld] {} finished picture: now step {} (its view's last step {}), its texture {}, {}x{}: the latest its "
                                     "steps drew in the last frame{}{}",
                            what, name, after.order, after.item, after.width, after.height, before.pass != nullptr ? " (another before)" : "",
                            k == 0 && g_stereo_left.load() ? "; full stereo: pictures within 64 pixels of the eye image's size (fix148)" : "");
                    } else if (!same_steps) {
                        spdlog::info("[LiveWorld] {} finished picture: to be found again (each of its pictures copied this frame): {}", what,
                            !had_roles ? std::string{ "its steps watched anew (copying just switched on)" }
                                       : fmt::format("the watched steps changed ({} of {} are new objects{})", new_steps, step_count,
                                             changed ? "; the frame's views changed" : ""));
                    } else {
                        spdlog::info("[LiveWorld] {} finished picture: to be found again (each of its pictures copied this frame): its steps drew "
                                     "none that can be taken in the last frame{}",
                            what, k == 0 && g_stereo_left.load() ? " (full stereo: none near the eye image's size)" : "");
                    }
                }
            }
            // (fix147) Full stereo, every 10 s: the left eye's copies, the pictures of another size not taken, and how often the
            // finished step was chosen anew.
            if (g_stereo_left.load()) {
                const int64_t now_ms = NowMs();
                if (g_left_stats_ms == 0) {
                    // (A new count: what came before, behind a menu or in an earlier stretch of full stereo, not in it.)
                    g_left_stats_ms = now_ms;
                    g_steps_changed = 0;
                    g_left_copies.store(0);
                    g_left_refused.store(0);
                    g_left_other_size.store(0);
                    std::scoped_lock _{ g_roles_mtx };
                    g_final_changes      = 0;
                    g_left_other_step[0] = 0;
                } else if (now_ms - g_left_stats_ms >= 10000) {
                    g_left_stats_ms = now_ms;
                    const uint64_t other = g_left_other_size.exchange(0);
                    std::string    step{};
                    uint64_t       changes = 0;
                    {
                        std::scoped_lock _{ g_roles_mtx };
                        step                 = g_left_other_step;
                        g_left_other_step[0] = 0;
                        changes              = g_final_changes;
                        g_final_changes      = 0;
                    }
                    spdlog::info("[LiveWorld] full stereo, the last 10 s: the left eye's picture copied {} times, refused by the copy {} times; "
                                 "pictures of another size than the eye image's {} times{} (fix148: within 64 pixels copied into the eye-sized copy); the finished pictures' steps chosen "
                                 "anew {} times, the watched steps changed {} times",
                        g_left_copies.exchange(0), g_left_refused.exchange(0), other,
                        other > 0 ? fmt::format(" (the last {}x{}, step {})", g_left_other_w.load(), g_left_other_h.load(),
                                        step.empty() ? std::string{ "?" } : step)
                                  : std::string{},
                        changes, g_steps_changed);
                    g_steps_changed = 0;
                }
            } else {
                g_left_stats_ms = 0;
            }
            // (The frame's views, for DLSS's calls: which view each is for.)
            DlssViews::SetViews(views, world);
            // The world's own last-frame pictures kept apart from the menu's views' (ViewHistory): with the world drawn first and at
            // least one view of the menu's. The frame's steps walked when they may have changed (the views did, or while learning).
            {
                auto&      vr      = VR::get();
                const auto xr      = vr != nullptr ? vr->m_openxr : nullptr;
                // (From the world's tenth frame drawn: its first frames, and the menu's views', are not yet as they will be.)
                const bool wanted  = g_draw.load() && g_frames_on >= 10 && xr != nullptr && xr->panel_world_history.load() && world_at == 0 &&
                                    views.size() >= 2;
                const uint32_t frame = vr != nullptr ? vr->m_render_frame_count.load() : 0;
                const bool     mono  = vr == nullptr || vr->is_mono_frame(frame);
                if (wanted != g_history_wanted) {
                    g_history_wanted = wanted;
                    g_places.reset();
                }
                // (Not kept apart while the world is drawn behind the menu: why, once for each reason in this menu, with its views and the
                // menus shown, for the log: as for the Missions menu in fix126's test.)
                if (g_draw.load() && g_frames_on >= 10 && !wanted) {
                    const int why = xr == nullptr || !xr->panel_world_history.load() ? 1 : (world_at < 0 ? 2 : (world_at != 0 ? 3 : 4));
                    if ((g_not_kept_logged & (1u << why)) == 0) {
                        g_not_kept_logged |= 1u << why;
                        std::string all{};
                        for (const auto& [graph, handle] : views) {
                            all += fmt::format("{}[{}]", all.empty() ? "" : " ", handle & 0xffffff);
                        }
                        spdlog::info("[LiveWorld] the views' histories not kept apart behind this menu ({}): {}; the frame's views: {} (the world's [{}])",
                            GameFlow::shownMenus(),
                            why == 1   ? "the setting is off"
                            : why == 2 ? "the world's view is not among the frame's views"
                            : why == 3 ? fmt::format("the world's view is not the first of them (it is at place {})", world_at)
                                       : "the menu has no 3D view of its own (nothing to keep apart from)",
                            all.empty() ? std::string{ "none" } : all, world & 0xffffff);
                    }
                }
                if (wanted) {
                    const bool every_view  = xr->panel_view_histories.load();
                    const bool reuse_plans = xr->panel_history_plans.load();
                    if (g_places == nullptr || views != g_places_views || world != g_places_world || every_view != g_places->every_view ||
                        reuse_plans != g_places->reuse_plans || ViewHistory::TakeWalkRequest() || ViewHistory::Learning() || ++g_places_age > 300) {
                        auto places         = std::make_shared<ViewHistory::Places>();
                        places->every_view  = every_view;
                        places->reuse_plans = reuse_plans;
                        // (The menu's last view when it has two or more: its picture is not shown (the Data menu's blue room, the others
                        // in it), so its histories are not kept apart: a quarter of the copies in the Data menu.)
                        if (menu.size() >= 2 && menu.back() < 32) {
                            places->planned &= ~(1u << menu.back());
                        }
                        int  order = 0, walked = 0, walk_failed = 0;
                        // (The frame's own graphs after it and before the next one, "CREndFrame" and "CRBeginFrame", are not walked
                        // here: their steps may be new objects each frame. Each is walked as it runs: WalkEdge.)
                        WalkAll(frame_graph, 0, ViewHistory::kFrameStep, 0xff, views, world, *places, order, walked, walk_failed);
                        // (Plans for every view, or for the world's alone: never taken for each other.)
                        if (!every_view) {
                            places->signature = (places->signature ^ 0x9e3779b97f4a7c15ull) * 0x100000001B3ull;
                        }
                        // (Steps not there at the last walk: new objects, as many of the views' steps are each frame. Never a place to
                        // save or put back: the plan's places must be the same steps every frame. Only against a walk of the frame just
                        // before, with the same views: against an older one, steps made again since once would look new (fix120's log:
                        // learning that started over found half the histories at such steps).)
                        if (g_places != nullptr && views == g_places_views && world == g_places_world && frame == g_places_frame + 1) {
                            for (auto& [pass, place] : places->steps) {
                                if (g_places->steps.find(pass) == g_places->steps.end()) {
                                    place.shared = true;
                                    place.fresh  = true;
                                }
                            }
                        }
                        if (g_places == nullptr) {
                            spdlog::info("[LiveWorld] the frame's steps found: {} (the world's {}, the menu's views' {}, the frame's own {}){}{}", walked,
                                places->counts[0], places->counts[1], places->counts[2],
                                walk_failed > 0 ? fmt::format("; {} could not be watched", walk_failed) : std::string{},
                                places->complete ? "" : "; not all of them (too many, or too deep)");
                        }
                        g_places       = std::move(places);
                        g_places_views = views;
                        g_places_world = world;
                        g_places_age   = 0;
                        g_places_frame = frame;
                    }
                    ViewHistory::BeginFrame(frame, mono, g_places);
                } else {
                    ViewHistory::BeginFrame(frame, mono, nullptr);
                }
            }
            if (log) {
                ++g_views_logged;
                std::string all{};
                for (const auto& [graph, handle] : views) {
                    all += fmt::format("{}[{}]", all.empty() ? "" : " ", handle & 0xffffff);
                }
                spdlog::info("[LiveWorld] the frame's views: {} (the world's view: [{}]{}, drawn {}){}; {} kinds of step watched{}", all.empty() ? "none" : all,
                    world & 0xffffff, world_at >= 0 ? "" : ", not among them", drawn ? "yes" : "no", text, g_hooked_count.load(),
                    failed > 0 ? fmt::format(" ({} steps could not be watched)", failed) : std::string{});
            }
        }

        bool Wanted()
        {
            if (!g_installed.load() || !MenuLayer::SeeThroughWanted() || !GameFlow::worldBehindMenu()) {
                return false;
            }
            // (Not near a loading screen: the world is not where you will be, or not there at all.)
            if (NowMs() - GameFlow::lastLoadingScreen() < 3000) {
                return false;
            }
            auto& vr = VR::get();
            if (vr == nullptr || !vr->m_present_index_ok.load()) {
                return false; // (the headset's side shows each frame's world by its index: not without it)
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            return xr != nullptr && xr->panel_live_world.load() && !xr->panel_refused;
        }

        // (fix181) Why the world is not wanted this frame (Wanted's checks in order), for the line when a moment's drop is held.
        const char* WhyNotWanted()
        {
            if (!MenuLayer::SeeThroughWanted()) {
                return !ModSettings::g_internalSettings.showQuadDisplay ? "no menu shown this frame"
                       : !ModSettings::g_internalSettings.menuWorldBehind ? "no world behind the menu this frame"
                       : !ModSettings::g_internalSettings.menuSeeThrough  ? "no see-through menu this frame (its movie not set up lately)"
                                                                           : "see-through menus not wanted";
            }
            if (!GameFlow::worldBehindMenu()) {
                return "no world (the main menu, a loading screen)";
            }
            if (NowMs() - GameFlow::lastLoadingScreen() < 3000) {
                return "near a loading screen";
            }
            auto& vr = VR::get();
            if (vr == nullptr || !vr->m_present_index_ok.load()) {
                return "the presents' frame index not known";
            }
            return "the live world setting off, or the panel refused";
        }

        // (fix181) A moment's drop of Wanted() may be held (LiveWorld::OnGraph, a few frames): the menu still shown, the world there, not
        // near a loading screen, see-through menus possible at all (the settings). Not the menu flags of this frame: a long frame of the
        // game's (the Data menu's 300-500 ms ones as its 3D pictures are made) dropped them for a frame, and the world was left to the
        // game, the panel showing its whole picture with nothing behind it (fix180's test).
        // (fix181, the review) And every menu shown still a see-through one (GameFlow's menus by their movies, kept through a long frame by
        // its frame rule): from a see-through menu to an opaque one (the Data menu to the pause menu or the star map) nothing is held, the
        // world not drawn with the headset's camera into the opaque menu's first frames.
        bool HoldAllowed()
        {
            return g_installed.load() && ModSettings::g_internalSettings.showQuadDisplay && MenuLayer::SeeThroughPossible() &&
                   GameFlow::worldBehindMenu() && NowMs() - GameFlow::lastLoadingScreen() >= 3000 && GameFlow::menusAllowSeeThrough();
        }

        // The game's Main (r13 at the check), checked once by its type.
        bool IsMain(uintptr_t object)
        {
            if (object == 0) {
                return false;
            }
            if (g_main.load(std::memory_order_relaxed) == object) {
                return true;
            }
            if (!RttiNames::HasBase(reinterpret_cast<const void*>(object), kMainCoded)) {
                return false;
            }
            g_main.store(object, std::memory_order_relaxed);
            return true;
        }

        // At the game's check, a shown menu with the full-screen flag found (the game's main thread, once a frame): the world kept
        // drawn as behind a loading screen whose place is ready, unless a loading screen is up (then the game's own exception decides).
        void OnCheck(safetyhook::Context& ctx)
        {
            if (!IsMain(ctx.r13)) {
                return;
            }
            // (The frame kept full size behind the menu, or the box again: the rectangle sent again when that changed.)
            FullFrame::OnMenuFrame();
            if (!g_draw.load(std::memory_order_acquire)) {
                return;
            }
            uint8_t loading = 1;
            if (!ReadU8(ctx.r13 + kMainLoading, loading) || loading != 0) {
                return;
            }
            // (Nor just after the loading screen's movie was first set up, before the game marks it up: checked here, on the game's
            // own thread, as the frame is decided.)
            if (NowMs() - GameFlow::lastLoadingScreen() < 3000) {
                return;
            }
            ctx.r15 |= 1;
            g_forced.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Install()
    {
        const auto at = utility::scan(MemoryScan::mod, kCheckPattern);
        if (!at) {
            spdlog::warn("[LiveWorld] the game's check (no world behind a full-screen menu) not found: the world as it was is shown behind menus");
            SetStatus("World behind menus: as it was when the menu opened (the game's check not found: another game version?)");
            return;
        }
        int32_t distance = 0;
        std::memcpy(&distance, reinterpret_cast<const void*>(*at + kViewsLoad + 3), sizeof(distance));
        const uintptr_t views = *at + kViewsLoad + 7 + static_cast<intptr_t>(distance);
        if (!InGame(views)) {
            spdlog::warn("[LiveWorld] the game's views are not where expected: left as it is");
            SetStatus("World behind menus: as it was when the menu opened (the game's check is not as expected)");
            return;
        }
        g_check_hook = safetyhook::create_mid(reinterpret_cast<void*>(*at + kWatchOffset), &OnCheck);
        if (!g_check_hook) {
            spdlog::warn("[LiveWorld] could not watch the game's check: the world as it was is shown behind menus");
            SetStatus("World behind menus: as it was when the menu opened (could not watch the game's check)");
            return;
        }
        g_views_global = views;
        g_installed    = true;
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        spdlog::info("[LiveWorld] the game's check found and watched (rva {:x}; the game's views at rva {:x}): the world can be kept drawn behind "
                     "see-through menus",
            *at + kWatchOffset - base, views - base);
        SetStatus("World behind menus: ready (kept drawn while a see-through menu is open)");
    }

    void OnGraph(void* graph, const char* name, bool before)
    {
        if (!before || name == nullptr || graph == nullptr) {
            return;
        }
        try {
            if (std::strcmp(name, "CREndFrame") == 0) {
                WalkEdge(graph, 0);
                return;
            }
            if (std::strcmp(name, "CRBeginFrame") == 0) {
                WalkEdge(graph, 1);
                // Wanted: the camera is the headset's from the next frame the game makes; the world kept drawn from the third such
                // frame (its first frames drawn already had the game's own camera, and they would be shown).
                // (A moment's drop, as when the menu's movie skips a frame, is ignored for a few frames; not near a loading screen.)
                // (fix181) Held on what does not change from frame to frame (HoldAllowed), not the menu flags of this frame (which the
                // hold needed until now: a frame without them was never held), and each hold logged.
                const bool now_wanted = Wanted();
                g_unwanted_frames     = now_wanted ? 0 : std::min(g_unwanted_frames + 1, 1000);
                const bool held       = !now_wanted && g_wanted_frames > 0 && g_unwanted_frames < 5 && HoldAllowed();
                const bool wanted     = now_wanted || held;
                if (held && g_unwanted_frames == 1) {
                    static int told = 0;
                    if (told++ < 40) {
                        spdlog::info("[LiveWorld] (fix181) the world kept drawn behind the menu through a moment's drop ({}): held up to 4 frames",
                            WhyNotWanted());
                    }
                } else if (!wanted && g_wanted_frames > 0 && g_unwanted_frames > 1) {
                    static int told = 0;
                    if (told++ < 40) {
                        spdlog::info("[LiveWorld] (fix181) a drop held {} frame(s), then the world left to the game ({})", g_unwanted_frames - 1,
                            WhyNotWanted());
                    }
                }
                g_wanted_frames       = wanted ? std::min(g_wanted_frames + 1, 1000) : 0;
                const bool draw   = wanted && g_wanted_frames >= 3;
                if (draw != g_draw.load()) {
                    g_draw.store(draw, std::memory_order_release);
                    g_frames_on    = 0;
                    g_drawn_logged = false;
                    // (Each menu: its views logged as they change, the first few times, and why its histories are not kept, if not.)
                    g_views_logged    = 0;
                    g_not_kept_logged = 0;
                    if (g_switches++ < 12) {
                        spdlog::info("[LiveWorld] {}", draw ? "the world is kept drawn behind the see-through menu"
                                                            : "the world is left to the game (no see-through menu, a loading screen, or the setting off)");
                    }
                }
                if (!wanted) {
                    if (g_history_wanted) {
                        g_history_wanted = false;
                        g_places.reset();
                    }
                    // (Its copies let go a few seconds after the menu closed.)
                    ViewHistory::Idle();
                }
                // (Full stereo: the world's view, the left eye, copied out as it is drawn too: the right eye's view writes over the
                // same picture after it.)
                const bool capture = wanted || StereoView::Active();
                // (fix147) Full stereo's left eye alone (no see-through menu wants the world): only pictures of the eye image's size
                // taken for it. Behind a menu the world's picture may be smaller (the menu's box): as before. (From one to the other,
                // the world's finished step is found again: the pictures it may be chosen from are not the same.)
                const bool stereo_left    = capture && !wanted;
                const bool stereo_changed = g_stereo_left.exchange(stereo_left, std::memory_order_acq_rel) != stereo_left;
                if (stereo_changed && stereo_left) {
                    g_stereo_left_since_ms = SteadyMs();
                    g_left_any_size_told   = false;
                }
                if (stereo_changed && capture == g_capture.load()) {
                    std::scoped_lock _{ g_roles_mtx };
                    g_seen[0] = Output{};
                    if (g_final[0].pass != nullptr) {
                        ++g_final_changes;
                        if (g_final_logs < 20) {
                            ++g_final_logs;
                            spdlog::info("[LiveWorld] the world's finished picture: to be found again ({})",
                                stereo_left ? "full stereo's left eye now: pictures near the eye image's size, copied at its size (fix148)" : "the world behind a menu now");
                        }
                    }
                    g_final[0] = Output{};
                }
                if (capture != g_capture.load()) {
                    g_capture.store(capture, std::memory_order_release);
                    if (g_switches++ < 12 && !wanted) {
                        spdlog::info("[LiveWorld] {}", capture ? "full stereo: the world's view (the left eye) copied as it is drawn"
                                                               : "full stereo: the world's view no longer copied");
                    }
                    // (The views are found again next time: their steps then may be others.)
                    PublishRoles(nullptr);
                    std::scoped_lock _{ g_roles_mtx };
                    g_seen.fill(Output{});
                    // (fix147) Logged as each change of the finished step is (the first 20 times).
                    if (std::any_of(g_final.begin(), g_final.end(), [](const Output& o) { return o.pass != nullptr; })) {
                        ++g_final_changes;
                        if (g_final_logs < 20) {
                            ++g_final_logs;
                            spdlog::info("[LiveWorld] the finished pictures: to be found again (copying switched {})", capture ? "on" : "off");
                        }
                    }
                    g_final.fill(Output{});
                }
                // (DLSS kept on behind the menu, each view its own, once its views are known: DlssViews.)
                DlssViews::OnFrame(draw);
                ModSettings::g_internalSettings.menuLiveWorld = wanted;
                return;
            }
            if (std::strcmp(name, "Frame") == 0 && g_capture.load()) {
                Watch(graph);
                // (Once a moment after it was switched on: does the game draw the world?)
                if (g_draw.load() && ++g_frames_on == 30 && !g_drawn_logged) {
                    g_drawn_logged = true;
                    uint32_t   world = 0;
                    bool       drawn = false;
                    const bool known = WorldView(world, drawn);
                    spdlog::info("[LiveWorld] half a second on: the game {} the world (its view [{}]; frames it was kept drawn {}); pictures copied so far: "
                                 "the world's {}, the menu's 3D pictures {}, their depth {}",
                        !known ? "has no world view for" : (drawn ? "draws" : "does NOT draw"), world & 0xffffff, g_forced.load(),
                        g_world_copies.load(), g_picture_copies.load(), g_depth_copies.load());
                    SetStatus(drawn ? "World behind menus: kept drawn while a see-through menu is open (working)"
                                    : "World behind menus: wanted, but the game did not draw the world (see the log)");
                }
            }
        } catch (...) {
        }
    }

    bool Active()
    {
        return g_draw.load();
    }

    bool Coming()
    {
        return g_draw.load() || Wanted();
    }

    bool WorldViewHandle(uint32_t& handle)
    {
        bool drawn = false;
        return WorldView(handle, drawn);
    }

    // (fix140, GpuTimer) Every kind of step in a graph through StepHook: the graph walked, its graphs and views in it too.
    int HookAllSteps(const void* graph)
    {
        if (graph == nullptr) {
            return 0;
        }
        int       visited = 0;
        const int before  = g_hooked_count.load(std::memory_order_acquire);
        const std::function<void(const void*, int)> walk = [&](const void* g, int depth) {
            std::vector<void*> children{};
            Children(g, children);
            for (void* child : children) {
                if (++visited > 8000) {
                    return;
                }
                if (RttiNames::HasBase(child, kGraphCoded)) { // (a view's RenderViewSubGraph is one too)
                    if (depth < 12) {
                        walk(child, depth + 1);
                    }
                } else if (RttiNames::HasBase(child, kStepCoded)) {
                    Hook(child);
                }
            }
        };
        try {
            walk(graph, 0);
        } catch (...) {
        }
        return g_hooked_count.load(std::memory_order_acquire) - before;
    }

    uintptr_t HookAddress()
    {
        return reinterpret_cast<uintptr_t>(&StepHook);
    }

    bool HookKind(uintptr_t table) { return HookTable(table, nullptr); }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return fmt::format("{} [world pictures {}, 3D pictures {}]; {}", g_status, g_world_copies.load(), g_picture_copies.load(), ViewHistory::Status());
    }
}
