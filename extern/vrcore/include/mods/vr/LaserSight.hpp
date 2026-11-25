#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include <DescriptorHeap.h>
#include <SpriteBatch.h>
#include <glm/glm.hpp>

#include "d3d12/TextureContext.hpp"


namespace slabi {
struct Constants;
}

namespace vrmod {
// The gun's laser sight (fix129, as Resident Evil 4 VR's): where the barrel's line meets what is drawn, found in the game's own
// depth picture.
//
// Every frame the game hands DLSS its depth picture (a tag) and the camera it drew the frame with (its constants: the lens, the
// camera's place and axes). When DLSS runs for the game's view the depth is complete: it is copied then, on the game's own commands
// (as the menu panel copies its 3D pictures' depth), with that frame's camera. At each present, on the mod's own commands, the copy
// is read at 64 points along the laser's line (the line projected with that camera) into a small picture, read back a frame or two
// later (never waited for). The first point where the line passes behind what is drawn is where it hits: a distance along the line,
// which the game side puts on its line as it is then. Without DLSS (TAA, FSR, behind menus) there is no depth: none recent.
class LaserSight {
public:
    static LaserSight& get();

    // (The game's thread, each frame the laser is shown.) Its line from the eye the frame is drawn from: its start as an offset in
    // the eye's axes (x right, y up, z ahead; the game's units: metres) and its way in them. Off: no depth copied.
    // frame: the game's frame whose head pose it was taken with (each copy is read with its own frame's line: fix131's review).
    void set_ray(bool on, const glm::vec3& origin, const glm::vec3& direction, uint32_t frame);

    struct Hit {
        bool fresh{false};    // a reading from the last quarter second
        bool hit{false};      // ...that found where the line meets what is drawn
        float distance{0.0f}; // ...this far along it (metres)
    };
    Hit latest() const;

    // (The game's render threads, from the DLSS hooks: the game's own view only.)
    void on_tags(const void* tags, uint32_t count, void* command_list);
    void on_constants(const slabi::Constants& values, uint32_t frame);
    // (fix151) Full stereo's right eye's camera (its view's DLSS constants): the left eye's is it moved back by the eyes' distance, when
    // the left eye's own does not come (fix149's test: none came in full stereo).
    void on_right_constants(const slabi::Constants& values, uint32_t frame);
    void on_evaluate(void* command_list, uint32_t frame, const void* const* inputs, uint32_t count);

    // (fix148, the game's thread, each frame the laser is shown) Its beam, as the line is: its start and way in the eye's axes, how far
    // it is seen (the game's units). Drawn into the eye pictures (draw_beam), hidden where the game's depth has something nearer.
    void set_beam(bool on, const glm::vec3& start, const glm::vec3& direction, float length, uint32_t frame);
    // (fix148, the present thread) The beam drawn into a headset image the picture of `frame` was just copied into (in the render target
    // state; its own size, the picture's). `from_left`: full stereo's right eye, whose camera is the frame's (the left eye's) moved
    // `eyes_apart` along its right, with its lens `frustum` (tan left, right, up, down); else the frame's own camera. False: not drawn
    // (no beam, or no camera or depth for that frame).
    bool draw_beam(ID3D12GraphicsCommandList* list, ID3D12Resource* target, uint32_t frame, bool from_left, const float frustum[4], float eyes_apart);
    // A beam drawn into the pictures lately (the last 150 ms): the headset's own strip for it not needed.
    bool beam_drawn_lately() const;
    // (fix148, full stereo) The beam can be drawn into the pictures of `frame` (its line and its own depth copy there): asked for both
    // eyes' frames first, so it is drawn in both eyes or in neither (never in one eye alone; the review).
    bool beam_ready(uint32_t frame) const;

    // (The present thread.) Readings finished read; a new one made of the newest copy (with the batch that packs depths).
    void on_present(DirectX::DX12::SpriteBatch* batch, uint32_t present_frame);
    bool wanted() const;
    void release();
    std::string status() const;

private:
    LaserSight() = default;

