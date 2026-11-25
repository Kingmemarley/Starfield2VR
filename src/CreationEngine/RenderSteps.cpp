#include "RenderSteps.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include "CreationEngineRendererModule.h"
#include "LiveWorld.h"
#include "MenuLayer.h"
#include "RenderPassIo.h"
#include "RttiNames.h"
#include "memory/ScanHelper.h"
#include <RE/C/CreationRendererPrivate.h>
#include <mods/VR.hpp>

namespace RenderSteps
{
    namespace
    {
        using GraphData = RE::CreationRendererPrivate::RenderGraphData;
        using StepFn    = uintptr_t (*)(void* pass, GraphData* data, void* pass_data);

        enum Phase : int
        {
            kWaiting = 0, // for a see-through menu open a moment
            kLogging = 1, // this frame's steps logged
            kArmed   = 2, // the next frame's pictures copied
            kCopying = 3,
            kDone    = 4,
        };
        std::atomic<int>                      g_phase{ kWaiting };
        // (fix173) Only with its setting on (RenderStepsTest, off): it ran once a session 1.5 s into the first see-through menu, logging about
        // 850 lines and saving menu_steps.jpg on the present thread (a stall of about 0.3 s: the Data menu's layer copy then too old, one
        // frame of the game's raw picture on the panel: the "flash" as the pause menu opened, 23:00:24.411-24.749 in the test's log).
        std::atomic<bool>                     g_on{ false };
        bool                                  g_was_wanted{ false }; // (render thread)
        std::chrono::steady_clock::time_point g_wanted_since{};

        // The kinds of step hooked: their type table and its function 7 as it was (filled before each table is changed).
        struct Hooked
        {
            uintptr_t table{ 0 };
            uintptr_t original{ 0 };
        };
        std::array<Hooked, 512> g_hooked{};
        std::atomic<int>        g_hooked_count{ 0 };

        // From the walk (the render thread, before any hook is in place; not changed after): each step's place and name.
        struct StepInfo
        {
            int         order{ -1 };
            std::string name;
            std::string kind;
        };
        std::unordered_map<void*, StepInfo> g_steps{};

