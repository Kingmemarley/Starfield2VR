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

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::unique_ptr<Framework> g_framework{};

namespace {
constexpr const char* CONFIG_FILENAME = "config.txt";
constexpr const char* LOG_FILENAME = "sfvr_log.txt";
constexpr const char* MENU_OPEN_KEY = "Framework_MenuOpen";
constexpr const char* MENU_IN_HEADSET_KEY = "Framework_MenuInHeadset";
constexpr const char* UI_SCALE_KEY = "Framework_UIScale";

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

            if (auto v = cfg.get<float>(UI_SCALE_KEY)) {
                m_ui_scale_setting = *v;
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

    apply_ui_scale();

    // The game hides the system cursor, so ImGui draws its own while the menu is open.
    ImGui::GetIO().MouseDrawCursor = m_draw_ui;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

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

    // With "menu in headset" the menu is drawn before the VR mod copies the backbuffer, so it
    // shows up in both eyes; otherwise it is drawn afterwards and only appears on the monitor.
    const bool menu_before_copy = m_menu_in_headset && m_draw_ui;

    if (menu_before_copy) {
        render_menu(device, true);
    }

    if (is_init_ok) {
        m_mods->on_present();
    }

    if (!menu_before_copy) {
        render_menu(device);
    }

    m_has_frame = false;
}

void Framework::render_menu(ID3D12Device* device, bool in_headset) {
    if (!m_has_frame || m_d3d12.cmd_ctxs.empty()) {
        return;
    }

    auto& cmd_ctx = m_d3d12.cmd_ctxs[m_d3d12.cmd_ctx_index++ % m_d3d12.cmd_ctxs.size()];
    auto swapchain = m_d3d12_hook->get_swap_chain();
    const auto bb_index = swapchain->GetCurrentBackBufferIndex();

    if (bb_index >= m_d3d12.rts.size() || m_d3d12.rts[bb_index] == nullptr) {
        spdlog::error("Backbuffer {} has no render target view, reinitializing", bb_index);
        deinit_d3d12();
        m_initialized = false;
        return;
    }

    cmd_ctx->wait(INFINITE);

    std::scoped_lock _{cmd_ctx->mtx};
    cmd_ctx->has_commands = true;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = m_d3d12.rts[bb_index].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rts[1]{m_d3d12.get_cpu_rtv(device, (D3D12::RTV)bb_index)};
    cmd_ctx->cmd_list->OMSetRenderTargets(1, rts, FALSE, nullptr);
    cmd_ctx->cmd_list->SetDescriptorHeaps(1, m_d3d12.srv_desc_heap.GetAddressOf());

    // (fix138) Drawn into the headset's picture: with the lens trimmed to what the eyes see (more of it below straight ahead than
    // above; with each eye's own lens, also off to one side), the middle of the picture is not straight ahead, so the menu is moved
    // to the picture's straight ahead, where both eyes see it in one place. Not on a menu's panel or the flat screen (the picture
    // shown flat in the room).
    ImDrawData* draw_data = ImGui::GetDrawData();
    ImVec2      shift{0.0f, 0.0f};
    if (in_headset && draw_data != nullptr) {
        auto& vr = VR::get();
        if (vr->is_hmd_active() && vr->get_runtime()->is_openxr() && !vr->m_openxr->panel_mode && !vr->m_openxr->flat_screen_mode) {
            const Vector4f f = vr->picture_frustum_of(vr->m_presenter_frame_count.load());
            if (f[1] > f[0] && f[2] > f[3]) {
                shift.x = ((0.0f - f[0]) / (f[1] - f[0]) - 0.5f) * draw_data->DisplaySize.x;
                shift.y = (f[2] / (f[2] - f[3]) - 0.5f) * draw_data->DisplaySize.y;
            }
        }
        draw_data->DisplayPos.x -= shift.x;
        draw_data->DisplayPos.y -= shift.y;
    }

    ImGui_ImplDX12_RenderDrawData(draw_data, cmd_ctx->cmd_list.Get());

    if (in_headset && draw_data != nullptr) {
        draw_data->DisplayPos.x += shift.x;
        draw_data->DisplayPos.y += shift.y;
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

    cmd_ctx->execute();
}

// The game window is usually much larger than the monitor (it is resized to the headset
// resolution), so the menu is scaled with the backbuffer height to stay readable.
void Framework::apply_ui_scale() {
    float scale = m_ui_scale_setting;

    if (scale <= 0.0f) {
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

    for (auto i = 0; i < 3; ++i) {
        auto& ctx = m_d3d12.cmd_ctxs.emplace_back(std::make_unique<d3d12::CommandContext>());

        if (!ctx->setup(L"vrcore::Framework::cmd_ctx")) {
            spdlog::error("[D3D12] Failed to create command context");
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = (UINT)D3D12::RTV::COUNT;
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

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = device;
    init_info.CommandQueue = m_d3d12_hook->get_command_queue();
    init_info.NumFramesInFlight = (int)swapchain_desc.BufferCount;
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
    const ImGuiCond cond = relayout ? ImGuiCond_Always : ImGuiCond_Appearing;
    const ImVec2 size{std::min(620.0f * scale, view.x * 0.5f), std::min(760.0f * scale, view.y * 0.6f)};
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), cond, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, cond);
    ImGui::SetNextWindowCollapsed(false, cond);
    ImGui::SetNextWindowBgAlpha(0.95f);
    m_menu_layout_dirty = false;
    m_menu_layout_w = view.x;
    m_menu_layout_h = view.y;
    m_menu_layout_scale = scale;

    bool open = true;

    if (ImGui::Begin("starfield2vr", &open)) {
        ImGui::TextWrapped("F11 toggles this menu. Both joystick clicks toggle it in VR.");

        if (!m_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", m_error.c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::Button("Save Config")) {
            save_config();
        }

        if (m_mods_initialized) {
            ImGui::SeparatorText("Quick tests");
            m_mods->on_draw_quick_ui();
            ImGui::Separator();
        }

        if (ImGui::CollapsingHeader("Menu")) {
            if (ImGui::Checkbox("Show this menu in the headset", &m_menu_in_headset)) {
                m_wants_save_config = true;
            }

            if (ImGui::SliderFloat("Menu scale (0 = automatic)", &m_ui_scale_setting, 0.0f, 6.0f, "%.1f")) {
                m_wants_save_config = true;
            }

            if (ImGui::Button("Centre this menu")) {
                m_menu_layout_dirty = true;
            }
        }

        ImGui::Separator();

        if (m_mods_initialized) {
            m_mods->on_draw_ui();
        }
    }

    ImGui::End();

    if (!open) {
        set_draw_ui(false);
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
    cfg.set<float>(UI_SCALE_KEY, m_ui_scale_setting);

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
}
