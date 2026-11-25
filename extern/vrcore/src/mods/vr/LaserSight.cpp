#include "mods/vr/LaserSight.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <shared_mutex>

#include <DescriptorHeap.h>
#include <d3dcompiler.h>
#include <DirectXColors.h>
#include <SpriteBatch.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "mods/VR.hpp"
#include "nvidia/StreamlineAbi.h"

namespace vrmod {
namespace {
// The game's depth tag (its DLSS step's slSetTag, then slEvaluateFeature, on one thread: Starfield's 0x142AB6C30). Held (a
// reference) and stamped: used only by the evaluation right after it (the game moves its DLSS steps between threads and makes its
// pictures again at each menu).
struct Tag {
    Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
    uint32_t state{0};
    int32_t lifecycle{0};
    uint32_t extent[4]{};
    std::chrono::steady_clock::time_point at{};
};
thread_local Tag t_tag{};
// A copy made as the depth was tagged (a tag valid only then): finished, with its frame, by the evaluation right after it.
struct Pending {
    int index{-1};
    std::chrono::steady_clock::time_point at{};
};
thread_local Pending t_pending{};
constexpr auto kTagFresh = std::chrono::milliseconds(20);
// (fix147) Full stereo: the world view's newest camera is its frame's when given this recently (a frame there takes 30-40 ms; its
// scene setup, which gives the camera, comes a few milliseconds before its DLSS step).
constexpr auto kCameraFresh = std::chrono::milliseconds(100);

// The states a depth tag may give that the copy can start from: read states (depth read, shader resource, copy source) or depth
// write alone; not the default "unknown" (all bits), a render target, unordered access, copy destination or a mix with them.
bool CopyableState(uint32_t state) {
    constexpr uint32_t reads = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    return state == D3D12_RESOURCE_STATE_DEPTH_WRITE || (state != 0 && (state & ~reads) == 0);
}

// The depth tag among some (slSetTag's array, or one of slEvaluateFeature's inputs).
const slabi::ResourceTag* DepthTag(const slabi::ResourceTag& tag) {
    if (!(tag.structType == slabi::kResourceTagType) || tag.type != slabi::kBufferTypeDepth) {
        return nullptr;
    }
    return &tag;
}

// The points along the line: from 0.3 m past the muzzle to 80 m, each about 9% further than the one before.
constexpr float kNearest = 0.3f;
constexpr float kFarthest = 80.0f;
// What is drawn nearer than this to the eye is your own gun, hands and arms (drawn into the same depth with a camera of their own):
// not a hit.
constexpr float kOwnBody = 0.6f;
// The line starts at the gun (or the head): the camera the game drew with is that near it, or its numbers are not where they seem.
constexpr float kNearCamera = 2.5f;

float Distance(int i, int count) {
    return kNearest * std::pow(kFarthest / kNearest, (float)i / (float)(count - 1));
}

bool Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
// (fix148) The beam drawn into the eye pictures (after fix148: the laser should fade out at the end of the red line, and it
// clipped through the gun's model). It was a strip the headset laid over the finished picture (an OpenXR quad layer):
// over everything, the gun too, and as bright at its end as at the gun. Laser sights in Fallout 4 VR are a beam mesh on the gun's
// barrel, hidden by the gun and the world as anything is. Here each pixel of a headset image (the picture of a frame just copied into
// it) looks along its own line of sight for the beam: where the two lines pass closest, the beam's point there, its width (at least
// about a pixel, dimmer when thinner than that) and its brightness along it (bright at the gun, fading to nothing at its end); the
// point hidden where the game's depth (a frame's copy, DLSS's: what the laser's dot is read from) has something nearer there. Added
// as light (a laser glows; it never darkens what is behind it). All of it in the depth's camera's axes (x right, y up, z ahead, the
// game's units): the numbers (14 rows) as root constants.
constexpr const char* kBeamShader = R"(
cbuffer Beam : register(b0) { float4 c[14]; };
Texture2D<float> Depth : register(t0);
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
// How much of the beam's point p is seen: the depth's four texels around where p is in it (each: p in front of what is drawn
// there, or nothing drawn), blended as a bilinear sample would (soft edges at the depth's resolution).
float Seen(float3 p)
{
    const float4 v = float4(p.x, p.y, p.z * c[12].y, 1.0);
    const float w = dot(c[8], v);
    if (w <= 1e-4) {
        return 1.0;
    }
    const float2 ndc = float2(dot(c[6], v), dot(c[7], v)) / w;
    if (any(abs(ndc) > 1.0)) {
        return 1.0;
    }
    uint dw, dh;
    Depth.GetDimensions(dw, dh);
    const float2 texel = float2(c[11].x + (0.5 + 0.5 * ndc.x) * c[11].z, c[11].y + (0.5 - 0.5 * ndc.y) * c[11].w) - 0.5;
    const int2 base = int2(floor(texel));
    const float2 f = texel - (float2)base;
    float seen[4];
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 q = clamp(base + int2(k & 1, k >> 1), int2(0, 0), int2((int)dw - 1, (int)dh - 1));
        const float z = Depth.Load(int3(q, 0));
        const bool drawn = c[12].x > 0.5 ? z > 0.0 : z < 1.0;
        float s = 1.0;
        if (drawn) {
            const float2 qn = float2((q.x + 0.5 - c[11].x) / c[11].z * 2.0 - 1.0, 1.0 - (q.y + 0.5 - c[11].y) / c[11].w * 2.0);
            const float4 nd = float4(qn, z, 1.0);
            const float vw = dot(c[10], nd);
            const float ahead = abs(vw) > 1e-6 ? dot(c[9], nd) / vw * c[12].y : 1e9;
            // (Nearer than the beam's point, by more than a little: it hides it. Your own gun and hands may come out nearest of all,
            // drawn with a camera of their own: still nearer.)
            s = p.z <= max(ahead, 0.0) + c[12].z + c[12].w * p.z ? 1.0 : 0.0;
        }
        seen[k] = s;
    }
    return lerp(lerp(seen[0], seen[1], f.x), lerp(seen[2], seen[3], f.x), f.y);
}
float4 ps_main(VsOut i) : SV_Target0
{
    const float3 o = c[0].xyz;
    const float3 d = normalize(c[1].xyz * i.uv.x + c[2].xyz * i.uv.y + c[3].xyz);
    const float3 s0 = c[4].xyz;
    const float  len = c[4].w;
    const float3 a = c[5].xyz;
    // Where the line of sight and the beam pass closest (the beam's point clamped to its length, the sight's to ahead of the eye).
    const float3 w0 = o - s0;
    const float b = dot(d, a);
    const float dd = dot(d, w0);
    const float ea = dot(a, w0);
    const float t0 = (b * ea - dd) / max(1.0 - b * b, 1e-6);
    const float s = clamp(ea + t0 * b, 0.0, len);
    const float3 p = s0 + a * s;
    const float t = dot(p - o, d);
    if (t <= 0.0) {
        discard;
    }
    const float across = length(o + d * t - p);
    const float radius = c[1].w;
    const float half_width = max(radius, t * c[0].w * 0.75);
    float k = 1.0 - smoothstep(0.0, half_width, across);
    k *= sqrt(saturate(radius / half_width));
    k *= smoothstep(0.0, c[3].w, s) * pow(saturate(1.0 - s / max(len, 1e-4)), 1.5);
    k *= c[2].w;
    if (k < 1.0 / 512.0) {
        discard;
    }
    k *= Seen(p);
    if (k < 1.0 / 512.0) {
        discard;
    }
    float3 rgb = c[13].rgb;
    if (c[13].w > 0.5) {
        rgb = pow(max(rgb, 0.0), 1.0 / 2.2); // (a target that is not sRGB: its values as the picture's, encoded)
    }
    return float4(rgb, k);
}
)";

constexpr float kBeamRadius = 0.0012f;  // the beam's core, half its width (the game's units: metres)
constexpr float kBeamFadeIn = 0.01f;    // brightening over its first centimetre (it starts in the muzzle's opening)
constexpr float kBeamOpacity = 0.9f;
constexpr float kBeamNear = 0.02f;      // (a point nearer the eye than this along its view is not drawn)
constexpr int kBeamRing = 16;            // (views made afresh each draw: 16 draws, several frames, before one is used again)
} // namespace

