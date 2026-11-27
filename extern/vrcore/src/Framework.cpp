// vrcore - framework core. Present/reset/rehook flow and D3D12 ImGui setup adapted from
// REFramework (MIT, praydog); RE Engine, D3D11, Lua and plugin code removed.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <GraphicsMemory.h>

#include <utility/Module.hpp>
#include <utility/String.hpp>

#include "mods/vr/d3d12/CommandContext.hpp"

#include "Framework.hpp"
#include "InputBlocker.hpp"
#include "mods/VR.hpp"
#include "mods/VRConfig.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::unique_ptr<Framework> g_framework{};

namespace {
constexpr const char* CONFIG_FILENAME = "config.txt";
constexpr const char* LOG_FILENAME = "sfvr_log.txt";
constexpr const char* MENU_OPEN_KEY = "Framework_MenuOpen";
constexpr const char* MENU_IN_HEADSET_KEY = "Framework_MenuInHeadset";
constexpr const char* UI_SCALE_KEY = "Framework_UIScale";
// (fix196) The menu's panel in the headset: its height and distance (metres).
constexpr const char* MENU_HEADSET_HEIGHT_KEY = "Framework_MenuHeadsetHeight";
constexpr const char* MENU_HEADSET_DISTANCE_KEY = "Framework_MenuHeadsetDistance";
// (fix198, the review) The main VR settings shown in this menu's Advanced tab too.
constexpr const char* MENU_MAIN_ROWS_HERE_KEY = "Framework_MainVrSettingsInMenu";
// (fix196) ImGui's draws in flight: its vertex buffers' ring and the framework's command lists, alike (draw_menu_into).
constexpr int kMenuDrawsInFlight = 6;
// (fix196) The menu's canvas for the headset's panel, in ImGui's own units (the window filled it before at about 620 x 760): drawn at up to
// 1.75 times that in pixels (as many as fit the game window's part on the desktop), shown 1.1 m tall at 1.75 m by default.
constexpr float kMenuCanvasW = 640.0f;
constexpr float kMenuCanvasH = 740.0f;

HMODULE find_own_module() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&find_own_module, &module);
    return module;
}
} // namespace

std::filesystem::path Framework::get_persistent_dir() {
    static const auto dir = []() {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

        auto path = std::filesystem::path{exe_path}.parent_path() / "sfvr";
        std::error_code ec{};
        std::filesystem::create_directories(path, ec);
        return path;
    }();

    return dir;
}

