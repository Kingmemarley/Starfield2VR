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

    bool m_menu_in_headset{true}; // draw the menu before the headset copy so it shows in VR
    float m_ui_scale_setting{0.0f}; // 0 = automatic (follows the backbuffer height)
    float m_applied_ui_scale{-1.0f};
    bool m_menu_layout_dirty{true}; // re-centre and resize the menu on the next draw
    float m_menu_layout_w{0.0f};
    float m_menu_layout_h{0.0f};
    float m_menu_layout_scale{0.0f};

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