void LaserSight::set_beam(bool on, const glm::vec3& start, const glm::vec3& direction, float length, uint32_t frame) {
    std::scoped_lock _{m_mtx};
    if (!on || !Finite(start) || !Finite(direction) || glm::length(direction) < 0.5f || !(length > 0.0f) || length > 50.0f) {
        m_beam_at = {};
        return;
    }
    m_beams[frame % m_beams.size()] = Beam{frame, true, start, glm::normalize(direction), length};
    m_beam_at = std::chrono::steady_clock::now();
}

bool LaserSight::beam_ready(uint32_t frame) const {
    std::scoped_lock _{m_mtx};
    if (m_beam_at == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() - m_beam_at > std::chrono::milliseconds(500)) {
        return false;
    }
    bool line = false;
    for (const auto& b : m_beams) {
        line = line || (b.valid && (int32_t)(frame - b.frame) >= 0 && frame - b.frame <= 4);
    }
    bool own = false;
    for (const auto& c : m_copies) {
        own = own || (c.has && c.texture != nullptr && c.camera.valid && c.frame == frame);
    }
    return line && own;
}

bool LaserSight::beam_drawn_lately() const {
    std::scoped_lock _{m_mtx};
    return m_beam_drawn_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - m_beam_drawn_at < std::chrono::milliseconds(150);
}

ID3D12PipelineState* LaserSight::setup_beam(DXGI_FORMAT target_format) {
    for (const auto& p : m_beam_psos) {
        if (p.format == target_format) {
            return p.pso.Get(); // (null: this format failed before)
        }
    }
    if (m_beam_failed) {
        return nullptr;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return nullptr;
    }
    // (A failure: the strip as before. The pipelines made for other formats are kept: a list may still use them; the review.)
    const auto fail = [&](const std::string& why) -> ID3D12PipelineState* {
        m_beam_failed = true;
        spdlog::error("[Laser] (fix148) the beam cannot be drawn into the pictures ({}): the headset's strip as before", why);
        return nullptr;
    };
    const auto compile = [&](const char* entry, const char* target) -> Microsoft::WRL::ComPtr<ID3DBlob> {
        Microsoft::WRL::ComPtr<ID3DBlob> code{}, errors{};
        const HRESULT hr = D3DCompile(kBeamShader, std::strlen(kBeamShader), "vr_laser_beam", nullptr, nullptr, entry, target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) || code == nullptr) {
            std::string message{};
            if (errors != nullptr) {
                message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            }
            spdlog::error("[Laser] (fix148) the beam's shader {} could not be compiled (hr {:x}): {}", entry, (uint32_t)hr, message);
            return nullptr;
        }
        return code;
    };
    if (m_beam_root == nullptr) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 14 * 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3DBlob> blob{}, errors{};
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_beam_root)))) {
            return fail("its root signature");
        }
    }
    // (Compiled once, for every format's pipeline.)
    if (m_beam_vs == nullptr || m_beam_ps == nullptr) {
        m_beam_vs = compile("vs_main", "vs_5_0");
        m_beam_ps = compile("ps_main", "ps_5_0");
        if (m_beam_vs == nullptr || m_beam_ps == nullptr) {
            return fail("its shaders");
        }
    }
    const auto& vs = m_beam_vs;
    const auto& ps = m_beam_ps;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_beam_root.Get();
    pd.VS = D3D12_SHADER_BYTECODE{vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = D3D12_SHADER_BYTECODE{ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = pd.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; // (light added: its colour times how much of it there is)
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO; // (the picture's own alpha kept)
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = target_format;
    pd.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso{};
    // (A slot for this format: a full list, or a pipeline that cannot be made, refused for it alone.)
    BeamPipeline* slot = nullptr;
    for (auto& p : m_beam_psos) {
        if (p.format == DXGI_FORMAT_UNKNOWN) {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr) {
        return nullptr;
    }
    slot->format = target_format;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        spdlog::error("[Laser] (fix148) the beam's pipeline for the headset images' format {} could not be made: the strip for them", (int)target_format);
        return nullptr;
    }
    if (m_beam_views == nullptr || m_beam_targets == nullptr) {
        try {
            m_beam_views = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, (size_t)kBeamRing);
            m_beam_targets = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                (size_t)kBeamRing);
        } catch (...) {
            m_beam_views.reset();
            m_beam_targets.reset();
            return fail("its views");
        }
    }
    slot->pso = pso;
    spdlog::info("[Laser] (fix148) the beam is drawn into the pictures (the headset images' format {}), hidden by what the game's depth has "
                 "nearer, fading along its length",
        (int)target_format);
    return slot->pso.Get();
}

