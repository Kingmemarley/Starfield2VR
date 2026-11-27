//
// Created by sergp on 6/26/2024.
//

#include "FullFrame.h"
#include "GpuTimer.h"
#include "LiveWorld.h"
#include "MenuCamera.h"
#include "Physical.h"
#include "RenderPassIo.h"
#include "RenderSteps.h"
#include "StereoView.h"
#include "ViewHistory.h"
#include "UiProbe.h"
#include "CreationEngineRendererModule.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include "NodeTreeDump.h"
#include "MotionWeapon.h"
#include "REL/Relocation.h"
#include "vr/TrackingSpace.h"
#include <CreationEngine/memory/offsets.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <_deps/directxtk12-src/Src/d3dx12.h>
#include <mods/VR.hpp>
#include <safetyhook/easy.hpp>

#include "ModSettings.h"

namespace
{
    // The per-eye history copies replaced when the game's picture changed size, kept until the graphics card is done with them.
    std::mutex                                                                           g_retired_mtx{};
    std::vector<std::pair<ComPtr<ID3D12Resource>, std::chrono::steady_clock::time_point>> g_retired_buffers{};
    // Each kind of per-eye copy (m_pastBuffer's index): when its picture last changed size, a pause after changes in quick succession,
    // and how many more frames its new copies are only filled (not yet put back).
    constexpr int kPerEyeKinds = 12;
    struct PerEyeKind
    {
        std::chrono::steady_clock::time_point changed_at{};
        std::chrono::steady_clock::time_point paused_until{};
        int                                   fresh{0};
        int                                   pause_logs{0};
    };
    PerEyeKind g_per_eye[kPerEyeKinds]{};

    // The game's TAA history (and its previous camera, so motion vectors) kept per eye: the "Nvidia DLSS and TAA Fix", in
    // stereo only (mono shows one view every frame, which the game's own history already follows; full stereo draws each eye with
    // a camera of its own, whose previous camera and history the game keeps itself). For the frame being rendered.
    bool PerEyeHistory()
    {
        static auto vr = VR::get();
        const uint32_t frame = vr->m_render_frame_count.load();
        // (AER draws an eye a frame as stereo does: its history kept per eye too.)
        return GameFlow::gStore.internalSettings.nvidiaAndTAAfix && !vr->is_mono_frame(frame) && !vr->is_native_frame(frame);
    }

    // (fix173) The menu's box when no menu movie gives it (GameFlow::menuBoxRows 0) and a menu is shown: the game's own 16:9 box, if it is
    // on and the frame is drawn in it (not kept full size behind the menu, FullFrame). A menu whose box was not known (the slate after its
    // first frames: only its 1024x1024 picture drawn) showed the whole 1996x2150 picture, the game's black bars above and below the box
    // with it (the inspected music box). As the game works it out: the picture's width, ceil(width / its aspect) high, centred; the panel
    // shows its rows centred (the same rows: 513 to 1636 of 2150).
    int GameBoxRows(int width, int height)
    {
        int box_w = 0, box_h = 0;
        // (Not when a menu's movie covers the whole picture: that menu fills it, whatever the box flag says; the review.)
        if (width <= 0 || height <= 0 || FullFrame::Active() || GameFlow::menuMovieFillsPicture(width, height) || !FullFrame::GameBox(box_w, box_h)) {
            return 0;
        }
        if (std::abs(box_w - width) <= 2 && box_h > 0 && box_h < height - 2) {
            return box_h;
        }
        // (A box as wide as the picture but its size not read (or not yet worked out): the game's 16:9. A box narrower than the picture (a
        // picture wider than 16:9): its whole height.)
        if (box_w <= 0 || box_h <= 0) {
            const int rows = static_cast<int>(std::lround(width * 9.0 / 16.0));
            return rows < height - 2 ? rows : 0;
        }
        return 0;
    }

