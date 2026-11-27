// vrcore - framework core: D3D12 present hook, window message hook, ImGui menu, mods and config.
// Structure follows REFramework's REFramework class (MIT, praydog), trimmed to D3D12 and
// stripped of RE Engine specifics.
#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <GraphicsMemory.h>

#include "D3D12Hook.hpp"
#include "Mods.hpp"
#include "WindowsMessageHook.hpp"
#include "mods/vr/d3d12/CommandContext.hpp"
#include "sdk/Math.hpp"

class Framework {
public:
    enum class RendererType : uint8_t {
        D3D11,
        D3D12,
    };

    // module may be null; the framework then locates its own DLL.
    Framework(HMODULE module);
    virtual ~Framework();

    bool is_valid() const { return m_valid; }
    bool is_dx11() const { return false; }
    bool is_dx12() const { return true; }
    RendererType get_renderer_type() const { return RendererType::D3D12; }

    HMODULE get_framework_module() const { return m_framework_module; }
    HMODULE get_game_module() const { return m_game_module; }
    const char* get_game_name() const { return m_game_name.c_str(); }

    // <game folder>/sfvr - logs, config and runtime binding files.
    static std::filesystem::path get_persistent_dir();
    static std::filesystem::path get_persistent_dir(const std::string& file) { return get_persistent_dir() / file; }

    auto& get_d3d12_hook() { return m_d3d12_hook; }
    std::recursive_mutex& get_hook_monitor_mutex() { return m_hook_monitor_mutex; }

    HWND get_window() const { return m_wnd; }

    bool is_drawing_ui() const { return m_draw_ui; }
    void set_draw_ui(bool state, bool should_save = true);

    uint32_t get_rendertarget_width_d3d12() const { return m_d3d12.rt_width; }
    uint32_t get_rendertarget_height_d3d12() const { return m_d3d12.rt_height; }

    const Mods* get_mods() const { return m_mods.get(); }

    // (fix196) The menu in the headset on a panel of its own (an OpenXR quad fixed in the room, in both eyes whatever the eye mode): drawn
    // into a picture of its own instead of into the game's. The picture drawn this present (null when it was not: the menu closed, not in
    // the headset, or drawn into the game's picture as before); the panel's height and distance (metres); a count of the times it is to be
    // put in front of you again (each opening, the "Centre this menu" button); the hand that points at it (-1: none yet). Present thread
    // (the picture); the others any thread.
    ID3D12Resource* headset_menu_texture() const { return m_menu_tex_drawn ? m_d3d12.menu_tex.Get() : nullptr; }
    float headset_menu_height() const { return m_menu_headset_height.load(); }
    float headset_menu_distance() const { return m_menu_headset_distance.load(); }
    uint32_t headset_menu_place_serial() const { return m_menu_place_serial.load(); }
    int headset_menu_hand() const { return m_vr_mouse_hand.load(); }

    // Builds the ImGui frame and runs Mod::on_frame. Call from the game thread once per frame.
    // When from_present is true (no game hook available) mod frame callbacks are skipped.
    void run_imgui_frame(bool from_present);

    void on_frame_d3d12();
    void on_post_present_d3d12();
    void on_reset();
    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param);

    bool hook_d3d12();

    void save_config();
    void request_save_config() { m_wants_save_config = true; }