bool LaserSight::draw_beam(ID3D12GraphicsCommandList* list, ID3D12Resource* target, uint32_t frame, bool from_left, const float frustum[4],
    float eyes_apart) {
    if (list == nullptr || target == nullptr || frustum == nullptr) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    // What is needed, taken under the lock: the beam for this frame, its picture's camera, a depth copy and its camera.
    Beam beam{};
    Camera picture{}, depth_camera{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
    DXGI_FORMAT depth_view = DXGI_FORMAT_UNKNOWN;
    uint32_t extent[4]{};
    bool depth_own = false;
    {
        std::scoped_lock _{m_mtx};
        if (m_beam_at == std::chrono::steady_clock::time_point{} || now - m_beam_at > std::chrono::milliseconds(500)) {
            return false; // (no beam shown)
        }
        const Beam* best = nullptr;
        for (const auto& b : m_beams) {
            if (b.valid && (int32_t)(frame - b.frame) >= 0 && frame - b.frame <= 4 && (best == nullptr || (int32_t)(b.frame - best->frame) > 0)) {
                best = &b;
            }
        }
        if (best == nullptr) {
            ++m_stat_beam_no_line;
            return false;
        }
        beam = *best;
        // The picture's camera: its frame's depth copy's (full stereo: the world view's, paired with it), else the camera given for that
        // frame (not in full stereo: there the frame numbers the cameras come with are not the pictures'). The depth: that copy, else the
        // newest of the two frames before.
        const Copy* own = nullptr;
        const Copy* newest = nullptr;
        for (const auto& c : m_copies) {
            if (!c.has || c.texture == nullptr || !c.camera.valid) {
                continue;
            }
            if (c.frame == frame) {
                own = &c;
            }
            if ((int32_t)(frame - c.frame) >= 0 && frame - c.frame <= 2 && (newest == nullptr || (int32_t)(c.frame - newest->frame) > 0)) {
                newest = &c;
            }
        }
        const auto& vr = VR::get();
        const auto& given = m_cameras[frame % m_cameras.size()];
        if (own != nullptr) {
            picture = own->camera;
        } else if (given.valid && given.frame == frame && vr != nullptr && !vr->is_native_frame(frame)) {
            picture = given;
        } else {
            ++m_stat_beam_no_camera;
            return false;
        }
        const Copy* used = own != nullptr ? own : newest;
        depth_own = used == own;
        // (A full stereo copy (its camera at the view's own origin) never read with a real camera, nor the other way round: the frames as
        // full stereo starts or ends; the review.)
        if (used != nullptr && used->camera.synthetic != picture.synthetic) {
            used = nullptr;
        }
        if (used == nullptr) {
            ++m_stat_beam_no_camera;
            return false;
        }
        depth = used->texture;
        depth_view = used->view_format;
        depth_camera = used->camera;
        std::copy(std::begin(used->extent), std::end(used->extent), std::begin(extent));
    }
    const auto target_desc = target->GetDesc();
    DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
    bool encode = false;
    switch (target_desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        target_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        target_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        target_format = target_desc.Format;
        encode = true;
        break;
    default:
        if (m_beam_logs < 2) {
            ++m_beam_logs;
            spdlog::warn("[Laser] (fix148) the headset image's format {} is not one the beam is drawn into", (int)target_desc.Format);
        }
        return false;
    }
    ID3D12PipelineState* const pso = setup_beam(target_format);
    if (pso == nullptr) {
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    const uint32_t width = (uint32_t)target_desc.Width, height = target_desc.Height;
    // The beam and the picture's lines of sight in the renderer's space (the picture camera's axes and place).
    const auto& cp = picture;
    // (Places from the picture camera's own place, never the renderer's large numbers added and taken away again: far from the world's
    // middle that rounding is a millimetre, the beam's width; the review.)
    const glm::vec3 start = cp.right * beam.start.x + cp.up * beam.start.y + cp.forward * beam.start.z;
    const glm::vec3 way = glm::normalize(cp.right * beam.direction.x + cp.up * beam.direction.y + cp.forward * beam.direction.z);
    const glm::vec3 eye = from_left ? cp.right * eyes_apart : glm::vec3{0.0f};
    // The picture's corners as tangents (right, up per unit ahead): its own lens (the camera's numbers, exactly as it drew), or the
    // right eye's lens given (full stereo's right eye).
    glm::vec2 tl{frustum[0], frustum[2]}, tr{frustum[1], frustum[2]}, bl{frustum[0], frustum[3]};
    if (!from_left) {
        const glm::vec4 mid = cp.project * glm::vec4{0.0f, 0.0f, cp.forward_sign, 1.0f};
        const float z = std::abs(mid.w) > 1e-6f ? mid.z / mid.w : 0.5f;
        const auto corner = [&](float nx, float ny, glm::vec2& out) {
            const glm::vec4 v = cp.unproject * glm::vec4{nx, ny, z, 1.0f};
            const float ahead = std::abs(v.w) > 1e-9f ? v.z / v.w * cp.forward_sign : 0.0f;
            if (!(ahead > 1e-6f)) {
                return false;
            }
            out = glm::vec2{v.x / v.w, v.y / v.w} / ahead;
            return std::isfinite(out.x) && std::isfinite(out.y);
        };
        if (!corner(-1.0f, 1.0f, tl) || !corner(1.0f, 1.0f, tr) || !corner(-1.0f, -1.0f, bl)) {
            return false;
        }
    }
    const auto dir_of = [&](const glm::vec2& tangent) { return cp.right * tangent.x + cp.up * tangent.y + cp.forward; };
    const glm::vec3 a_w = dir_of(tr) - dir_of(tl), b_w = dir_of(bl) - dir_of(tl), c_w = dir_of(tl);
    // Where it is in the picture (for the part drawn): its two ends, the near one moved up to just ahead of the eye.
    glm::vec3 near_end = start, far_end = start + way * beam.length;
    const auto ahead_of = [&](const glm::vec3& p) { return glm::dot(p - eye, cp.forward); };
    const float a0 = ahead_of(near_end), a1 = ahead_of(far_end);
    if (a0 <= kBeamNear && a1 <= kBeamNear) {
        std::scoped_lock _{m_mtx};
        ++m_stat_beam_off_view;
        m_beam_drawn_at = now; // (nothing of it in view: handled)
        return true;
    }
    if (a0 <= kBeamNear) {
        near_end += (far_end - near_end) * ((kBeamNear - a0) / (a1 - a0));
    } else if (a1 <= kBeamNear) {
        far_end += (near_end - far_end) * ((kBeamNear - a1) / (a0 - a1));
    }
    const auto to_pixel = [&](const glm::vec3& p) {
        const glm::vec3 r = p - eye;
        const float z = glm::dot(r, cp.forward);
        const glm::vec2 tan{glm::dot(r, cp.right) / z, glm::dot(r, cp.up) / z};
        return glm::vec2{(tan.x - tl.x) / (tr.x - tl.x) * (float)width, (tan.y - tl.y) / (bl.y - tl.y) * (float)height};
    };
    const glm::vec2 p0 = to_pixel(near_end), p1 = to_pixel(far_end);
    const float pixel_tan = glm::length(a_w) / (float)width;
    const float pad = 6.0f + std::min(64.0f, kBeamRadius / std::max(std::min(ahead_of(near_end), ahead_of(far_end)), kBeamNear) / pixel_tan * 2.0f);
    const float x0 = std::clamp(std::floor(std::min(p0.x, p1.x) - pad), 0.0f, (float)width);
    const float x1 = std::clamp(std::ceil(std::max(p0.x, p1.x) + pad), 0.0f, (float)width);
    const float y0 = std::clamp(std::floor(std::min(p0.y, p1.y) - pad), 0.0f, (float)height);
    const float y1 = std::clamp(std::ceil(std::max(p0.y, p1.y) + pad), 0.0f, (float)height);
    if (!(x1 > x0) || !(y1 > y0) || !std::isfinite(x0) || !std::isfinite(y0)) {
        std::scoped_lock _{m_mtx};
        ++m_stat_beam_off_view;
        m_beam_drawn_at = now;
        return true;
    }
    // Everything in the depth camera's axes (x right, y up, z ahead) from its place.
    const auto& cd = depth_camera;
    const auto at_d = [&](const glm::vec3& p) {
        const glm::vec3 r = (cp.position - cd.position) + p;
        return glm::vec3{glm::dot(r, cd.right), glm::dot(r, cd.up), glm::dot(r, cd.forward)};
    };
    const auto way_d = [&](const glm::vec3& v) { return glm::vec3{glm::dot(v, cd.right), glm::dot(v, cd.up), glm::dot(v, cd.forward)}; };
    const glm::vec3 o = at_d(eye), a = way_d(a_w), b = way_d(b_w), c = way_d(c_w), s = at_d(start), d = way_d(way);
    const auto row = [](const glm::mat4& m, int r) { return glm::vec4{m[0][r], m[1][r], m[2][r], m[3][r]}; };
    float k[14][4]{};
    const auto put = [&](int i, const glm::vec3& v, float w) {
        k[i][0] = v.x;
        k[i][1] = v.y;
        k[i][2] = v.z;
        k[i][3] = w;
    };
    const auto put4 = [&](int i, const glm::vec4& v) {
        k[i][0] = v.x;
        k[i][1] = v.y;
        k[i][2] = v.z;
        k[i][3] = v.w;
    };
    put(0, o, pixel_tan);
    put(1, a, kBeamRadius);
    put(2, b, kBeamOpacity);
    put(3, c, kBeamFadeIn);
    put(4, s, beam.length);
    put(5, d, 0.0f);
    put4(6, row(cd.project, 0));
    put4(7, row(cd.project, 1));
    put4(8, row(cd.project, 3));
    put4(9, row(cd.unproject, 2));
    put4(10, row(cd.unproject, 3));
    put4(11, glm::vec4{(float)extent[1], (float)extent[0], (float)std::max(extent[2], 1u), (float)std::max(extent[3], 1u)});
    // (How much nearer the depth must be to hide it: little with the picture's own depth (the gun hides the beam right at the muzzle),
    // more with another view's or frame's (full stereo's right eye, an older frame); the review.)
    const bool tight = depth_own && !from_left;
    put4(12, glm::vec4{cd.inverted ? 1.0f : 0.0f, cd.forward_sign, tight ? 0.005f : 0.03f, tight ? 0.01f : 0.02f});
    put4(13, glm::vec4{1.0f, 0.04f, 0.02f, encode ? 1.0f : 0.0f});
    // Fresh views in the ring: the depth copy's, the target's.
    const uint32_t slot = m_beam_next++ % (uint32_t)kBeamRing;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = depth_view;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth.Get(), &view, m_beam_views->GetCpuHandle(slot));
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = target_format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target, &rtv, m_beam_targets->GetCpuHandle(slot));
    const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{m_beam_targets->GetCpuHandle(slot)};
    list->OMSetRenderTargets(1, targets, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH};
    const D3D12_RECT scissor{(LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[]{m_beam_views->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m_beam_root.Get());
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, 14 * 4, k, 0);
    list->SetGraphicsRootDescriptorTable(1, m_beam_views->GetGpuHandle(slot));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    std::scoped_lock _{m_mtx};
    ++m_stat_beam_drawn;
    m_beam_drawn_at = now;
    if (m_beam_logs < 4) {
        ++m_beam_logs;
        spdlog::info("[Laser] (fix148) the beam drawn into the picture of frame {} ({}; its depth of frame {}): from {:.2f},{:.2f} to {:.2f},{:.2f} in "
                     "the picture ({}x{}), {:.2f} long",
            frame, from_left ? "full stereo's right eye, from the left eye's camera" : "its own camera", depth_camera.frame, p0.x, p0.y, p1.x, p1.y,
            width, height, beam.length);
    }
    return true;
}

namespace {
} // namespace

LaserSight& LaserSight::get() {
    static LaserSight instance{};
    return instance;
}

void LaserSight::set_ray(bool on, const glm::vec3& origin, const glm::vec3& direction, uint32_t frame) {
    std::scoped_lock _{m_mtx};
    if (!on || !Finite(origin) || !Finite(direction) || glm::length(direction) < 0.5f) {
        m_ray_at = {};
        return;
    }
    m_origin = origin;
    m_direction = glm::normalize(direction);
    m_ray_at = std::chrono::steady_clock::now();
    m_lines[frame % m_lines.size()] = Line{frame, true, m_origin, m_direction};
}

bool LaserSight::wanted() const {
    std::scoped_lock _{m_mtx};
    return m_ray_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - m_ray_at < std::chrono::milliseconds(500);
}

LaserSight::Hit LaserSight::latest() const {
    std::scoped_lock _{m_mtx};
    Hit hit = m_hit;
    hit.fresh = m_hit_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - m_hit_at < std::chrono::milliseconds(250);
    return hit;
}

void LaserSight::on_tags(const void* tags, uint32_t count, void* command_list) {
    if (tags == nullptr || count == 0 || count > 32) {
        return;
    }
    const auto* list = static_cast<const slabi::ResourceTag*>(tags);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* tag = DepthTag(list[i]);
        if (tag == nullptr) {
            continue;
        }
        // (A depth tag with no picture: the game let go of its depth. Nothing kept.)
        if (tag->resource == nullptr || !(tag->resource->structType == slabi::kResourceType) || tag->resource->native == nullptr) {
            t_tag = Tag{};
            return;
        }
        // (The laser not shown: nothing kept, so no old depth of the game's is held through a menu.)
        if (!wanted()) {
            t_tag = Tag{};
            t_pending = Pending{};
            return;
        }
        Tag held{};
        held.depth = static_cast<ID3D12Resource*>(tag->resource->native);
        held.state = tag->resource->state;
        held.lifecycle = tag->lifecycle;
        held.extent[0] = tag->extent.top;
        held.extent[1] = tag->extent.left;
        held.extent[2] = tag->extent.width;
        held.extent[3] = tag->extent.height;
        held.at = std::chrono::steady_clock::now();
        t_tag = held;
        t_pending = Pending{};
        std::scoped_lock _{m_mtx};
        if (!m_tag_logged) {
            m_tag_logged = true;
            const auto desc = held.depth->GetDesc();
            spdlog::info("[Laser] the game's depth tag for DLSS: {}x{} (format {}, flags {:#x}), state {:#x}, lifecycle {} ({}), used part {},{} "
                         "{}x{}, its commands {}",
                (uint64_t)desc.Width, desc.Height, (int)desc.Format, (uint32_t)desc.Flags, held.state, held.lifecycle,
                held.lifecycle == 0 ? "valid only as tagged: copied then" : "valid until DLSS runs: copied then", held.extent[1], held.extent[0],
                held.extent[2], held.extent[3], command_list != nullptr ? "given" : "none");
        }
        // (Valid only as tagged: copied now, on the commands it came with, in the state it gives for now.)
        if (held.lifecycle == 0 && command_list != nullptr) {
            const int index = record_copy(static_cast<ID3D12GraphicsCommandList*>(command_list), held.depth.Get(), held.state, held.extent);
            if (index >= 0) {
                t_pending = Pending{index, held.at};
            }
        }
        return;
    }
}

