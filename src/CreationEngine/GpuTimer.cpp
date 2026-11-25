#include "GpuTimer.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "LiveWorld.h"
#include "StereoView.h"
#include <Framework.hpp>
#include <mods/VR.hpp>
#include <mods/vr/d3d12/CommandContext.hpp>

namespace GpuTimer
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int      kSlots = 4;      // frames being timed, resolved or read at once
        constexpr uint32_t kPairs = 4096;   // steps timed in a frame, at most (a frame has about 1,300)
        constexpr uint32_t kEvery = 31;     // one frame timed in this many (odd: the eyes, taking turns, timed alike)
        constexpr uint32_t kWalkEvery = 120; // the frame's graphs walked again (every kind of step hooked) every this many frames...
        constexpr uint32_t kWalkRarely = 1200; // ...or this many, once a few walks found no new kinds
        constexpr uint32_t kNameSize = 40;

        // A step's view: the graph data's +0x108 indexed by its +0x140, its handle at +0x24 (24 bits). Its list as the game keeps it:
        // [[data+0x138]+0x120]+0x60 (the game's getter, 0x142A818D0, makes one when there is none: not called here).
        constexpr uintptr_t kDataRecords = 0x108, kDataIndex = 0x140, kRecordKey = 0x24;
        constexpr uintptr_t kDataHolder = 0x138, kHolderContext = 0x120, kContextList = 0x60;

        enum State : int
        {
            kFree      = 0,
            kRecording = 1,
            kRecorded  = 2,
            kResolving = 3,
        };
        struct Entry
        {
            char    name[kNameSize]{};
            uint8_t kind{ 0 }; // 0 the world's view (the left eye), 1 the right eye's, 2 a menu's, 3 the frame's own
            bool    closed{ false };
        };
        struct Slot
        {
            std::atomic<int>          state{ kFree };
            uint32_t                  frame{ 0 };
            uint32_t                  world{ 0xFFFFFFFF };
            std::atomic<uint32_t>     count{ 0 };
            std::atomic<int>          in_flight{ 0 }; // (steps begun and not yet ended)
            uint32_t                  waited{ 0 };    // (presents it waited, recorded, for steps still in flight)
            std::array<Entry, kPairs> entries{};
            d3d12::CommandContext     commands{};
            uint64_t                  fence_target{ 0 };
            uint32_t                  resolved{ 0 };
        };
        std::array<Slot, kSlots> g_slots{};
        std::atomic<int>         g_current{ -1 };
        std::atomic<bool>        g_on{ false }; // (set from the setting each frame: GameSettingsComponent)
        ComPtr<ID3D12QueryHeap>  g_heap{};
        ComPtr<ID3D12Resource>   g_readback{};
        std::atomic<bool>        g_ready{ false };
        bool                     g_failed{ false };
        uint32_t                 g_frames{ 0 };      // (render thread) frames begun
        int                      g_walks_left{ 0 };  // (render thread) graphs still to walk this round
        int                      g_walk_new{ 0 };    // (render thread) kinds newly hooked this round
        int                      g_quiet_walks{ 0 }; // (render thread) rounds in a row that found none
        uint64_t                 g_frequency{ 0 };
        std::atomic<uint32_t>    g_no_list{ 0 };     // steps of timed frames not timed: no list yet as they began

        // (Present thread) The last 10 s: per view kind, the frames it was in, the steps' sum, the spans; per step (by name), its sum.
        struct Totals
        {
            uint32_t frames{ 0 };
            uint32_t view_frames[4]{};
            double   steps_ms[4]{};
            double   span_ms[4]{};
            uint32_t steps[4]{};
            std::map<std::pair<int, std::string>, double> by_name{};
        };
        Totals                                g_totals{};
        std::chrono::steady_clock::time_point g_logged_at{ std::chrono::steady_clock::now() };

        bool ReadBytes(uintptr_t at, void* out, size_t size)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        template <class T>
        bool Read(uintptr_t at, T& out)
        {
            return ReadBytes(at, &out, sizeof(T));
        }

        ID3D12GraphicsCommandList* ListOf(void* data)
        {
            uintptr_t holder = 0, context = 0, list = 0;
            if (data == nullptr || !Read(reinterpret_cast<uintptr_t>(data) + kDataHolder, holder) || holder == 0 ||
                !Read(holder + kHolderContext, context) || context == 0 || !Read(context + kContextList, list)) {
                return nullptr;
            }
            return reinterpret_cast<ID3D12GraphicsCommandList*>(list);
        }

        // The step's name (its +8: a string), into the entry.
        void ReadName(const void* pass, char (&out)[kNameSize])
        {
            out[0]           = 0;
            const char* text = nullptr;
            if (!Read(reinterpret_cast<uintptr_t>(pass) + 8, text) || text == nullptr) {
                return;
            }
            __try {
                size_t i = 0;
                for (; i < kNameSize - 1 && text[i] != 0; ++i) {
                    out[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
                }
                out[i] = 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out[0] = 0;
            }
        }

        // (Render thread) The query heap and the read-back buffer, made once; every query in the heap written once on the game's
        // queue before any is used (resolving one never written can hang or remove the device: fix140's review).
        bool Ready()
        {
            if (g_ready.load(std::memory_order_acquire)) {
                return true;
            }
            if (g_failed) {
                return false;
            }
            auto device = g_framework->get_d3d12_hook()->get_device();
            auto queue  = g_framework->get_d3d12_hook()->get_command_queue();
            if (device == nullptr || queue == nullptr) {
                return false;
            }
            D3D12_QUERY_HEAP_DESC heap{};
            heap.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heap.Count = kSlots * kPairs * 2;
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = static_cast<UINT64>(heap.Count) * sizeof(uint64_t);
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const auto fail = [&](const char* what) {
                g_failed = true;
                g_heap.Reset();
                g_readback.Reset();
                spdlog::warn("[GpuTimer] {}: no GPU times", what);
                return false;
            };
            if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&g_heap))) ||
                FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))) ||
                FAILED(queue->GetTimestampFrequency(&g_frequency)) || g_frequency == 0) {
                return fail("its timestamp queries could not be made");
            }
            g_heap->SetName(L"GPU timer: the steps' timestamps");
            g_readback->SetName(L"GPU timer: the steps' timestamps read back");
            for (auto& slot : g_slots) {
                if (!slot.commands.setup(L"GPU timer: resolving the timestamps")) {
                    return fail("its command lists could not be made");
                }
            }
            auto& first = g_slots[0].commands;
            for (UINT i = 0; i < heap.Count; ++i) {
                first.cmd_list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
            }
            first.has_commands = true;
            first.execute();
            if (!first.has_commands) {
                first.wait(INFINITE); // (done before any is used, and its list ready again)
            }
            if (first.has_commands) {
                return fail("its timestamps could not be started");
            }
            g_ready.store(true, std::memory_order_release);
            spdlog::info("[GpuTimer] ready: one frame in {} timed, each step on the main kind of list, logged every 10 s (ticks a second {})", kEvery,
                g_frequency);
            return true;
        }

        // (Present thread) A timed frame's results, read back: added to the totals.
        void Add(Slot& slot, const uint64_t* stamps)
        {
            const uint32_t n = std::min(slot.resolved, kPairs);
            if (n == 0) {
                return;
            }
            const double ms_per_tick = 1000.0 / static_cast<double>(g_frequency);
            uint64_t     first[4]{}, last[4]{};
            bool         any[4]{};
            ++g_totals.frames;
            for (uint32_t i = 0; i < n; ++i) {
                const auto& e = slot.entries[i];
                if (!e.closed) {
                    continue;
                }
                const uint64_t t0 = stamps[2 * i], t1 = stamps[2 * i + 1];
                if (t1 < t0 || t1 - t0 > g_frequency) {
                    continue; // (not a time: a step whose stamps were not both written this frame)
                }
                const int    k  = e.kind & 3;
                const double ms = static_cast<double>(t1 - t0) * ms_per_tick;
                g_totals.steps_ms[k] += ms;
                ++g_totals.steps[k];
                g_totals.by_name[{ k, std::string{ e.name[0] != 0 ? e.name : "?" } }] += ms;
                first[k] = any[k] ? std::min(first[k], t0) : t0;
                last[k]  = any[k] ? std::max(last[k], t1) : t1;
                any[k]   = true;
            }
            for (int k = 0; k < 4; ++k) {
                if (any[k]) {
                    ++g_totals.view_frames[k];
                    g_totals.span_ms[k] += static_cast<double>(last[k] - first[k]) * ms_per_tick;
                }
            }
        }

        void Log()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_logged_at < std::chrono::seconds(10)) {
                return;
            }
            g_logged_at = now;
            const auto& t = g_totals;
            if (t.frames == 0) {
                return;
            }
            static const char* views[4]{ "the world's view (in full stereo the left eye)", "full stereo's right eye", "menus' 3D views", "the frame's own steps" };
            std::string text = std::format("[GpuTimer] the last 10 s, {} frames timed ({} steps not timed: no list yet), GPU time a frame on average:", t.frames,
                g_no_list.exchange(0));
            for (int k = 0; k < 4; ++k) {
                if (t.view_frames[k] == 0) {
                    continue;
                }
                const double f = t.view_frames[k];
                text += std::format(" {} (in {} frames): {:.2f} ms from its first step's start to its last's end, its {:.0f} steps {:.2f} ms;", views[k],
                    t.view_frames[k], t.span_ms[k] / f, t.steps[k] / f, t.steps_ms[k] / f);
            }
            spdlog::info("{}", text);
            // The costliest steps of the left and right eyes' views.
            for (int k = 0; k < 2; ++k) {
                if (t.view_frames[k] == 0) {
                    continue;
                }
                std::vector<std::pair<double, std::string>> costs{};
                for (const auto& [key, ms] : t.by_name) {
                    if (key.first == k) {
                        costs.emplace_back(ms, key.second);
                    }
                }
                std::sort(costs.begin(), costs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string list{};
                for (size_t i = 0; i < costs.size() && i < 15; ++i) {
                    list += std::format("{}{} {:.2f}", i == 0 ? "" : ", ", costs[i].second, costs[i].first / t.view_frames[k]);
                }
                spdlog::info("[GpuTimer]   {}'s costliest steps (ms a frame): {}", views[k], list);
            }
            g_totals = Totals{};
        }
    }

    void SetOn(bool on)
    {
        g_on.store(on, std::memory_order_relaxed);
    }

    void OnGraph(void* graph, const char* name, bool before)
    {
        if (!before || name == nullptr || graph == nullptr) {
            return;
        }
        const bool begin = std::strcmp(name, "CRBeginFrame") == 0;
        if (begin) {
            // The frame timed before has all its steps begun (the game waits for a graph's steps before the next one starts; any still
            // ending are waited for at the present).
            if (const int s = g_current.exchange(-1); s >= 0) {
                g_slots[s].waited = 0;
                g_slots[s].state.store(kRecorded, std::memory_order_release);
            }
            ++g_frames;
            // (The walks: every 120 frames, every 1200 once three found no new kinds of step.)
            if (g_on.load(std::memory_order_relaxed) && g_frames % (g_quiet_walks >= 3 ? kWalkRarely : kWalkEvery) == 1) {
                g_walks_left = 3; // (this frame's CRBeginFrame, Frame and CREndFrame)
                g_walk_new   = 0;
            }
        }
        if (g_walks_left > 0 && (begin || std::strcmp(name, "Frame") == 0 || std::strcmp(name, "CREndFrame") == 0)) {
            g_walk_new += LiveWorld::HookAllSteps(graph);
            if (--g_walks_left == 0) {
                g_quiet_walks = g_walk_new == 0 ? g_quiet_walks + 1 : 0;
            }
        }
        if (!begin || !g_on.load(std::memory_order_relaxed) || g_frames % kEvery != 0 || !Ready()) {
            return;
        }
        auto& vr = VR::get();
        for (int s = 0; s < kSlots; ++s) {
            auto& slot = g_slots[s];
            if (slot.state.load(std::memory_order_acquire) != kFree) {
                continue;
            }
            slot.frame = vr != nullptr ? vr->m_render_frame_count.load() : 0;
            uint32_t world = 0xFFFFFFFF;
            slot.world = LiveWorld::WorldViewHandle(world) ? (world & 0xFFFFFF) : 0xFFFFFFFF;
            slot.count.store(0, std::memory_order_relaxed);
            slot.in_flight.store(0, std::memory_order_relaxed);
            slot.state.store(kRecording, std::memory_order_release);
            g_current.store(s, std::memory_order_release);
            return;
        }
    }

    bool Timing()
    {
        return g_current.load(std::memory_order_relaxed) >= 0;
    }

    int Begin(void* data)
    {
        const int s = g_current.load(std::memory_order_acquire);
        if (s < 0) {
            return -1;
        }
        auto* list = ListOf(data);
        if (list == nullptr) {
            g_no_list.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return -1;
        }
        auto&          slot = g_slots[s];
        const uint32_t i    = slot.count.fetch_add(1, std::memory_order_relaxed);
        if (i >= kPairs) {
            return -1;
        }
        slot.in_flight.fetch_add(1, std::memory_order_acq_rel);
        slot.entries[i].closed = false;
        list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(s) * kPairs * 2 + i * 2);
        return s * static_cast<int>(kPairs) + static_cast<int>(i);
    }

    void End(int pair, void* pass, void* data)
    {
        if (pair < 0) {
            return;
        }
        const int      s    = pair / static_cast<int>(kPairs);
        const uint32_t i    = static_cast<uint32_t>(pair) % kPairs;
        auto&          slot = g_slots[s];
        auto*          list = ListOf(data);
        if (list != nullptr) {
            list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(s) * kPairs * 2 + i * 2 + 1);
            // Whose step: its view's handle.
            uint32_t  index = 0, key = 0;
            uintptr_t record = 0;
            uint8_t   kind   = 3;
            if (Read(reinterpret_cast<uintptr_t>(data) + kDataIndex, index) && index < 64 &&
                Read(reinterpret_cast<uintptr_t>(data) + kDataRecords + 8 * static_cast<uintptr_t>(index), record) && record != 0 &&
                Read(record + kRecordKey, key)) {
                key &= 0xFFFFFF;
                kind = key == slot.world ? 0 : StereoView::IsEyeView(key) ? 1 : 2;
            }
            auto& entry = slot.entries[i];
            ReadName(pass, entry.name);
            entry.kind   = kind;
            entry.closed = true;
        }
        slot.in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }

    void OnPresent()
    {
        if (!g_ready.load(std::memory_order_acquire)) {
            return;
        }
        auto& vr = VR::get();
        if (vr == nullptr) {
            return;
        }
        const uint32_t presented = vr->m_presenter_frame_count.load();
        for (int s = 0; s < kSlots; ++s) {
            auto&     slot  = g_slots[s];
            const int state = slot.state.load(std::memory_order_acquire);
            // A timed frame whose lists were all handed to the GPU (a frame before the one presented now) and whose steps have all ended
            // (or were waited for a while): its stamps resolved into the read-back buffer, on the game's queue after them.
            if (state == kRecorded && static_cast<int32_t>(presented - slot.frame) > 0 &&
                (slot.in_flight.load(std::memory_order_acquire) <= 0 || ++slot.waited > 30)) {
                slot.resolved = std::min(slot.count.load(std::memory_order_acquire), kPairs);
                if (slot.resolved == 0) {
                    slot.state.store(kFree, std::memory_order_release);
                    continue;
                }
                const UINT first = static_cast<UINT>(s) * kPairs * 2;
                slot.commands.cmd_list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, slot.resolved * 2, g_readback.Get(),
                    static_cast<UINT64>(first) * sizeof(uint64_t));
                slot.commands.has_commands = true;
                slot.commands.execute();
                if (slot.commands.has_commands) {
                    // (Its list could not be closed: given up, the timer off.)
                    spdlog::warn("[GpuTimer] the timestamps could not be resolved: the timer stops");
                    g_ready.store(false, std::memory_order_release);
                    g_failed = true;
                    return;
                }
                slot.fence_target = slot.commands.fence_value;
                slot.state.store(kResolving, std::memory_order_release);
            } else if (state == kResolving && slot.commands.fence != nullptr && slot.commands.fence->GetCompletedValue() >= slot.fence_target) {
                const UINT  first = static_cast<UINT>(s) * kPairs * 2;
                D3D12_RANGE range{ static_cast<SIZE_T>(first) * sizeof(uint64_t), static_cast<SIZE_T>(first + slot.resolved * 2) * sizeof(uint64_t) };
                void*       mapped = nullptr;
                if (SUCCEEDED(g_readback->Map(0, &range, &mapped)) && mapped != nullptr) {
                    Add(slot, reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + range.Begin));
                    D3D12_RANGE none{ 0, 0 };
                    g_readback->Unmap(0, &none);
                }
                slot.commands.wait(0); // (done: its list ready for the next time)
                slot.state.store(kFree, std::memory_order_release);
            }
        }
        Log();
    }

    std::string Status()
    {
        return g_ready.load() ? std::string{ "ready" } : g_failed ? std::string{ "could not be made" } : std::string{ "not started" };
    }
}