        // The logged frame's large colour pictures drawn, and the ones chosen to copy (set between the two frames).
        struct Candidate
        {
            int         rank{ 0 }; // its place in the frame (the walk's, else after it in the order run)
            void*       pass{ nullptr };
            uint32_t    item{ 0 };
            std::string label;
        };
        std::mutex                                                        g_mtx{};
        std::vector<Candidate>                                            g_candidates{};
        std::map<void*, std::vector<std::pair<uint32_t, std::string>>>    g_targets{};
        std::set<std::pair<void*, uint32_t>>                              g_copied{}; // (g_mtx)
        std::atomic<int>                                                  g_calls{ 0 };
        std::atomic<int>                                                  g_copies{ 0 };
        std::mutex                                                        g_status_mtx{};
        std::string                                                       g_status{ "Render steps test: off (setting RenderStepsTest)" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
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

        bool DerivesFrom(const void* object, const char* type_name)
        {
            __try {
                return utility::rtti::derives_from(object, type_name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Of that kind (or one made from it), by its coded type name (read from the game's type information: RttiNames), or by its
        // plain one through kananlib, which compares "class CreationRendererPrivate::RenderPass" (fix106 gave kananlib the coded
        // name, and nothing matched).
        bool IsKind(const void* object, const char* coded_name, const char* plain_name)
        {
            if (RttiNames::HasBase(object, coded_name)) {
                return true;
            }
            const std::string as_class  = std::string{ "class " } + plain_name;
            const std::string as_struct = std::string{ "struct " } + plain_name;
            return DerivesFrom(object, as_class.c_str()) || DerivesFrom(object, as_struct.c_str());
        }

        // Its kind (its type's decorated name, shortened: the name in its type descriptor, +0x10, ".?AV...").
        bool RawTypeName(const void* object, const char*& out)
        {
            __try {
                const void* descriptor = utility::rtti::get_type_info(object);
                out                    = descriptor != nullptr ? reinterpret_cast<const char*>(descriptor) + 0x10 : nullptr;
                return out != nullptr && out[0] == '.';
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        std::string KindOf(const void* object)
        {
            const char* raw = nullptr;
            if (!RawTypeName(object, raw)) {
                return "?";
            }
            std::string s{ raw, strnlen(raw, 160) };
            if (s.rfind(".?AV", 0) == 0) {
                s = s.substr(4);
            }
            if (const auto at = s.find("@CreationRendererPrivate@@"); at != std::string::npos) {
                s.resize(at);
            } else if (s.size() > 2 && s.compare(s.size() - 2, 2, "@@") == 0) {
                s.resize(s.size() - 2);
            }
            return s;
        }

        uintptr_t StepHook(void* pass, GraphData* data, void* pass_data);

        StepFn OriginalOf(uintptr_t table)
        {
            const int n = g_hooked_count.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i) {
                if (g_hooked[i].table == table) {
                    return reinterpret_cast<StepFn>(g_hooked[i].original);
                }
            }
            return nullptr;
        }

        bool SetSlot(uintptr_t slot_address, uintptr_t value)
        {
            auto* slot = reinterpret_cast<uintptr_t*>(slot_address);
            DWORD old  = 0;
            if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                return false;
            }
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(value));
            VirtualProtect(slot, sizeof(uintptr_t), old, &old);
            return true;
        }

        void Unhook()
        {
            const int n = g_hooked_count.load(std::memory_order_acquire);
            int       restored = 0;
            for (int i = 0; i < n; ++i) {
                uintptr_t now = 0;
                const uintptr_t slot = g_hooked[i].table + 7 * sizeof(uintptr_t);
                if (ReadPointer(slot, now) && now == reinterpret_cast<uintptr_t>(&StepHook) && SetSlot(slot, g_hooked[i].original)) {
                    ++restored;
                }
            }
            spdlog::info("[Steps] the {} kinds of step hooked for the test are back as they were ({} restored)", n, restored);
        }

        // The frame's steps, in order: the graph's steps (a small array of 0x150-byte containers at +0x228, each step at +0x20), each
        // a step or a graph of its own, walked into. Their kinds' tables gathered.
        void Walk(void* graph, int depth, int& order, std::vector<uintptr_t>& tables, std::unordered_set<uintptr_t>& seen_tables, int& lines)
        {
            const uintptr_t g = reinterpret_cast<uintptr_t>(graph);
            uint32_t count = 0, bits = 0;
            if (!ReadU32(g + 0x228, count) || !ReadU32(g + 0x230, bits) || count > 512) {
                return;
            }
            uintptr_t data = g + 0x238;
            if (static_cast<int32_t>(bits) >= 0 && !ReadPointer(g + 0x238, data)) {
                return;
            }
            for (uint32_t i = 0; i < count && order < 1500; ++i) {
                uintptr_t pass_address = 0;
                if (!ReadPointer(data + static_cast<uintptr_t>(i) * 0x150 + 0x20, pass_address) || pass_address == 0) {
                    continue;
                }
                void*     pass  = reinterpret_cast<void*>(pass_address);
                uintptr_t table = 0;
                if (!ReadPointer(pass_address, table) || !InGame(table)) {
                    continue;
                }
                char name[64]{};
                ReadName(pass, name);
                const bool  is_graph = IsKind(pass, ".?AVRenderGraph@CreationRendererPrivate@@", "CreationRendererPrivate::RenderGraph");
                const bool  is_step  = IsKind(pass, ".?AVRenderPass@CreationRendererPrivate@@", "CreationRendererPrivate::RenderPass");
                const auto  kind     = KindOf(pass);
                const int   place    = order++;
                g_steps.emplace(pass, StepInfo{ place, name, kind });
                if (lines < 1400) {
                    ++lines;
                    spdlog::info("[Steps] {:>{}}#{} '{}' ({}){}", "", depth * 2, place, name, kind,
                        is_graph ? " [a graph of steps]" : (is_step ? "" : fmt::format(" [not a step: {}]", RttiNames::Describe(pass))));
                }
                if (is_graph) {
                    if (depth < 8) {
                        Walk(pass, depth + 1, order, tables, seen_tables, lines);
                    }
                } else if (is_step && seen_tables.insert(table).second) {
                    tables.push_back(table);
                }
            }
        }

        bool WalkAndHook(void* frame_graph)
        {
            std::vector<uintptr_t>        tables{};
            std::unordered_set<uintptr_t> seen{};
            int                           order = 0, lines = 0;
            spdlog::info("[Steps] the frame's steps while a see-through menu is open (the \"Frame\" graph, walked in order):");
            Walk(frame_graph, 0, order, tables, seen, lines);
            int hooked = 0, skipped = 0;
            for (const uintptr_t table : tables) {
                const uintptr_t slot     = table + 7 * sizeof(uintptr_t);
                uintptr_t       original = 0;
                // (Not one already hooked: its function 7 in this mod, the interface pass's; but the live world's watch, which calls
                // the game's own, is gone through.)
                if (!ReadPointer(slot, original) || (!InGame(original) && original != LiveWorld::HookAddress())) {
                    ++skipped;
                    continue;
                }
                const int n = g_hooked_count.load(std::memory_order_relaxed);
                if (n >= static_cast<int>(g_hooked.size())) {
                    break;
                }
                g_hooked[n] = Hooked{ table, original };
                g_hooked_count.store(n + 1, std::memory_order_release);
                if (SetSlot(slot, reinterpret_cast<uintptr_t>(&StepHook))) {
                    ++hooked;
                }
            }
            spdlog::info("[Steps] {} steps walked; {} kinds of step hooked for two frames ({} left as they are)", order, hooked, skipped);
            return hooked > 0;
        }

        bool KeptFormat(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return true;
            default:
                return false;
            }
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

        void Observe(int phase, void* pass, GraphData* data, void* pass_data)
        {
            RenderPassIo::PassIo io{};
            const bool have_io = pass_data != nullptr && RenderPassIo::ReadPassIo(pass_data, io);
            if (phase == kLogging) {
                const int call = g_calls++;
                StepInfo info{};
                if (const auto it = g_steps.find(pass); it != g_steps.end()) {
                    info = it->second;
                } else {
                    char name[64]{};
                    ReadName(pass, name);
                    info.name = name;
                    info.kind = KindOf(pass);
                }
                std::string pictures;
                for (uint32_t i = 0; have_io && i < io.count; ++i) {
                    const auto&         item  = io.items[i];
                    const uint32_t      state = RE::CreationRendererPrivate::RenderPassItem::getDXGIState(item.flags);
                    D3D12_RESOURCE_DESC desc{};
                    if (item.resource == nullptr || !RenderPassIo::DescOf(item.resource, desc)) {
                        pictures += fmt::format(" [{}] state {:#x} (not a picture)", i, state);
                        continue;
                    }
                    // (Which picture: the same one in two steps of the frame has the same address.)
                    pictures += fmt::format(" [{}] state {:#x} {}x{} format {}{} @{:x}", i, state, (uint64_t)desc.Width, desc.Height, (int)desc.Format,
                        desc.SampleDesc.Count > 1 ? " (multisampled)" : "", reinterpret_cast<uintptr_t>(item.resource) & 0xffffff);
                    const bool drawn = (state & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != 0;
                    // (One level only: a step drawing a smaller level of a picture leaves its first level in another state.)
                    if (drawn && desc.Width >= 640 && desc.Height >= 360 && desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
                        KeptFormat(desc.Format)) {
                        Candidate c{};
                        c.rank  = info.order >= 0 ? info.order : 100000 + call;
                        c.pass  = pass;
                        c.item  = i;
                        c.label = fmt::format("step #{} '{}' ({}), picture [{}] {}x{} format {}, state {:#x}", info.order, info.name, info.kind, i,
                            (uint64_t)desc.Width, desc.Height, (int)desc.Format, state);
                        std::scoped_lock _{ g_mtx };
                        if (g_candidates.size() < 2000) {
                            g_candidates.push_back(std::move(c));
                        }
                    }
                }
                if (call < 1200) {
                    spdlog::info("[Steps] run {}: step #{} '{}' ({}) on thread {}:{}", call, info.order, info.name, info.kind, GetCurrentThreadId(),
                        have_io ? (pictures.empty() ? std::string{ " no pictures" } : pictures) : std::string{ " (its pictures not read)" });
                }
                return;
            }
            // kCopying: the chosen pictures of this step, copied right after it.
            const auto it = g_targets.find(pass);
            if (it == g_targets.end() || !have_io) {
                return;
            }
            auto* list = CommandListOf(data);
            auto& vr   = VR::get();
            if (list == nullptr || vr == nullptr) {
                return;
            }
            for (const auto& [item, label] : it->second) {
                if (item >= io.count || io.items[item].resource == nullptr) {
                    continue;
                }
                const auto state = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[item].flags));
                // (Still drawn by this step; and each chosen picture once, though its step may run again in the frame.)
                if ((state & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) == 0) {
                    continue;
                }
                {
                    std::scoped_lock _{ g_mtx };
                    if (!g_copied.insert({ pass, item }).second) {
                        continue;
                    }
                }
                if (vr->m_d3d12.record_step_snapshot(list, io.items[item].resource, state, label) > 0) {
                    ++g_copies;
                }
            }
        }

        uintptr_t StepHook(void* pass, GraphData* data, void* pass_data)
        {
            const uintptr_t table    = *reinterpret_cast<const uintptr_t*>(pass);
            const StepFn    original = OriginalOf(table);
            const uintptr_t result   = original != nullptr ? original(pass, data, pass_data) : 0;
            const int       phase    = g_phase.load(std::memory_order_acquire);
            if (phase == kLogging || phase == kCopying) {
                try {
                    Observe(phase, pass, data, pass_data);
                } catch (...) {
                }
            }
            return result;
        }

        // Between the two frames: up to 16 pictures to copy (the frame's last 12, in its order, and 4 spread over the rest).
        void Choose()
        {
            std::vector<Candidate> all{};
            {
                std::scoped_lock _{ g_mtx };
                all = std::move(g_candidates);
                g_candidates.clear();
            }
            std::stable_sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
            // (Each step's picture once.)
            std::vector<Candidate> unique{};
            std::set<std::pair<void*, uint32_t>> seen{};
            for (auto& c : all) {
                if (seen.insert({ c.pass, c.item }).second) {
                    unique.push_back(std::move(c));
                }
            }
            std::vector<size_t> pick{};
            const size_t        n = unique.size();
            if (n <= 16) {
                for (size_t i = 0; i < n; ++i) {
                    pick.push_back(i);
                }
            } else {
                const size_t early = n - 12;
                for (size_t k = 0; k < 4; ++k) {
                    pick.push_back(k * early / 4);
                }
                for (size_t i = early; i < n; ++i) {
                    pick.push_back(i);
                }
            }
            g_targets.clear();
            for (const size_t i : pick) {
                g_targets[unique[i].pass].push_back({ unique[i].item, unique[i].label });
            }
            spdlog::info("[Steps] {} large colour pictures drawn in the logged frame ({} different); {} chosen to copy in the next frame", all.size(), n, pick.size());
        }
    }

    void OnGraph(void* graph, const char* name, bool before)
    {
        if (!before || name == nullptr || graph == nullptr) {
            return;
        }
        const int phase = g_phase.load(std::memory_order_acquire);
        if (phase == kDone) {
            return;
        }
        const bool frame = std::strcmp(name, "Frame") == 0;
        const bool begin = std::strcmp(name, "CRBeginFrame") == 0;
        try {
            if (phase == kWaiting) {
                if (!frame) {
                    return;
                }
                if (!g_on.load(std::memory_order_relaxed)) {
                    g_was_wanted = false; // (fix173: waits, its setting off)
                    return;
                }
                const auto now    = std::chrono::steady_clock::now();
                const bool wanted = MenuLayer::SeeThroughWanted();
                if (!wanted) {
                    g_was_wanted = false;
                    return;
                }
                if (!g_was_wanted) {
                    g_was_wanted   = true;
                    g_wanted_since = now;
                    return;
                }
                if (now - g_wanted_since < std::chrono::milliseconds(1500)) {
                    return;
                }
                if (!WalkAndHook(graph)) {
                    spdlog::info("[Steps] no step could be hooked: the test ends here");
                    SetStatus("Render steps test: ended (no step could be hooked)");
                    g_phase.store(kDone, std::memory_order_release);
                    return;
                }
                SetStatus("Render steps test: logging one frame");
                g_phase.store(kLogging, std::memory_order_release);
                return;
            }
            if (phase == kLogging && begin) {
                Choose();
                g_phase.store(kArmed, std::memory_order_release);
                return;
            }
            if (phase == kArmed && frame) {
                SetStatus("Render steps test: copying one frame's pictures");
                g_phase.store(kCopying, std::memory_order_release);
                return;
            }
            if (phase == kCopying && begin) {
                g_phase.store(kDone, std::memory_order_release);
                Unhook();
                if (auto& vr = VR::get(); vr != nullptr) {
                    vr->m_d3d12.finish_step_snapshots();
                }
                spdlog::info("[Steps] the test is done: {} steps run in the logged frame, {} pictures copied (put together in menu_steps.jpg in a "
                             "moment)",
                    g_calls.load(), g_copies.load());
                SetStatus(fmt::format("Render steps test: done ({} pictures, menu_steps.jpg)", g_copies.load()));
            }
        } catch (...) {
        }
    }

    void SetOn(bool on)
    {
        if (g_on.exchange(on, std::memory_order_relaxed) != on && g_phase.load(std::memory_order_acquire) == kWaiting) {
            SetStatus(on ? "Render steps test: waits for a see-through menu" : "Render steps test: off (setting RenderStepsTest)");
        }
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