bool LaserSight::camera_from(const slabi::Constants& values, uint32_t frame, Camera& out, std::string& why) const {
    out = Camera{};
    out.frame = frame;
    const glm::mat4 m = glm::make_mat4(values.cameraViewToClip.m);
    const float* e = values.cameraViewToClip.m;
    const auto near0 = [](float v) { return std::abs(v) < 1e-4f; };
    // Streamline's matrices are row-major for row vectors (clip = v M): a perspective one then has its w column (3, 7, 15) zero
    // but for the view's depth (11). Read as glm's columns, that is clip = M v. The other way round (column vectors): transposed.
    // (Its depth term about 1, as a perspective lens has: a matrix the other way round, with a 0.1 or so there, not mistaken for it.)
    if (near0(e[3]) && near0(e[7]) && near0(e[15]) && std::abs(std::abs(e[11]) - 1.0f) < 0.1f) {
        out.project = m;
        out.forward_sign = e[11] > 0.0f ? 1.0f : -1.0f;
    } else if (near0(e[12]) && near0(e[13]) && near0(e[15]) && std::abs(std::abs(e[14]) - 1.0f) < 0.1f) {
        out.project = glm::transpose(m);
        out.forward_sign = e[14] > 0.0f ? 1.0f : -1.0f;
    } else {
        why = std::format("the lens is not a perspective one as expected ({:.3f} {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} {:.3f})", e[3], e[7], e[11],
            e[15], e[12], e[13], e[14], e[15]);
        return false;
    }
    out.unproject = glm::inverse(out.project);
    out.position = {values.cameraPos.x, values.cameraPos.y, values.cameraPos.z};
    out.right = {values.cameraRight.x, values.cameraRight.y, values.cameraRight.z};
    out.up = {values.cameraUp.x, values.cameraUp.y, values.cameraUp.z};
    out.forward = {values.cameraFwd.x, values.cameraFwd.y, values.cameraFwd.z};
    out.inverted = values.depthInverted == slabi::Boolean::eTrue;
    // The camera's axes: unit lengths, at right angles.
    const auto unit = [](const glm::vec3& v) { return std::abs(glm::length(v) - 1.0f) < 0.02f; };
    if (!Finite(out.position) || !unit(out.right) || !unit(out.up) || !unit(out.forward) || std::abs(glm::dot(out.right, out.up)) > 0.02f ||
        std::abs(glm::dot(out.right, out.forward)) > 0.02f || std::abs(glm::dot(out.up, out.forward)) > 0.02f) {
        why = "the camera's axes are not as expected (not unit lengths at right angles)";
        return false;
    }
    // A point 10 m ahead: in front (w above 0), on the picture; one to the right of it further right, one above it higher; one
    // further ahead deeper (smaller with the depth inverted).
    Sample ahead{}, right{}, above{}, farther{};
    if (!project(out, out.position + out.forward * 10.0f, ahead) || !project(out, out.position + out.forward * 10.0f + out.right, right) ||
        !project(out, out.position + out.forward * 10.0f + out.up, above) || !project(out, out.position + out.forward * 20.0f, farther)) {
        why = "a point ahead of the camera does not come out in front of it";
        return false;
    }
    if (std::abs(ahead.ndc.x) > 0.6f || std::abs(ahead.ndc.y) > 0.6f || right.ndc.x <= ahead.ndc.x || above.ndc.y <= ahead.ndc.y ||
        (out.inverted ? farther.depth >= ahead.depth : farther.depth <= ahead.depth) || ahead.depth < 0.0f || ahead.depth > 1.0f) {
        why = std::format("the camera's numbers do not agree (ahead at {:.2f},{:.2f} depth {:.5f}; to the right x {:.2f}; above y {:.2f}; further depth {:.5f})",
            ahead.ndc.x, ahead.ndc.y, ahead.depth, right.ndc.x, above.ndc.y, farther.depth);
        return false;
    }
    out.valid = true;
    return true;
}