    // (fix192) The panel up in play with no menu movie set up (none of the menus' lists shown) and no see-through menu: it then shows the
    // game's whole picture, the whole height, opaque (about 1.9 m tall: it fills most of the view). Seen in fix191's test right after a
    // load was chosen in the pause menu (10:15:30 and 10:17:24, 0.8 s each, until the loading screen). What keeps the panel up then is not
    // known (the game's mouse cursor alone, most likely): only logged for now, at its start (with the movies drawn in the frame before)
    // and its end. Once a frame, at the frame's start; 40 starts a session at most.
    void WatchTallPanel(const std::vector<std::string>& drawn_before)
    {
        static bool    in_run = false;
        static bool    told   = false; // (this run's start logged)
        static int     logs   = 0;
        static int64_t frames = 0;
        static std::chrono::steady_clock::time_point since{};
        const auto& s    = ModSettings::g_internalSettings;
        const bool  tall = s.showQuadDisplay && s.menuWorldBehind && !s.menuSeeThrough && !s.menuLoadingScreen && s.menuBoxRows == 0 &&
                          GameFlow::shownMenus() == "none";
        if (tall) {
            ++frames;
            if (!in_run) {
                in_run = true;
                told   = false;
                frames = 1;
                since  = std::chrono::steady_clock::now();
            }
            // (Its start logged once the movies of a frame in it are known: the list is kept only after a frame in this state, so from
            // its second frame.)
            if (!told && (!drawn_before.empty() || frames >= 2)) {
                told = true;
                if (logs < 40) {
                    std::string movies{};
                    for (const auto& m : drawn_before) {
                        std::string name = m;
                        if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) {
                            name = name.substr(slash + 1);
                        }
                        movies += (movies.empty() ? "" : ", ") + name;
                    }
                    int box_w = 0, box_h = 0;
                    const bool box_on = FullFrame::GameBox(box_w, box_h);
                    spdlog::info("[VR] (fix192) Menu panel: up in play with no menu movie set up and no see-through menu: it shows the game's whole "
                                 "picture, the whole height, opaque; the movies drawn in the frame before: {}; the player there: {}; the frame kept "
                                 "full size behind a menu (FullFrame): {}; the game's own 16:9 box: {}",
                        movies.empty() ? std::string{ "none" } : movies, CreationEngineSingletonManager::GetPlayerRef() != nullptr ? "yes" : "no",
                        FullFrame::Active() ? "yes" : "no", box_on ? std::format("on ({}x{})", box_w, box_h) : std::string{ "off" });
                }
            }
        } else if (in_run) {
            in_run = false;
            if (told && logs++ < 40) {
                spdlog::info("[VR] (fix192) Menu panel: that state ended after {} frames ({} ms){}", frames,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count(),
                    s.showQuadDisplay ? std::format(" (the menus shown now: {}{})", GameFlow::shownMenus(), s.menuLoadingScreen ? ", a loading screen" : "")
                                      : std::string{ " (the panel down)" });
            }
        }
    }

    // (fix203) Character creation on the panel: how it is shown, from its 30th frame (the box and the menus settled), and again at each change
    // while it stays open (a message box over it must change nothing: before, it made the panel see-through with the world drawn from your
    // head). Once a frame, at the frame's start; 40 lines a session at most.
    void WatchChargenPanel()
    {
        static bool        open   = false;
        static int         frames = 0;
        static int         logs   = 0;
        static std::string last{};
        const auto&        s = ModSettings::g_internalSettings;
        if (!s.showQuadDisplay || !GameFlow::isChargenShown()) {
            open   = false;
            frames = 0;
            last.clear();
            return;
        }
        if (!open) {
            open   = true;
            frames = 0;
        }
        if (++frames < 30 || logs >= 40) {
            return;
        }
        const auto        backbuffer = VR::get()->get_backbuffer_size();
        const std::string state =
            std::format("see-through {}, the world kept drawn behind it {}, the frame kept full size {}, its box {} of {} rows, round the panel {}",
                s.menuSeeThrough ? "yes" : "no", LiveWorld::Active() ? "yes" : "no", FullFrame::Active() ? "yes" : "no", s.menuBoxRows,
                (int)backbuffer[1], s.menuGlowWhenEmpty ? "the soft light" : s.menuWorldBehind ? "the world as it was" : "no world");
        if (state != last) {
            ++logs;
            spdlog::info("[Chargen] (fix203) character creation on the panel{}: {} (the menus shown: {})", last.empty() ? "" : " (changed)", state,
                GameFlow::shownMenus());
            last = state;
        }
    }

    // (fix173) Sudden drops of the frame rate (it dropped hard, then came back, while playing in one area): each second's frame
    // rate against the 10 s before it; a second under 60% of those logged with what may explain it: its frame times, the video memory, the
    // eyes' mode, a menu or loading screen, the physics update's time. At most a line each 10 s, 60 a session; nothing else done. (At the
    // frame's start, CRBeginFrame: once a frame.)
    void WatchFrameRate()
    {
        using clock = std::chrono::steady_clock;
        struct Watch
        {
            clock::time_point    last{};
            clock::time_point    window_start{};
            std::vector<float>   times{};
            std::array<float, 10> fps{};
            int                  filled{ 0 };
            int                  next{ 0 };
            clock::time_point    last_log{};
            int                  logs{ 0 };
            bool                 loading{ false }; // (a loading screen or no player in the second: not a drop to explain)
        };
        static std::mutex mtx{};
        static Watch      w{};
        std::scoped_lock  _{ mtx };
        const auto        now = clock::now();
        if (w.last == clock::time_point{}) {
            w.last = w.window_start = now;
            return;
        }
        const float ms = std::chrono::duration<float, std::milli>(now - w.last).count();
        w.last         = now;
        // (A gap of 2 s or more: a load, the game paused in the background: started over.)
        if (ms >= 2000.0f) {
            w.window_start = now;
            w.times.clear();
            w.filled = 0;
            w.next   = 0;
            w.loading = false;
            return;
        }
        if (w.times.size() < 1024) {
            w.times.push_back(ms);
        }
        w.loading = w.loading || ModSettings::g_internalSettings.menuLoadingScreen || GameFlow::isLoadingScreenShown() ||
                    !GameFlow::worldBehindMenu();
        const float seconds = std::chrono::duration<float>(now - w.window_start).count();
        if (seconds < 1.0f) {
            return;
        }
        const float fps = static_cast<float>(w.times.size()) / seconds;
        float before = 0.0f;
        if (w.filled >= 10) {
            for (const float f : w.fps) {
                before += f;
            }
            before /= 10.0f;
        }
        if (w.filled >= 10 && before >= 10.0f && fps < 0.6f * before && !w.loading && w.logs < 60 &&
            (w.last_log == clock::time_point{} || now - w.last_log >= std::chrono::seconds(10))) {
            w.last_log = now;
            ++w.logs;
            std::vector<float> sorted = w.times;
            std::sort(sorted.begin(), sorted.end(), std::greater<float>());
            float sum = 0.0f;
            int   over_50 = 0;
            for (const float t : w.times) {
                sum += t;
                over_50 += t > 50.0f ? 1 : 0;
            }
            std::string longest{};
            for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
                longest += std::format("{}{:.0f}", i == 0 ? "" : ", ", sorted[i]);
            }
            auto&          vr       = VR::get();
            const uint32_t used     = vr->m_d3d12.m_mem_used_mb.load();
            const uint32_t budget   = vr->m_d3d12.m_mem_budget_mb.load();
            const int64_t  over_at  = vr->m_d3d12.m_mem_over_since_ms.load();
            const int64_t  now_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            const char*    eyes     = vr->is_native() ? "full stereo" : vr->is_aer() ? "AER" : vr->is_mono() ? "mono" : "stereo";
            int            calls    = 0;
            float          physics  = 0.0f, physics_max = 0.0f;
            const bool     timed    = Physical::TakePhysicsTimes(calls, physics, physics_max);
            const auto&    settings = ModSettings::g_internalSettings;
            spdlog::info("[FPS] (fix173) the frame rate dropped: {:.1f} frames a second in the last {:.1f} s, against {:.1f} in the 10 s before; its "
                         "frames {:.1f} ms on average, {} over 50 ms, the longest {} ms; video memory {} of {} MB{}; the eyes {}; {}; physics update {}",
                fps, seconds, before, w.times.empty() ? 0.0f : sum / static_cast<float>(w.times.size()), over_50, longest, used, budget,
                over_at != 0 ? std::format(" (OVER its budget for {:.1f} s)", (now_ms - over_at) / 1000.0) : std::string{}, eyes,
                settings.showQuadDisplay ? std::format("a menu shown ({}; {}{})", GameFlow::shownMenus(), settings.menuSeeThrough ? "see-through" : "opaque",
                                               LiveWorld::Active() ? ", the world kept drawn behind it" : "")
                : GameFlow::isInDialogue() ? std::string{ "in a conversation" } : std::string{ "no menu" },
                timed ? std::format("{} runs, {:.2f} ms on average, the longest {:.2f} ms", calls, physics, physics_max) : std::string{ "not timed" });
        } else {
            int   calls = 0;
            float a = 0.0f, b = 0.0f;
            Physical::TakePhysicsTimes(calls, a, b); // (the next second's own)
        }
        w.fps[w.next] = fps;
        w.next        = (w.next + 1) % 10;
        w.filled      = std::min(w.filled + 1, 10);
        w.window_start = now;
        w.times.clear();
        w.loading = false;
    }
}