Framework::Framework(HMODULE module)
    : m_framework_module{module != nullptr ? module : find_own_module()},
      m_game_module{GetModuleHandleW(nullptr)}
{
    try {
        // (fix143) The last session's log kept as sfvr_log_previous.txt (a test's log was lost to a short session played after it).
        {
            std::error_code ec{};
            const auto      log = get_persistent_dir(LOG_FILENAME);
            if (std::filesystem::exists(log, ec)) {
                std::filesystem::rename(log, get_persistent_dir("sfvr_log_previous.txt"), ec);
            }
        }
        auto logger = spdlog::basic_logger_mt("vrcore", get_persistent_dir(LOG_FILENAME).string(), true);
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::info);
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] [tid %t] %v");
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what());
    }

    spdlog::info("vrcore framework starting (game module {:x}, framework module {:x})", (uintptr_t)m_game_module, (uintptr_t)m_framework_module);

    m_mods = std::make_unique<Mods>();

    {
        std::scoped_lock _{m_hook_monitor_mutex};
        hook_d3d12();
    }

    m_last_present_time = std::chrono::steady_clock::now();
    m_last_message_time = std::chrono::steady_clock::now();

    m_d3d_monitor_thread = std::make_unique<std::jthread>([this](std::stop_token stop_token) {
        while (!stop_token.stop_requested() && !m_terminating) {
            hook_monitor();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
}

Framework::~Framework() {
    spdlog::info("vrcore framework shutting down");

    m_terminating = true;

    if (m_d3d_monitor_thread != nullptr) {
        m_d3d_monitor_thread->request_stop();

        if (m_d3d_monitor_thread->joinable()) {
            m_d3d_monitor_thread->join();
        }
    }

    if (m_mods_fully_initialized) {
        save_config();
    }

    deinit_d3d12();

    if (m_initialized) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

bool Framework::hook_d3d12() {
    if (LoadLibraryA("d3d12.dll") == nullptr) {
        spdlog::error("d3d12.dll not found");
        return false;
    }

    m_d3d12_hook.reset();
    m_d3d12_hook = std::make_unique<D3D12Hook>();
    m_d3d12_hook->on_present([this](D3D12Hook&) { on_frame_d3d12(); });
    m_d3d12_hook->on_post_present([this](D3D12Hook&) { on_post_present_d3d12(); });
    m_d3d12_hook->on_resize_buffers([this](D3D12Hook&) { on_reset(); });
    m_d3d12_hook->on_resize_target([this](D3D12Hook&) { on_reset(); });

    if (m_d3d12_hook->hook()) {
        spdlog::info("Hooked DirectX 12");
        m_valid = true;
        return true;
    }

    spdlog::error("Failed to hook DirectX 12");

    if (!m_d3d12_hook->unhook()) {
        spdlog::error("Cannot unhook D3D12, this might crash");
    }

    m_valid = false;
    return false;
}

void Framework::hook_monitor() {
    if (!m_hook_monitor_mutex.try_lock()) {
        m_last_present_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return;
    }

    std::lock_guard _{m_hook_monitor_mutex, std::adopt_lock};

    const auto now = std::chrono::steady_clock::now();

    if (m_d3d12_hook != nullptr && m_d3d12_hook->is_inside_present()) {
        return;
    }

    // No present for 5 seconds: the swapchain was probably recreated without our hook seeing it.
    if (now - m_last_present_time > std::chrono::seconds(5)) {
        spdlog::info("No present seen for 5 seconds, rehooking D3D12");
        hook_d3d12();
        m_last_present_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    // Window procedure replaced by someone else: rehook on the next frame.
    if (m_initialized && m_wnd != nullptr && m_windows_message_hook != nullptr && !m_windows_message_hook->is_hook_intact()) {
        m_message_hook_requested = true;
    }
}

bool Framework::initialize() {
    if (m_initialized) {
        return true;
    }

    // Give the game a moment to finish creating its swapchain resources.
    if (m_frames_since_init < 60) {
        ++m_frames_since_init;
        return false;
    }

    if (m_d3d12_hook == nullptr || !m_d3d12_hook->is_hooked()) {
        return false;
    }

    auto device = m_d3d12_hook->get_device();
    auto swap_chain = m_d3d12_hook->get_swap_chain();

    if (device == nullptr || swap_chain == nullptr) {
        spdlog::info("Device or swapchain not available yet");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_chain->GetDesc(&swap_desc);
    m_wnd = swap_desc.OutputWindow;

    spdlog::info("Window handle: {:x}", (uintptr_t)m_wnd);

    if (ImGui::GetCurrentContext() == nullptr) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        set_imgui_style();

        static const auto imgui_ini = get_persistent_dir("imgui.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        ImGui_ImplWin32_Shutdown();
    }

    if (!ImGui_ImplWin32_Init(m_wnd)) {
        spdlog::error("Failed to initialize ImGui Win32 backend");
        return false;
    }

    if (!init_d3d12()) {
        spdlog::error("Failed to initialize D3D12 resources");
        return false;
    }

    initialize_windows_message_hook();
    InputBlocker::get().install();

    if (m_first_frame) {
        m_first_frame = false;

        spdlog::info("Initializing mods");

        if (auto e = m_mods->on_initialize()) {
            m_error = e->empty() ? "An unknown error has occurred." : *e;
            spdlog::error("Mod initialization failed: {}", m_error);
        } else {
            m_mods_initialized = true;

            utility::Config cfg{get_persistent_dir(CONFIG_FILENAME).string()};
            m_mods->on_config_load(cfg, true);

            if (auto open = cfg.get<bool>(MENU_OPEN_KEY)) {
                m_draw_ui = *open;
            }

            InputBlocker::get().set_blocking(m_draw_ui);

            if (auto v = cfg.get<bool>(MENU_IN_HEADSET_KEY)) {
                m_menu_in_headset = *v;
            }

            if (auto v = cfg.get<bool>(MENU_MAIN_ROWS_HERE_KEY)) {
                m_main_rows_here = *v; // (fix198)
            }

            if (auto v = cfg.get<float>(UI_SCALE_KEY)) {
                m_ui_scale_setting = *v;
                m_ui_scale_committed = *v; // (fix196)
            }

            if (auto v = cfg.get<float>(MENU_HEADSET_HEIGHT_KEY); v && std::isfinite(*v)) {
                m_menu_headset_height = std::clamp(*v, 0.5f, 2.5f);
                m_menu_height_edit = m_menu_headset_height.load();
            }

            if (auto v = cfg.get<float>(MENU_HEADSET_DISTANCE_KEY); v && std::isfinite(*v)) {
                m_menu_headset_distance = std::clamp(*v, 0.6f, 3.5f);
                m_menu_distance_edit = m_menu_headset_distance.load();
            }
        }
    }

    m_initialized = true;
    return true;
}

// Runs once on the render thread after initialize(); mods create D3D/VR resources here.
bool Framework::first_frame_initialize() {
    if (!m_error.empty() || !m_mods_initialized) {
        return false;
    }

    if (!m_first_frame_d3d_initialize) {
        return m_mods_fully_initialized;
    }

    m_first_frame_d3d_initialize = false;

    spdlog::info("Running D3D thread initialization of mods");

    if (auto e = m_mods->on_initialize_d3d_thread()) {
        m_error = e->empty() ? "An unknown error has occurred." : *e;
        spdlog::error("D3D initialization of mods failed: {}", m_error);
        return false;
    }

    m_mods_fully_initialized = true;
    save_config();

    return true;
}

bool Framework::initialize_windows_message_hook() {
    if (m_wnd == nullptr) {
        return false;
    }

    m_last_message_time = std::chrono::steady_clock::now();
    m_windows_message_hook.reset();
    m_windows_message_hook = std::make_unique<WindowsMessageHook>(m_wnd);
    m_windows_message_hook->on_message = [this](auto wnd, auto msg, auto w_param, auto l_param) {
        return on_message(wnd, msg, w_param, l_param);
    };

    m_message_hook_requested = false;
    return true;
}

void Framework::run_imgui_frame(bool from_present) {
    std::scoped_lock _{m_imgui_mtx};

    m_has_frame = false;

    if (!m_initialized || !m_d3d12.imgui_initialized) {
        return;
    }

    const bool is_init_ok = m_error.empty() && m_mods_fully_initialized;

    consume_input();

    // (fix196, the review) The menu's sliders let go (last frame none was held, wherever it was drawn): their values taken now; saved when
    // changed.
    if (!m_menu_sliders_active) {
        m_ui_scale_committed = m_ui_scale_setting;
        const float height = std::clamp(m_menu_height_edit, 0.5f, 2.5f);
        const float distance = std::clamp(m_menu_distance_edit, 0.6f, 3.5f);
        if (height != m_menu_headset_height.load() || distance != m_menu_headset_distance.load()) {
            m_menu_headset_height = height;
            m_menu_headset_distance = distance;
            m_wants_save_config = true;
        }
    }
    m_menu_sliders_active = false;

    // (fix196) This frame laid out for the headset's own panel (the menu drawn into a picture of its own, shown fixed in the room in both
    // eyes), or as before (drawn into the game's picture, or on the monitor only).
    {
        bool quad = m_draw_ui && m_menu_in_headset && headset_menu_possible();
        // (Its panel not taken by the headset for a second of this opening (from 1.5 s into it): drawn into the game's picture for the rest
        // of it, rather than shown nowhere in the headset; tried again at the next opening.)
        const auto now = std::chrono::steady_clock::now();
        if (!m_draw_ui) {
            m_menu_quad_since = {};
            m_menu_quad_given_up = false;
        } else if (quad) {
            if (m_menu_quad_since == std::chrono::steady_clock::time_point{}) {
                m_menu_quad_since = now;
            }
            const auto xr = VR::get()->m_openxr; // (a copy: it may be made anew on another thread; headset_menu_possible: not null)
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            const int64_t shown_ms = xr != nullptr ? xr->fw_menu_submitted_ms.load() : 0;
            if (!m_menu_quad_given_up && now - m_menu_quad_since > std::chrono::milliseconds(1500) && now_ms - shown_ms > 1000) {
                m_menu_quad_given_up = true;
                spdlog::warn("[Framework] (fix196) the menu's own panel has not reached the headset for a second: drawn into the game's picture "
                             "until the menu is next opened");
            }
            quad = !m_menu_quad_given_up;
        }
        if (quad != m_menu_quad) {
            m_menu_quad = quad;
            m_menu_layout_dirty = true; // (back on the monitor's layout: centred again)
        }
        if (m_menu_quad) {
            update_menu_canvas();
        }
    }

    apply_ui_scale();

    // The game hides the system cursor, so ImGui draws its own while the menu is open.
    // (fix196: in the headset's panel too: it is drawn where the controller's aim meets it.)
    ImGui::GetIO().MouseDrawCursor = m_draw_ui;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // (fix196) The headset's panel: ImGui's display is the canvas (its popups kept inside it), and the controllers' aim on it is its mouse.
    if (m_menu_quad) {
        ImGui::GetIO().DisplaySize = ImVec2{m_menu_canvas.x + m_menu_canvas.w, m_menu_canvas.y + m_menu_canvas.h};
        feed_headset_pointer();
    } else {
        release_headset_pointer();
    }

    if (is_init_ok && !from_present) {
        m_mods->on_pre_imgui_frame();
    }

    ImGui::NewFrame();

    if (is_init_ok && !from_present) {
        m_mods->on_frame();
    }

    draw_ui();
    m_last_draw_ui = m_draw_ui;

    ImGui::EndFrame();
    ImGui::Render();

    m_has_frame = true;
    // (fix196) How this frame was laid out, for its presents.
    m_frame_menu_quad = m_menu_quad;
    m_frame_canvas = m_menu_canvas;

    if (!from_present && m_wants_save_config.exchange(false)) {
        save_config();
    }
}

void Framework::on_frame_d3d12() {
    std::scoped_lock _{m_imgui_mtx};

    m_last_present_time = std::chrono::steady_clock::now();

    if (!m_initialized) {
        if (initialize()) {
            spdlog::info("vrcore framework initialized");
        }

        return;
    }

    if (m_message_hook_requested) {
        initialize_windows_message_hook();
    }

    auto device = m_d3d12_hook->get_device();
    auto command_queue = m_d3d12_hook->get_command_queue();

    if (device == nullptr || command_queue == nullptr) {
        spdlog::error("D3D12 device or command queue was null, reinitializing");
        m_initialized = false;
        return;
    }

    const auto is_init_ok = first_frame_initialize();

    // If the game thread did not build an ImGui frame (no engine hook yet), build one here.
    if (!m_has_frame) {
        run_imgui_frame(true);
    }

    // (fix196) With the menu on the headset's own panel: drawn into its picture before the VR mod's present (which hands that to the
    // headset), and into the backbuffer only after the headset's copy of it, for the monitor (the game's picture in the headset stays
    // without it). Its picture not drawn (not made, or not possible): as before.
    m_menu_tex_drawn = m_frame_menu_quad && m_draw_ui && render_menu_texture(device);

    // With "menu in headset" the menu is drawn before the VR mod copies the backbuffer, so it
    // shows up in both eyes; otherwise it is drawn afterwards and only appears on the monitor.
    const bool menu_before_copy = m_menu_in_headset && m_draw_ui && !m_menu_tex_drawn;

    if (menu_before_copy) {
        render_menu(device, true);
    }

    if (is_init_ok) {
        m_mods->on_present();
    }

    m_menu_tex_drawn = false; // (its copy for the headset is made in that present only)

    if (!menu_before_copy) {
        render_menu(device);
    }

    m_has_frame = false;
}

void Framework::render_menu(ID3D12Device* device, bool in_headset) {
    if (!m_has_frame || m_d3d12.cmd_ctxs.empty()) {
        return;
    }

    auto swapchain = m_d3d12_hook->get_swap_chain();
    const auto bb_index = swapchain->GetCurrentBackBufferIndex();

    if (bb_index >= m_d3d12.rts.size() || m_d3d12.rts[bb_index] == nullptr) {
        spdlog::error("Backbuffer {} has no render target view, reinitializing", bb_index);
        deinit_d3d12();
        m_initialized = false;
        return;
    }

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) {
        return;
    }

    // (fix138) Drawn into the headset's picture: with the lens trimmed to what the eyes see (more of it below straight ahead than
    // above; with each eye's own lens, also off to one side), the middle of the picture is not straight ahead, so the menu is moved
    // to the picture's straight ahead, where both eyes see it in one place. Not on a menu's panel or the flat screen (the picture
    // shown flat in the room).
    ImVec2 shift{0.0f, 0.0f};
    if (in_headset) {
        auto& vr = VR::get();
        if (vr->is_hmd_active() && vr->get_runtime()->is_openxr() && !vr->m_openxr->panel_mode && !vr->m_openxr->flat_screen_mode) {
            const Vector4f f = vr->picture_frustum_of(vr->m_presenter_frame_count.load());
            if (f[1] > f[0] && f[2] > f[3]) {
                shift.x = ((0.0f - f[0]) / (f[1] - f[0]) - 0.5f) * draw_data->DisplaySize.x;
                shift.y = (f[2] / (f[2] - f[3]) - 0.5f) * draw_data->DisplaySize.y;
            }
        }
    }

    // (fix196: through draw_menu_into, as the headset panel's picture is drawn.)
    draw_menu_into(m_d3d12.rts[bb_index].Get(), m_d3d12.get_cpu_rtv(device, (D3D12::RTV)bb_index), D3D12_RESOURCE_STATE_PRESENT,
        draw_data->DisplayPos.x - shift.x, draw_data->DisplayPos.y - shift.y, draw_data->DisplaySize.x, draw_data->DisplaySize.y, false);
}

bool Framework::draw_menu_into(ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_RESOURCE_STATES state, float x, float y, float w,
    float h, bool clear) {
    ImDrawData* draw_data = ImGui::GetDrawData();
    // (ImGui draws nothing, and does not move its ring on, for an empty display: nor does the command lists' ring, so they stay together.)
    if (draw_data == nullptr || target == nullptr || m_d3d12.cmd_ctxs.empty() || !(w >= 1.0f) || !(h >= 1.0f) || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return false;
    }

    auto& cmd_ctx = m_d3d12.cmd_ctxs[m_d3d12.cmd_ctx_index++ % m_d3d12.cmd_ctxs.size()];
    cmd_ctx->wait(INFINITE);

    std::scoped_lock _{cmd_ctx->mtx};
    cmd_ctx->has_commands = true;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = target;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

    if (clear) {
        const float see_through[4]{0.0f, 0.0f, 0.0f, 0.0f};
        cmd_ctx->cmd_list->ClearRenderTargetView(rtv, see_through, 0, nullptr);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rts[1]{rtv};
    cmd_ctx->cmd_list->OMSetRenderTargets(1, rts, FALSE, nullptr);
    cmd_ctx->cmd_list->SetDescriptorHeaps(1, m_d3d12.srv_desc_heap.GetAddressOf());

    // (The part of the canvas drawn: ImGui maps its display's place and size onto the whole target.)
    const ImVec2 pos = draw_data->DisplayPos;
    const ImVec2 size = draw_data->DisplaySize;
    draw_data->DisplayPos = ImVec2{x, y};
    draw_data->DisplaySize = ImVec2{w, h};
    ImGui_ImplDX12_RenderDrawData(draw_data, cmd_ctx->cmd_list.Get());
    draw_data->DisplayPos = pos;
    draw_data->DisplaySize = size;

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = state;
    cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

    cmd_ctx->execute();
    return true;
}

bool Framework::render_menu_texture(ID3D12Device* device) {
    if (!m_has_frame || !m_d3d12.menu_tex_format_ok || m_d3d12.menu_tex_failed || device == nullptr) {
        return false;
    }
    const auto w = (uint32_t)m_frame_canvas.w;
    const auto h = (uint32_t)m_frame_canvas.h;
    if (w < 64 || h < 64 || w > 8192 || h > 8192) {
        return false;
    }
    if (m_d3d12.menu_tex == nullptr || m_d3d12.menu_tex_w != w || m_d3d12.menu_tex_h != h) {
        // (Made again at another size: the old one let go only once the graphics card is done with it: its draws and the headset's copies
        // of it are all on the game's queue; else kept until it is.)
        retire_menu_texture();

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = m_d3d12.bb_format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = m_d3d12.bb_format;
        Microsoft::WRL::ComPtr<ID3D12Resource> made{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, &clear_value, IID_PPV_ARGS(&made)))) {
            spdlog::error("[Framework] (fix196) the menu's picture for the headset ({}x{}) could not be made: the menu is drawn into the game's "
                          "picture this session", w, h);
            m_d3d12.menu_tex_failed = true;
            return false;
        }
        made->SetName(L"vrcore::Framework menu picture (headset panel)");
        device->CreateRenderTargetView(made.Get(), nullptr, m_d3d12.get_cpu_rtv(device, (D3D12::RTV)D3D12::MENU_RTV));
        m_d3d12.menu_tex = made;
        m_d3d12.menu_tex_w = w;
        m_d3d12.menu_tex_h = h;
        spdlog::info("[Framework] (fix196) the menu in the headset on a panel of its own: its picture {}x{} (ImGui scale {:.2f}), at the desktop "
                     "window's {:.0f},{:.0f}; shown {:.2f} m tall at {:.2f} m", w, h, m_frame_canvas.scale, m_frame_canvas.x, m_frame_canvas.y,
            m_menu_headset_height.load(), m_menu_headset_distance.load());
    }
    return draw_menu_into(m_d3d12.menu_tex.Get(), m_d3d12.get_cpu_rtv(device, (D3D12::RTV)D3D12::MENU_RTV), D3D12_RESOURCE_STATE_COMMON,
        m_frame_canvas.x, m_frame_canvas.y, (float)w, (float)h, true);
}

bool Framework::headset_menu_possible() const {
    if (!m_d3d12.menu_tex_format_ok || m_d3d12.menu_tex_failed || !m_mods_fully_initialized) {
        return false;
    }
    auto& vr = VR::get();
    if (vr == nullptr || vr->get_runtime() == nullptr || !vr->get_runtime()->is_openxr() || !vr->is_hmd_active()) {
        return false;
    }
    const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
    return xr != nullptr && xr->ready() && !xr->fw_menu_refused.load();
}

void Framework::update_menu_canvas() {
    // The part of the game window's picture shown on the desktop (its window is often taller than the screen: the headset's size), so the
    // menu laid out there is where the mouse can reach all of it; the headset's panel shows the same canvas, whatever its place.
    float ox = 0.0f, oy = 0.0f, avail_w = 0.0f, avail_h = 0.0f;
    RECT client{};
    if (m_wnd != nullptr && GetClientRect(m_wnd, &client) && client.right > 0 && client.bottom > 0) {
        POINT top_left{0, 0};
        ClientToScreen(m_wnd, &top_left);
        const RECT area{top_left.x, top_left.y, top_left.x + client.right, top_left.y + client.bottom};
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        RECT seen{};
        if (GetMonitorInfo(MonitorFromWindow(m_wnd, MONITOR_DEFAULTTONEAREST), &info) && IntersectRect(&seen, &area, &info.rcWork)) {
            ox = (float)(seen.left - top_left.x);
            oy = (float)(seen.top - top_left.y);
            avail_w = (float)(seen.right - seen.left);
            avail_h = (float)(seen.bottom - seen.top);
        } else {
            avail_w = (float)client.right;
            avail_h = (float)client.bottom;
        }
    }
    // Its scale: the "Menu scale" setting if set, else as large as fits there, between 1 and 1.75 (steps of 0.05: the style is not made
    // again for a pixel's change). The headset's panel is the same size in metres whatever the scale: only its sharpness changes.
    float scale = 1.0f;
    if (m_ui_scale_committed > 0.0f) {
        scale = std::clamp(m_ui_scale_committed, 0.5f, 4.0f);
    } else if (avail_w > 0.0f && avail_h > 0.0f) {
        scale = std::clamp(std::min(avail_w / kMenuCanvasW, avail_h / kMenuCanvasH), 1.0f, 1.75f);
    }
    scale = std::floor(scale * 20.0f + 1e-3f) / 20.0f;
    m_menu_canvas.x = std::max(0.0f, ox);
    m_menu_canvas.y = std::max(0.0f, oy);
    m_menu_canvas.w = std::round(kMenuCanvasW * scale);
    m_menu_canvas.h = std::round(kMenuCanvasH * scale);
    m_menu_canvas.scale = scale;
}

void Framework::feed_headset_pointer() {
    auto& io = ImGui::GetIO();
    auto& m = m_vr_mouse;
    const auto now = std::chrono::steady_clock::now();
    const float dt = m.last == std::chrono::steady_clock::time_point{} ? 0.0f : std::clamp(std::chrono::duration<float>(now - m.last).count(), 0.0f, 0.1f);
    m.last = now;

    auto& vr = VR::get();
    runtimes::OpenXR::FwMenuPointer p{};
    bool fresh = false;
    bool trigger[2]{false, false};
    if (vr != nullptr && vr->get_runtime() != nullptr && vr->get_runtime()->is_openxr() && vr->is_using_controllers()) {
        const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
        if (xr != nullptr) {
            p = xr->get_fw_menu_pointer();
            fresh = p.shown && now - p.at < std::chrono::milliseconds(150);
        }
        // (Read whether or not either hand is on the menu, so a pull that began off it is known as one.)
        trigger[0] = vr->is_action_active(vr->m_action_trigger, vr->get_left_joystick());
        trigger[1] = vr->is_action_active(vr->m_action_trigger, vr->get_right_joystick());
    }

    // The pointing hand: the one whose trigger was pulled last while on the menu; else whichever is on it (the right first).
    int hand = m_vr_mouse_hand.load();
    for (int h = 0; h < 2; ++h) {
        if (fresh && p.hit[h] && trigger[h] && !m.trigger_was[h]) {
            hand = h;
        }
    }
    if (!(fresh && hand >= 0 && hand <= 1 && p.hit[hand])) {
        hand = fresh && p.hit[1] ? 1 : (fresh && p.hit[0] ? 0 : hand);
    }
    m_vr_mouse_hand = hand;
    const bool on_menu = fresh && hand >= 0 && hand <= 1 && p.hit[hand];

    // (Another hand took over while the button was held: let go first.)
    if (m.down && hand != m.last_hand) {
        io.AddMouseButtonEvent(0, false);
        m.down = false;
    }
    if (on_menu) {
        io.AddMousePosEvent(m_menu_canvas.x + p.u[hand] * m_menu_canvas.w, m_menu_canvas.y + p.v[hand] * m_menu_canvas.h);
        // Its trigger is the left button: a press starts only with a pull begun while on the menu (one held as the menu opens, or brought
        // onto it from off it, clicks nothing), and lasts while the trigger is held.
        if (!trigger[hand]) {
            m.armed = true;
        }
        const bool down = m.down ? trigger[hand] : (m.armed && trigger[hand] && !m.trigger_was[hand]);
        if (down != m.down) {
            io.AddMouseButtonEvent(0, down);
            m.down = down;
        }
        // Its stick's up and down scroll what it points at (about 4 notches a second at full tilt, nothing in its middle quarter).
        const Vector2f axis = hand == 0 ? vr->get_left_stick_axis() : vr->get_right_stick_axis();
        const float y = axis.y;
        if (std::isfinite(y) && std::abs(y) > 0.25f && dt > 0.0f) {
            io.AddMouseWheelEvent(0.0f, (y > 0.0f ? 1.0f : -1.0f) * (std::abs(y) - 0.25f) / 0.75f * 4.0f * dt);
        }
    } else if (m.down) {
        // (Off the menu: the button let go; the mouse stays where it was, for the desktop's.)
        io.AddMouseButtonEvent(0, false);
        m.down = false;
    }
    m.last_hand = hand;
    m.trigger_was[0] = trigger[0];
    m.trigger_was[1] = trigger[1];
}

void Framework::release_headset_pointer() {
    auto& m = m_vr_mouse;
    if (m.down) {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        m.down = false;
    }
    m.last = {};
}

bool Framework::wait_for_gpu() {
    auto queue = m_d3d12_hook != nullptr ? m_d3d12_hook->get_command_queue() : nullptr;
    auto device = m_d3d12_hook != nullptr ? m_d3d12_hook->get_device() : nullptr;
    if (queue == nullptr || device == nullptr || m_terminating) {
        return false;
    }
    if (m_d3d12.idle_fence == nullptr) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_d3d12.idle_fence)))) {
            m_d3d12.idle_fence.Reset();
            return false;
        }
        m_d3d12.idle_value = 0;
    }
    if (m_d3d12.idle_event == nullptr) {
        m_d3d12.idle_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_d3d12.idle_event == nullptr) {
            return false;
        }
    }
    const UINT64 value = ++m_d3d12.idle_value;
    if (FAILED(queue->Signal(m_d3d12.idle_fence.Get(), value))) {
        return false;
    }
    if (m_d3d12.idle_fence->GetCompletedValue() < value) {
        // (A signal left over from an earlier wait that timed out is cleared first: only this value's completion ends this wait.)
        ResetEvent(m_d3d12.idle_event);
        if (SUCCEEDED(m_d3d12.idle_fence->SetEventOnCompletion(value, m_d3d12.idle_event))) {
            WaitForSingleObject(m_d3d12.idle_event, 2000);
        }
    }
    if (m_d3d12.idle_fence->GetCompletedValue() < value) {
        spdlog::warn("[Framework] (fix196) waited 2 s for the graphics card to finish its work: the menu's old picture is kept for now");
        return false;
    }
    return true;
}