bool LaserSight::project(const Camera& camera, const glm::vec3& point, Sample& out) const {
    const glm::vec3 d = point - camera.position;
    const float ahead = glm::dot(d, camera.forward);
    const glm::vec4 view{glm::dot(d, camera.right), glm::dot(d, camera.up), ahead * camera.forward_sign, 1.0f};
    const glm::vec4 clip = camera.project * view;
    if (!(clip.w > 1e-4f) || !std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z)) {
        return false;
    }
    out.ahead = ahead;
    out.ndc = {clip.x / clip.w, clip.y / clip.w};
    out.depth = clip.z / clip.w;
    return true;
}

void LaserSight::on_constants(const slabi::Constants& values, uint32_t frame) {
    if (!(values.structType == slabi::kConstantsType)) {
        return;
    }
    Camera camera{};
    std::string why{};
    const bool ok = camera_from(values, frame, camera, why);
    std::scoped_lock _{m_mtx};
    auto& slot = m_cameras[frame % m_cameras.size()];
    slot = camera;
    // (fix147) Also the world view's newest (the hooks give only its cameras: in full stereo the left eye's, not the right's), numbered,
    // for a copy whose frame no camera came with (full stereo). (Not usable: none newer to pair with either.)
    m_world = slot;
    m_world.valid = ok;
    if (ok) {
        m_lens = camera; // (fix151: its lens kept for full stereo, where none comes)
        if (const auto& vr = VR::get(); vr != nullptr) {
            m_lens_near = vr->m_nearz;
            m_lens_far = vr->m_farz;
        }
    }
    m_world_at = std::chrono::steady_clock::now();
    ++m_world_serial;
    ++m_stat_cameras;
    if (!ok) {
        slot.valid = false;
        if (m_refusals++ < 4) {
            spdlog::warn("[Laser] frame {}: the game's camera for DLSS is not used for the laser's dot: {}", frame, why);
        }
    } else if (m_check_logs < 1) {
        ++m_check_logs;
        spdlog::info("[Laser] the game's camera for DLSS checked (frame {}): {} vectors, forward {}, depth {}", frame,
            camera.project == glm::make_mat4(values.cameraViewToClip.m) ? "row" : "column", camera.forward_sign > 0.0f ? "+z" : "-z",
            camera.inverted ? "inverted (1 near)" : "not inverted");
    }
}

void LaserSight::on_right_constants(const slabi::Constants& values, uint32_t frame) {
    if (!(values.structType == slabi::kConstantsType)) {
        return;
    }
    Camera camera{};
    std::string why{};
    const bool ok = camera_from(values, frame, camera, why);
    std::scoped_lock _{m_mtx};
    auto& slot = m_right_cameras[frame % m_right_cameras.size()];
    slot = camera;
    slot.valid = ok;
}

int LaserSight::record_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t state, const uint32_t extent[4]) {
    if (list == nullptr || depth == nullptr || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return -1;
    }
    if (!CopyableState(state)) {
        if (!m_state_logged) {
            m_state_logged = true;
            spdlog::warn("[Laser] the game's depth tag gives the state {:#x}, not one it can be copied from as it is: not copied (the dot at the gun "
                         "sight's distance)",
                state);
        }
        return -1;
    }
    const auto desc = depth->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.Width < 320 ||
        desc.Height < 200 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return -1;
    }
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT;
        break;
    default:
        return -1; // (a depth format whose value is not a 32-bit float: not read)
    }
    const auto now = std::chrono::steady_clock::now();
    const int index = m_next_copy;
    auto& copy = m_copies[index];
    bool fresh = false;
    if (copy.texture == nullptr || copy.width != (uint32_t)desc.Width || copy.height != desc.Height || copy.format != desc.Format ||
        copy.texture->GetDesc().MipLevels != desc.MipLevels) {
        // (Not made again and again: once in half a second at most.)
        if (copy.made_at != std::chrono::steady_clock::time_point{} && now - copy.made_at < std::chrono::milliseconds(500)) {
            return -1;
        }
        auto device = g_framework->get_d3d12_hook()->get_device();
        if (device == nullptr) {
            return -1;
        }
        // (The same kind of picture, so the whole of it is copied as it is: a depth picture too, readable by a shader.)
        D3D12_RESOURCE_DESC copy_desc = desc;
        copy_desc.Alignment = 0;
        copy_desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> made{};
        copy.made_at = now;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&made)))) {
            return -1;
        }
        made->SetName(L"Laser sight: the game's depth");
        if (copy.texture != nullptr) {
            m_retired.push_back(Retired{copy.texture, now, VR::get()->m_presents_done.load()});
        }
        const auto made_at = copy.made_at;
        copy = Copy{};
        copy.made_at = made_at;
        copy.texture = made;
        copy.format = desc.Format;
        copy.view_format = view;
        copy.width = (uint32_t)desc.Width;
        copy.height = desc.Height;
        fresh = true;
    }
    // On the game's commands, in the state its tag gives for this moment: copied, both back as they were.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& b : barriers) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    UINT count = 0;
    const auto from = (D3D12_RESOURCE_STATES)state;
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Transition.pResource = depth;
        barriers[count].Transition.StateBefore = from;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++count;
    }
    if (!fresh) {
        barriers[count].Transition.pResource = copy.texture.Get();
        barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        ++count;
    }
    if (count > 0) {
        list->ResourceBarrier(count, barriers);
    }
    list->CopyResource(copy.texture.Get(), depth);
    count = 0;
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Transition.pResource = depth;
        barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[count].Transition.StateAfter = from;
        ++count;
    }
    barriers[count].Transition.pResource = copy.texture.Get();
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ++count;
    list->ResourceBarrier(count, barriers);
    // (Its frame and camera when DLSS runs: finish_copy. Not read until then.)
    copy.has = false;
    copy.read = false;
    std::copy(extent, extent + 4, std::begin(copy.extent));
    if (copy.extent[2] == 0 || copy.extent[3] == 0) {
        copy.extent[0] = copy.extent[1] = 0;
        copy.extent[2] = copy.width;
        copy.extent[3] = copy.height;
    }
    m_next_copy = (m_next_copy + 1) % kCopies;
    ++m_stat_copies;
    return index;
}

bool LaserSight::LensOf(const Camera& camera, glm::vec4& lrtb) {
    // (The picture's corners unprojected at a middling depth, as draw_beam finds them: their tangents.)
    const glm::vec4 mid = camera.project * glm::vec4{0.0f, 0.0f, camera.forward_sign, 1.0f};
    const float z = std::abs(mid.w) > 1e-6f ? mid.z / mid.w : 0.5f;
    const auto corner = [&](float nx, float ny, glm::vec2& out) {
        const glm::vec4 v = camera.unproject * glm::vec4{nx, ny, z, 1.0f};
        const float ahead = std::abs(v.w) > 1e-9f ? v.z / v.w * camera.forward_sign : 0.0f;
        if (!(ahead > 1e-6f)) {
            return false;
        }
        out = glm::vec2{v.x / v.w, v.y / v.w} / ahead;
        return std::isfinite(out.x) && std::isfinite(out.y);
    };
    glm::vec2 tl{}, br{};
    if (!corner(-1.0f, 1.0f, tl) || !corner(1.0f, -1.0f, br)) {
        return false;
    }
    lrtb = glm::vec4{tl.x, br.x, tl.y, br.y};
    return true;
}