uintptr_t onUpdateConstantBufferViewDetour(uint8_t copyPresentToPast, uint8_t orphoProjection, uintptr_t pCameraTransforms, uintptr_t vararg1Parent, uintptr_t vararg3Parent,
                                           double unk, char unk2, RE::RenderPassConstantBufferView* camerablocks)
{
    return CreationEngineRendererModule::Get()->onUpdateConstantBufferView(copyPresentToPast, orphoProjection, pCameraTransforms, vararg1Parent, vararg3Parent, unk, unk2,
                                                                           camerablocks);
}

__int64 onRenderGraphRenderStartDetour(RE::CreationRendererPrivate::RenderGraph* rcx, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData, __int64 r8, __int64 r9)
{
    return CreationEngineRendererModule::Get()->onRenderGraphRenderStart(rcx, pRenderGraphData, r8, r9);
}

//__int64 onRenderFrameStartDetour(void* rcx, __int64 rdx, __int64 r8, __int64 r9)
//{
//    return CreationEngineRendererModule::Get()->onRenderFrameStart(rcx, rdx, r8, r9);
//}

std::unique_ptr<FunctionHook> m_onWindowMessageHook;

LRESULT CALLBACK WndProcDetour(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    using func_t              = decltype(WndProcDetour);
    static auto original_func = m_onWindowMessageHook->get_original<func_t>();
    //    spdlog::info("Window message hit {}", message);
    return original_func(hWnd, message, wParam, lParam);
}

void CreationEngineRendererModule::InstallHooks()
{
//    // 0x25dafe4 - this one works but sometimes give 2 ticks
//    REL::Relocation<uintptr_t> onWorldTickFn{ (uintptr_t )mod + 0x25e267c };
//    m_worldTick_hook = safetyhook::create_inline((void*)onWorldTickFn.address(), (void*)worldTick);
////        std::make_unique<FunctionHook>(onWorldTickFn.address(), reinterpret_cast<uint64_t>(&worldTick));
//    if(!m_worldTick_hook) {
//        spdlog::error("Failed to create hook for onWorldTick");
//    }

    REL::Relocation<uintptr_t> setReflexMarkerInternalFn{ GameStore::MemoryOffsets::Nvidia::onSetReflexMarkerInternal() };
    m_setReflexMarkerInternalHook = safetyhook::create_inline((void*)setReflexMarkerInternalFn.address(), (void*)setReflexMarkerInternal);
    if(!m_setReflexMarkerInternalHook) {
        spdlog::error("Failed to create hook for setReflexMarkerInternal");
    }

    //    REL::Relocation<RE::CreationEngineSettings**> settings{ REL::ID(878340) };
    REL::Relocation<RE::CreationEngineSettings**> settings{ GameStore::MemoryOffsets::GlobalRenderSettings() };
    m_creationEngineSettings = settings.get();

    REL::Relocation<uintptr_t> onUpdateConstantBufferViewAddr{ GameStore::MemoryOffsets::CreationRenderer::OnUpdateConstantBufferView() };
    m_onUpdateConstantBufferViewHook = std::make_unique<FunctionHook>(onUpdateConstantBufferViewAddr.address(), reinterpret_cast<uint64_t>(&onUpdateConstantBufferViewDetour));
    m_onUpdateConstantBufferViewHook->create();

    //    REL::Relocation<uintptr_t> onRenderGraphRenderStartFuncAddr{ REL::ID(1079045) };
    REL::Relocation<uintptr_t> onRenderGraphRenderStartFuncAddr{ GameStore::MemoryOffsets::CreationRenderer::RenderGraphFrameStart() };
    m_onRenderGraphRenderStartHook = std::make_unique<FunctionHook>(onRenderGraphRenderStartFuncAddr.address(), reinterpret_cast<uint64_t>(&onRenderGraphRenderStartDetour));
    m_onRenderGraphRenderStartHook->create();

    // ID is exactly incrementing frames 149000
    // 202136 ID is function on start is frame start on end is frame end, however it is significantly decreases fps
    //    REL::Relocation<uintptr_t> onRenderFrameStartFuncAddr{ REL::ID(202136) };
//    REL::Relocation<uintptr_t> onRenderFrameStartFuncAddr{ GameStore::MemoryOffsets::CreationRenderer::RenderGraphRenderPipelineExecute() };
//    m_onRenderFrameStartHook = std::make_unique<FunctionHook>(onRenderFrameStartFuncAddr.address(), reinterpret_cast<uint64_t>(&onRenderFrameStartDetour));
//    m_onRenderFrameStartHook->create();

    REL::Relocation<uintptr_t> taa_vfunc7_hook_addr{ GameStore::MemoryOffsets::CreationRenderer::OnTaaVFunc7() };
    taa_vfunc7_hook = std::make_unique<FunctionHook>(taa_vfunc7_hook_addr.address(), reinterpret_cast<uint64_t>(&CreationEngineRendererModule::onTaaPass));
    taa_vfunc7_hook->create();
}