void Framework::retire_menu_texture() {
    if (m_d3d12.menu_tex != nullptr) {
        m_d3d12.menu_tex_retired.push_back(m_d3d12.menu_tex);
        m_d3d12.menu_tex.Reset();
    }
    m_d3d12.menu_tex_w = m_d3d12.menu_tex_h = 0;
    if (m_d3d12.menu_tex_retired.empty()) {
        return;
    }
    if (wait_for_gpu()) {
        m_d3d12.menu_tex_retired.clear();
    } else if (m_terminating) {
        // (The game closing, the graphics card not known to be done: kept for good, its memory freed with the process.)
        for (auto& kept : m_d3d12.menu_tex_retired) {
            kept.Detach();
        }
        m_d3d12.menu_tex_retired.clear();
    }
}

// The game window is usually much larger than the monitor (it is resized to the headset
// resolution), so the menu is scaled with the backbuffer height to stay readable.
void Framework::apply_ui_scale() {
    float scale = m_ui_scale_committed; // (fix196: as set once its slider is let go)

    if (m_menu_quad) {
        scale = m_menu_canvas.scale; // (fix196: the headset panel's canvas, update_menu_canvas)
    } else if (scale <= 0.0f) {
        scale = std::max(1.0f, (float)m_d3d12.rt_height / 1080.0f);
    }

    scale = std::clamp(scale, 0.5f, 8.0f);

    if (std::abs(scale - m_applied_ui_scale) < 0.01f) {
        return;
    }

    m_applied_ui_scale = scale;

    ImGui::GetStyle() = ImGuiStyle{}; // ScaleAllSizes multiplies, so start from the defaults
    set_imgui_style();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
}