bool LaserSight::camera_for(uint32_t frame, Camera& out, bool& by_view, bool* by_right, bool* by_lens) const {
    by_view = false;
    const auto& camera = m_cameras[frame % m_cameras.size()];
    if (camera.valid && camera.frame == frame) {
        out = camera;
        return true;
    }
    // (fix147) Full stereo: the world view's (the left eye's) camera and its depth do not come with the same frame number (fix146's
    // test: no copy there ever found its frame's camera, the last one paired was of the frame before full stereo began). Its scene
    // setup gives DLSS the camera, its DLSS step the depth, in that order, each once a frame: the newest camera, if recent and not
    // paired with an earlier copy, is this one's, given it under the copy's frame. (Mono, stereo and AER: their frame's camera only.)
    const auto& vr = VR::get();
    if (vr == nullptr || !vr->is_native_frame(frame)) {
        return false;
    }
    // (fix151) Full stereo: the right eye's camera of this very frame (its view set up just after the left eye's, before the left eye's
    // DLSS step), moved back along its right by the eyes' distance (StereoView's: the right eye's camera is the left eye's moved so; the
    // same lens and axes): the left eye's. fix149's test had no camera of the left eye's own in full stereo, so the beam and the dot fell
    // back to the old strip and the gun sight's distance.
    // (Only with the left eye's own lens: the same as the camera's, as with the symmetric projection; the review.)
    glm::vec4 left_lens{0.0f};
    {
        auto runtime = vr->get_runtime();
        std::shared_lock _{runtime->eyes_mtx};
        const auto& f = runtime->eye_frustums[0];
        left_lens = glm::vec4{f[0], f[1], f[2], f[3]};
    }
    const auto same_lens = [&](const Camera& c) {
        glm::vec4 t{};
        return LensOf(c, t) && glm::all(glm::lessThan(glm::abs(t - left_lens), glm::vec4{0.01f}));
    };
    if (const auto& right = m_right_cameras[frame % m_right_cameras.size()]; right.valid && right.frame == frame && same_lens(right)) {
        const float apart = vr->m_stereo_eyes_apart.load(std::memory_order_relaxed);
        if (std::isfinite(apart) && apart > 0.0f && apart < 1.0f) {
            out = right;
            out.position = right.position - right.right * apart;
            if (by_right != nullptr) {
                *by_right = true;
            }
            if (by_right != nullptr && !m_by_right_logged) {
                m_by_right_logged = true;
                spdlog::info("[Laser] (fix151) full stereo: the depth of frame {} read with the right eye's camera of that frame moved back {:.4f} "
                             "along its right (the left eye's own camera not given)",
                    frame, apart);
            }
            return true;
        }
    }
    // (fix151, the review) In steady full stereo the game hands DLSS no camera at all (its 30 s lines: no jitter from either eye), so
    // neither eye's comes. The beam and its depth are both read in the frame's own camera's axes (the line is handed over in them, from
    // that eye), so its place and axes cancel out: only its lens is needed, the last one the game gave (the same lens for every eye
    // with the symmetric projection: checked against the left eye's). The camera put at its own origin, its axes the view's.
    // (And the game's near and far as they were: the depth's distances are read with them; the review.)
    const bool same_planes = std::abs(vr->m_nearz - m_lens_near) <= 0.01f * std::max(m_lens_near, 1e-3f) &&
                             std::abs(vr->m_farz - m_lens_far) <= 0.01f * std::max(m_lens_far, 1.0f);
    if (m_lens.valid && same_planes && same_lens(m_lens)) {
        out = m_lens;
        out.frame = frame;
        out.synthetic = true;
        out.position = glm::vec3{0.0f};
        out.right = glm::vec3{1.0f, 0.0f, 0.0f};
        out.up = glm::vec3{0.0f, 1.0f, 0.0f};
        out.forward = glm::vec3{0.0f, 0.0f, 1.0f};
        if (by_lens != nullptr && !m_by_lens_logged) {
            m_by_lens_logged = true;
            spdlog::info("[Laser] (fix151) full stereo: the depth of frame {} read with the last camera's lens at the view's own origin (the game "
                         "gives DLSS no camera in full stereo)",
                frame);
        }
        if (by_lens != nullptr) {
            *by_lens = true;
        }
        return true;
    }
    if (!m_world.valid || m_world_serial == m_paired_serial || std::chrono::steady_clock::now() - m_world_at > kCameraFresh) {
        return false;
    }
    out = m_world;
    out.frame = frame;
    by_view = true;
    return true;
}

void LaserSight::note_no_camera(uint32_t frame) {
    ++m_stat_no_camera;
    const auto& vr = VR::get();
    if (m_no_camera_logs >= 3 || vr == nullptr || !vr->is_native_frame(frame)) {
        return;
    }
    ++m_no_camera_logs;
    const auto& slot = m_cameras[frame % m_cameras.size()];
    const double age = m_world_serial == 0 ? -1.0 : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_world_at).count();
    spdlog::info("[Laser] (fix147) full stereo: no camera for the depth of frame {} (the camera kept in its place is of frame {}{}; the world view's "
                 "newest is of frame {}, {:.1f} ms old: {})",
        frame, slot.frame, slot.valid ? "" : ", not usable", m_world.frame, age,
        m_world_serial == 0                ? "none given yet"
        : !m_world.valid                   ? "not usable"
        : m_world_serial == m_paired_serial ? "already paired with an earlier copy"
                                            : "too old");
}

void LaserSight::finish_copy(int index, uint32_t frame) {
    auto& copy = m_copies[index];
    Camera camera{};
    bool by_view = false, by_right = false, by_lens = false;
    if (!camera_for(frame, camera, by_view, &by_right, &by_lens)) {
        note_no_camera(frame);
        return;
    }
    if (by_right) {
        ++m_stat_by_right;
    }
    if (by_lens) {
        ++m_stat_by_lens;
    }
    // (fix147) The world view's newest camera now paired (not again with a later copy): when it is this copy's own frame's too.
    if (by_view || (m_world.valid && m_world.frame == frame)) {
        m_paired_serial = m_world_serial;
    }
    if (by_view) {
        ++m_stat_by_view;
        if (!m_by_view_logged) {
            m_by_view_logged = true;
            spdlog::info("[Laser] (fix147) full stereo: the depth of frame {} read with the world view's newest camera (given to DLSS for frame {}, "
                         "{:.1f} ms before): the game's camera and depth come with different frame numbers there",
                frame, m_world.frame, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_world_at).count());
        }
    }
    copy.frame = frame;
    copy.camera = camera;
    copy.has = true;
    copy.read = false;
    m_last_copy_frame = frame;
    if (!m_copy_logged) {
        m_copy_logged = true;
        spdlog::info("[Laser] the game's depth copied for DLSS's frame {}: {}x{}, its part used {},{} {}x{} (each frame the laser is shown)", frame,
            copy.width, copy.height, copy.extent[1], copy.extent[0], copy.extent[2], copy.extent[3]);
    }
}

void LaserSight::on_evaluate(void* command_list, uint32_t frame, const void* const* inputs, uint32_t count) {
    const auto now = std::chrono::steady_clock::now();
    Tag tag = t_tag;
    const Pending pending = t_pending;
    t_tag = Tag{};
    t_pending = Pending{};
    if (!wanted()) {
        return;
    }
    // (A depth tag among the evaluation's own inputs: its state is the one now. Preferred.)
    bool local = false;
    if (inputs != nullptr && count <= 16) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto* base = static_cast<const slabi::BaseStructure*>(inputs[i]);
            if (base == nullptr || !(base->structType == slabi::kResourceTagType)) {
                continue;
            }
            const auto* found = DepthTag(*static_cast<const slabi::ResourceTag*>(inputs[i]));
            if (found != nullptr && found->resource != nullptr && found->resource->structType == slabi::kResourceType &&
                found->resource->native != nullptr) {
                tag = Tag{};
                tag.depth = static_cast<ID3D12Resource*>(found->resource->native);
                tag.state = found->resource->state;
                tag.lifecycle = 1;
                tag.extent[0] = found->extent.top;
                tag.extent[1] = found->extent.left;
                tag.extent[2] = found->extent.width;
                tag.extent[3] = found->extent.height;
                tag.at = now;
                local = true;
                break;
            }
        }
    }
    std::scoped_lock _{m_mtx};
    // (Copied as it was tagged, just before: its frame and camera now (also when the evaluation passes a depth tag of its own: the
    // copy has what DLSS gets).)
    if (pending.index >= 0 && now - pending.at < kTagFresh) {
        finish_copy(pending.index, frame);
        return;
    }
    // (Else, a tag valid until DLSS runs, from just before: copied now, in its state for now.)
    if (tag.depth == nullptr || now - tag.at > kTagFresh || (!local && tag.lifecycle == 0)) {
        ++m_stat_no_tag;
        return;
    }
    // (fix147) No camera to read it with: not copied (the whole depth copied each frame, never read).
    {
        Camera camera{};
        bool by_view = false;
        if (!camera_for(frame, camera, by_view)) {
            note_no_camera(frame);
            return;
        }
    }
    const int index = record_copy(static_cast<ID3D12GraphicsCommandList*>(command_list), tag.depth.Get(), tag.state, tag.extent);
    if (index >= 0) {
        finish_copy(index, frame);
    }
}