struct CameraBlockSnapshot
{
    Matrix4x4f cameraMatrix;
    Vector2f   cameraPositionOrJitter;
    int        frameNumber;
};

std::unordered_map<uintptr_t, CameraBlockSnapshot> pastProjections{};
// (fix189, the review) The game fills its views' constant records on several of its job threads at once (onUpdateConstantBufferView): the
// map of each view's previous camera is read and written there only under this lock (a new view added while another thread looked one up
// could move the map's entries under it).
std::mutex pastProjectionsMutex{};

__int64 CreationEngineRendererModule::onRenderGraphRenderStart(RE::CreationRendererPrivate::RenderGraph* pGraph, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData,
                                                               __int64 i1, __int64 i2)
{
    using func_t              = decltype(onRenderGraphRenderStartDetour);
    static auto original_func = m_onRenderGraphRenderStartHook->get_original<func_t>();
    RenderGraphStart(pGraph, pRenderGraphData, true);
    auto result = original_func(pGraph, pRenderGraphData, i1, i2);
    RenderGraphStart(pGraph, pRenderGraphData, false);
    return result;
}

bool CreationEngineRendererModule::ValidateResource(ID3D12Resource* source, ComPtr<ID3D12Resource> pastBuffer[4], int index)
{
    if (source == nullptr || index < 0 || index >= kPerEyeKinds) {
        return false;
    }
    auto&      kind = g_per_eye[index];
    const auto now  = std::chrono::steady_clock::now();
    if (now < kind.paused_until) {
        return false;
    }
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    {
        // Copies replaced a while ago: the frames that used them are done on the graphics card by now.
        std::scoped_lock _{g_retired_mtx};
        std::erase_if(g_retired_buffers, [&](const auto& r) { return now - r.second > std::chrono::seconds(3); });
    }
    if (pastBuffer[0] != nullptr) {
        D3D12_RESOURCE_DESC desc2 = pastBuffer[0]->GetDesc();
        // (CopyResource needs the two the same in all of these.)
        if (desc.Dimension != desc2.Dimension || desc.Width != desc2.Width || desc.Height != desc2.Height || desc.DepthOrArraySize != desc2.DepthOrArraySize ||
            desc.MipLevels != desc2.MipLevels || desc.Format != desc2.Format || desc.SampleDesc.Count != desc2.SampleDesc.Count ||
            desc.SampleDesc.Quality != desc2.SampleDesc.Quality) {
            spdlog::info("Resource size mismatch {} {} {} {} {} {} {} {}", fmt::ptr(source), desc.Width, desc.Height, desc.Format, fmt::ptr(pastBuffer[0].Get()), desc2.Width,
                         desc2.Height, desc2.Format);
            // (The game's picture changed size, as a menu opens or closes. The old copies are let go a few seconds later, not now: the
            // last frames' copy commands, recorded with them, may not have run on the graphics card yet. Released now, the card used
            // freed memory and the driver timed out: the crashes as a second menu opened in stereo, right after this line.)
            {
                std::scoped_lock _{g_retired_mtx};
                for (int i = 0; i < 4; ++i) {
                    if (pastBuffer[i] != nullptr) {
                        g_retired_buffers.emplace_back(std::move(pastBuffer[i]), now);
                    }
                    pastBuffer[i].Reset();
                }
            }
            // (Changing again within a second, as a picture whose size keeps changing would: its copies are paused a few seconds rather
            // than made anew, and kept, every time.)
            const bool again = kind.changed_at != std::chrono::steady_clock::time_point{} && now - kind.changed_at < std::chrono::seconds(1);
            kind.changed_at  = now;
            if (again) {
                kind.paused_until = now + std::chrono::seconds(5);
                if (kind.pause_logs++ < 5) {
                    spdlog::info("[VR] per-eye history {}: its picture changed size twice within a second: not kept per eye for 5 s", index);
                }
                return false;
            }
        }
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    if (pastBuffer[0] == nullptr || pastBuffer[1] == nullptr) {
        D3D12_HEAP_PROPERTIES heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (int i = 0; i < 4; i++) {
            pastBuffer[i].Reset(); // (none in use: only after a failed attempt, which was never copied with)
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(pastBuffer[i].GetAddressOf()))))
            {
                spdlog::error("[VR] Failed to create resource copy copy for AFR backbuffer {}.", i);
                return false;
            }
        }
        kind.fresh = 2; // (both of its copies filled before either is put back: one frame of each eye)
    }
    return true;
}