void Framework::on_post_present_d3d12() {
    if (!m_initialized || !m_mods_fully_initialized) {
        return;
    }

    if (m_d3d12.graphics_memory != nullptr) {
        m_d3d12.graphics_memory->Commit(m_d3d12_hook->get_command_queue());
    }

    m_mods->on_post_present();
}

void Framework::on_reset() {
    std::scoped_lock _{m_imgui_mtx};

    spdlog::info("Device reset");

    deinit_d3d12();

    if (m_mods_initialized) {
        m_mods->on_device_reset();
    }

    m_has_frame = false;
    m_initialized = false;

    // Only the very first initialization waits 60 frames; after a resize re-initialize quickly.
    m_frames_since_init = 57;
}

bool Framework::init_d3d12() {
    deinit_d3d12();

    auto device = m_d3d12_hook->get_device();
    auto swapchain = m_d3d12_hook->get_swap_chain();

    m_d3d12.graphics_memory = std::make_unique<DirectX::GraphicsMemory>(device);

    // (fix196) As many command lists as ImGui keeps draws in flight (kMenuDrawsInFlight, given to it below): with the menu drawn twice a
    // present (its headset panel's picture and the monitor's), each ImGui buffer is reused only after the list that last read it is done.
    for (auto i = 0; i < kMenuDrawsInFlight; ++i) {
        auto& ctx = m_d3d12.cmd_ctxs.emplace_back(std::make_unique<d3d12::CommandContext>());

        if (!ctx->setup(L"vrcore::Framework::cmd_ctx")) {
            spdlog::error("[D3D12] Failed to create command context");
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = (UINT)D3D12::RTV::COUNT + 1; // (fix196: and the headset panel's picture, D3D12::MENU_RTV)
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_d3d12.rtv_desc_heap)))) {
            spdlog::error("[D3D12] Failed to create RTV descriptor heap");
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = (UINT)D3D12::SRV::COUNT;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_d3d12.srv_desc_heap)))) {
            spdlog::error("[D3D12] Failed to create SRV descriptor heap");
            return false;
        }
    }

    DXGI_SWAP_CHAIN_DESC swapchain_desc{};

    if (FAILED(swapchain->GetDesc(&swapchain_desc))) {
        spdlog::error("[D3D12] Failed to get swapchain description");
        return false;
    }

    if (swapchain_desc.BufferCount > (UINT)D3D12::RTV::COUNT) {
        spdlog::warn("[D3D12] Too many backbuffers ({})", swapchain_desc.BufferCount);
    }

    for (UINT i = 0; i < swapchain_desc.BufferCount && i < (UINT)D3D12::RTV::COUNT; ++i) {
        if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&m_d3d12.rts[i])))) {
            device->CreateRenderTargetView(m_d3d12.rts[i].Get(), nullptr, m_d3d12.get_cpu_rtv(device, (D3D12::RTV)i));
        } else {
            spdlog::error("[D3D12] Failed to get backbuffer {}", i);
            m_d3d12.rts[i].Reset();
        }
    }

    if (m_d3d12.rts[0] == nullptr) {
        spdlog::error("[D3D12] No backbuffer available");
        return false;
    }

    const auto bb_desc = m_d3d12.rts[0]->GetDesc();
    m_d3d12.rt_width = (uint32_t)bb_desc.Width;
    m_d3d12.rt_height = (uint32_t)bb_desc.Height;

    spdlog::info("[D3D12] Backbuffer {}x{} format {}", m_d3d12.rt_width, m_d3d12.rt_height, (int)bb_desc.Format);

    // (fix196) The headset panel's picture of the menu is made in the backbuffer's format (ImGui's pipeline is made for it) and copied as it
    // is into an 8-bit sRGB swapchain: only for an 8-bit RGBA backbuffer; any other (HDR's), the menu is drawn into the game's picture.
    m_d3d12.bb_format = bb_desc.Format;
    m_d3d12.menu_tex_format_ok = bb_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bb_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!m_d3d12.menu_tex_format_ok && !m_menu_quad_logged) {
        m_menu_quad_logged = true;
        spdlog::info("[Framework] (fix196) the backbuffer's format ({}) is not 8-bit RGBA: the menu is drawn into the game's picture in the headset",
            (int)bb_desc.Format);
    }

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = device;
    init_info.CommandQueue = m_d3d12_hook->get_command_queue();
    init_info.NumFramesInFlight = kMenuDrawsInFlight; // (fix196: was the backbuffer count; see the command lists above)
    init_info.RTVFormat = bb_desc.Format;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = m_d3d12.srv_desc_heap.Get();
    init_info.UserData = this;
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
        auto self = (Framework*)info->UserData;
        auto device = self->m_d3d12_hook->get_device();
        const auto index = self->m_d3d12.next_srv++ % (uint32_t)D3D12::SRV::COUNT;

        *out_cpu = self->m_d3d12.get_cpu_srv(device, (int)index);
        *out_gpu = self->m_d3d12.get_gpu_srv(device, (int)index);
    };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};

    if (!ImGui_ImplDX12_Init(&init_info)) {
        spdlog::error("[D3D12] Failed to initialize the ImGui DX12 backend");
        return false;
    }

    m_d3d12.imgui_initialized = true;
    return true;
}

