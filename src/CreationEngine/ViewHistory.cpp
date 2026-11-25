#include "ViewHistory.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "RenderPassIo.h"
#include "RttiNames.h"

#include <mods/VR.hpp>
#include <CreationEngine/models/GameFlow.h>
#include "ViewHistoryPlan.h"
#include <RE/C/CreationRendererPrivate.h>

namespace ViewHistory
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        using GraphData = RE::CreationRendererPrivate::RenderGraphData;
        using Clock     = std::chrono::steady_clock;

        constexpr uint32_t kLearnFrames  = 4;              // frames watched before the plan is made
        constexpr uint64_t kMaxBytes     = 768ull << 20;   // all the copies together (those let go but maybe still in use too: every view's)
        constexpr uint64_t kMaxOne       = 96ull << 20;    // one copy
        constexpr size_t   kMaxUses      = 400000;         // uses watched while learning (else the plan is not made)
        constexpr int      kPersistent   = 12;             // a view's persistent pictures (RenderGraphDataResourcesList: for the log)
        constexpr int      kRoleStrikes  = 3;              // a history not as learned this many times: no longer kept
        constexpr int      kMaxRelearns  = 6;              // learned again at most this many times while a menu is open
        constexpr int      kMaxRestarts  = 10;             // learning started over at most this many times while a menu is open
        constexpr int      kLearnLogs    = 3;              // "watching" logged this many times while a menu is open

        enum Phase : int
        {
            kOff      = 0,
            kLearning = 1,
            kPlanning = 2,
            kKeeping  = 3,
            kNothing  = 4, // (nothing to keep apart, or given up: until the menu closes)
        };

        struct Tables
        {
            std::vector<ViewHistoryPlan::Action>        actions{};
            std::unordered_map<void*, std::vector<int>> at[2]{}; // the actions at a step, in frames of a parity
            std::unique_ptr<std::atomic<uint32_t>[]>    done{};  // (each action's last frame, +1: once a frame)
            std::set<uint64_t>                          roles{};
            std::unordered_map<uintptr_t, std::vector<uint64_t>> resource_roles{}; // (each kept picture: the views' histories it is)
            std::unordered_map<uintptr_t, std::string>  descriptions{};   // (...for the log)
            uint64_t                                    signature{ 0 };
            uint64_t                                    frame_signature{ 0 };
            size_t                                      kept{ 0 };
            // (The steps it acts at, by their view and name as they were planned: a plan used again only where they are still those.)
            std::vector<std::tuple<void*, uint8_t, std::string>> steps{};
        };

        struct Slot
        {
            ComPtr<ID3D12Resource> copy{};
            D3D12_RESOURCE_DESC    desc{};
            bool                   ready{ false }; // in the copy-source state (else common: just made)
            bool                   saved{ false };
            uint32_t               saved_frame{ 0 };
            uint32_t               used_frame{ 0 };
            uint64_t               bytes{ 0 };
        };
        using SlotKey = std::tuple<int, uint64_t, uint64_t>; // (eye, history, the picture's kind)

        std::atomic<int>      g_phase{ kOff };
        std::atomic<uint32_t> g_frame{ 0 };
        std::atomic<bool>     g_mono{ true };

        std::mutex g_mtx{};
        // (Kept alive a few frames after they are replaced: steps of an earlier frame may still look at them.)
        std::array<std::shared_ptr<const Places>, 4> g_places_keep{};
        int                                          g_places_next{ 0 };
        std::atomic<const Places*>                   g_places{ nullptr };
        std::array<std::shared_ptr<const Tables>, 4> g_tables_keep{};
        int                                          g_tables_next{ 0 };
        std::atomic<const Tables*>                   g_tables{ nullptr };
        // Plans made in this game session, by the frame's views (at most 8, the newest last): a menu's views seen again (going back to
        // a menu, a preview shown again) take theirs at once, without learning again (fix122's test: switching between the Data menu,
        // Skills, Missions and the Inventory made it learn again and again, until it gave up for the rest of the menu).
        std::vector<std::shared_ptr<const Tables>>   g_plans{};
        Clock::time_point                            g_keeping_since{};     // (a plan kept since: after a while, learning again is allowed again)
        Clock::time_point                            g_gave_up_at{};        // (given up: tried again a few seconds on, or for other views)
        uint64_t                                     g_gave_up_signature{ 0 };
        int                                          g_give_up_logs{ 0 };
        int                                          g_plan_used_logs{ 0 };
        // Views planned with nothing to keep (by the views' and the frame's own steps' signatures): not learned again for them. And
        // the tries again while a menu is open (a few seconds on, or for other views): not without end.
        std::set<std::pair<uint64_t, uint64_t>>      g_nothing{};
        int                                          g_timed_retries{ 0 };
        int                                          g_view_retries{ 0 };
        int                                          g_long_logs{ 0 };   // (the lines logged at the end of each learning)
        int                                          g_strike_logs{ 0 };
        std::shared_ptr<const Places>                g_plan_places{};
        // The frame's own graphs around it ("CREndFrame", "CRBeginFrame"): their steps as last walked, as they ran.
        using StepMap = std::unordered_map<void*, StepPlace>;
        std::array<std::array<std::shared_ptr<const StepMap>, 4>, 2> g_edges_keep{};
        int                                                          g_edges_next[2]{};
        std::atomic<const StepMap*>                                  g_edges[2]{};
        std::shared_ptr<const StepMap>                               g_edges_now[2]{};
        bool                                                         g_edges_new[2]{};      // (new objects seen from one walk to the next)
        bool                                                         g_edges_incomplete{ false };
        int                                                          g_edges_logged{ 0 };
        bool                                                         g_frame_steps_logged{ false };
        bool                                                         g_fresh_logged{ false };
        std::atomic<uint64_t>                                        g_unknown_steps{ 0 }; // (steps run while learning, not found by the walk)
        std::map<std::string, int>                                   g_unknown_kinds{}; // (under g_mtx: their kinds, for the log)
        // (Under g_mtx.) The pictures those steps wrote (they cannot be placed in the frame's order: none of the world's histories they
        // write is kept apart; a step that only reads one reads either the world's or the menu's, as before). The copies themselves are
        // safe wherever such a step runs: each is made right after a step, from and back to the state that step's list gives (the game
        // plans the state changes of the steps' tracked pictures before it records, fix120's research), so only what a history holds is at stake.
        std::set<uintptr_t>                                          g_unknown_resources{};
        std::set<uintptr_t>                                          g_unknown_touched{}; // (...and every picture they used: never resting)
        std::map<uintptr_t, std::string>                             g_unknown_users{}; // (a picture: the kind of step that wrote it first)
        std::set<uintptr_t>                                          g_plan_unknown{};  // (as the plan was made: what MakeTables leaves alone)
        // (Under g_mtx.) Pictures used by a step on a command list that is not a direct one (the compute queue: the game does not keep it
        // in step with a copy on the direct one): left alone too. (The game runs its compute steps on the direct queue by default:
        // bForceComputeOnGraphicsPipe.)
        std::set<uintptr_t>                                          g_compute_resources{};
        std::set<uintptr_t>                                          g_plan_compute{};
        int                                                          g_unknown_off_logged{ 0 };
        // (Under g_mtx, for the log.) The items read while learning, by what was found: a texture's picture, a texture entry with no picture
        // attached, no entry, not a texture (buffers, tokens, data); unfound steps' runs whose list could not be read or not all of it;
        // and the walked steps whose textures were not all found, by name.
        uint64_t                                                     g_items[4]{};
        uint64_t                                                     g_untracked{ 0 }; // (pictures found whose state the game does not track)
        uint64_t                                                     g_attached{ 0 };  // (...attached by their step as it ran)
        std::set<uintptr_t>                                          g_persistent[3]{}; // (each group's views' persistent pictures, for the log)
        uint64_t                                                     g_unknown_no_list{ 0 };
        uint64_t                                                     g_unknown_many{ 0 };
        uint64_t                                                     g_unknown_not_found{ 0 }; // (their textures whose picture was not found)
        std::map<std::string, int>                                   g_unfound_by_step{};

        uint32_t                                          g_learn_first{ 0 };
        uint32_t                                          g_learn_last{ 0 };
        uint64_t                                          g_learn_signature{ 0 };
        std::shared_ptr<const Places>                     g_learn_places{}; // (the steps as learning started: what changed, if it starts over)
        int                                               g_restarts{ 0 };
        int                                               g_learn_logged{ 0 };
        int                                               g_restarts_logged{ 0 };
        std::vector<ViewHistoryPlan::Use>                 g_uses{};
        bool                                              g_uses_full{ false };
        std::unordered_map<uintptr_t, std::string>        g_descriptions{};
        std::future<ViewHistoryPlan::Result>              g_planning{};
        std::vector<std::future<ViewHistoryPlan::Result>> g_abandoned{}; // (plans no longer wanted: let finish on their own)
        size_t                                            g_planned_uses{ 0 };
        int                                               g_relearns{ 0 };
        Clock::time_point                                 g_last_learn{};

        std::map<SlotKey, Slot>                                                      g_slots{};
        std::vector<std::tuple<ComPtr<ID3D12Resource>, Clock::time_point, uint64_t, uint32_t>> g_retired{}; // (the copy, when, its size, VR::m_presents_done then)
        uint64_t                                                                     g_bytes{ 0 }; // copies kept and let go (until released)
        // (fix189, under g_mtx) The card's video memory sample last looked at (VR's D3D12Component::m_mem_samples), g_bytes then (the copies
        // made since are not in that sample's use yet), and the copies refused for the budget (for the log).
        uint32_t                                                                     g_mem_sample_seen{ 0 };
        uint64_t                                                                     g_bytes_at_sample{ 0 };
        uint64_t                                                                     g_budget_refusals{ 0 };
        std::atomic<bool>                                                            g_any_retired{ false };
        // (Presents so far, any thread: a copy let go is released only some presents on, ReleaseRetired.)
        uint32_t Presents()
        {
            auto& vr = VR::get();
            return vr != nullptr ? vr->m_presents_done.load(std::memory_order_relaxed) : 0;
        }
        std::map<uint64_t, std::pair<int, uint32_t>>                                 g_role_strikes{}; // (strikes, the frame of the last)
        std::set<uint64_t>                                                           g_roles_off{};
        std::atomic<bool>                                                            g_want_walk{ false };
        std::atomic<bool>                                                            g_any_abandoned{ false };
        uint32_t                                                                     g_last_trim{ 0 };

        std::atomic<uint64_t> g_saves{ 0 }, g_loads{ 0 }, g_not_saved{ 0 }, g_mismatches{ 0 }, g_compute{ 0 }, g_failed{ 0 };
        std::atomic<bool>     g_taa_logged{ false };
        Clock::time_point     g_last_log{};
        uint64_t              g_logged[5]{};
        int                   g_plans_logged{ 0 };
        std::set<uint64_t>    g_logged_plans{}; // (this menu: the views whose plans were logged, each once)
        // This menu (the world kept drawn behind it), for its summary as it closes: its frames by phase, the menus shown, the times it
        // was given up and why last.
        std::array<uint32_t, 5> g_session_frames{};
        std::set<std::string>   g_session_menus{};
        int                     g_session_give_ups{ 0 };
        std::string             g_session_last_give_up{};
        int                     g_session_logs{ 0 };
        std::string           g_status{ "World's own last-frame pictures behind menus: not needed yet" };

        ID3D12Resource* Persistent(void* data, uint32_t index, int parity)
        {
            __try {
                return static_cast<GraphData*>(data)->getResourceByIndex(index, parity);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        bool DescOf(ID3D12Resource* resource, D3D12_RESOURCE_DESC& out)
        {
            __try {
                out = resource->GetDesc();
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool DefaultHeap(ID3D12Resource* resource)
        {
            D3D12_HEAP_PROPERTIES heap{};
            D3D12_HEAP_FLAGS      flags{};
            __try {
                return SUCCEEDED(resource->GetHeapProperties(&heap, &flags)) && heap.Type == D3D12_HEAP_TYPE_DEFAULT;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SameDesc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
        {
            return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
                   a.MipLevels == b.MipLevels && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count && a.Layout == b.Layout &&
                   a.Flags == b.Flags;
        }

        uint64_t DescKey(const D3D12_RESOURCE_DESC& d)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (const uint64_t v : { (uint64_t)d.Dimension, (uint64_t)d.Width, (uint64_t)d.Height, (uint64_t)d.DepthOrArraySize, (uint64_t)d.MipLevels,
                     (uint64_t)d.Format, (uint64_t)d.SampleDesc.Count, (uint64_t)d.Layout, (uint64_t)d.Flags }) {
                h = (h ^ v) * 0x100000001B3ull;
            }
            return h;
        }

        std::string Describe(const D3D12_RESOURCE_DESC& d)
        {
            switch (d.Dimension) {
            case D3D12_RESOURCE_DIMENSION_BUFFER:
                return fmt::format("a buffer of {} bytes", (uint64_t)d.Width);
            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                return fmt::format("{}x{}x{} format {}", (uint64_t)d.Width, d.Height, d.DepthOrArraySize, (int)d.Format);
            default:
                return fmt::format("{}x{} format {}", (uint64_t)d.Width, d.Height, (int)d.Format);
            }
        }

        // (Bytes a pixel of a format takes: the common ones; others counted as the largest.)
        uint64_t BytesPerPixel(DXGI_FORMAT f)
        {
            const int v = static_cast<int>(f);
            if (v >= 1 && v <= 4) {
                return 16;
            }
            if (v >= 5 && v <= 8) {
                return 12;
            }
            if (v >= 9 && v <= 22) {
                return 8;
            }
            if ((v >= 23 && v <= 47) || v == 67 || (v >= 87 && v <= 93)) {
                return 4;
            }
            if (v >= 48 && v <= 59) {
                return 2;
            }
            if (v >= 60 && v <= 65) {
                return 1;
            }
            return 16;
        }

        // A picture whose copy can be made and put back whole: one level, one sample, in ordinary video memory (not a tiled one,
        // not one the processor reads or writes), not too large for one copy (its size worked out from its format).
        bool Usable(ID3D12Resource* resource, D3D12_RESOURCE_DESC& d)
        {
            if (!DescOf(resource, d)) {
                return false;
            }
            bool     shape = false;
            uint64_t bytes = 0;
            switch (d.Dimension) {
            // (Never a buffer: the game's buffers are parts of larger shared ones, which a whole copy would overwrite.)
            case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                shape = d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 && d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN;
                bytes = d.Width * d.Height * BytesPerPixel(d.Format);
                break;
            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                shape = d.MipLevels == 1 && d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN;
                bytes = d.Width * d.Height * d.DepthOrArraySize * BytesPerPixel(d.Format);
                break;
            default:
                break;
            }
            // (Not a depth picture: the game gives those their states by another table than the one read here, 0x142B02FD0.)
            return shape && bytes + bytes / 4 <= kMaxOne &&
                   (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY |
                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0 &&
                   DefaultHeap(resource);
        }

        // A view's persistent pictures as they are now (the game gives them only while they rest as unordered access).
        std::vector<uintptr_t> PersistentOf(void* data)
        {
            std::vector<uintptr_t> out{};
            for (int index = 0; data != nullptr && index < kPersistent; ++index) {
                for (int parity = 0; parity < 2; ++parity) {
                    if (auto* resource = Persistent(data, index, parity); resource != nullptr) {
                        out.push_back(reinterpret_cast<uintptr_t>(resource));
                    }
                }
            }
            return out;
        }

        // An item's state, known where the game tracks it; and where it does not, only for one of the view's persistent pictures (the
        // anti-aliasing history among them) that the step needs as unordered access: those rest in that state (their state word,
        // context+0x44, set so at creation and given back after their last tracked use), the game plans no change of them there and
        // attaches them without one, and the anti-aliasing step makes none (fix120b's check of the exe). 0: not known.
        uint32_t KnownState(const RenderPassIo::PassIo::Item& it, const std::vector<uintptr_t>& persistent)
        {
            const uint32_t state = RE::CreationRendererPrivate::RenderPassItem::getDXGIState(it.flags);
            if (it.tracked) {
                return state;
            }
            return state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                           std::find(persistent.begin(), persistent.end(), reinterpret_cast<uintptr_t>(it.resource)) != persistent.end()
                       ? state
                       : 0u;
        }

        // An untracked item that is one of the view's persistent pictures: it rests in unordered access (KnownState's reason), whatever
        // state the step asks for (the anti-aliasing step reads its history so, in the shader-read state it lists). Its state is taken
        // as that only where the plan found nothing else using the picture in the frame with a tracked state (ViewHistoryPlan's
        // WithRestingStates): the menu's views' anti-aliasing histories are put back there, after the view before reads them.
        bool Resting(const RenderPassIo::PassIo::Item& it, const std::vector<uintptr_t>& persistent)
        {
            return !it.tracked && std::find(persistent.begin(), persistent.end(), reinterpret_cast<uintptr_t>(it.resource)) != persistent.end();
        }

        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter  = after;
            return b;
        }

        // (fix189, under g_mtx) A new copy of this size would bring the card's video memory within 64 MB of the budget Windows gives the
        // game: the memory in use as last sampled (the present thread, once a second: D3D12Component::sample_memory), plus the copies made
        // since that sample, plus this one. Over the budget Windows moves the game's data out to system memory and frames slow down many
        // times (a test's log: 7010 of 6835 MB, the Data menu at about 11 fps); kMaxBytes alone did not look at what the card has. Not
        // known yet (no sample, or no budget given): false, as before. (Only 64 MB: a tested PC plays with 230-280 MB free, and the Data
        // menu's own jump was about 630 MB of which these copies were about 240: a wider margin would refuse nearly every copy, and the
        // menus' 3D previews would lose their own history again, for little gain.)
        constexpr uint64_t kBudgetMarginMb = 64;
        bool g_refused_near_budget{ false }; // (fix189, under g_mtx) SlotFor's last refusal was NearBudget's
        bool NearBudget(uint64_t bytes, uint64_t& used_mb, uint64_t& budget_mb, uint64_t& since_mb)
        {
            auto& vr = VR::get();
            if (vr == nullptr) {
                return false;
            }
            const uint32_t samples = vr->m_d3d12.m_mem_samples.load();
            used_mb                = vr->m_d3d12.m_mem_used_mb.load();
            budget_mb              = vr->m_d3d12.m_mem_budget_mb.load();
            if (samples == 0 || budget_mb == 0) {
                return false;
            }
            if (samples != g_mem_sample_seen) { // (a new sample: what was made until now is in its use)
                g_mem_sample_seen = samples;
                g_bytes_at_sample = g_bytes;
            }
            since_mb                = (g_bytes > g_bytes_at_sample ? g_bytes - g_bytes_at_sample : 0) >> 20;
            const uint64_t this_mb  = (bytes + (1ull << 20) - 1) >> 20;
            return used_mb + since_mb + this_mb + kBudgetMarginMb > budget_mb;
        }

        // The copy for this eye and history, made if there is none (outside the lock: making one takes a moment; the other steps'
        // copies go on meanwhile).
        Slot* SlotFor(int eye, uint64_t role, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& d, std::unique_lock<std::mutex>& lock)
        {
            const SlotKey key{ eye, role, DescKey(d) };
            if (const auto it = g_slots.find(key); it != g_slots.end()) {
                return &it->second;
            }
            // (Its size, and room for it in the budget, first: none made when there is none.)
            ComPtr<ID3D12Device> device{};
            uint64_t             bytes = 0;
            auto                 desc  = d;
            desc.Alignment             = 0;
            if (SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device)))) {
                bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            }
            if (device == nullptr || bytes == 0 || bytes > kMaxOne || g_bytes + bytes > kMaxBytes) {
                if (g_failed++ < 4) {
                    spdlog::warn("[History] a copy of a view's {} cannot be made ({} MB; {} MB of copies already): not kept apart", Describe(d),
                        bytes >> 20, g_bytes >> 20);
                }
                return nullptr;
            }
            // (fix189) Nor near the card's video memory budget (NearBudget): the view's picture is then not kept apart this time (asked
            // again the next frame: no strike, the review; memory comes and goes).
            if (uint64_t used_mb = 0, budget_mb = 0, since_mb = 0; NearBudget(bytes, used_mb, budget_mb, since_mb)) {
                g_refused_near_budget = true;
                if (g_budget_refusals++ < 8) {
                    spdlog::warn("[History] (fix189) a copy of a view's {} not made: video memory {} of {} MB (+{} MB of copies made since it was "
                                 "measured, +{} MB this one) would come within {} MB of its budget; not kept apart ({} MB of copies already)",
                        Describe(d), used_mb, budget_mb, since_mb, bytes >> 20, kBudgetMarginMb, g_bytes >> 20);
                }
                return nullptr;
            }
            g_bytes += bytes; // (reserved)
            lock.unlock();
            ComPtr<ID3D12Resource> made{};
            D3D12_HEAP_PROPERTIES  heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&made)))) {
                made.Reset();
            }
            lock.lock();
            if (const auto it = g_slots.find(key); it != g_slots.end()) {
                g_bytes -= std::min(g_bytes, bytes); // (made meanwhile by another step: ours dropped)
                return &it->second;
            }
            if (made == nullptr) {
                g_bytes -= std::min(g_bytes, bytes);
                if (g_failed++ < 4) {
                    spdlog::warn("[History] a copy of a view's {} could not be made: not kept apart", Describe(d));
                }
                return nullptr;
            }
            made->SetName(L"Menu panel: a view's own last-frame picture");
            auto& slot = g_slots[key];
            slot.copy  = made;
            slot.desc  = d;
            slot.bytes = bytes;
            return &slot;
        }

        // (Under the lock.) A history strike: not as learned, or its copy not made. Three in a while and it is no longer kept.
        void Strike(uint64_t role, uint32_t frame, const char* why)
        {
            auto& [strikes, last] = g_role_strikes[role];
            if (frame - last > 300) {
                strikes = 0; // (a while without one: forgotten)
            }
            last = frame;
            if (++strikes >= kRoleStrikes && g_roles_off.insert(role).second && g_strike_logs++ < 12) {
                spdlog::info("[History] one of the views' pictures is no longer kept apart: {}", why);
            }
        }

        // Right after the world's last use of it: what the world left in it, copied (from the state the step left it in, and back).
        void Save(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t state, int eye, uint64_t role, uint32_t frame)
        {
            D3D12_RESOURCE_DESC d{};
            if (!DescOf(resource, d)) {
                return;
            }
            std::unique_lock lock{ g_mtx };
            g_refused_near_budget = false;
            Slot* slot = SlotFor(eye, role, resource, d, lock);
            if (slot == nullptr && g_refused_near_budget) {
                return; // (fix189) video memory near its budget now: not this frame, and no strike (the history is not given up for it)
            }
            if (slot == nullptr) {
                g_role_strikes[role] = { kRoleStrikes - 1, frame };
                Strike(role, frame, "its copy cannot be made");
                return;
            }
            slot->used_frame = frame;
            const auto             game = static_cast<D3D12_RESOURCE_STATES>(state);
            D3D12_RESOURCE_BARRIER to[2]{}, back[2]{};
            UINT                   n = 0;
            if (game != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                to[n]   = Transition(resource, game, D3D12_RESOURCE_STATE_COPY_SOURCE);
                back[n] = Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, game);
                ++n;
            }
            to[n]   = Transition(slot->copy.Get(), slot->ready ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            back[n] = Transition(slot->copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ++n;
            list->ResourceBarrier(n, to);
            list->CopyResource(slot->copy.Get(), resource);
            list->ResourceBarrier(n, back);
            slot->ready       = true;
            slot->saved       = true;
            slot->saved_frame = frame;
            ++g_saves;
        }

        // Right after the last use of it by anything else: the world's copy put back (for the frame it is for: its eye's).
        void Load(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t state, int eye, uint64_t role, uint32_t frame)
        {
            D3D12_RESOURCE_DESC d{};
            if (!DescOf(resource, d)) {
                return;
            }
            std::scoped_lock _{ g_mtx };
            const auto it = g_slots.find(SlotKey{ eye, role, DescKey(d) });
            if (it == g_slots.end() || !it->second.saved || !it->second.ready || frame - it->second.saved_frame > 4 || !SameDesc(it->second.desc, d)) {
                ++g_not_saved;
                return;
            }
            it->second.used_frame = frame;
            const auto game = static_cast<D3D12_RESOURCE_STATES>(state);
            if (game != D3D12_RESOURCE_STATE_COPY_DEST) {
                const auto to = Transition(resource, game, D3D12_RESOURCE_STATE_COPY_DEST);
                list->ResourceBarrier(1, &to);
            }
            list->CopyResource(resource, it->second.copy.Get());
            if (game != D3D12_RESOURCE_STATE_COPY_DEST) {
                const auto back = Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, game);
                list->ResourceBarrier(1, &back);
            }
            ++g_loads;
        }

        void Learn(void* pass, const StepPlace& place, void* data, void* pass_data, bool taa, ListOf list_of, uint32_t frame, uint32_t empty_before)
        {
            RenderPassIo::PassIo io{};
            const bool           have_io = pass_data != nullptr && RenderPassIo::ReadPassIo(pass_data, io);
            struct Found
            {
                ID3D12Resource*     resource;
                int32_t             item;
                uint32_t            state;    // (0: not known, KnownState)
                D3D12_RESOURCE_DESC desc;
                bool                attached; // (attached by the step as it ran: the game may change its state then)
                bool                resting;  // (untracked, one of the view's persistent pictures: Resting)
            };
            std::vector<Found>     found{};
            uint64_t               counts[4]{};
            uint64_t               untracked = 0;
            std::vector<uintptr_t> view_persistent{}; // (read once, for an item whose state the game does not track: KnownState)
            bool                   view_read = false;
            for (uint32_t i = 0; have_io && i < io.count; ++i) {
                const auto& it = io.items[i];
                ++counts[it.found & 3];
                untracked += it.found == RenderPassIo::kFound && !it.tracked ? 1 : 0;
                D3D12_RESOURCE_DESC d{};
                if (it.found == RenderPassIo::kFound && Usable(it.resource, d)) {
                    if (!it.tracked && !view_read) {
                        view_read       = true;
                        view_persistent = PersistentOf(data);
                    }
                    found.push_back(Found{ it.resource, static_cast<int32_t>(i), KnownState(it, view_persistent), d, ((empty_before >> i) & 1u) != 0,
                        Resting(it, view_persistent) });
                }
            }
            // (The anti-aliasing step's items and its view's persistent pictures, logged once. Those pictures are not counted as its uses:
            // the steps that use them have them among their own items, where the game attaches them.)
            if (taa && !g_taa_logged.exchange(true)) {
                std::string text{};
                for (uint32_t i = 0; have_io && i < io.count; ++i) {
                    const auto&         it = io.items[i];
                    D3D12_RESOURCE_DESC d{};
                    text += fmt::format(" [{}] state {:#x}{} {}", i, RE::CreationRendererPrivate::RenderPassItem::getDXGIState(it.flags),
                        it.tracked ? "" : " (not tracked)",
                        it.found == RenderPassIo::kFound && DescOf(it.resource, d)
                            ? fmt::format("{} @{:x}", Describe(d), reinterpret_cast<uintptr_t>(it.resource) & 0xffffff)
                            : std::string{ it.found == RenderPassIo::kEmpty ? "(no picture attached)" : "(not found)" });
                }
                std::string persistent{};
                for (int index = 0; data != nullptr && index < kPersistent; ++index) {
                    for (int parity = 0; parity < 2; ++parity) {
                        D3D12_RESOURCE_DESC d{};
                        if (auto* resource = Persistent(data, index, parity); resource != nullptr && DescOf(resource, d)) {
                            persistent += fmt::format(" [{}.{}] {} @{:x}", index, parity, Describe(d), reinterpret_cast<uintptr_t>(resource) & 0xffffff);
                        }
                    }
                }
                spdlog::info("[History] the anti-aliasing step '{}' uses {} items:{}; its view's persistent pictures:{}", place.name, io.total, text,
                    persistent.empty() ? std::string{ " none" } : persistent);
            }
            // (Its view's persistent pictures: which views have which, and whether the steps have them among their items: for the log.)
            const std::vector<uintptr_t> persistent = taa ? PersistentOf(data) : std::vector<uintptr_t>{};
            bool direct = false;
            if (!found.empty() && list_of != nullptr) {
                auto* list = list_of(data);
                direct     = list != nullptr && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
            }
            const bool single = !place.shared;
            std::scoped_lock _{ g_mtx };
            if (g_phase.load() != kLearning) {
                return;
            }
            for (int k = 0; k < 4; ++k) {
                g_items[k] += counts[k];
            }
            for (const uintptr_t r : persistent) {
                g_persistent[place.group % 3].insert(r);
            }
            g_untracked += untracked;
            // (A texture of a walked step whose picture was not found: a use not seen. Logged by the step's name; not a reason to leave the
            // histories alone, as at worst a history kept then holds what it holds now.)
            if (const uint64_t unfound = counts[RenderPassIo::kNotFound] + counts[RenderPassIo::kEmpty];
                unfound > 0 && (g_unfound_by_step.size() < 64 || g_unfound_by_step.count(place.name) != 0)) {
                g_unfound_by_step[place.name] += static_cast<int>(unfound);
            }
            if (g_uses.size() + found.size() + 2 > kMaxUses) {
                g_uses_full = true;
                return;
            }
            // (The step ran: twice in a frame and it is never a place to save or put back. Not all of its items read: not fully seen.)
            g_uses.push_back(ViewHistoryPlan::Use{ frame, place.order, place.view, pass, ViewHistoryPlan::kRan, 0, 0, 0, false, false, single });
            if ((have_io && io.total > io.count) || (!have_io && io.total > 0)) {
                g_uses.push_back(ViewHistoryPlan::Use{ frame, place.order, place.view, pass, ViewHistoryPlan::kBlind, 0, 0, 0, false, false, single });
            }
            for (const auto& f : found) {
                const auto key = reinterpret_cast<uintptr_t>(f.resource);
                if (g_descriptions.size() < 20000 && g_descriptions.find(key) == g_descriptions.end()) {
                    g_descriptions.emplace(key, Describe(f.desc));
                }
                // (Used on the compute queue too: the game does not keep that in step with a copy on the direct one. Left alone.)
                if (!direct && g_compute_resources.size() < 20000) {
                    g_compute_resources.insert(key);
                }
                // (Its state not known (KnownState): never a place to save or put back, unless it is a resting picture (the plan
                // then takes its resting state where it can). Attached by the step as it ran: never a place either.)
                ViewHistoryPlan::Use use{ frame, place.order, place.view, pass, f.item, f.item, key, f.state, direct, false,
                    single && (f.state != 0 || f.resting) && !f.attached };
                use.resting = f.resting;
                g_uses.push_back(use);
                g_attached += f.attached ? 1 : 0;
            }
        }

        // What a step the walk did not find uses: its textures' pictures, each with whether it writes it (its state a write, or not known;
        // on a list that is not a direct one, any use: the compute queue is not kept in step with a copy on the direct one); its textures
        // whose picture was not found; its list not readable at all, or with more items than are read.
        struct Uses
        {
            std::vector<std::pair<uintptr_t, bool>> pictures{};
            uint32_t                                not_found{ 0 };
            bool                                    no_list{ false };
            bool                                    too_many{ false };
        };

        Uses UsedBy(void* graph_data, void* pass_data, ListOf list_of)
        {
            Uses                 out{};
            RenderPassIo::PassIo io{};
            const bool           have_io = pass_data != nullptr && RenderPassIo::ReadPassIo(pass_data, io);
            out.no_list  = pass_data == nullptr || (!have_io && io.total > 0);
            out.too_many = have_io && io.total > io.count;
            bool direct  = true;
            for (uint32_t i = 0; have_io && i < io.count; ++i) {
                const auto& it = io.items[i];
                if (it.found == RenderPassIo::kFound) {
                    if (out.pictures.empty() && list_of != nullptr) {
                        auto* list = list_of(graph_data);
                        direct     = list != nullptr && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
                    }
                    const uint32_t state = RE::CreationRendererPrivate::RenderPassItem::getDXGIState(it.flags);
                    out.pictures.emplace_back(reinterpret_cast<uintptr_t>(it.resource),
                        !direct || !it.tracked || !ViewHistoryPlan::Known(state) || ViewHistoryPlan::Writes(state));
                } else if (it.found != RenderPassIo::kNotOne) {
                    ++out.not_found;
                }
            }
            return out;
        }

        std::string KindOf(void* pass)
        {
            std::string kind = RttiNames::Describe(pass);
            return kind.substr(0, kind.find(" < ")); // (its own type, not its bases)
        }

        // While learning: a step that ran and was not found by the walk (made during the frame, or in a graph not walked). Its place in
        // the frame is not known: none of the pictures it writes is kept apart (what it writes could be lost or put in the world's
        // history at the wrong moment); what it only reads it reads as before (the world's, or the menu's). Its kind noted for the log.
        void NoteUnknown(void* pass, void* graph_data, void* pass_data, ListOf list_of)
        {
            const uint64_t    run  = g_unknown_steps.fetch_add(1, std::memory_order_relaxed);
            const Uses        uses = UsedBy(graph_data, pass_data, list_of);
            const std::string kind = run < 4000 ? KindOf(pass) : std::string{};
            std::scoped_lock  _{ g_mtx };
            if (g_phase.load() != kLearning) {
                return;
            }
            g_unknown_no_list += uses.no_list ? 1 : 0;
            g_unknown_many += uses.too_many ? 1 : 0;
            g_unknown_not_found += uses.not_found;
            for (const auto& [r, written] : uses.pictures) {
                if (written && g_unknown_resources.insert(r).second && !kind.empty() && g_unknown_users.size() < 20000) {
                    g_unknown_users.emplace(r, kind);
                }
                // (Any use, a read too: its state there not known, so never taken as resting: the planner's resting rule.)
                if (g_unknown_touched.size() < 20000) {
                    g_unknown_touched.insert(r);
                }
            }
            if (!kind.empty() && (g_unknown_kinds.size() < 24 || g_unknown_kinds.count(kind) != 0)) {
                ++g_unknown_kinds[kind];
            }
        }

        // While keeping: a step the walk did not find (one that runs only now and then, unseen while learning; or with a plan used
        // again as a menu opens again, never learned with): if it writes one of the world's histories kept apart, that history is no
        // longer kept (what it holds then is not known), at once.
        void CheckUnknown(void* pass, void* graph_data, void* pass_data, ListOf list_of)
        {
            const Tables* tables = g_tables.load(std::memory_order_acquire);
            if (tables == nullptr || tables->resource_roles.empty()) {
                return;
            }
            const Uses uses = UsedBy(graph_data, pass_data, list_of);
            for (const auto& [r, written] : uses.pictures) {
                if (!written) {
                    continue;
                }
                if (const auto it = tables->resource_roles.find(r); it != tables->resource_roles.end()) {
                    const auto       d = tables->descriptions.find(r);
                    std::scoped_lock _{ g_mtx };
                    bool             newly = false;
                    for (const uint64_t role : it->second) {
                        newly |= g_roles_off.insert(role).second;
                    }
                    if (newly && g_unknown_off_logged++ < 8) {
                        spdlog::info("[History] one of the views' pictures is no longer kept apart: {} (@{:x}) is also written by a step the walk did "
                                     "not find, {}",
                            d != tables->descriptions.end() ? d->second : std::string{ "?" }, r & 0xffffff, KindOf(pass));
                    }
                }
            }
        }

        void Keep(void* pass, void* data, void* pass_data, ListOf list_of, uint32_t frame, uint32_t empty_before)
        {
            const Tables* tables = g_tables.load(std::memory_order_acquire);
            if (tables == nullptr) {
                return;
            }
            const auto& at = tables->at[frame & 1];
            const auto  it = at.find(pass);
            if (it == at.end()) {
                return;
            }
            auto* list = list_of != nullptr ? list_of(data) : nullptr;
            if (list == nullptr || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                ++g_compute;
                return;
            }
            RenderPassIo::PassIo io{};
            const bool           have_io = pass_data != nullptr && RenderPassIo::ReadPassIo(pass_data, io);
            const bool           mono    = g_mono.load();
            std::vector<uintptr_t> view_persistent{}; // (read once, for an item whose state the game does not track: KnownState)
            bool                   view_read = false;
            for (const int index : it->second) {
                const auto& a = tables->actions[index];
                {
                    std::scoped_lock _{ g_mtx };
                    if (g_roles_off.count(a.role) != 0) {
                        continue;
                    }
                }
                // (The step's item, as it is now: its picture, and the state the step has it in until the game's next change of it. Not
                // a picture the step attached itself as it ran this time: the game may have changed its state then.)
                ID3D12Resource* resource = nullptr;
                uint32_t        state    = 0;
                const bool      attached = a.item >= 0 && a.item < 32 && ((empty_before >> a.item) & 1u) != 0;
                const bool      listed   = have_io && a.item >= 0 && static_cast<uint32_t>(a.item) < io.count;
                if (listed && !io.items[a.item].tracked && !view_read) {
                    view_read       = true;
                    view_persistent = PersistentOf(data);
                }
                uint32_t known = listed ? KnownState(io.items[a.item], view_persistent) : 0u;
                // (A resting picture the plan took as in unordered access: so now too.)
                if (listed && known == 0 && a.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && Resting(io.items[a.item], view_persistent)) {
                    known = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
                if (listed && !attached && known != 0) {
                    resource = io.items[a.item].resource;
                    state    = known;
                }
                // (Only where it is as it was learned: the same picture, in the same state, once a frame.)
                const bool again = tables->done[index].exchange(frame + 1) == frame + 1;
                if (again || resource == nullptr || reinterpret_cast<uintptr_t>(resource) != a.resource || state != a.state) {
                    ++g_mismatches;
                    std::scoped_lock _{ g_mtx };
                    Strike(a.role, frame,
                        again ? "its step ran twice in a frame"
                              : (attached ? "its step attached its picture as it ran"
                                          : (listed && known == 0 ? "its state is not known now"
                                                                  : (resource == nullptr ? "its picture is not there now" : "another picture, or another state, now"))));
                    continue;
                }
                // (In stereo the world's histories are each eye's (its camera is the eye's); the menu's views' cameras are the same for
                // both eyes, so theirs are one for both, as in mono: half the copies' memory, and each reads its own last frame.)
                const bool per_eye = !mono && a.view == ViewHistoryPlan::kWorld;
                if (a.op == ViewHistoryPlan::kSave) {
                    Save(list, resource, state, per_eye ? static_cast<int>(frame & 1) : 0, a.role, frame);
                } else {
                    Load(list, resource, state, per_eye ? static_cast<int>((frame + a.ahead) & 1) : 0, a.role, frame);
                }
            }
        }

        template <class T>
        void Publish(std::array<std::shared_ptr<const T>, 4>& keep, int& next, std::atomic<const T*>& now, std::shared_ptr<const T> value)
        {
            keep[next] = value;
            next       = (next + 1) % static_cast<int>(keep.size());
            now.store(value.get(), std::memory_order_release);
        }

        // (Under the lock.) The copies let go (a few seconds on: the GPU may still be using them).
        void RetireSlots()
        {
            for (auto& [key, slot] : g_slots) {
                if (slot.copy != nullptr) {
                    g_retired.emplace_back(slot.copy, Clock::now(), slot.bytes, Presents());
                    g_any_retired = true;
                }
            }
            g_slots.clear();
        }

        // (Under the lock.) The copies of histories a plan now in use does not keep: let go (another plan's, as when the menu changed;
        // used again later, they start from the next save).
        void RetireSlotsNotIn(const std::set<uint64_t>& roles)
        {
            for (auto it = g_slots.begin(); it != g_slots.end();) {
                if (roles.count(std::get<1>(it->first)) == 0) {
                    if (it->second.copy != nullptr) {
                        g_retired.emplace_back(it->second.copy, Clock::now(), it->second.bytes, Presents());
                        g_any_retired = true;
                    }
                    it = g_slots.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // (Under the lock.) Copies let go long enough ago: released.
        void ReleaseRetired()
        {
            const auto now = Clock::now();
            std::erase_if(g_retired, [&](const auto& r) {
                // (1.5 s and 8 presents on: the CPU cannot be that many frames ahead of the GPU, even through long stalls; 10 s whatever.)
                const auto age = now - std::get<1>(r);
                if ((age > std::chrono::milliseconds(1500) && Presents() - std::get<3>(r) >= 8) || age > std::chrono::seconds(10)) {
                    g_bytes -= std::min(g_bytes, std::get<2>(r));
                    return true;
                }
                return false;
            });
            g_any_retired = !g_retired.empty();
            std::erase_if(g_abandoned, [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
            g_any_abandoned = !g_abandoned.empty();
        }

        void Abandon()
        {
            if (g_planning.valid()) {
                g_abandoned.push_back(std::move(g_planning));
                g_planning      = {};
                g_any_abandoned = true;
            }
        }

        void StartLearning(uint32_t frame, const std::shared_ptr<const Places>& places, const char* why)
        {
            Abandon();
            g_uses.clear();
            g_uses.reserve(64 * 1024);
            g_uses_full = false;
            g_descriptions.clear();
            g_learn_first     = frame;
            g_learn_last      = frame;
            g_learn_signature = places->signature;
            g_learn_places    = places;
            g_edges_incomplete = false;
            g_last_learn      = Clock::now();
            g_role_strikes.clear();
            g_roles_off.clear();
            g_unknown_steps = 0;
            g_unknown_kinds.clear();
            g_unknown_resources.clear();
            g_unknown_touched.clear();
            g_unknown_users.clear();
            g_compute_resources.clear();
            std::fill(std::begin(g_items), std::end(g_items), 0);
            g_untracked = 0;
            g_attached  = 0;
            for (auto& set : g_persistent) {
                set.clear();
            }
            g_unfound_by_step.clear();
            g_unknown_no_list    = 0;
            g_unknown_many       = 0;
            g_unknown_not_found  = 0;
            g_want_walk  = true; // (from a fresh list of the frame's steps: a changed one starts the learning again)
            Publish(g_tables_keep, g_tables_next, g_tables, std::shared_ptr<const Tables>{});
            g_phase.store(kLearning, std::memory_order_release);
            if (g_learn_logged++ < kLearnLogs) {
                spdlog::info("[History] watching the frame's steps for {} frames ({}): which of each view's pictures are its own last frame's",
                    kLearnFrames, why);
            }
            g_status = "World's own last-frame pictures behind menus: being found";
        }

        // (Under the lock.) Nothing kept for these views: tried again a few seconds on, or at once when the frame's views change.
        void GiveUp(const char* why)
        {
            Abandon();
            Publish(g_tables_keep, g_tables_next, g_tables, std::shared_ptr<const Tables>{});
            g_phase.store(kNothing, std::memory_order_release);
            g_uses.clear();
            g_uses.shrink_to_fit();
            const Places* places = g_places.load(std::memory_order_acquire);
            g_gave_up_at         = Clock::now();
            g_gave_up_signature  = places != nullptr ? places->signature : 0;
            ++g_session_give_ups;
            g_session_last_give_up = why;
            if (g_give_up_logs++ < 6) {
                spdlog::info("[History] not kept apart for now (tried again in a few seconds, or when the menu's views change): {}", why);
            }
            g_status = fmt::format("The views' own last-frame pictures behind menus: not kept ({})", why);
        }

        // (Under the lock.) A plan made before for these views (the same views, the same frame's own steps, every step it acts at still
        // there): null if none.
        std::shared_ptr<const Tables> FindPlan(const Places& places)
        {
            // (Without plans used again: only the last one, for a menu opened again with the same views, as before fix123.)
            for (auto it = g_plans.rbegin(); it != g_plans.rend() && (places.reuse_plans || it == g_plans.rbegin()); ++it) {
                const auto& t = *it;
                if (t->signature != places.signature || t->frame_signature != places.frame_signature) {
                    continue;
                }
                // (Each step it acts at still there, not new, and still the same kind of step in the same view: an object freed and made
                // again at the same place would be another step.)
                if (std::all_of(t->steps.begin(), t->steps.end(), [&](const auto& step) {
                        const auto& [pass, view, name] = step;
                        const auto s                    = places.steps.find(pass);
                        return s != places.steps.end() && !s->second.fresh && s->second.view == view && s->second.name == name;
                    })) {
                    return t;
                }
            }
            return nullptr;
        }

        void RememberPlan(const std::shared_ptr<const Tables>& t)
        {
            std::erase_if(g_plans, [&](const auto& p) { return p->signature == t->signature && p->frame_signature == t->frame_signature; });
            g_plans.push_back(t);
            if (g_plans.size() > 8) {
                g_plans.erase(g_plans.begin());
            }
        }

        void ForgetPlan(const Tables* t)
        {
            std::erase_if(g_plans, [&](const auto& p) { return p.get() == t; });
        }

        // (Under the lock.) Keeping with a plan made before (its histories' strikes forgotten: they were for the other plan's moments).
        void UsePlan(const std::shared_ptr<const Tables>& t, const char* why)
        {
            Abandon();
            Publish(g_tables_keep, g_tables_next, g_tables, t);
            g_role_strikes.clear();
            g_roles_off.clear();
            RetireSlotsNotIn(t->roles);
            g_uses.clear(); // (learning, if it was: stopped)
            g_uses.shrink_to_fit();
            g_phase.store(kKeeping, std::memory_order_release);
            g_keeping_since = Clock::now();
            g_status        = fmt::format("The views' own last-frame pictures behind menus: {} kept apart", t->kept);
            if (g_plan_used_logs++ < 12) {
                spdlog::info("[History] {} of the views' pictures kept apart again, with the plan made before for these views ({})", t->kept, why);
            }
        }

        void Stop(const char* why)
        {
            Abandon();
            const int phase = g_phase.exchange(kOff);
            // (This menu's summary: how much of it the views' histories were kept apart, and why not otherwise.)
            const uint32_t frames = g_session_frames[0] + g_session_frames[1] + g_session_frames[2] + g_session_frames[3] + g_session_frames[4];
            if (frames > 0 && g_session_logs++ < 60) {
                std::string menus{};
                for (const auto& m : g_session_menus) {
                    menus += fmt::format("{}{}", menus.empty() ? "" : "; ", m);
                }
                spdlog::info("[History] the world let go behind the menu ({}): {} frames; its histories kept apart in {} ({:.0f}%), learning {}, planning {}, "
                             "not kept (given up, or nothing to keep) {}, not started {}; given up {} times{}",
                    menus.empty() ? std::string{ "?" } : menus, frames, g_session_frames[kKeeping], 100.0 * g_session_frames[kKeeping] / frames,
                    g_session_frames[kLearning], g_session_frames[kPlanning], g_session_frames[kNothing], g_session_frames[kOff], g_session_give_ups,
                    g_session_give_ups > 0 ? fmt::format(" (the last: {})", g_session_last_give_up) : std::string{});
            }
            g_session_frames = {};
            g_session_menus.clear();
            g_session_give_ups     = 0;
            g_session_last_give_up = {};
            Publish(g_tables_keep, g_tables_next, g_tables, std::shared_ptr<const Tables>{});
            Publish(g_places_keep, g_places_next, g_places, std::shared_ptr<const Places>{});
            RetireSlots();
            g_uses.clear();
            g_uses.shrink_to_fit();
            g_descriptions.clear();
            g_role_strikes.clear();
            g_roles_off.clear();
            g_relearns = 0;
            g_restarts = 0;
            g_timed_retries = 0;
            g_view_retries = 0;
            g_learn_logged = 0;
            g_plans_logged = 0; // (each menu's plans and first restarts logged, whatever came before)
            g_logged_plans.clear();
            g_restarts_logged = 0;
            g_long_logs = 0;
            g_give_up_logs = 0;
            g_plan_used_logs = 0;
            g_learn_places.reset();
            for (int which = 0; which < 2; ++which) {
                Publish(g_edges_keep[which], g_edges_next[which], g_edges[which], std::shared_ptr<const StepMap>{});
                g_edges_now[which].reset();
                g_edges_new[which] = false;
            }
            if (phase == kKeeping) {
                spdlog::info("[History] stopped ({}): {} saved, {} put back so far", why, g_saves.load(), g_loads.load());
            }
        }

        void MakeTables(const ViewHistoryPlan::Result& planned, const Places& places)
        {
            // The world's histories also written by steps the walk did not find (their moments in the frame not known): left alone, with
            // the other picture of the same history (a history may alternate between two).
            // (And those used on the compute queue: the game does not keep that in step with a copy on the direct one.)
            ViewHistoryPlan::Result result = planned;
            const auto              off_why = [&](uintptr_t r) -> std::string {
                if (g_plan_unknown.count(r) != 0) {
                    const auto user = g_unknown_users.find(r);
                    return user != g_unknown_users.end() ? "written by a step the walk did not find, " + user->second
                                                         : std::string{ "written by a step the walk did not find" };
                }
                return g_plan_compute.count(r) != 0 ? std::string{ "used on the compute queue" } : std::string{};
            };
            std::set<uint64_t>  roles_off{};
            std::set<uintptr_t> resources_off{};
            for (const auto& a : result.actions) {
                if (!off_why(a.resource).empty()) {
                    roles_off.insert(a.role);
                }
            }
            for (const auto& a : result.actions) {
                if (roles_off.count(a.role) != 0) {
                    resources_off.insert(a.resource);
                }
            }
            for (const auto& k : result.kept) {
                if (!off_why(k.resource).empty()) {
                    resources_off.insert(k.resource);
                }
            }
            std::string left{};
            int         left_count = 0;
            std::erase_if(result.kept, [&](const ViewHistoryPlan::Kept& k) {
                if (resources_off.count(k.resource) == 0) {
                    return false;
                }
                ++left_count;
                const auto        d   = g_descriptions.find(k.resource);
                const std::string why = off_why(k.resource);
                left += fmt::format("{}{} (@{:x}: {})", left.empty() ? "" : "; ", d != g_descriptions.end() ? d->second : std::string{ "?" },
                    k.resource & 0xffffff, why.empty() ? std::string{ "the other picture of its history is" } : why);
                return true;
            });
            std::erase_if(result.actions, [&](const ViewHistoryPlan::Action& a) {
                return roles_off.count(a.role) != 0 || resources_off.count(a.resource) != 0;
            });
            if (left_count > 0) {
                result.skipped["also written where their moments are not known, or used on the compute queue"] += left_count;
                if (g_logged_plans.count(g_learn_signature) == 0 && g_logged_plans.size() < 8) {
                    spdlog::info("[History] the world's histories left alone (their moments not all known): {}", left);
                }
            }
            auto tables             = std::make_shared<Tables>();
            tables->signature       = g_learn_signature;
            tables->frame_signature = places.frame_signature;
            tables->kept      = result.kept.size();
            tables->actions   = result.actions;
            tables->done      = std::make_unique<std::atomic<uint32_t>[]>(std::max<size_t>(1, result.actions.size()));
            for (size_t i = 0; i < result.actions.size(); ++i) {
                const auto& a = result.actions[i];
                if (tables->at[0].count(a.pass) == 0 && tables->at[1].count(a.pass) == 0) {
                    const auto s = places.steps.find(a.pass);
                    tables->steps.emplace_back(a.pass, s != places.steps.end() ? s->second.view : uint8_t{ 0xff },
                        s != places.steps.end() ? s->second.name : std::string{});
                }
                tables->at[a.parity & 1][a.pass].push_back(static_cast<int>(i));
                tables->roles.insert(a.role);
                auto& roles = tables->resource_roles[a.resource];
                if (std::find(roles.begin(), roles.end(), a.role) == roles.end()) {
                    roles.push_back(a.role);
                }
                if (const auto d = g_descriptions.find(a.resource); d != g_descriptions.end()) {
                    tables->descriptions[a.resource] = d->second;
                }
            }
            // (At a step where one view's history is saved and another's put back, possibly in the same picture: the save first, so it
            // takes what that step's view left there.)
            for (auto& at : tables->at) {
                for (auto& [pass, list] : at) {
                    std::stable_sort(list.begin(), list.end(), [&](int x, int y) {
                        return tables->actions[x].op == ViewHistoryPlan::kSave && tables->actions[y].op != ViewHistoryPlan::kSave;
                    });
                }
            }
            // The log: what is kept apart, where; and the histories the plan left alone, each with why.
            const auto name = [&](void* pass) {
                const auto it = places.steps.find(pass);
                return it != places.steps.end() ? fmt::format("#{} '{}'", it->second.order, it->second.name) : std::string{ "?" };
            };
            const auto view_name = [](uint8_t view) {
                return view == ViewHistoryPlan::kWorld ? std::string{ "the world's" } : fmt::format("the menu's view {}'s", view);
            };
            std::string kept{};
            for (const auto& k : result.kept) {
                const auto d = g_descriptions.find(k.resource);
                kept += fmt::format("\n[History]   {} (@{:x}): read by {} {}; saved after {} [{}], put back after {} [{}]",
                    d != g_descriptions.end() ? d->second : std::string{ "?" }, k.resource & 0xffffff, view_name(k.view), name(k.first_read),
                    name(k.save_pass), k.save_item, name(k.load_pass), k.load_item);
            }
            std::string notes{};
            for (size_t i = 0; i < result.notes.size() && i < 24; ++i) {
                const auto& n = result.notes[i];
                const auto  d = g_descriptions.find(n.resource);
                notes += fmt::format("\n[History]   left alone: {} (@{:x}, {}): {}; it would be saved after {} (state {:#x}), put back after {} (state {:#x})",
                    d != g_descriptions.end() ? d->second : std::string{ "?" }, n.resource & 0xffffff, view_name(n.view), n.why, name(n.save_pass),
                    n.save_state, name(n.load_pass), n.load_state);
            }
            std::string skipped{};
            for (const auto& [why, n] : result.skipped) {
                skipped += fmt::format("{}{} {}", skipped.empty() ? "" : "; ", n, why);
            }
            // (The views' persistent pictures: the world's and the menu's views' the same ones or not, and how many the steps have among
            // their items.)
            size_t same = 0, listed = 0;
            for (const uintptr_t r : g_persistent[kWorldStep]) {
                same += g_persistent[kMenuStep].count(r);
            }
            for (const auto& set : g_persistent) {
                for (const uintptr_t r : set) {
                    listed += g_descriptions.count(r);
                }
            }
            if (g_logged_plans.size() < 8 && g_logged_plans.insert(g_learn_signature).second) {
                spdlog::info("[History] the frame's steps watched for {} frames ({} uses of {} pictures; items read: {} pictures found, {} textures with no "
                             "picture attached, {} not found, {} not textures; {} pictures whose state the game does not track, {} attached by their step "
                             "as it ran; {} runs of steps the walk did not find; steps: the world's {}, the menu's views' {}, the frame's own {}; the views' "
                             "persistent pictures: the world's {}, the menu's views' {}, the same ones {}, among the steps' items {}; picture lookups "
                             "indexed {} times so far, {} entries): {} of the world's pictures kept apart ({} copies a frame){}{}{}",
                    kLearnFrames, g_planned_uses, result.resources, g_items[RenderPassIo::kFound], g_items[RenderPassIo::kEmpty],
                    g_items[RenderPassIo::kNotFound], g_items[RenderPassIo::kNotOne], g_untracked, g_attached, g_unknown_steps.load(), places.counts[kWorldStep],
                    places.counts[kMenuStep], places.counts[kFrameStep], g_persistent[kWorldStep].size(), g_persistent[kMenuStep].size(), same, listed,
                    RenderPassIo::g_index_builds.load(), RenderPassIo::g_index_entries.load(), result.kept.size(), result.actions.size() / 2, kept,
                    skipped.empty() ? std::string{} : fmt::format("\n[History]   left alone: {}", skipped), notes);
            }
            g_descriptions.clear();
            if (result.actions.empty()) {
                // (For these views, nothing: not learned again for them, should they be seen again.)
                if (g_nothing.size() >= 16) {
                    g_nothing.clear();
                }
                g_nothing.insert({ tables->signature, tables->frame_signature });
                GiveUp(left_count > 0 ? "the views' histories found are all written where their moments are not known (see the log)"
                                      : "none of its pictures found to keep apart (see the log)");
                return;
            }
            RememberPlan(tables);
            RetireSlotsNotIn(tables->roles);
            g_keeping_since = Clock::now();
            Publish(g_tables_keep, g_tables_next, g_tables, std::shared_ptr<const Tables>{ tables });
            g_phase.store(kKeeping, std::memory_order_release);
            g_status = fmt::format("The views' own last-frame pictures behind menus: {} kept apart", result.kept.size());
        }

        // Learned again: at once (now), or not sooner than a few seconds after the last time; not again and again.
        void Relearn(uint32_t frame, const std::shared_ptr<const Places>& places, const char* why, bool now)
        {
            if (g_relearns >= kMaxRelearns) {
                GiveUp("its steps keep changing");
                return;
            }
            if (!now && Clock::now() - g_last_learn < std::chrono::seconds(3)) {
                return; // (tried at a later frame; meanwhile nothing kept: every history off)
            }
            ++g_relearns;
            StartLearning(frame, places, why);
        }

        // (Under the lock.) Learning starts over (its frames not one after another, or the views' steps changed): why, logged the first
        // few times; and not more than a few times while a menu is open.
        void Restart(uint32_t frame, const std::shared_ptr<const Places>& places)
        {
            const bool steps_changed = places->signature != g_learn_signature;
            if (g_restarts_logged < 4) {
                ++g_restarts_logged;
                if (steps_changed && g_learn_places != nullptr) {
                    // (Steps of each group found now and not then, and then and not now.)
                    int added[3]{}, gone[3]{};
                    for (const auto& [pass, place] : places->steps) {
                        if (g_learn_places->steps.find(pass) == g_learn_places->steps.end()) {
                            ++added[place.group % 3];
                        }
                    }
                    for (const auto& [pass, place] : g_learn_places->steps) {
                        if (places->steps.find(pass) == places->steps.end()) {
                            ++gone[place.group % 3];
                        }
                    }
                    spdlog::info("[History] learning starts over at frame {}: the frame's views changed (steps new / gone: the world's {} / {}, the "
                                 "menu's views' {} / {}, the frame's own {} / {}; steps now {}, then {})",
                        frame, added[kWorldStep], gone[kWorldStep], added[kMenuStep], gone[kMenuStep], added[kFrameStep], gone[kFrameStep],
                        places->steps.size(), g_learn_places->steps.size());
                } else {
                    spdlog::info("[History] learning starts over: the frames were not one after another (frame {} after {})", frame, g_learn_last);
                }
            }
            if (++g_restarts > kMaxRestarts) {
                GiveUp("learning started over too many times (see the log)");
                return;
            }
            StartLearning(frame, places, steps_changed ? "the frame's views changed" : "the frames were not one after another");
        }
    }

    void BeginFrame(uint32_t frame, bool mono, std::shared_ptr<const Places> places)
    {
        std::scoped_lock _{ g_mtx };
        ReleaseRetired();
        if (places == nullptr) {
            if (g_phase.load() != kOff) {
                Stop("no world kept drawn behind a menu");
            }
            return;
        }
        if (mono != g_mono.load()) {
            // (Mono and stereo keep different copies: one, or one for each eye.)
            g_mono.store(mono);
            RetireSlots();
        }
        Publish(g_places_keep, g_places_next, g_places, places);
        g_frame.store(frame, std::memory_order_release);
        const int phase = g_phase.load();
        // (This menu, for its summary as it closes: its frames by phase, and the menus shown.)
        ++g_session_frames[std::clamp(phase, 0, 4)];
        if (g_session_frames[0] + g_session_frames[1] + g_session_frames[2] + g_session_frames[3] + g_session_frames[4] == 1 ||
            (frame % 30) == 0) {
            if (g_session_menus.size() < 12) {
                g_session_menus.insert(GameFlow::shownMenus());
            }
        }
        if (phase == kNothing) {
            // (Given up for now: tried again a few seconds on, or at once for other views (another menu, a preview shown or gone),
            // with the plan made before for them if there is one. Without plans used again: given up while this menu is open.)
            // (Views known to have nothing to keep: not tried again. A few tries a few seconds on, their counts kept: learning again
            // and again in quick succession still gives up; for other views, their own counts, a dozen times while this menu is open.)
            const bool other_views = places->signature != g_gave_up_signature;
            const bool nothing     = g_nothing.count({ places->signature, places->frame_signature }) != 0;
            const bool timed       = !other_views && Clock::now() - g_gave_up_at > std::chrono::seconds(8) && g_timed_retries < 3;
            if (places->reuse_plans && !nothing && ((other_views && g_view_retries < 12) || timed)) {
                if (other_views) {
                    ++g_view_retries;
                    g_relearns = 0;
                    g_restarts = 0;
                } else {
                    ++g_timed_retries;
                }
                if (!places->complete) {
                    g_gave_up_at        = Clock::now();
                    g_gave_up_signature = places->signature;
                } else if (const auto plan = FindPlan(*places); plan != nullptr) {
                    UsePlan(plan, "the frame's views changed");
                } else {
                    StartLearning(frame, places, other_views ? "the frame's views changed" : "trying again");
                }
            } else if (other_views) {
                g_gave_up_signature = places->signature; // (other views, not tried: as given up for them)
                g_gave_up_at        = Clock::now();
            }
            return;
        }
        if (phase == kOff) {
            if (!places->complete) {
                GiveUp("the frame's steps could not all be found");
                return;
            }
            // (Views known to have nothing to keep: as given up for them, without learning again.)
            if (g_nothing.count({ places->signature, places->frame_signature }) != 0) {
                g_phase.store(kNothing, std::memory_order_release);
                g_gave_up_at        = Clock::now();
                g_gave_up_signature = places->signature;
                g_status            = "The views' own last-frame pictures behind menus: nothing to keep apart for this menu";
                return;
            }
            // (The same views as when a plan was made before, and every step it saves or puts back at still there: it is used at once.)
            if (const auto plan = FindPlan(*places); plan != nullptr) {
                UsePlan(plan, "a see-through menu opened again");
                return;
            }
            StartLearning(frame, places, "a see-through menu with the world behind it");
            return;
        }
        if (phase == kKeeping) {
            const Tables* tables = g_tables.load();
            // (The frame's views changed: the plan made before for them, if there is one, else learned again at once.)
            if (tables != nullptr && tables->signature != places->signature) {
                if (const auto plan = places->complete && places->reuse_plans ? FindPlan(*places) : nullptr; plan != nullptr) {
                    UsePlan(plan, "the frame's views changed");
                } else {
                    Relearn(frame, places, "the frame's steps changed", true);
                }
                return;
            }
            // (Every history it keeps no longer as learned: learned again. Only its own histories counted: strikes of the plan before,
            // from its last steps after the switch, are not this one's.)
            const auto off = tables == nullptr ? size_t{ 0 }
                                               : static_cast<size_t>(std::count_if(tables->roles.begin(), tables->roles.end(),
                                                     [](uint64_t role) { return g_roles_off.count(role) != 0; }));
            if (tables != nullptr && !tables->roles.empty() && off >= tables->roles.size()) {
                ForgetPlan(tables);
                Relearn(frame, places, "its steps are no longer as they were learned", false);
                return;
            }
            // (A plan kept a while: learning again, should the views change, starts its count again; only learning again and again in
            // quick succession gives up.)
            if (places->reuse_plans && Clock::now() - g_keeping_since > std::chrono::seconds(4)) {
                g_relearns = 0;
                g_restarts = 0;
            }
            // (Copies not used for a while, as after a change of the picture's size: let go.)
            if (frame - g_last_trim > 60) {
                g_last_trim = frame;
                for (auto it = g_slots.begin(); it != g_slots.end();) {
                    if (frame - it->second.used_frame > 240) {
                        g_retired.emplace_back(it->second.copy, Clock::now(), it->second.bytes, Presents());
                        g_any_retired = true;
                        it            = g_slots.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        if (phase == kLearning) {
            if (places->signature != g_learn_signature || frame != g_learn_last + 1) {
                // (Other views while learning, seen before: their plan at once.)
                if (places->signature != g_learn_signature && places->complete && places->reuse_plans) {
                    if (const auto plan = FindPlan(*places); plan != nullptr) {
                        UsePlan(plan, "the frame's views changed while learning");
                        return;
                    }
                }
                if (frame != g_learn_last) {
                    Restart(frame, places);
                }
                return;
            }
            g_learn_last = frame;
            // (The "Frame" graph's own steps found again each frame while learning: if they are new objects each frame, said once. Their
            // uses are still seen; a plan made then is not used again when a menu opens again.)
            if (g_learn_places != nullptr && places->frame_signature != g_learn_places->frame_signature && !g_frame_steps_logged) {
                g_frame_steps_logged = true;
                spdlog::info("[History] the \"Frame\" graph's own steps changed while learning (now {} steps in all, then {})", places->steps.size(),
                    g_learn_places->steps.size());
            }
            // (Which of the views' steps are new objects from one frame to the next: said once, by name.)
            if (!g_fresh_logged) {
                std::map<std::string, int> fresh{};
                int                        counts[3]{};
                for (const auto& [pass, place] : places->steps) {
                    if (place.fresh) {
                        ++fresh[place.name];
                        ++counts[place.group % 3];
                    }
                }
                if (!fresh.empty()) {
                    g_fresh_logged = true;
                    std::vector<std::pair<int, std::string>> by_count{};
                    for (const auto& [name, n] : fresh) {
                        by_count.emplace_back(n, name);
                    }
                    std::sort(by_count.rbegin(), by_count.rend());
                    std::string names{};
                    for (size_t i = 0; i < by_count.size() && i < 16; ++i) {
                        names += fmt::format("{}{} x{}", names.empty() ? "" : ", ", by_count[i].second, by_count[i].first);
                    }
                    spdlog::info("[History] steps that are new objects since the frame before (never a place to save or put back): the world's {}, the "
                                 "menu's views' {}, the frame's own {}; by name: {}",
                        counts[kWorldStep], counts[kMenuStep], counts[kFrameStep], names);
                }
            }
            if (frame - g_learn_first >= kLearnFrames) {
                if (g_uses_full) {
                    GiveUp("too many uses to watch");
                    return;
                }
                if (!places->complete || g_edges_incomplete) {
                    GiveUp("the frame's steps could not all be found");
                    return;
                }
                // (Steps ran that the walk did not find: they cannot be placed in the frame's order. The pictures they wrote were read as
                // they ran: none of the world's histories among them is kept apart (MakeTables); what they only read, or never touch, does
                // not matter. Logged with their kinds; and the walked steps whose textures were not all found, by name.)
                g_plan_unknown = g_unknown_resources;
                g_plan_compute = g_compute_resources;
                // (A resting picture a step the walk did not find used, even only reading it: its state there not known, so not taken as
                // resting.)
                int resting = 0, unrested = 0;
                for (auto& u : g_uses) {
                    if (u.resting) {
                        if (g_unknown_touched.count(u.resource) != 0) {
                            u.resting = false;
                            u.single  = u.single && u.state != 0;
                            ++unrested;
                        } else {
                            ++resting;
                        }
                    }
                }
                if (g_long_logs < 4) {
                    spdlog::info("[History] {} uses of the views' resting pictures (untracked, in unordered access while nothing tracked uses them); {} "
                                 "not taken so (a step the walk did not find used the picture)",
                        resting, unrested);
                }
                const bool long_log = g_long_logs++ < 4;
                if (const uint64_t unknown = g_unknown_steps.load(); unknown > 0 && long_log) {
                    std::string kinds{};
                    for (const auto& [kind, n] : g_unknown_kinds) {
                        kinds += fmt::format("{}{} x{}", kinds.empty() ? "" : ", ", kind, n);
                    }
                    spdlog::info("[History] {} runs of steps the walk did not find while watching: they wrote {} pictures (none of the views' "
                                 "histories among those is kept apart); their textures not found {}; runs whose list could not be read {}, with more "
                                 "items than are read {}. Their kinds: {}",
                        unknown, g_unknown_resources.size(), g_unknown_not_found, g_unknown_no_list, g_unknown_many, kinds);
                }
                if (!g_unfound_by_step.empty() && long_log) {
                    std::vector<std::pair<int, std::string>> by_count{};
                    for (const auto& [step, n] : g_unfound_by_step) {
                        by_count.emplace_back(n, step);
                    }
                    std::sort(by_count.rbegin(), by_count.rend());
                    std::string steps{};
                    for (size_t i = 0; i < by_count.size() && i < 16; ++i) {
                        steps += fmt::format("{}{} x{}", steps.empty() ? "" : ", ", by_count[i].second, by_count[i].first);
                    }
                    spdlog::info("[History] the frame's steps whose textures were not all found (uses not seen; not a reason to leave a history "
                                 "alone): {}", steps);
                }
                // (Steps whose uses are not seen: marked in each frame watched. The frame's own graphs' steps too, as last walked.)
                const auto mark_blind = [&](const StepMap& steps) {
                    for (const auto& [pass, place] : steps) {
                        if (place.blind) {
                            for (uint32_t f = g_learn_first; f < frame; ++f) {
                                g_uses.push_back(ViewHistoryPlan::Use{ f, place.order, place.view, pass, ViewHistoryPlan::kBlind, 0, 0, 0, false, false, false });
                            }
                        }
                    }
                };
                mark_blind(places->steps);
                for (const auto& edge : g_edges_now) {
                    if (edge != nullptr) {
                        mark_blind(*edge);
                    }
                }
                // (The plan made on another thread: no pause in the frame.)
                g_phase.store(kPlanning, std::memory_order_release);
                g_plan_places  = places;
                g_planned_uses = g_uses.size();
                try {
                    g_planning = std::async(std::launch::async, [uses = std::move(g_uses), every = places->every_view, planned = places->planned]() {
                        return ViewHistoryPlan::Plan(uses, every, planned);
                    });
                } catch (...) {
                    g_uses = {};
                    GiveUp("the plan could not be made");
                    return;
                }
                g_uses = {};
            }
            return;
        }
        if (phase == kPlanning) {
            if (g_planning.valid() && g_planning.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                ViewHistoryPlan::Result result{};
                bool                    made = true;
                try {
                    result = g_planning.get();
                } catch (...) {
                    made = false;
                }
                g_planning = {};
                if (!made) {
                    GiveUp("the plan could not be made");
                } else if (g_plan_places != nullptr && g_plan_places->signature == places->signature) {
                    MakeTables(result, *g_plan_places);
                } else if (const auto plan = places->complete && places->reuse_plans ? FindPlan(*places) : nullptr; plan != nullptr) {
                    UsePlan(plan, "the frame's views changed while planning");
                } else {
                    Restart(frame, places);
                }
                g_plan_places.reset();
            }
            return;
        }
        const auto now = Clock::now();
        if (phase == kKeeping && now - g_last_log > std::chrono::seconds(10)) {
            g_last_log = now;
            const uint64_t v[5]{ g_saves.load(), g_loads.load(), g_not_saved.load(), g_mismatches.load(), g_compute.load() };
            spdlog::info("[History] last 10 s: {} saved, {} put back, {} not put back (no copy yet), {} not done (not as learned), {} on compute "
                         "lists; copies {} MB; picture lookups indexed {} times so far ({} entries)",
                v[0] - g_logged[0], v[1] - g_logged[1], v[2] - g_logged[2], v[3] - g_logged[3], v[4] - g_logged[4], g_bytes >> 20,
                RenderPassIo::g_index_builds.load(), RenderPassIo::g_index_entries.load());
            std::copy(std::begin(v), std::end(v), std::begin(g_logged));
        }
    }

    void Idle()
    {
        if (!g_any_retired.load(std::memory_order_relaxed) && !g_any_abandoned.load(std::memory_order_relaxed) &&
            g_phase.load(std::memory_order_relaxed) == kOff) {
            return;
        }
        std::scoped_lock _{ g_mtx };
        if (g_phase.load() != kOff) {
            Stop("no world kept drawn behind a menu");
        }
        ReleaseRetired();
    }

    bool TakeWalkRequest()
    {
        return g_want_walk.exchange(false);
    }

    bool Learning()
    {
        const int phase = g_phase.load(std::memory_order_acquire);
        return phase == kLearning;
    }

    void EdgeSteps(int which, std::unordered_map<void*, StepPlace> steps, bool complete)
    {
        if (which < 0 || which > 1) {
            return;
        }
        std::scoped_lock _{ g_mtx };
        if (!Learning()) {
            return;
        }
        g_edges_incomplete |= !complete;
        // (Never a place to save or put back: a plan used again when a menu opens again would not find them, if they are new.)
        for (auto& [pass, place] : steps) {
            place.shared = true;
        }
        // (The same steps as last time: kept as they are.)
        const auto& now = g_edges_now[which];
        if (now != nullptr && now->size() == steps.size() &&
            std::all_of(steps.begin(), steps.end(), [&](const auto& s) { return now->find(s.first) != now->end(); })) {
            return;
        }
        if (now != nullptr && !g_edges_new[which]) {
            g_edges_new[which] = true;
            if (g_edges_logged++ < 4) {
                const size_t fresh = static_cast<size_t>(
                    std::count_if(steps.begin(), steps.end(), [&](const auto& s) { return now->find(s.first) == now->end(); }));
                spdlog::info("[History] the frame's own '{}' graph has new steps from one frame to the next ({} of {} new): found again each time it runs",
                    which == 0 ? "CREndFrame" : "CRBeginFrame", fresh, steps.size());
            }
        }
        auto map            = std::make_shared<const StepMap>(std::move(steps));
        g_edges_now[which]  = map;
        Publish(g_edges_keep[which], g_edges_next[which], g_edges[which], map);
    }

    bool Keeping()
    {
        return g_phase.load(std::memory_order_acquire) == kKeeping;
    }

    bool Engaged()
    {
        const int phase = g_phase.load(std::memory_order_acquire);
        return phase == kLearning || phase == kPlanning || phase == kKeeping;
    }

    bool Busy()
    {
        const int phase = g_phase.load(std::memory_order_acquire);
        return phase == kLearning || phase == kKeeping;
    }

    uint32_t EmptyBefore(void* pass, void* pass_data)
    {
        const int phase = g_phase.load(std::memory_order_acquire);
        if (phase == kKeeping) {
            // (Only a step with something to do in this frame.)
            const Tables* tables = g_tables.load(std::memory_order_acquire);
            if (tables == nullptr) {
                return 0;
            }
            // (Either parity: the frame number is read again after the step runs.)
            if (tables->at[0].find(pass) == tables->at[0].end() && tables->at[1].find(pass) == tables->at[1].end()) {
                return 0;
            }
        } else if (phase != kLearning) {
            return 0;
        }
        RenderPassIo::PassIo io{};
        if (pass_data == nullptr || !RenderPassIo::ReadPassIo(pass_data, io)) {
            return 0;
        }
        uint32_t empty = 0;
        for (uint32_t i = 0; i < io.count && i < 32; ++i) {
            if (io.items[i].kind == RenderPassIo::kTexture && io.items[i].found != RenderPassIo::kFound) {
                empty |= 1u << i;
            }
        }
        return empty;
    }

    void AfterStep(void* pass, void* graph_data, void* pass_data, bool taa, ListOf list_of, uint32_t empty_before)
    {
        const int phase = g_phase.load(std::memory_order_acquire);
        if (phase != kLearning && phase != kKeeping) {
            return;
        }
        const Places* places = g_places.load(std::memory_order_acquire);
        if (places == nullptr) {
            return;
        }
        const StepPlace* place = nullptr;
        if (const auto it = places->steps.find(pass); it != places->steps.end()) {
            place = &it->second;
        } else {
            // (One of the frame's own graphs' steps, around it.)
            for (const auto& edge : g_edges) {
                const StepMap* steps = edge.load(std::memory_order_acquire);
                if (steps == nullptr) {
                    continue;
                }
                if (const auto at = steps->find(pass); at != steps->end()) {
                    place = &at->second;
                    break;
                }
            }
        }
        if (place == nullptr) {
            // (While learning: a step that ran and was not found by the walk; its place in the frame not known. The pictures it writes
            // are noted: none of the world's histories among them is kept apart.)
            if (phase == kLearning) {
                NoteUnknown(pass, graph_data, pass_data, list_of);
            } else {
                CheckUnknown(pass, graph_data, pass_data, list_of);
            }
            return;
        }
        const uint32_t frame = g_frame.load(std::memory_order_acquire);
        if (phase == kLearning) {
            Learn(pass, *place, graph_data, pass_data, taa, list_of, frame, empty_before);
        } else {
            Keep(pass, graph_data, pass_data, list_of, frame, empty_before);
        }
    }

    std::string Status()
    {
        std::scoped_lock _{ g_mtx };
        return fmt::format("{} [saved {}, put back {}]", g_status, g_saves.load(), g_loads.load());
    }
}