bool LaserSight::setup_readings() {
    if (m_readings_ready || m_readings_failed) {
        return m_readings_ready;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    m_readings_failed = true; // (unless all of it is made)
    try {
        m_views = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            (size_t)kCopies);
    } catch (...) {
        spdlog::error("[Laser] its views' table could not be made: the dot stays at the gun sight's distance");
        return false;
    }
    for (auto& reading : m_readings) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kSamples;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D12Resource> target{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target))) ||
            !reading.target.setup(device, target.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, L"Laser sight: a reading")) {
            spdlog::error("[Laser] a reading's picture could not be made: the dot stays at the gun sight's distance");
            return false;
        }
        D3D12_HEAP_PROPERTIES back_heap{};
        back_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC back{};
        back.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        back.Width = (UINT64)kSamples * 4;
        back.Height = 1;
        back.DepthOrArraySize = 1;
        back.MipLevels = 1;
        back.Format = DXGI_FORMAT_UNKNOWN;
        back.SampleDesc.Count = 1;
        back.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&back_heap, D3D12_HEAP_FLAG_NONE, &back, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&reading.readback)))) {
            spdlog::error("[Laser] a reading's read-back buffer could not be made: the dot stays at the gun sight's distance");
            return false;
        }
        reading.readback->SetName(L"Laser sight: a reading read back");
    }
    static_assert(kSamples * 4 % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);
    m_readings_failed = false;
    m_readings_ready = true;
    return true;
}

void LaserSight::finish(Reading& reading, const uint8_t* bytes) {
    // Each pixel: the depth's four bytes (0 off the picture, or where nothing is drawn: no hit there).
    float depths[kSamples]{};
    std::memcpy(depths, bytes, sizeof(depths));
    const auto& camera = reading.camera;
    // Where the line crosses behind what is drawn: the point before in front of it, this one behind it by no more than the step
    // between them (and a little): a surface it meets, not something nearer that only hides it from the eye.
    float last_gap = -1.0f;
    bool last_known = false;
    float last_distance = 0.0f;
    for (int i = 0; i < kSamples; ++i) {
        const auto& s = reading.samples[i];
        if (!s.on_picture) {
            if (i > 0 && reading.samples[i - 1].on_picture) {
                break; // (off the picture: nothing further is known)
            }
            continue;
        }
        const float scene = depths[i];
        // (Nothing drawn there (the sky), or your own gun, hands or arms: the line's point is in front of what is there.)
        const auto in_front = [&]() {
            last_gap = -1000.0f;
            last_known = true;
            last_distance = s.distance;
        };
        if (!std::isfinite(scene) || (camera.inverted ? !(scene > 0.0f) : !(scene < 1.0f))) {
            in_front();
            continue;
        }
        // The scene's distance along the camera's forward there.
        const glm::vec4 view = camera.unproject * glm::vec4{s.ndc.x, s.ndc.y, scene, 1.0f};
        if (!(std::abs(view.w) > 1e-6f)) {
            last_known = false;
            continue;
        }
        const float scene_ahead = view.z / view.w * camera.forward_sign;
        if (!(scene_ahead > kOwnBody)) {
            in_front();
            continue;
        }
        const float gap = s.ahead - scene_ahead; // above 0: the line's point is behind what is drawn
        if (gap > 0.0f) {
            const float step = i > 0 ? s.distance - reading.samples[i - 1].distance : 0.0f;
            if (gap <= step * 1.25f + 0.1f && (i == 0 || (last_known && last_gap <= 0.0f))) {
                // (Between the two points: where the gap would be 0.)
                float at = s.distance;
                if (i > 0 && last_known && gap - last_gap > 1e-4f) {
                    at = last_distance + (s.distance - last_distance) * (-last_gap) / (gap - last_gap);
                }
                std::scoped_lock _{m_mtx};
                m_hit = Hit{true, true, at};
                m_hit_at = std::chrono::steady_clock::now();
                ++m_stat_hits;
                if (!m_first_hit_logged) {
                    m_first_hit_logged = true;
                    spdlog::info("[Laser] the laser's line met what is drawn {:.2f} m along it (the first time; frame {})", at, camera.frame);
                }
                return;
            }
        }
        last_gap = gap;
        last_known = true;
        last_distance = s.distance;
    }
    std::scoped_lock _{m_mtx};
    m_hit = Hit{true, false, 0.0f};
    m_hit_at = std::chrono::steady_clock::now();
    ++m_stat_misses;
}