void Framework::deinit_d3d12() {
    for (auto& ctx : m_d3d12.cmd_ctxs) {
        if (ctx != nullptr) {
            ctx->reset();
        }
    }

    m_d3d12.cmd_ctxs.clear();

    // (fix196) The headset panel's picture: let go once the graphics card is done with it (the headset's copies of it are on the game's
    // queue, not on the lists above), else kept until it is (for good as the game closes); made again at the next opening.
    retire_menu_texture();
    m_d3d12.menu_tex_failed = false;
    m_d3d12.idle_fence.Reset();
    m_d3d12.idle_value = 0;
    if (m_d3d12.idle_event != nullptr) {
        CloseHandle(m_d3d12.idle_event);
        m_d3d12.idle_event = nullptr;
    }
    m_menu_tex_drawn = false;

    if (m_d3d12.imgui_initialized && ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplDX12_Shutdown();
    }

    m_d3d12.imgui_initialized = false;
    m_d3d12.rts = {};
    m_d3d12.rtv_desc_heap.Reset();
    m_d3d12.srv_desc_heap.Reset();
    m_d3d12.graphics_memory.reset();
    m_d3d12.next_srv = 1;
}

bool Framework::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    m_last_message_time = std::chrono::steady_clock::now();

    if (!m_initialized) {
        return true;
    }

    if (message == WM_KEYDOWN && w_param == (WPARAM)m_menu_key && (l_param & (1 << 30)) == 0) {
        set_draw_ui(!m_draw_ui);
        return false;
    }

    if (m_draw_ui) {
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui_ImplWin32_WndProcHandler(wnd, message, w_param, l_param);
        }

        // While the menu is open the game gets no mouse or keyboard input at all.
        const bool is_input_message = message == WM_INPUT ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            message == WM_SETCURSOR;

        if (is_input_message) {
            return false;
        }
    }

    return m_mods->on_message(wnd, message, w_param, l_param);
}