    static constexpr int kSamples = 64;
    static constexpr int kCopies = 2; // (each the game's whole depth, about 34 MB: a reading reads the newest, its slot written two frames on)
    static constexpr int kReadings = 3;

    // A frame's camera, as the game gave DLSS it.
    struct Camera {
        uint32_t frame{0};
        bool valid{false};
        glm::mat4 project{1.0f}; // the camera's view (x right, y up, z forward times `forward_sign`) to clip
        glm::mat4 unproject{1.0f};
        glm::vec3 position{0.0f}, right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
        float forward_sign{1.0f};
        bool inverted{true}; // depth 1 near, 0 far
        bool synthetic{false}; // (fix151) full stereo's: the last lens at the view's own origin (never mixed with a real camera)
    };
    struct Copy {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
        uint32_t width{0}, height{0};
        uint32_t frame{0};
        bool has{false};
        bool read{false};             // (a reading made of it)
        Camera camera{};
        uint32_t extent[4]{};         // top, left, width, height of the picture's part used (DLSS's: the frame's size)
        ID3D12Resource* view_of{nullptr}; // (its view in the table made for this texture)
        std::chrono::steady_clock::time_point made_at{}; // (made or tried last: not again and again)
    };
    struct Sample {
        float distance{0.0f}; // along the line
        float ahead{0.0f};    // the point's distance along the camera's forward
        glm::vec2 ndc{0.0f};
        float depth{0.0f};    // the point's own depth in the picture
        bool on_picture{false};
    };
    struct Reading {
        d3d12::TextureContext target{}; // 64 x 1, each pixel a depth's 4 bytes
        Microsoft::WRL::ComPtr<ID3D12Resource> readback{};
        bool busy{false};
        uint64_t fence_value{0};
        int copy_index{-1}; // (the copy it reads: its view not made again while it runs)
        Camera camera{};
        std::array<Sample, kSamples> samples{};
    };
    struct Retired {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture{};
        std::chrono::steady_clock::time_point at{};
        uint32_t presents{0};
    };

    bool camera_from(const slabi::Constants& values, uint32_t frame, Camera& out, std::string& why) const;
    bool project(const Camera& camera, const glm::vec3& point, Sample& out) const;
    void finish(Reading& reading, const uint8_t* bytes);
    // (Under m_mtx.) The depth copied on the game's commands into the next copy; its index, or -1.
    int record_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t state, const uint32_t extent[4]);
    // (Under m_mtx.) A copy made: its frame and that frame's camera.
    void finish_copy(int index, uint32_t frame);
    // (Under m_mtx.) The camera a depth copy of this frame is read with: the one the game gave DLSS for that frame; in full stereo, else
    // the world view's newest one not paired yet, given it under this frame (by_view: fix147). False: none (the copy not made, or not used).
    bool camera_for(uint32_t frame, Camera& out, bool& by_view, bool* by_right = nullptr, bool* by_lens = nullptr) const;
    // (fix151) A camera's lens as the tangents of its picture's edges (left, right, up, down; as the headset's frustums).
    static bool LensOf(const Camera& camera, glm::vec4& lrtb);
    // (Under m_mtx.) No camera for a copy of this frame: counted, and the first few in full stereo logged with what there was.
    void note_no_camera(uint32_t frame);
    bool setup_readings();