private:
    bool initialize();
    bool first_frame_initialize();
    bool initialize_windows_message_hook();
    bool init_d3d12();
    void deinit_d3d12();
    void draw_ui();
    void consume_input();
    void hook_monitor();
    void set_imgui_style();

    HMODULE m_framework_module{};
    HMODULE m_game_module{};
    HWND m_wnd{};
    std::string m_game_name{"Starfield"};

    std::unique_ptr<Mods> m_mods{};
    std::unique_ptr<D3D12Hook> m_d3d12_hook{};
    std::unique_ptr<WindowsMessageHook> m_windows_message_hook{};

    std::recursive_mutex m_imgui_mtx{};
    std::recursive_mutex m_hook_monitor_mutex{};
    std::unique_ptr<std::jthread> m_d3d_monitor_thread{};

    std::chrono::steady_clock::time_point m_last_present_time{};
    std::chrono::steady_clock::time_point m_last_message_time{};

    std::string m_error{};

    std::atomic<bool> m_valid{false};
    bool m_initialized{false};
    bool m_first_frame{true};
    bool m_first_frame_d3d_initialize{true};
    bool m_mods_initialized{false};
    bool m_mods_fully_initialized{false};
    bool m_has_frame{false};
    bool m_message_hook_requested{false};
    bool m_draw_ui{false};
    bool m_last_draw_ui{false};
    std::atomic<bool> m_wants_save_config{false};
    std::atomic<bool> m_terminating{false};
    uint32_t m_frames_since_init{0};

    void render_menu(ID3D12Device* device, bool in_headset = false);
    void apply_ui_scale();
    // (fix196) The menu's own rows: its size and distance in the headset (Main tab), the rest (Advanced tab).
    void draw_menu_main_rows();
    void draw_menu_advanced_rows();
    // (fix198, the review) Advanced: "Show the main VR settings here too" and, with it on, the main VR settings in a group.
    void draw_main_rows_here();
    // (fix196) Draws this frame's menu into `target` (its view `rtv`; resting in `state` before and after), the part of the menu's canvas
    // from (x, y), w by h pixels, over a cleared, see-through picture if `clear`. One command list and one ImGui draw each call (their
    // rings advance together: each ImGui buffer is reused only after the list that last read it is done).
    bool draw_menu_into(ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_RESOURCE_STATES state, float x, float y, float w, float h, bool clear);
    // (fix196) The headset panel's picture of the menu, made (again) at the canvas's size, and drawn.
    bool render_menu_texture(ID3D12Device* device);
    // (fix196) The headset's panel can show the menu now (an OpenXR headset in use, a backbuffer format the picture can be made in, the
    // runtime has not refused the panel).
    bool headset_menu_possible() const;
    // (fix196) Where the menu is laid out for the headset's panel (the game thread): its size, and its place in the game window's part on
    // the desktop, so the mouse can reach all of it there too.
    void update_menu_canvas();
    // (fix196) The controllers' aim on the headset's panel turned into ImGui's mouse (the game thread, before ImGui's new frame).
    void feed_headset_pointer();
    void release_headset_pointer();
    // (fix196) Waits until the graphics card has done everything handed to the game's queue so far (before a picture it may read is let go);
    // false when that could not be made sure of (a timeout, no queue, the game closing): the picture is then kept (retire_menu_texture).
    bool wait_for_gpu();
    // (fix196) Lets the headset panel's picture go once the graphics card is surely done with it; else keeps it (with any kept before) until
    // a later wait finishes, or for good as the game closes.
    void retire_menu_texture();

    bool m_menu_in_headset{true}; // draw the menu before the headset copy so it shows in VR
    // (fix198, the review) Advanced: the main VR settings shown here too (they are in the game's Settings > VR tab), off by default.
    bool m_main_rows_here{false};
    float m_ui_scale_setting{0.0f}; // 0 = automatic (follows the backbuffer height)
    float m_applied_ui_scale{-1.0f};
    bool m_menu_layout_dirty{true}; // re-centre and resize the menu on the next draw
    float m_menu_layout_w{0.0f};
    float m_menu_layout_h{0.0f};
    float m_menu_layout_scale{0.0f};

    // (fix196) The menu on the headset's own panel.
    struct MenuCanvas {
        float x{0.0f}, y{0.0f}; // its place in the game window's picture (pixels)
        float w{0.0f}, h{0.0f}; // its size (pixels)
        float scale{1.0f};      // ImGui's scale for it
    };
    bool m_menu_quad{false};        // (game thread) this frame laid out for the headset's panel
    bool m_frame_menu_quad{false};  // (with the frame built: for its presents)
    MenuCanvas m_menu_canvas{};
    MenuCanvas m_frame_canvas{};
    bool m_menu_tex_drawn{false};   // (present thread) its picture drawn this present
    bool m_menu_quad_logged{false};
    std::chrono::steady_clock::time_point m_menu_quad_since{}; // (this opening: since when it was wanted on its own panel)
    bool m_menu_quad_given_up{false};                          // (this opening: it never reached the headset; in the picture instead)
    float m_ui_scale_committed{0.0f}; // (the "Menu scale" setting as used: taken once its slider is let go, not at each step of a drag)
    // (fix196, the review) The panel's size and distance as used (read on the present thread), and the copies the sliders edit, used
    // once a slider is let go (each step of a drag would move the panel under the pointer dragging it). The sliders active this frame
    // (set as they are drawn, cleared each frame): the edits, and the "Menu scale", taken only while none is (run_imgui_frame).
    std::atomic<float> m_menu_headset_height{1.1f};    // metres tall
    float m_menu_height_edit{1.1f};
    std::atomic<float> m_menu_headset_distance{1.75f}; // metres away
    float m_menu_distance_edit{1.75f};
    bool m_menu_sliders_active{false};
    std::atomic<uint32_t> m_menu_place_serial{0};
    std::atomic<int> m_vr_mouse_hand{-1};
    struct VrMouse {
        bool down{false};      // ImGui's left button held by a trigger
        bool armed{false};     // the trigger seen let go since the menu opened (a pull held as it opens clicks nothing)
        bool trigger_was[2]{false, false};
        int last_hand{-1};
        std::chrono::steady_clock::time_point last{};
    } m_vr_mouse{};

    std::array<uint8_t, 256> m_last_keys{};
    int32_t m_menu_key{VK_F11};

    struct D3D12 {
        enum class RTV : int {
            BACKBUFFER_0,
            BACKBUFFER_1,
            BACKBUFFER_2,
            BACKBUFFER_3,
            BACKBUFFER_4,
            BACKBUFFER_5,
            BACKBUFFER_6,
            BACKBUFFER_7,
            COUNT,
        };

        enum class SRV : int {
            IMGUI_FONT,
            COUNT = 64,
        };

        std::vector<std::unique_ptr<d3d12::CommandContext>> cmd_ctxs{};
        uint32_t cmd_ctx_index{0};

        // (fix196) The menu's own picture for the headset's panel (the backbuffer's format: ImGui's pipeline is made for it), its view at
        // the RTV heap's slot after the backbuffers', resting in the common state.
        static constexpr int MENU_RTV = (int)RTV::COUNT;
        Microsoft::WRL::ComPtr<ID3D12Resource> menu_tex{};
        uint32_t menu_tex_w{0};
        uint32_t menu_tex_h{0};
        DXGI_FORMAT bb_format{DXGI_FORMAT_UNKNOWN};
        bool menu_tex_format_ok{false};
        bool menu_tex_failed{false};
        // (fix196) Pictures let go while the graphics card could not be made sure to be done with them: kept until it is.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> menu_tex_retired{};
        // (fix196) For wait_for_gpu.
        Microsoft::WRL::ComPtr<ID3D12Fence> idle_fence{};
        UINT64 idle_value{0};
        HANDLE idle_event{nullptr};

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_desc_heap{};
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_desc_heap{};
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, (size_t)RTV::COUNT> rts{};
        std::unique_ptr<DirectX::GraphicsMemory> graphics_memory{};

        uint32_t rt_width{0};
        uint32_t rt_height{0};
        uint32_t next_srv{1}; // 0 is reserved for the font
        bool imgui_initialized{false};

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_rtv(ID3D12Device* device, RTV rtv) const {
            auto h = rtv_desc_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)rtv * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            return h;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_srv(ID3D12Device* device, int index) const {
            auto h = srv_desc_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return h;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_srv(ID3D12Device* device, int index) const {
            auto h = srv_desc_heap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += (UINT64)index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return h;
        }
    } m_d3d12{};
};

extern std::unique_ptr<Framework> g_framework;