void Framework::set_draw_ui(bool state, bool should_save) {
    std::scoped_lock _{m_imgui_mtx};

    // (fix196) Opened: its headset panel put in front of you again, and a trigger held as it opens clicks nothing until let go.
    if (state && !m_draw_ui) {
        ++m_menu_place_serial;
        m_vr_mouse.armed = false;
    }
    m_draw_ui = state;
    InputBlocker::get().set_blocking(state);

    if (should_save) {
        m_wants_save_config = true;
    }
}

void Framework::consume_input() {
    // nothing to do: ImGui gets input through the message hook
}

void Framework::draw_ui() {
    if (!m_draw_ui) {
        return;
    }

    // Centred (straight ahead in the headset) and a fixed share of the view. Applied every time
    // the menu opens, and again whenever the game window or the text size changes, so a size
    // remembered from the small desktop window at startup can't leave it tiny or off to a side.
    const auto scale = m_applied_ui_scale > 0.0f ? m_applied_ui_scale : 1.0f;
    const ImVec2 view = ImGui::GetMainViewport()->Size;
    const bool relayout = m_menu_layout_dirty || std::abs(view.x - m_menu_layout_w) > 1.0f || std::abs(view.y - m_menu_layout_h) > 1.0f ||
                          std::abs(scale - m_menu_layout_scale) > 0.01f;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_menu_quad) {
        // (fix196) On the headset's own panel: the window fills its canvas (the panel shows the canvas), there on the desktop too, where
        // the mouse can reach all of it; not moved, resized or folded there (the panel would show a part).
        ImGui::SetNextWindowPos(ImVec2{m_menu_canvas.x, m_menu_canvas.y}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2{m_menu_canvas.w, m_menu_canvas.h}, ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        m_menu_layout_dirty = true; // (laid out again as before when it is next drawn on the monitor or into the game's picture)
    } else {
        const ImGuiCond cond = relayout ? ImGuiCond_Always : ImGuiCond_Appearing;
        const ImVec2 size{std::min(620.0f * scale, view.x * 0.5f), std::min(760.0f * scale, view.y * 0.6f)};
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), cond, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, cond);
        ImGui::SetNextWindowCollapsed(false, cond);
        m_menu_layout_dirty = false;
        m_menu_layout_w = view.x;
        m_menu_layout_h = view.y;
        m_menu_layout_scale = scale;
    }
    ImGui::SetNextWindowBgAlpha(0.95f);

    bool open = true;

    if (ImGui::Begin("starfield2vr", &open, flags)) {
        // (fix198) F11 only: both thumbsticks clicked no longer open or close it.
        menu_ui::hint(m_menu_quad ? "F11, or the close button at this window's top right, closes this menu. Point a controller at it and pull its "
                                    "trigger to click; push its stick up or down to scroll."
                                  : "F11 opens and closes this menu.");

        if (!m_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", m_error.c_str());
            ImGui::PopStyleColor();
        }

        // (fix196) Three tabs: Main (the few settings most players need), Advanced (the rest, by feature), Debug (tests, diagnostics,
        // troubleshooting). Each tab's rows scroll under the tab bar, which stays in view (clicked with the mouse or a controller's pointer).
        if (ImGui::BeginTabBar("sfvr_tabs")) {
            const auto tab_body = [](const char* id, auto&& body) {
                ImGui::BeginChild(id, ImVec2{0.0f, 0.0f}, ImGuiChildFlags_None);
                body();
                ImGui::Spacing();
                ImGui::EndChild();
            };
            if (ImGui::BeginTabItem("Main")) {
                tab_body("main", [&] {
                    // (fix198) With the game's Settings > VR tab working, the main VR settings are there: Main keeps what concerns this
                    // menu (the mods' parts 0 and 1 still say what they must: the game version, a missing runtime, VectorXR).
                    const bool in_game_tab = VRConfig::s_game_vr_tab.load();
                    if (m_mods_initialized && in_game_tab) {
                        m_mods->on_draw_main_ui(0);
                        m_mods->on_draw_main_ui(1);
                        ImGui::SeparatorText("This menu");
                    } else if (m_mods_initialized) {
                        m_mods->on_draw_main_ui(0);
                        ImGui::SeparatorText("View");
                        m_mods->on_draw_main_ui(1);
                        ImGui::SeparatorText("Body");
                        m_mods->on_draw_main_ui(2);
                        ImGui::SeparatorText("Turning");
                        m_mods->on_draw_main_ui(3);
                        ImGui::SeparatorText("Controllers");
                        m_mods->on_draw_main_ui(4);
                        ImGui::SeparatorText("HUD and this menu");
                        m_mods->on_draw_main_ui(5);
                    }
                    draw_menu_main_rows();
                    ImGui::Spacing();
                    if (ImGui::Button("Save settings")) {
                        save_config();
                    }
                    menu_ui::hint("Settings are also saved as this menu opens and closes, and as the game exits.");
                });
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Advanced")) {
                tab_body("advanced", [&] {
                    menu_ui::hint("Everything else, by feature: open a group to see its settings.");
                    if (m_mods_initialized && VRConfig::s_game_vr_tab.load()) {
                        draw_main_rows_here(); // (fix198, the review)
                    }
                    if (m_mods_initialized) {
                        ImGui::SeparatorText("Gameplay");
                        m_mods->on_draw_advanced_ui(0);
                        ImGui::SeparatorText("View and performance");
                        m_mods->on_draw_advanced_ui(1);
                        ImGui::SeparatorText("Menus, controllers and the monitor");
                        m_mods->on_draw_advanced_ui(2);
                    }
                    draw_menu_advanced_rows();
                });
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug")) {
                tab_body("debug", [&] {
                    menu_ui::hint("Tests, diagnostics and troubleshooting: leave these as they are unless you are testing something.");
                    if (m_mods_initialized) {
                        m_mods->on_draw_debug_ui();
                    }
                });
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    ImGui::End();

    if (!open) {
        set_draw_ui(false);
    }
}

// (fix196) This menu's own rows on the Main tab (under "HUD and this menu"): its size and distance in the headset.
void Framework::draw_menu_main_rows() {
    // (Edit copies: the panel changes once the slider is let go, run_imgui_frame; while dragged it stays still under the pointer.)
    ImGui::SliderFloat("Menu size (m tall)", &m_menu_height_edit, 0.5f, 2.5f, "%.2f");
    m_menu_sliders_active |= ImGui::IsItemActive();
    ImGui::SliderFloat("Menu distance (m)", &m_menu_distance_edit, 0.6f, 3.5f, "%.2f");
    m_menu_sliders_active |= ImGui::IsItemActive();
    if (ImGui::Button("Centre this menu")) {
        m_menu_layout_dirty = true;
        ++m_menu_place_serial; // (in the headset: in front of you now)
    }
    menu_ui::hint("This menu's size and distance in the headset. It is put in front of you each time it opens, and stays there.");
}

// (fix198, the review) With the game's Settings > VR tab working, the main VR settings are there and not on Main. Asked for, they are
// shown here too (for when that tab is not shown, another mod replacing the Settings menu, say): the mods draw their Main rows while
// s_main_rows_here is set, in the same order and under the same headings as Main has them without the game's tab.
void Framework::draw_main_rows_here() {
    if (ImGui::Checkbox("Show the main VR settings here too", &m_main_rows_here)) {
        m_wants_save_config = true;
        spdlog::info("[QuickTest] (fix198) the main VR settings in the F11 menu's Advanced tab too: {}", m_main_rows_here ? "on" : "off");
    }
    menu_ui::hint("For when the game's Settings > VR tab is not shown (e.g. another mod replaces the Settings menu): the same settings in a "
                  "group below (the eye mode, sharpness, recentring, the body, turning, the controllers, the HUD's size).");
    if (!m_main_rows_here || !menu_ui::group("Main VR settings", true)) {
        return;
    }
    VRConfig::s_main_rows_here = true;
    ImGui::PushID("main_rows_here");
    ImGui::SeparatorText("Recentring");
    m_mods->on_draw_main_ui(0);
    ImGui::SeparatorText("View");
    m_mods->on_draw_main_ui(1);
    ImGui::SeparatorText("Body");
    m_mods->on_draw_main_ui(2);
    ImGui::SeparatorText("Turning");
    m_mods->on_draw_main_ui(3);
    ImGui::SeparatorText("Controllers");
    m_mods->on_draw_main_ui(4);
    ImGui::SeparatorText("HUD");
    m_mods->on_draw_main_ui(5);
    ImGui::PopID();
    VRConfig::s_main_rows_here = false;
    ImGui::Spacing();
}

// (fix196) This menu's other rows, on the Advanced tab.
void Framework::draw_menu_advanced_rows() {
    if (!menu_ui::group("This menu")) {
        return;
    }
    if (ImGui::Checkbox("Show this menu in the headset", &m_menu_in_headset)) {
        m_wants_save_config = true;
    }
    menu_ui::hint("Off: on the monitor only (use the mouse there).");

    if (ImGui::SliderFloat("Menu scale (0 = automatic)", &m_ui_scale_setting, 0.0f, 6.0f, "%.1f")) {
        m_wants_save_config = true;
    }
    // (Used once let go, run_imgui_frame: each step of a drag would make the menu's style, and its headset panel's picture, anew.)
    m_menu_sliders_active |= ImGui::IsItemActive();
    menu_ui::hint("The text's size on the monitor; in the headset, how sharp this menu is (its size there is the Main tab's).");

    if (!m_menu_quad && m_menu_in_headset && m_draw_ui) {
        menu_ui::hint(!m_d3d12.menu_tex_format_ok ? "In the headset this menu is drawn into the game's picture: the game's picture format has no "
                                                    "panel of its own for it."
                      : m_d3d12.menu_tex_failed   ? "In the headset this menu is drawn into the game's picture: its panel's picture could not "
                                                    "be made."
                                                  : "In the headset this menu is drawn into the game's picture (no OpenXR headset in use, or "
                                                    "the runtime refused its panel).");
    }
}

void Framework::save_config() {
    if (!m_mods_initialized) {
        return;
    }

    spdlog::info("Saving config to {}", get_persistent_dir(CONFIG_FILENAME).string());

    utility::Config cfg{};
    m_mods->on_config_save(cfg);
    cfg.set<bool>(MENU_OPEN_KEY, m_draw_ui);
    cfg.set<bool>(MENU_IN_HEADSET_KEY, m_menu_in_headset);
    cfg.set<bool>(MENU_MAIN_ROWS_HERE_KEY, m_main_rows_here); // (fix198)
    cfg.set<float>(UI_SCALE_KEY, m_ui_scale_setting);
    cfg.set<float>(MENU_HEADSET_HEIGHT_KEY, m_menu_headset_height.load());
    cfg.set<float>(MENU_HEADSET_DISTANCE_KEY, m_menu_headset_distance.load());

    if (!cfg.save(get_persistent_dir(CONFIG_FILENAME).string())) {
        spdlog::error("Failed to save config");
    }
}

void Framework::set_imgui_style() {
    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    // (fix196) A little more room around rows and in the tabs (easier to hit with a controller's pointer), softer corners, and a calmer
    // blue-grey for headers and tabs.
    style.WindowPadding = ImVec2{12.0f, 10.0f};
    style.FramePadding = ImVec2{8.0f, 4.0f};
    style.ItemSpacing = ImVec2{8.0f, 6.0f};
    style.ItemInnerSpacing = ImVec2{6.0f, 4.0f};
    style.ScrollbarSize = 16.0f;
    style.TabRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.SeparatorTextBorderSize = 2.0f;
    auto* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4{0.06f, 0.08f, 0.11f, 1.0f};
    c[ImGuiCol_Header] = ImVec4{0.16f, 0.24f, 0.34f, 0.85f};
    c[ImGuiCol_HeaderHovered] = ImVec4{0.22f, 0.34f, 0.48f, 0.95f};
    c[ImGuiCol_HeaderActive] = ImVec4{0.26f, 0.42f, 0.60f, 1.0f};
    c[ImGuiCol_Tab] = ImVec4{0.12f, 0.17f, 0.24f, 1.0f};
    c[ImGuiCol_TabHovered] = ImVec4{0.26f, 0.42f, 0.60f, 1.0f};
    c[ImGuiCol_TabSelected] = ImVec4{0.20f, 0.33f, 0.48f, 1.0f};
    c[ImGuiCol_TabSelectedOverline] = ImVec4{0.89f, 0.70f, 0.27f, 1.0f};
    c[ImGuiCol_SeparatorHovered] = ImVec4{0.26f, 0.42f, 0.60f, 1.0f};
    c[ImGuiCol_CheckMark] = ImVec4{0.45f, 0.72f, 0.95f, 1.0f};
    c[ImGuiCol_SliderGrab] = ImVec4{0.36f, 0.58f, 0.82f, 1.0f};
    c[ImGuiCol_SliderGrabActive] = ImVec4{0.45f, 0.72f, 0.95f, 1.0f};
    c[ImGuiCol_Button] = ImVec4{0.16f, 0.24f, 0.34f, 1.0f};
    c[ImGuiCol_ButtonHovered] = ImVec4{0.22f, 0.34f, 0.48f, 1.0f};
    c[ImGuiCol_ButtonActive] = ImVec4{0.26f, 0.42f, 0.60f, 1.0f};
    c[ImGuiCol_FrameBg] = ImVec4{0.11f, 0.15f, 0.21f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = ImVec4{0.16f, 0.23f, 0.32f, 1.0f};
    c[ImGuiCol_FrameBgActive] = ImVec4{0.20f, 0.30f, 0.42f, 1.0f};
}