    mutable std::mutex m_mtx{};
    // The line (the game's thread): from the eye, in its axes.
    glm::vec3 m_origin{0.0f}, m_direction{0.0f, 0.0f, 1.0f}; // (the newest)
    struct Line {
        uint32_t frame{0};
        bool valid{false};
        glm::vec3 origin{0.0f}, direction{0.0f, 0.0f, 1.0f};
    };
    std::array<Line, 8> m_lines{}; // (by frame)
    // (fix148) The beam by frame (its start and way in the eye's axes, as the line), and what draws it.
    struct Beam {
        uint32_t frame{0};
        bool valid{false};
        glm::vec3 start{0.0f}, direction{0.0f, 0.0f, 1.0f};
        float length{0.0f};
    };
    std::array<Beam, 8> m_beams{};
    std::chrono::steady_clock::time_point m_beam_at{};
    // (The pipeline for a render target format, made the first time: kept, one for each format, never let go while in use; null: none.)
    ID3D12PipelineState* setup_beam(DXGI_FORMAT target_format);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_beam_root{};
    Microsoft::WRL::ComPtr<ID3DBlob> m_beam_vs{}, m_beam_ps{};
    struct BeamPipeline {
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso{};
    };
    std::array<BeamPipeline, 4> m_beam_psos{};
    bool m_beam_failed{false};
    std::unique_ptr<DirectX::DescriptorHeap> m_beam_views{}; // (a ring: a fresh view each draw, never one the GPU may still read)
    std::unique_ptr<DirectX::DescriptorHeap> m_beam_targets{};
    uint32_t m_beam_next{0};
    std::chrono::steady_clock::time_point m_beam_drawn_at{};
    uint32_t m_stat_beam_drawn{0}, m_stat_beam_no_camera{0}, m_stat_beam_no_line{0}, m_stat_beam_off_view{0};
    int m_beam_logs{0};
    std::chrono::steady_clock::time_point m_ray_at{};
    // The game's view's cameras by frame, and its depth tag (each frame: its DLSS step tags, then evaluates, on one thread).
    std::array<Camera, 4> m_cameras{};
    std::array<Camera, 4> m_right_cameras{}; // (fix151) full stereo's right eye's, by frame
    mutable uint32_t m_stat_by_right{0}; // (counted where it is chosen: camera_for, under the lock)
    mutable bool m_by_right_logged{false};
    Camera m_lens{};                       // (fix151) the last camera the game gave that was usable: its lens for full stereo
    float m_lens_near{0.0f}, m_lens_far{0.0f}; // (fix151) the game's near and far as that lens was given (the depth's distances by them)
    mutable bool m_by_lens_logged{false};
    uint32_t m_stat_by_lens{0};
    // (fix147) Full stereo: the game's camera and depth for the world's view (the left eye) did not come with the same frame number
    // (fix146's test: a camera never found for a copy's frame), so a copy there is paired with the world view's newest camera (its
    // DLSS step comes after its scene setup, which gives the camera), once: each camera numbered, the last one paired kept.
    Camera m_world{};
    std::chrono::steady_clock::time_point m_world_at{};
    uint64_t m_world_serial{0};
    uint64_t m_paired_serial{0};
    std::array<Copy, kCopies> m_copies{};
    std::vector<Retired> m_retired{};
    int m_next_copy{0};
    std::array<Reading, kReadings> m_readings{};
    bool m_readings_ready{false};
    bool m_readings_failed{false};
    std::unique_ptr<DirectX::DescriptorHeap> m_views{};
    // The result.
    Hit m_hit{};
    std::chrono::steady_clock::time_point m_hit_at{};
    // For the log.
    int m_check_logs{0};
    bool m_tag_logged{false};
    bool m_copy_logged{false};
    bool m_first_hit_logged{false};
    bool m_far_logged{false};
    bool m_state_logged{false};
    bool m_by_view_logged{false};
    int m_no_camera_logs{0};
    int m_refusals{0};
    std::chrono::steady_clock::time_point m_stats_at{};
    std::chrono::steady_clock::time_point m_unwanted_since{};
    uint32_t m_last_copy_frame{0}, m_last_present_frame{0};
    uint32_t m_stat_copies{0}, m_stat_readings{0}, m_stat_hits{0}, m_stat_misses{0}, m_stat_no_tag{0}, m_stat_no_camera{0};
    uint32_t m_stat_cameras{0}, m_stat_by_view{0}; // (fix147: the cameras the game gave; copies paired with the world view's newest)
    std::string m_status{"not used yet"};
};
} // namespace vrmod