void CreationEngineRendererModule::SwapBuffer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* originalBuffer, int index, int copyFromSlot, int copyToSlot)
{
    if (!ValidateResource(originalBuffer, m_pastBuffer[index], index)) {
        return;
    }
    if (ModSettings::g_debug.taaHistoryInvert) {
        std::swap(copyFromSlot, copyToSlot);
    }
    auto saveBuffer       = m_pastBuffer[index][copyToSlot].Get();
    auto alterFrameBuffer = m_pastBuffer[index][copyFromSlot].Get();
    CopyResource(cmdList, originalBuffer, saveBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
    // (Copies just made hold nothing yet: the game's own history is left as it is until both have been filled.)
    if (auto& kind = g_per_eye[index]; kind.fresh > 0) {
        --kind.fresh;
        return;
    }
    CopyResource(cmdList, alterFrameBuffer, originalBuffer, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CreationEngineRendererModule::RenderGraphStart(RE::CreationRendererPrivate::RenderGraph* pGraph, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData, bool before)
{
    // (Each of the frame's top-level graphs: the steps' picture lookups are indexed afresh; the game waits for a graph's steps before
    // the next graph starts.)
    if (before) {
        RenderPassIo::NewFrame();
    }
    if (m_startFramePass == nullptr && strcmp(pGraph->name, "CRBeginFrame") == 0) {
        m_startFramePass = pGraph;
    }
    if (m_endFramePass == nullptr && strcmp(pGraph->name, "CREndFrame") == 0) {
        m_endFramePass = pGraph;
    }
    if (m_framePass == nullptr && strcmp(pGraph->name, "Frame") == 0) {
        m_framePass = pGraph;
    }
    auto vr = VR::get();
    if (m_startFramePass == pGraph && before) {
        // (fix192) The interface movies drawn in the frame just ended, kept before the list is emptied for the new one (only while the panel
        // was up with no see-through menu and no menu box, as in the state the log line below watches: not each frame of every menu).
        std::vector<std::string> drawn_before{};
        if (const auto& was = ModSettings::g_internalSettings; was.showQuadDisplay && !was.menuSeeThrough && was.menuBoxRows == 0) {
            drawn_before = GameFlow::uiPartsDrawn();
        }
        GameFlow::resetGameState();
        ModSettings::g_internalSettings.showQuadDisplay = GameFlow::isShowingMenu();
        ModSettings::g_internalSettings.menuWorldBehind = ModSettings::g_internalSettings.showQuadDisplay && GameFlow::worldBehindMenu();
        ModSettings::g_internalSettings.menuSeeThrough  = ModSettings::g_internalSettings.menuWorldBehind && GameFlow::menusAllowSeeThrough();
        ModSettings::g_internalSettings.menuButtonBacking = ModSettings::g_internalSettings.menuSeeThrough && GameFlow::menuWantsButtonBacking();
        ModSettings::g_internalSettings.menuShownSerial   = GameFlow::menuShownSerial();
        ModSettings::g_internalSettings.menuLoadingScreen = ModSettings::g_internalSettings.showQuadDisplay && GameFlow::isLoadingScreenShown(); // (fix164)
        // (fix183) Black showed outside the background when the surface map and the star map opened: that black now has
        // the glow, as loading screens do.
        // (fix203) And character creation: the world as it was behind it is the last frame before it opened (the game's black fade, or the room
        // from where your head was), nothing to do with the character shown; the soft light in the menu's own colours instead.
        ModSettings::g_internalSettings.menuGlowWhenEmpty =
            ModSettings::g_internalSettings.showQuadDisplay && (GameFlow::isStarMapShown() || GameFlow::isChargenShown());
        {
            const auto backbuffer = vr->get_backbuffer_size();
            int        rows       = ModSettings::g_internalSettings.showQuadDisplay ? GameFlow::menuBoxRows((int)backbuffer[0], (int)backbuffer[1]) : 0;
            // (fix173) No box from the menus' movies: the game's own, if its box is on (GameBoxRows).
            const int game_rows = ModSettings::g_internalSettings.showQuadDisplay && rows == 0 ? GameBoxRows((int)backbuffer[0], (int)backbuffer[1]) : 0;
            static int told_rows = -1;
            if (game_rows != told_rows && (game_rows > 0 || told_rows > 0)) {
                static int tells = 0;
                if (tells++ < 40) {
                    spdlog::info("[VR] (fix173) Menu panel: {} (the menus shown: {})",
                        game_rows > 0 ? std::format("no menu movie gives its box: the game's own 16:9 box taken, {} of {} rows", game_rows, (int)backbuffer[1])
                                      : std::string{ "its box from the menus' movies again (or none)" },
                        GameFlow::shownMenus());
                }
            }
            told_rows = game_rows;
            ModSettings::g_internalSettings.menuBoxRows = game_rows > 0 ? game_rows : rows;
        }
        WatchTallPanel(drawn_before); // (fix192)
        WatchFrameRate(); // (fix173)
        WatchChargenPanel(); // (fix203)
    } else if (!before && m_framePass == pGraph) {
        auto fc          = GameFlow::renderLoopFrameCount();
        auto context     = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(pRenderGraphData->getCommandList());
        auto commandList = context->pID3D12CommandList;
        // (Not with the world drawn behind a see-through menu: the frame has several views then, each with its anti-aliasing step;
        // the world's own history is kept per eye by ViewHistory.)
        if (vr->is_hmd_active() && PerEyeHistory() && !LiveWorld::Active() && ModConstants::enabledResourcesToCopy[1]) {
            auto resource = pRenderGraphData->getResourceByIndex(1, (fc - 1) & 1);
            SwapBuffer(commandList, resource, 1, (fc - 1) & 1, fc & 1);
        }
    }
    // Test build: the order of the frame's render graphs (where the interface is drawn).
    UiProbe::OnRenderGraph(pGraph->name, before);
    // The world kept drawn behind a see-through menu, and its pictures (and the menu's 3D pictures) copied (LiveWorld.h). (Before the
    // test below: the steps it watches are hooked first.)
    LiveWorld::OnGraph(pGraph, pGraph->name, before);
    // (fix140: one frame in 30, each step's GPU time, GpuTimer.h; it begins at CRBeginFrame.)
    GpuTimer::OnGraph(pGraph, pGraph->name, before);
    // (Full stereo: the right eye's own history list kept as the game keeps its own, StereoView.)
    StereoView::OnGraph(pGraph->name, before);
    // Test build: the steps of the frame while a see-through menu is open (RenderSteps.h).
    RenderSteps::OnGraph(pGraph, pGraph->name, before);
}

//__int64 CreationEngineRendererModule::onRenderFrameStart(void* pVoid, __int64 i, __int64 i1, __int64 i2)
//{
//    using func_t              = decltype(onRenderFrameStartDetour);
//    static auto original_func = m_onRenderFrameStartHook->get_original<func_t>();
////    spdlog::info("Frame Submit Start[{:X}], fc=[m={},r={}]", GetCurrentThreadId(), GameFlow::gameLoopFrameCount(), GameFlow::renderLoopFrameCount());
//    auto result = original_func(pVoid, i, i1, i2);
//    return result;
//}


void CreationEngineRendererModule::SetWindowSize(int width, int height)
{
    if (ModSettings::g_debug.disableWindowResize) {
        return;
    }

    static std::atomic<bool> inside_change{ false };
    static int               last_synced_frame{ 1000 };
    static uint32_t          last_hmd_width{ 0 }, last_hmd_height{ 0 };
    auto                     fc = GameFlow::renderLoopFrameCount();
    if(inside_change.exchange(true)) {
        return;
    }

    // Checked every few seconds (the game keeps resetting its display rect), and at once when the headset's render size
    // changes (the resolution scale was changed).
    const auto&    vr_now     = VR::get();
    const uint32_t hmd_width  = vr_now->is_hmd_active() ? (uint32_t)vr_now->get_hmd_width() : 0;
    const uint32_t hmd_height = vr_now->is_hmd_active() ? (uint32_t)vr_now->get_hmd_height() : 0;
    // (Not before the first regular check: the window is left alone during the game's start, as before.)
    const bool     size_changed = last_hmd_width != 0 && (hmd_width != last_hmd_width || hmd_height != last_hmd_height);
    if (m_creationEngineSettings == nullptr || *m_creationEngineSettings == nullptr || (!size_changed && (fc - last_synced_frame) < 5*72)) {
        inside_change.store(false);
        return;
    }
    last_synced_frame = fc;
    last_hmd_width    = hmd_width;
    last_hmd_height   = hmd_height;
    inside_change.store(false);

    // On the main menu the window can be kept on the desktop screen instead of at the headset's
    // size (which is bigger than most monitors), so the menu can also be used with the mouse.
    // The menu is shown on the flat virtual screen in the headset either way.
    bool fit_to_monitor = false;
    RECT monitor_work{};
    if (width == 0 || height == 0) {
        auto vr = VR::get();
        if (!vr->is_hmd_active()) {
            return;
        }
        width   = vr->get_hmd_width();
        height  = vr->get_hmd_height();
        if(width == 0 || height == 0) {
            return;
        }
        if (ModConstants::desktopWindowOnMainMenu && GameFlow::isMainMenuShowing()) {
            const auto  hwnd = g_framework->get_window();
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (hwnd != nullptr && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &info)) {
                RECT  frame{ 0, 0, 0, 0 };
                AdjustWindowRectEx(&frame, (DWORD)GetWindowLong(hwnd, GWL_STYLE), GetMenu(hwnd) != NULL, (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
                const int fit_w = (info.rcWork.right - info.rcWork.left) - (frame.right - frame.left);
                const int fit_h = (info.rcWork.bottom - info.rcWork.top) - (frame.bottom - frame.top);
                if (fit_w > 320 && fit_h > 240 && (fit_w < width || fit_h < height)) {
                    width          = fit_w;
                    height         = fit_h;
                    monitor_work   = info.rcWork;
                    fit_to_monitor = true;
                }
            }
        }
    }

    auto window       = (*m_creationEngineSettings)->pHwindow;
    int  windowWidth  = window->windowCX - window->windowX;
    int  windowHeight = window->windowCY - window->windowY;
    if (windowWidth == width && windowHeight == height) {
        return;
    }


    spdlog::debug("Setting window size to {} {}", width, height);
    auto ce_rect = &(*m_creationEngineSettings)->displayGameSettings.displayRect;

    ce_rect->cx = width + ce_rect->x;
    ce_rect->cy = height + ce_rect->y;
    (*m_creationEngineSettings)->displayGameSettings.flags |= 0x100;
//    WINDOWPOS pos;
//    pos.hwnd            = (*m_creationEngineSettings)->pHwindow->windowHandle;
//    pos.hwndInsertAfter = nullptr;
//    pos.x               = rect->x;
//    pos.y               = rect->y;
//    pos.cx              = width;
//    pos.cy              = height;
//    pos.flags           = 0;
//    SendMessage((*m_creationEngineSettings)->pHwindow->windowHandle, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&pos));
    auto  hWnd      = g_framework->get_window();

    // The engine rect above is refreshed every few seconds (the game keeps resetting it), but the
    // window itself only needs moving when its client area is actually the wrong size.
    if (RECT client{}; GetClientRect(hWnd, &client) && client.right - client.left == width && client.bottom - client.top == height) {
        return;
    }

    RECT  rect      = { 0, 0, (LONG)width, (LONG)height };
    DWORD dwStyle   = GetWindowLong(hWnd, GWL_STYLE);
    DWORD dwExStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    BOOL  hasMenu   = GetMenu(hWnd) != NULL;
    AdjustWindowRectEx(&rect, dwStyle, hasMenu, dwExStyle);

    int nWidth  = rect.right - rect.left;
    int nHeight = rect.bottom - rect.top;
    spdlog::info("Setting window size to {} {}{}", nWidth, nHeight, fit_to_monitor ? " (main menu: fitted to the desktop screen)" : "");
    SetWindowPos(hWnd, nullptr, fit_to_monitor ? monitor_work.left : 0, fit_to_monitor ? monitor_work.top : 0, nWidth, nHeight, SWP_ASYNCWINDOWPOS);
}

uintptr_t CreationEngineRendererModule::onUpdateConstantBufferView(uint8_t copyCurrentToPast, uint8_t resetHistory, uintptr_t i2, uintptr_t i3, uintptr_t i4, double d, char i5,
                                                                   RE::RenderPassConstantBufferView* pView)
{
    using func_t              = decltype(onUpdateConstantBufferViewDetour);
    static auto original_func = m_onUpdateConstantBufferViewHook->get_original<func_t>();
    static auto vr            = VR::get();
    const uint8_t game_reset  = resetHistory; // (as the game asked: for the log)
    if (ModSettings::g_debug.taaHistoryReset && vr->is_hmd_active()) {
        resetHistory = 1;
    }
    // Switching between mono and stereo: the history kept so far is of the other arrangement (stereo keeps one per eye, saved
    // long ago when coming back from mono), so the frames around the switch start over.
    {
        static bool     last_mono   = false;
        static bool     last_native = false;
        static bool     last_aer    = false;
        static bool     last_live   = false;
        static uint32_t reset_until = 0;
        const uint32_t  rendered    = vr->m_render_frame_count.load();
        const bool      mono        = vr->is_mono_frame(rendered);
        const bool      native      = vr->is_native_frame(rendered);
        const bool      aer         = vr->is_aer_frame(rendered);
        if (mono != last_mono || native != last_native || aer != last_aer) {
            last_aer    = aer;
            last_native = native;
            last_mono   = mono;
            reset_until = rendered + 2; // (one frame of each eye, as the per-eye DLSS resets)
        }
        // (Likewise when the world starts or stops being drawn behind a see-through menu: the history kept so far is the menu's
        // views', or from before the menu opened.)
        if (const bool live = LiveWorld::Active(); live != last_live) {
            last_live   = live;
            reset_until = rendered + 2;
        }
        if (vr->is_hmd_active() && rendered < reset_until) {
            resetHistory = 1;
        }
    }
    auto        result        = original_func(copyCurrentToPast, resetHistory, i2, i3, i4, d, i5, pView);
    // Behind a see-through menu, the world's camera (MenuCamera moves it with the head): whether the anti-aliasing sees its motion (its
    // previous camera not its current one) and whether its history is thrown away, counted for the log.
    if (LiveWorld::Active() && MenuCamera::IsWorldRecord(i2)) {
        const bool still = std::memcmp(&pView->unk150, &pView->unk50, sizeof(pView->unk50)) == 0;
        MenuCamera::OnViewConstants(copyCurrentToPast != 0, game_reset != 0, resetHistory != 0 && game_reset == 0, still);
    }
    //    spdlog::info("update camera blocks {} index {} handles[{},{},{}] st ptr {}", fmt::ptr(pView), indexAfterPresent, cameraHandleId, cameraHandleId2, cameraHandleId3,
    //    fmt::ptr(cameraBlocks->data));
    // (Each eye's previous camera, in stereo, goes with each eye's history pictures: behind a see-through menu only while ViewHistory
    // keeps those per eye, else the anti-aliasing would match one eye's camera with the other eye's picture: fix120's research. There,
    // the world's camera alone: the menu's views keep one history for both eyes, their last frame's, as their cameras are the same
    // for both (the eye's offset and lens are the world camera's only), so their previous camera stays the last frame's: fix127.)
    if (copyCurrentToPast && vr->is_hmd_active() && PerEyeHistory() &&
        (!LiveWorld::Active() || (ViewHistory::Keeping() && MenuCamera::IsWorldRecord(i2)))) {
        auto key = reinterpret_cast<uintptr_t>(pView) & 0xFFFFFFFFFFFFFFC0;
        std::scoped_lock past_lock{ pastProjectionsMutex }; // (fix189)
        if (pastProjections.find(key) != pastProjections.end()) {
            auto temp                         = pastProjections[key];
            auto past2past                    = &pastProjections[key];
            past2past->cameraMatrix           = pView->unk150;
            past2past->cameraPositionOrJitter = pView->unk190;
            past2past->frameNumber            = pView->unk198;
            if (!resetHistory) {
                pView->unk150 = temp.cameraMatrix;
                pView->unk190 = temp.cameraPositionOrJitter;
                pView->unk198 = temp.frameNumber;
            }
        }
        else {
            CameraBlockSnapshot past{};
            past.cameraMatrix           = pView->unk150;
            past.cameraPositionOrJitter = pView->unk190;
            past.frameNumber            = pView->unk198;
            pastProjections[key]        = past;
        }
    }
    return result;
}

uintptr_t CreationEngineRendererModule::onTaaPass(RE::CreationRendererPrivate::RenderPass* pPass, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData,
                                                  RE::CreationRendererPrivate::RenderPassData* renderPassData)
{
    static auto instance    = Get();
    static auto original_fn = instance->taa_vfunc7_hook->get_original<decltype(CreationEngineRendererModule::onTaaPass)>();
    auto        result      = original_fn(pPass, pRenderGraphData, renderPassData);
    if(!PerEyeHistory() || LiveWorld::Active()) {
        return result;
    }
    auto        fc          = GameFlow::renderLoopFrameCount();
    auto        context     = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(pRenderGraphData->getCommandList());
    auto        commandList = context->pID3D12CommandList;
    static auto vr          = VR::get();

    if (vr->is_hmd_active() && ModConstants::enabledResourcesToCopy[2]) {
        auto resource = pRenderGraphData->getResourceByIndex(2, (fc - 1) & 1);
        instance->SwapBuffer(commandList, resource, 2, (fc - 1) & 1, fc & 1);
    }
    if (vr->is_hmd_active() && ModConstants::enabledResourcesToCopy[3]) {
        auto resource = pRenderGraphData->getResourceByIndex(3, (fc - 1) & 1);
        instance->SwapBuffer(commandList, resource, 3, (fc - 1) & 1, fc & 1);
    }
    return result;
}

/*

void logWithExtraData(std::string_view message) {
    spdlog::info("[{:X}:{}] frameCount[m={},r={}]", GetCurrentThreadId(), message.data(), GameFlow::gameLoopFrameCount(), GameFlow::renderLoopFrameCount());
}
*/


uintptr_t CreationEngineRendererModule::setReflexMarkerInternal(uintptr_t rcx, uint32_t marker, uint32_t oldFrameIndex)
{
//    logWithExtraData(std::format("setReflexMarkerInternal[{}, f={}]", marker, oldFrameIndex));
    static auto instance = Get();
    static bool engine_notified = false;
    static     auto        vr            = VR::get();
    static auto cameraModule = CreationEngineCameraManager::Get();
    static bool            sync_marker_started = false;
    static int frames_since_reset = 0;
    if ((marker == 6 || marker == 0 || marker == 1) && !engine_notified) {
        engine_notified = true;
        instance->SetWindowSize(0,0);
        vr->on_wait_rendering(oldFrameIndex);
        vr->m_engine_frame_count = oldFrameIndex;
        vr->on_begin_rendering(oldFrameIndex);
        vr->update_hmd_state(oldFrameIndex);
        g_framework->run_imgui_frame(false);


        if(vr->get_runtime()->loaded) {
            frames_since_reset++;
            if(vr->m_engine_frame_count % 2 == 0) {
                sync_marker_started = true;
            } else {
                sync_marker_started = false;
            }
        }
        // Head, hands and the aim rotation for this engine frame (the body and the camera use the same sample).
        tracking::Snapshot();
        cameraModule->UpdateWorldCamera();
        // (Full stereo: the right eye's view made and kept as the world's, and whether it can be played now.)
        StereoView::OnFrameStart();
        cameraModule->UpdateFirstPersonCamera();
        NodeTreeDump::run_if_requested();
        if (auto root = CreationEngineSingletonManager::GetSceneGraphRoot(); root != nullptr) {
            MotionWeapon::report_pieces_if_requested(root->worldCamera);
        }
    }
    // Reset notification if marker is 1
    if (marker == 1) {
        engine_notified = false;
    }

    if (marker == 2) {
        vr->m_render_frame_count = oldFrameIndex;
    }

    if (marker == 4) {
        vr->m_presenter_frame_count = oldFrameIndex;
    }

    if (vr->tracing() && marker != 0 && marker != 1) {
        vr->trace(std::format("reflex m={}", marker), oldFrameIndex, (int)vr->eye_for_frame(oldFrameIndex));
    }


    if(vr->get_runtime()->loaded && marker == 1 && sync_marker_started) {
        /*
         * as we sync on game loop L eye + frame + game loop R eye + frame Enc
         * we don't expect any async frames comes into this loop
         * if we detect async frame we reset sync and let engine to handle it
         */
        sync_marker_started = false;
    } else if(frames_since_reset > 100 && marker > 1 && marker < 5 && sync_marker_started && vr->get_runtime()->loaded && !vr->is_single()) {
        spdlog::info("Detected frame inconsistency m={} (the next pair waits for the frame before it by index)", marker);
        vr->m_skip_next_present = true;
        frames_since_reset = 0;
        sync_marker_started = false;
    }
    return instance->m_setReflexMarkerInternalHook.call<uintptr_t>(rcx, marker, oldFrameIndex);
}
