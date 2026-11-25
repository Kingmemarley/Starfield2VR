#include "Foveation.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include "CreationEngineRendererModule.h"
#include "LiveWorld.h"
#include "RenderPassIo.h"
#include "StereoView.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <RE/C/CreationRendererPrivate.h>
#include <mods/VR.hpp>

namespace Foveation
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        using GraphData = RE::CreationRendererPrivate::RenderGraphData;
        using PassFn    = uintptr_t (*)(void* pass, GraphData* data, void* pass_data);

        // Starfield 1.16.236: ReprojectShadingRateImageRenderPass's function 7 (the step: the last frame's rates moved along the motion
        // vectors into the view's shading-rate picture, item [0], left in the UAV state; on a cut, cleared), checked by its first bytes;
        // the renderer's "VRS on" byte (the game's switch and bEnableVariableRateShading, set at 0x14298C100: the step does nothing
        // without it, and the passes bind no picture).
        constexpr const char* kStepCoded = ".?AVReprojectShadingRateImageRenderPass@CreationRendererPrivate@@";
        constexpr uintptr_t   kStepRva   = 0x2B59040;
        constexpr uint8_t     kStepBytes[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41 };
        constexpr uintptr_t   kVrsOnRva  = 0x5E7E31C;
        // A step's view: the graph data's +0x108, indexed by its +0x140 (as the game's steps find it); in the view's record its handle
        // (+0x24, 24 bits), the share of its pictures it draws into (+0x10: left, right, bottom, top, as 0..1; 0x142A01AD0) and, through
        // +0x38 and the pointer there, the size of its pictures (+0x54, +0x58; read so at 0x142B59269, fix139's review).
        constexpr uintptr_t kDataRecords  = 0x108;
        constexpr uintptr_t kDataIndex    = 0x140;
        constexpr uintptr_t kRecordKey    = 0x24;
        constexpr uintptr_t kRecordShare  = 0x10;
        constexpr uintptr_t kRecordSizes  = 0x38;
        constexpr uintptr_t kSizesWidth   = 0x54;
        constexpr uintptr_t kSizesHeight  = 0x58;
        // Where the lens's rounded corners begin (the share of the way to the lens's edge, along the line from its centre: its field of
        // view's corners are at 1.41; the headset's hidden corners, a tenth of its picture, start about here).
        constexpr float kCorner = 1.2f;
        // A margin around each lens's field of view (tan, about 3 degrees): the headset moves the picture as the head turns after it was
        // drawn, so a little beyond the lens's edge can come into view (fix139's review).
        constexpr float kGuard = 0.06f;
        // The rate set: 2x2, the game's own coarsest. Its tiled lighting reads the picture too and takes a square as coarse only by the
        // 2x2 rate's bits (0x4 across, 0x1 down: RecordIndirectArgs, TiledDeferredLight; its own step writes only 0, 1, 4 and 5), so 4x4
        // (0xA) would be lit at full rate there (fix139's review).
        constexpr uint8_t kRate2x2 = 0x5; // D3D12_SHADING_RATE_2X2

        PassFn                g_original{ nullptr };
        uintptr_t             g_base{ 0 };
        std::atomic<bool>     g_on{ true };
        std::atomic<float>    g_ring{ 0.9f };
        std::atomic<bool>     g_installed{ false };
        std::atomic<uint64_t> g_calls{ 0 }, g_applied[3]{}, g_vrs_off{ 0 }, g_other_views{ 0 }, g_unusable{ 0 };
        std::atomic<uint64_t> g_steps_ever{ 0 }; // (never reset: retired patterns' release, fix139's review)
        std::atomic<int>      g_unusable_logs{ 0 };

        // The device's variable rate shading (once, from the picture's device): tier 2 (a picture) needed, and its squares' size.
        std::once_flag g_caps_once{};
        bool           g_tier2{ false };
        uint32_t       g_tile{ 0 };

        // The patterns made (one per eye or mono, size and lens): their rates in an upload buffer laid out as the picture, and the boxes
        // to copy. Kept for the session (small: a few kB each).
        struct Pattern
        {
            int                    kind{ -1 }; // 0 left, 1 right, 2 mono
            std::array<int32_t, 8> geometry{}; // the picture's size in squares, the view's rectangle, the square's size
            std::array<int32_t, 13> lens{};     // the drawn frustum, both lenses, the ring (quantized)
            ComPtr<ID3D12Resource> buffer{};
            uint32_t               pitch{ 0 };
            std::vector<D3D12_BOX> boxes{};
            uint32_t               coarse{ 0 }, ring{ 0 };
        };
        std::mutex           g_patterns_mtx{};
        std::vector<Pattern> g_patterns{};
        // (Patterns let go when too many were made, e.g. a view's rectangle changing: their buffers kept until the GPU is surely done with
        // them, a couple of thousand steps later.)
        std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> g_retired{};
        std::array<bool, 3>  g_pattern_logged{};

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

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        void Unusable(const std::string& why)
        {
            g_unusable.fetch_add(1, std::memory_order_relaxed);
            if (g_unusable_logs.fetch_add(1) < 6) {
                spdlog::info("[Foveation] a view's shading-rate picture left as the game made it: {}", why);
            }
        }

        // Frustum tangents {left, right, top, bottom} (left and bottom negative).
        using Tan = std::array<float, 4>;

        bool ValidTan(const Tan& f)
        {
            return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]) && std::isfinite(f[3]) && f[1] > f[0] && f[2] > f[3];
        }

        // How coarse a square may be for one eye: 2 outside its lens (beyond its field of view, or in its rounded corners), 1 in its
        // outer ring, 0 not at all (the game's rate). The square's extent in tangents: x0..x1, y0..y1. From the point of the square
        // nearest the lens's centre (straight ahead), its share of the way to the lens's edge in that direction.
        int Class(const Tan& lens, float x0, float x1, float y0, float y1, float ring)
        {
            if (x1 <= lens[0] - kGuard || x0 >= lens[1] + kGuard || y1 <= lens[3] - kGuard || y0 >= lens[2] + kGuard) {
                return 2;
            }
            const float cx = std::clamp(0.0f, x0, x1);
            const float cy = std::clamp(0.0f, y0, y1);
            const float ex = cx >= 0.0f ? lens[1] : -lens[0];
            const float ey = cy >= 0.0f ? lens[2] : -lens[3];
            const float r  = std::sqrt((cx / ex) * (cx / ex) + (cy / ey) * (cy / ey));
            return r > kCorner ? 2 : r > ring ? 1 : 0;
        }

        int32_t Q(float v)
        {
            return static_cast<int32_t>(std::lround(v * 4096.0f));
        }

        // (Under g_patterns_mtx.) The pattern for this eye, size and lens: made the first time.
        const Pattern* PatternFor(ID3D12Device* device, int kind, uint32_t width, uint32_t height, int32_t vx, int32_t vy, int32_t vw, int32_t vh,
            const Tan& drawn, const Tan& left, const Tan& right, float ring)
        {
            const std::array<int32_t, 8>  geometry{ static_cast<int32_t>(width), static_cast<int32_t>(height), vx, vy, vw, vh,
                static_cast<int32_t>(g_tile), 0 };
            const std::array<int32_t, 13> lens{ Q(drawn[0]), Q(drawn[1]), Q(drawn[2]), Q(drawn[3]), Q(left[0]), Q(left[1]), Q(left[2]),
                Q(left[3]), Q(right[0]), Q(right[1]), Q(right[2]), Q(right[3]), Q(ring) };
            for (const auto& p : g_patterns) {
                if (p.kind == kind && p.geometry == geometry && p.lens == lens) {
                    return p.buffer != nullptr ? &p : nullptr;
                }
            }
            for (auto it = g_retired.begin(); it != g_retired.end();) {
                it = g_steps_ever.load(std::memory_order_relaxed) >= it->first ? g_retired.erase(it) : it + 1;
            }
            if (g_patterns.size() >= 16) {
                g_retired.emplace_back(g_steps_ever.load(std::memory_order_relaxed) + 2000, std::move(g_patterns.front().buffer));
                g_patterns.erase(g_patterns.begin());
            }
            if (g_retired.size() >= 64) {
                return nullptr; // (a size or lens changing all the time: not followed until the old ones are let go)
            }
            Pattern p{};
            p.kind     = kind;
            p.geometry = geometry;
            p.lens     = lens;
            p.pitch    = (width + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            std::vector<uint8_t> rates(static_cast<size_t>(p.pitch) * height, 0);
            std::vector<uint8_t> classes(static_cast<size_t>(width) * height, 0);
            const float          tile = static_cast<float>(g_tile);
            for (uint32_t ty = 0; ty < height; ++ty) {
                // (The square's rows as shares of the view's rectangle, then as tangents: top down.)
                const float v0 = (static_cast<float>(ty) * tile - static_cast<float>(vy)) / static_cast<float>(vh);
                const float v1 = (static_cast<float>(ty + 1) * tile - static_cast<float>(vy)) / static_cast<float>(vh);
                if (v1 <= 0.0f || v0 >= 1.0f) {
                    continue; // (not in the view's rectangle: not used)
                }
                const float y_top    = drawn[2] - std::clamp(v0, 0.0f, 1.0f) * (drawn[2] - drawn[3]);
                const float y_bottom = drawn[2] - std::clamp(v1, 0.0f, 1.0f) * (drawn[2] - drawn[3]);
                for (uint32_t tx = 0; tx < width; ++tx) {
                    const float u0 = (static_cast<float>(tx) * tile - static_cast<float>(vx)) / static_cast<float>(vw);
                    const float u1 = (static_cast<float>(tx + 1) * tile - static_cast<float>(vx)) / static_cast<float>(vw);
                    if (u1 <= 0.0f || u0 >= 1.0f) {
                        continue;
                    }
                    const float x0 = drawn[0] + std::clamp(u0, 0.0f, 1.0f) * (drawn[1] - drawn[0]);
                    const float x1 = drawn[0] + std::clamp(u1, 0.0f, 1.0f) * (drawn[1] - drawn[0]);
                    // (Mono: one picture for both eyes, so the finer of the two: only what neither sees, or both see blurred.)
                    int c = 0;
                    if (kind == 0) {
                        c = Class(left, x0, x1, y_bottom, y_top, ring);
                    } else if (kind == 1) {
                        c = Class(right, x0, x1, y_bottom, y_top, ring);
                    } else {
                        c = std::min(Class(left, x0, x1, y_bottom, y_top, ring), Class(right, x0, x1, y_bottom, y_top, ring));
                    }
                    classes[static_cast<size_t>(ty) * width + tx] = static_cast<uint8_t>(c);
                    rates[static_cast<size_t>(ty) * p.pitch + tx] = c != 0 ? kRate2x2 : 0;
                    p.coarse += c == 2 ? 1 : 0;
                    p.ring += c == 1 ? 1 : 0;
                }
            }
            // The boxes: each row's runs of one coarse class; rows with the same runs one after another made one box.
            struct Run
            {
                uint32_t x0, x1;
                uint8_t  c;
                bool     operator==(const Run&) const = default;
            };
            std::vector<Run> open{};
            uint32_t         open_from = 0;
            const auto       close     = [&](uint32_t to) {
                for (const auto& r : open) {
                    p.boxes.push_back(D3D12_BOX{ r.x0, open_from, 0, r.x1, to, 1 });
                }
            };
            for (uint32_t ty = 0; ty <= height; ++ty) {
                std::vector<Run> runs{};
                if (ty < height) {
                    for (uint32_t tx = 0; tx < width;) {
                        const uint8_t c = classes[static_cast<size_t>(ty) * width + tx];
                        uint32_t      e = tx + 1;
                        while (e < width && classes[static_cast<size_t>(ty) * width + e] == c) {
                            ++e;
                        }
                        if (c != 0) {
                            runs.push_back(Run{ tx, e, c });
                        }
                        tx = e;
                    }
                }
                if (runs != open) {
                    close(ty);
                    open      = std::move(runs);
                    open_from = ty;
                }
            }
            // (Its upload buffer, laid out as the picture: copied from, never changed.)
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = rates.size();
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* mapped          = nullptr;
            if (p.boxes.empty() ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.buffer))) ||
                FAILED(p.buffer->Map(0, nullptr, &mapped)) || mapped == nullptr) {
                p.buffer.Reset();
            } else {
                std::memcpy(mapped, rates.data(), rates.size());
                p.buffer->Unmap(0, nullptr);
                p.buffer->SetName(L"Foveation: a shading-rate pattern");
            }
            // (For the log, once for each eye and mono: the pattern, a character a square: '.' the game's, 'o' 2x2, '#' 4x4.)
            if (!g_pattern_logged[kind]) {
                g_pattern_logged[kind] = true;
                static const char* names[3]{ "the left eye's", "the right eye's", "mono's (both eyes)" };
                spdlog::info("[Foveation] {} pattern: {}x{} squares of {} px over the view's {}x{} at {},{} (the drawn frustum tan {:.3f} {:.3f} {:.3f} "
                             "{:.3f}): {} squares {} (outside the lens), {} at 2x2 (its outer ring, past {:.2f} of the way to its edge), {} boxes{}",
                    names[kind], width, height, g_tile, vw, vh, vx, vy, drawn[0], drawn[1], drawn[2], drawn[3], p.coarse, "at 2x2",
                    p.ring, ring, p.boxes.size(), p.buffer != nullptr ? std::string{} : std::string{ "; its buffer could not be made: left as the game's" });
                for (uint32_t ty = 0; ty < height; ++ty) {
                    std::string row(width, '.');
                    for (uint32_t tx = 0; tx < width; ++tx) {
                        const uint8_t c = classes[static_cast<size_t>(ty) * width + tx];
                        row[tx]         = c == 2 ? '#' : c == 1 ? 'o' : '.';
                    }
                    spdlog::info("[Foveation]   {}", row);
                }
            }
            g_patterns.push_back(std::move(p));
            const Pattern& made = g_patterns.back();
            return made.buffer != nullptr ? &made : nullptr;
        }

        void Apply(GraphData* data, void* pass_data)
        {
            auto& vr = VR::get();
            if (vr == nullptr || !vr->is_hmd_active() || !vr->get_runtime()->is_openxr() || vr->m_openxr == nullptr) {
                return;
            }
            uint8_t vrs_on = 0;
            if (!Read(At(kVrsOnRva), vrs_on) || vrs_on == 0) {
                g_vrs_off.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // (A menu up: the world behind it drawn in the menu's way (its box, no DLSS), seen through a panel; or the flat screen: left
            // as the game's.)
            if (vr->m_openxr->panel_mode || vr->m_openxr->flat_screen_mode || LiveWorld::Active()) {
                return;
            }
            // Its view, and so whose eye.
            uint32_t  index = 0, key = 0;
            uintptr_t record = 0;
            if (!Read(reinterpret_cast<uintptr_t>(data) + kDataIndex, index) || index >= 64 ||
                !Read(reinterpret_cast<uintptr_t>(data) + kDataRecords + 8 * static_cast<uintptr_t>(index), record) || record == 0 ||
                !Read(record + kRecordKey, key)) {
                Unusable("its view's record could not be read");
                return;
            }
            key &= 0xFFFFFF;
            uint32_t world = 0;
            const bool eye_view = StereoView::IsEyeView(key);
            // (The world's handle compared by its low 24 bits, as the record's: fix140's test found it never matched in full, so only
            // full stereo's right eye was given its pattern.)
            if (!eye_view && (!LiveWorld::WorldViewHandle(world) || key != (world & 0xFFFFFF))) {
                g_other_views.fetch_add(1, std::memory_order_relaxed); // (a menu's 3D view: its own picture, not seen through a lens)
                return;
            }
            const uint32_t frame = vr->m_render_frame_count.load();
            int            kind  = 0;
            if (eye_view) {
                kind = 1;
            } else if (vr->is_mono_frame(frame)) {
                kind = 2;
            } else if (vr->is_native_frame(frame)) {
                kind = 0;
            } else {
                // (The eye the picture is handed to, by the frame's turn: with Swap Eyes the camera draws the other, but this lens sees it.)
                kind = vr->eye_for_frame(frame) == VRRuntime::Eye::LEFT ? 0 : 1;
                // (For the check at the view's DLSS step, which knows its frame for sure: UpscalerAfrNvidiaModule, m_foveation_frames.)
                // (Marked once a frame: a second world view claiming the same frame is one labelled wrongly.)
                if (vr->m_foveation_frames[frame & 7].exchange(frame + 1, std::memory_order_relaxed) == frame + 1) {
                    vr->m_foveation_misses.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // The frustum it is drawn with (and submitted with), and each eye's lens (its field of view as the headset gives it).
            Tan drawn{}, left{}, right{};
            {
                auto*            runtime = vr->get_runtime();
                std::shared_lock _{ runtime->eyes_mtx };
                const Vector4f&  f = kind == 2 ? runtime->mono_frustum : runtime->eye_frustums[kind];
                drawn              = { f[0], f[1], f[2], f[3] };
                const Vector4f&  a = runtime->lens_frustums[0];
                const Vector4f&  b = runtime->lens_frustums[1];
                left               = { a[0], a[1], a[2], a[3] };
                right              = { b[0], b[1], b[2], b[3] };
            }
            if (!ValidTan(drawn) || !ValidTan(left) || !ValidTan(right)) {
                return;
            }
            // Its shading-rate picture: one texel a square, 8-bit, in the UAV state.
            RenderPassIo::PassIo io{};
            D3D12_RESOURCE_DESC  desc{};
            if (!RenderPassIo::ReadPassIo(pass_data, io) || io.count < 1 || io.items[0].resource == nullptr ||
                !RenderPassIo::DescOf(io.items[0].resource, desc)) {
                Unusable("its shading-rate picture could not be found");
                return;
            }
            const auto state = static_cast<D3D12_RESOURCE_STATES>(RE::CreationRendererPrivate::RenderPassItem::getDXGIState(io.items[0].flags));
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R8_UINT || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                Unusable(std::format("its picture is not as expected ({}x{} format {}, state {:#x})", static_cast<uint64_t>(desc.Width), desc.Height,
                    static_cast<int>(desc.Format), static_cast<uint32_t>(state)));
                return;
            }
            ComPtr<ID3D12Device> device{};
            if (FAILED(io.items[0].resource->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
                return;
            }
            std::call_once(g_caps_once, [&] {
                D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
                if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
                    g_tier2   = o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
                    g_tile    = o6.ShadingRateImageTileSize;
                }
                spdlog::info("[Foveation] the card's variable rate shading: tier {}, squares of {} px", g_tier2 ? 2 : 1, g_tile);
            });
            if (!g_tier2 || g_tile == 0) {
                return;
            }
            // The view's rectangle in its pictures, as the game computes it (0x142A01AD0): the share at +0x10 of the size through +0x38.
            float     share[4]{};
            uintptr_t holder = 0, sizes = 0;
            uint32_t  w = 0, h = 0;
            if (!ReadBytes(record + kRecordShare, share, sizeof(share)) || !Read(record + kRecordSizes, holder) || holder == 0 || !Read(holder, sizes) ||
                !Read(sizes + kSizesWidth, w) || !Read(sizes + kSizesHeight, h) || w == 0 || h == 0 || w > 16384 || h > 16384) {
                Unusable("its view's rectangle could not be read");
                return;
            }
            const int32_t vx = static_cast<int32_t>(static_cast<float>(w) * share[0]);
            const int32_t vw = std::max(1, static_cast<int32_t>(static_cast<float>(w) * (share[1] - share[0])));
            const int32_t vy = static_cast<int32_t>(static_cast<float>(h) * share[3]);
            const int32_t vh = std::max(1, static_cast<int32_t>(static_cast<float>(h) * (share[2] - share[3])));
            const auto    tiles_w = (w + g_tile - 1) / g_tile, tiles_h = (h + g_tile - 1) / g_tile;
            if (desc.Width != tiles_w || desc.Height != tiles_h || vx < 0 || vy < 0 || vx + vw > static_cast<int32_t>(w) || vy + vh > static_cast<int32_t>(h)) {
                Unusable(std::format("its picture ({}x{} squares) does not match its view ({}x{} at {},{} of {}x{})", static_cast<uint64_t>(desc.Width),
                    desc.Height, vw, vh, vx, vy, w, h));
                return;
            }
            auto* context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(data->getCommandList());
            ID3D12GraphicsCommandList* list = context != nullptr ? context->pID3D12CommandList : nullptr;
            if (list == nullptr || (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
                Unusable("no command list to record on");
                return;
            }
            ID3D12Resource* buffer = nullptr;
            uint32_t        pitch  = 0;
            std::vector<D3D12_BOX> boxes{};
            {
                std::scoped_lock _{ g_patterns_mtx };
                const Pattern* p = PatternFor(device.Get(), kind, static_cast<uint32_t>(desc.Width), desc.Height, vx, vy, vw, vh, drawn, left, right,
                    std::clamp(g_ring.load(), 0.6f, 1.2f));
                if (p == nullptr) {
                    return;
                }
                buffer = p->buffer.Get();
                pitch  = p->pitch;
                boxes  = p->boxes;
            }
            D3D12_RESOURCE_BARRIER to{}, back{};
            to.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to.Transition.pResource   = io.items[0].resource;
            to.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            to.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            back                      = to;
            back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            back.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource                          = io.items[0].resource;
            dst.Type                               = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex                   = 0;
            src.pResource                          = buffer;
            src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset             = 0;
            src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UINT;
            src.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
            src.PlacedFootprint.Footprint.Height   = desc.Height;
            src.PlacedFootprint.Footprint.Depth    = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            list->ResourceBarrier(1, &to);
            for (const auto& box : boxes) {
                list->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
            }
            list->ResourceBarrier(1, &back);
            g_applied[kind].fetch_add(1, std::memory_order_relaxed);
        }

        uintptr_t Hooked(void* pass, GraphData* data, void* pass_data)
        {
            const uintptr_t result = g_original(pass, data, pass_data);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            g_steps_ever.fetch_add(1, std::memory_order_relaxed);
            if (g_on.load(std::memory_order_relaxed) && data != nullptr && pass_data != nullptr) {
                try {
                    Apply(data, pass_data);
                } catch (...) {
                }
            }
            return result;
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        // (The type table of the object itself, offset 0 in it: its complete object locator says so.)
        std::optional<uintptr_t> vtable{};
        for (const uintptr_t table : utility::rtti::find_vtables(MemoryScan::mod, kStepCoded)) {
            const uintptr_t locator = *reinterpret_cast<const uintptr_t*>(table - 8);
            if (locator != 0 && *reinterpret_cast<const uint32_t*>(locator + 4) == 0) {
                vtable = table;
                break;
            }
        }
        if (!vtable || g_base == 0) {
            spdlog::warn("[Foveation] the game's shading-rate step (ReprojectShadingRateImageRenderPass) not found: foveated shading off");
            return;
        }
        auto*           slot = reinterpret_cast<uintptr_t*>(*vtable) + 7;
        const uintptr_t fn   = *slot;
        // (As in Starfield 1.16.236: the game's own function there, by its address and first bytes. Hooked first, at start: anything
        // hooking it later calls this.)
        if (fn != At(kStepRva) || !SameCode(kStepRva, kStepBytes, sizeof(kStepBytes))) {
            spdlog::warn("[Foveation] the game's shading-rate step is not as in Starfield 1.16.236 (function 7 at rva {:x}): foveated shading off", fn - g_base);
            return;
        }
        // (fix190) The renderer's "VRS on" byte, read at every step without a check of its own.
        if (!GameCode::Known({ kVrsOnRva })) {
            spdlog::warn("[Foveation] the renderer's VRS switch (rva {:x}) not known in this game version (GameCode): foveated shading off", kVrsOnRva);
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
            spdlog::warn("[Foveation] the game's shading-rate step: its type table could not be changed: foveated shading off");
            return;
        }
        g_original = reinterpret_cast<PassFn>(fn);
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(reinterpret_cast<uintptr_t>(&Hooked)));
        VirtualProtect(slot, sizeof(uintptr_t), old, &old);
        g_installed = true;
        spdlog::info("[Foveation] the game's shading-rate step hooked (type table rva {:x}, function 7 rva {:x})", *vtable - g_base, fn - g_base);
    }

    void SetOptions(bool on, float ring)
    {
        g_on.store(on, std::memory_order_relaxed);
        g_ring.store(ring, std::memory_order_relaxed);
    }

    std::string Status()
    {
        if (!g_installed.load()) {
            return "not installed";
        }
        return std::format("{} (ring from {:.2f}); since last: steps {}, patterns laid: left eye {}, right eye {}, mono {}; the game's VRS off {}, "
                           "menu views {}, left as the game's {}; the eye checked at DLSS: right {}, wrong {}, not marked {}",
            g_on.load() ? "on" : "off", g_ring.load(), g_calls.exchange(0), g_applied[0].exchange(0), g_applied[1].exchange(0), g_applied[2].exchange(0),
            g_vrs_off.exchange(0), g_other_views.exchange(0), g_unusable.exchange(0), VR::get()->m_foveation_hits.exchange(0),
            VR::get()->m_foveation_misses.exchange(0), VR::get()->m_foveation_unmarked.exchange(0));
    }
}