void LaserSight::on_present(DirectX::DX12::SpriteBatch* batch, uint32_t present_frame) {
    const auto now = std::chrono::steady_clock::now();
    // Readings finished: read (the GPU done with them: never waited for).
    for (auto& reading : m_readings) {
        if (!reading.busy) {
            continue;
        }
        auto& commands = reading.target.commands;
        if (commands.fence == nullptr || commands.fence->GetCompletedValue() < reading.fence_value) {
            continue;
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{0, (SIZE_T)kSamples * 4};
        if (SUCCEEDED(reading.readback->Map(0, &range, &mapped)) && mapped != nullptr) {
            uint8_t bytes[kSamples * 4]{};
            std::memcpy(bytes, mapped, sizeof(bytes));
            const D3D12_RANGE none{0, 0};
            reading.readback->Unmap(0, &none);
            finish(reading, bytes);
        }
        commands.wait(0); // (done: its commands made ready again)
        reading.busy = false;
    }
    {
        std::scoped_lock _{m_mtx};
        // Copies let go a while ago: no longer in use on the GPU (1.5 s and 8 presents on).
        const uint32_t presents = VR::get()->m_presents_done.load();
        std::erase_if(m_retired, [&](const Retired& r) {
            return (now - r.at > std::chrono::milliseconds(1500) && presents - r.presents >= 8) || now - r.at > std::chrono::seconds(10);
        });
        m_last_present_frame = present_frame;
        // (Not shown for 5 s (a menu open that long: its video memory needed there), the copies let go, their memory with them; a
        // short look at a menu keeps them, not made again as it closes.)
        const bool shown = m_ray_at != std::chrono::steady_clock::time_point{} && now - m_ray_at < std::chrono::milliseconds(500);
        if (shown) {
            m_unwanted_since = {};
        } else if (m_unwanted_since == std::chrono::steady_clock::time_point{}) {
            m_unwanted_since = now;
        } else if (now - m_unwanted_since > std::chrono::seconds(5)) {
            bool busy = false;
            for (const auto& r : m_readings) {
                busy = busy || r.busy;
            }
            for (auto& copy : m_copies) {
                if (copy.texture != nullptr && !busy) {
                    m_retired.push_back(Retired{copy.texture, now, presents});
                    copy = Copy{};
                }
            }
        }
        // (Every 10 s while in use, for the log.)
        if (m_stats_at == std::chrono::steady_clock::time_point{}) {
            m_stats_at = now;
        } else if (now - m_stats_at > std::chrono::seconds(10)) {
            m_stats_at = now;
            if (m_stat_copies + m_stat_no_tag + m_stat_no_camera > 0) {
                spdlog::info("[Laser] last 10 s: {} depth copies, {} readings: the line met something {} times, nothing {} times; no depth tag {} "
                             "times, no camera {} times (the last copy's frame {}, the frame presented {}); {} cameras given, {} copies read with the "
                             "world view's newest camera, {} with the right eye's moved back, {} with the last lens at the view's origin (full stereo)",
                    m_stat_copies, m_stat_readings, m_stat_hits, m_stat_misses, m_stat_no_tag, m_stat_no_camera, m_last_copy_frame, m_last_present_frame,
                    m_stat_cameras, m_stat_by_view, m_stat_by_right, m_stat_by_lens);
            }
            if (m_stat_beam_drawn + m_stat_beam_no_camera + m_stat_beam_no_line + m_stat_beam_off_view > 0) {
                spdlog::info("[Laser] (fix148) last 10 s, the beam: drawn into {} pictures, out of view in {}; not drawn: no camera or depth for "
                             "the picture's frame {} times, no beam line for it {} times (the headset's strip then)",
                    m_stat_beam_drawn, m_stat_beam_off_view, m_stat_beam_no_camera, m_stat_beam_no_line);
            }
            m_stat_beam_drawn = m_stat_beam_no_camera = m_stat_beam_no_line = m_stat_beam_off_view = 0;
            m_status = std::format("Laser sight: {} readings, {} hits in the last 10 s", m_stat_readings, m_stat_hits);
            m_stat_copies = m_stat_readings = m_stat_hits = m_stat_misses = m_stat_no_tag = m_stat_no_camera = 0;
            m_stat_cameras = m_stat_by_view = m_stat_by_right = m_stat_by_lens = 0;
        }
    }
    if (batch == nullptr || !wanted() || !setup_readings()) {
        return;
    }
    Reading* free = nullptr;
    for (auto& reading : m_readings) {
        if (!reading.busy) {
            free = &reading;
            break;
        }
    }
    if (free == nullptr) {
        return;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return;
    }
    // The newest copy of a frame being presented (or the one before: its commands submitted before this present's, never one the
    // game's commands have not run yet), not read yet; all that is used of it taken now, under the lock (the game's thread writes
    // the copies).
    int newest = -1;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture{};
    uint32_t width = 0, height = 0, extent[4]{};
    Camera camera{};
    glm::vec3 origin{}, direction{};
    {
        std::scoped_lock _{m_mtx};
        for (int i = 0; i < kCopies; ++i) {
            const auto& c = m_copies[i];
            if (!c.has || c.read || c.texture == nullptr || !c.camera.valid || (int32_t)(present_frame - c.frame) < 0 || present_frame - c.frame > 1) {
                continue;
            }
            if (newest < 0 || (int32_t)(c.frame - m_copies[newest].frame) > 0) {
                newest = i;
            }
        }
        if (newest < 0) {
            return;
        }
        auto& copy = m_copies[newest];
        // (The line starts at the gun, near the eye: its offset from the eye no more than a few metres, or it is not what it seems.)
        if (glm::length(m_origin) > kNearCamera) {
            copy.read = true;
            if (!m_far_logged) {
                m_far_logged = true;
                spdlog::warn("[Laser] the laser's start is {:.2f} m from your eye ({:.2f} {:.2f} {:.2f} in its axes): not used, the dot stays at the "
                             "gun sight's distance",
                    glm::length(m_origin), m_origin.x, m_origin.y, m_origin.z);
            }
            return;
        }
        // (Its view made again (a new copy, of another size): not while a reading of its old one may still run.)
        if (copy.view_of != copy.texture.Get()) {
            for (const auto& r : m_readings) {
                if (r.busy && r.copy_index == newest) {
                    return;
                }
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = copy.view_format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            view.Texture2D.PlaneSlice = 0;
            device->CreateShaderResourceView(copy.texture.Get(), &view, m_views->GetCpuHandle((size_t)newest));
            copy.view_of = copy.texture.Get();
        }
        copy.read = true;
        texture = copy.texture;
        width = copy.width;
        height = copy.height;
        std::copy(std::begin(copy.extent), std::end(copy.extent), std::begin(extent));
        camera = copy.camera;
        // (The line in the renderer's space: from that frame's camera, along its own axes, as handed over from your eye.)
        const auto& c = copy.camera;
        // (The line taken with that frame's head pose, as its camera was: else the newest before it, else the newest.)
        glm::vec3 lo = m_origin, ld = m_direction;
        const Line* best = nullptr;
        for (const auto& line : m_lines) {
            if (line.valid && (int32_t)(c.frame - line.frame) >= 0 && (best == nullptr || (int32_t)(line.frame - best->frame) > 0)) {
                best = &line;
            }
        }
        if (best != nullptr && c.frame - best->frame <= 4) {
            lo = best->origin;
            ld = best->direction;
        }
        origin = c.position + c.right * lo.x + c.up * lo.y + c.forward * lo.z;
        direction = glm::normalize(c.right * ld.x + c.up * ld.y + c.forward * ld.z);
    }
    // The line's points, projected with the camera the copy's frame was drawn with.
    Reading& reading = *free;
    reading.camera = camera;
    for (int i = 0; i < kSamples; ++i) {
        auto& s = reading.samples[i];
        s = Sample{};
        s.distance = Distance(i, kSamples);
        Sample p{};
        if (!project(camera, origin + direction * s.distance, p) || std::abs(p.ndc.x) > 1.0f || std::abs(p.ndc.y) > 1.0f) {
            continue;
        }
        p.distance = s.distance;
        p.on_picture = true;
        s = p;
    }
    auto& commands = reading.target.commands;
    auto* list = commands.cmd_list.Get();
    if (list == nullptr) {
        return;
    }
    // (Cleared first: the batch draws without blending, but nothing of an earlier reading may be left.)
    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)kSamples, 1.0f, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH};
    const D3D12_RECT scissor{0, 0, kSamples, 1};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[] = {reading.target.get_rtv()};
    const float zero[4]{0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtv[0], zero, 0, nullptr);
    list->OMSetRenderTargets(1, rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {m_views->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    batch->SetViewport(viewport);
    batch->Begin(list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Deferred);
    const float u0 = (float)extent[1], v0 = (float)extent[0], uw = (float)extent[2], vh = (float)extent[3];
    for (int i = 0; i < kSamples; ++i) {
        const auto& s = reading.samples[i];
        float x = -1.0f, y = -1.0f; // (off the picture: the shader writes 0)
        if (s.on_picture) {
            x = std::floor(u0 + (0.5f + 0.5f * s.ndc.x) * uw) + 0.5f;
            y = std::floor(v0 + (0.5f - 0.5f * s.ndc.y) * vh) + 0.5f;
        }
        const RECT dest{i, 0, i + 1, 1};
        batch->Draw(m_views->GetGpuHandle((size_t)newest), DirectX::XMUINT2{width, height}, dest, DirectX::XMVectorSet(x, y, 0.0f, 1.0f));
    }
    batch->End();
    // Into the read-back buffer, the picture back as a target.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = reading.target.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = reading.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kSamples;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kSamples * 4;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = reading.target.texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    commands.has_commands = true;
    commands.execute();
    // (Its commands could not be closed or sent: the readings stop, rather than read a buffer never written.)
    if (!commands.waiting_for_fence) {
        spdlog::error("[Laser] a reading's commands could not be sent: the laser's dot stays at the gun sight's distance");
        commands.reset();
        std::scoped_lock _{m_mtx};
        m_readings_ready = false;
        m_readings_failed = true;
        return;
    }
    reading.fence_value = commands.fence_value;
    reading.copy_index = newest;
    reading.busy = true;
    std::scoped_lock _{m_mtx};
    ++m_stat_readings;
}

void LaserSight::release() {
    for (auto& reading : m_readings) {
        if (reading.busy) {
            reading.target.commands.wait(2000);
            reading.busy = false;
        }
        reading.target.reset();
        reading.readback.Reset();
    }
    std::scoped_lock _{m_mtx};
    m_readings_ready = false;
    m_readings_failed = false;
    m_views.reset();
    // (The copies: the game's commands may still copy into them. Let go as any copy is, a while on.)
    const auto now = std::chrono::steady_clock::now();
    const uint32_t presents = VR::get()->m_presents_done.load();
    for (auto& copy : m_copies) {
        if (copy.texture != nullptr) {
            m_retired.push_back(Retired{copy.texture, now, presents});
        }
        copy = Copy{};
    }
}

std::string LaserSight::status() const {
    std::scoped_lock _{m_mtx};
    return m_status;
}
} // namespace vrmod
