// Adapted from REFramework (MIT, praydog): eye selection now follows the frame index of the
// frame being presented (VR::get_present_eye) and OpenXR copies are size-checked.
#include <openvr.h>
#include <utility/ScopeGuard.hpp>

#include "Framework.hpp"
#include "mods/VR.hpp"

#include <ResourceUploadBatch.h>
#include <CommonStates.h>
#include <RenderTargetState.h>
#include <ScreenGrab.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <utility>
#include <vector>
#include <wincodec.h>

#include "mods/vr/D3D12Component.hpp"
#include "mods/vr/LaserSight.hpp"
#include "nvidia/UpscalerAfrNvidiaModule.h" // (fix179: the DLSS calls' time in a menu's slow frames)
#include "ModSettings.h"

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    m_menu_layer.present_started = std::chrono::steady_clock::now(); // (fix192: for the log, the work before the layer's check)
    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }
    sample_memory(); // (fix165: once a second)
    free_retired();  // (fix181: pictures let go earlier, freed once the graphics card is past them)

    auto& hook = g_framework->get_d3d12_hook();

    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    // Stereo screenshot (F11 > VR > Stereo): save the next left and right eye images once the
    // menu is closed, so the two eyes can be compared side by side.
    if (vr->m_screenshot_armed && !g_framework->is_drawing_ui()) {
        const auto now = std::chrono::steady_clock::now();
        if (vr->m_screenshot_not_before == std::chrono::steady_clock::time_point{}) {
            vr->m_screenshot_not_before = now + std::chrono::seconds(2);
        }
        if (now >= vr->m_screenshot_not_before) {
            if (!vr->m_screenshot_done[0] && !vr->m_screenshot_done[1] && vr->m_trace_remaining.load() <= 0) {
                vr->m_trace_remaining = 12; // also log what each eye's frame did
            }
            const auto eye = vr->get_present_eye();
            const auto idx = eye == VRRuntime::Eye::LEFT ? 0 : 1;
            if (!vr->m_screenshot_done[idx]) {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const auto path = Framework::get_persistent_dir(
                    std::format("stereo_{}_{}.jpg", vr->m_screenshot_number, idx == 0 ? "left" : "right"));
                m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
                const auto hr = DirectX::SaveWICTextureToFile(command_queue, backbuffer.Get(), GUID_ContainerFormatJpeg, path.wstring().c_str(),
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                spdlog::info("[VR] stereo screenshot {} eye -> {} (hr {:x})", idx == 0 ? "left" : "right", path.string(), (uint32_t)hr);
                vr->m_screenshot_done[idx] = true;
            }
            if (vr->m_screenshot_done[0] && vr->m_screenshot_done[1]) {
                vr->m_screenshot_armed = false;
            }
        }
    }

    // Render-steps test: its pictures put together once their frame has run (nothing kept otherwise).
    process_step_snapshots(command_queue);

    if (!m_backbuffer_is_8bit) {
        auto command_list = m_backbuffer_copy.commands.cmd_list.Get();
        m_backbuffer_copy.commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        m_backbuffer_copy.commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        m_backbuffer_copy.commands.clear_rtv(m_converted_eye_tex, clear_color, D3D12_RESOURCE_STATE_PRESENT);

        // Convert the backbuffer to 8-bit. (Left in the present state: every copy from it below, and the clear above next frame,
        // take it from that state.)
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        m_backbuffer_copy.commands.execute();
    }

    auto eye_texture = m_backbuffer_is_8bit ? backbuffer : m_converted_eye_tex.texture;

    auto runtime = vr->get_runtime();
    const auto frame_count = vr->m_frame_count;
    const auto present_eye = vr->get_present_eye();
    // A game menu on the panel that stays in the room (OpenXR).
    const bool panel = runtime->is_openxr() && vr->m_openxr->panel_mode;
    // Mono (OpenXR): this frame is both eyes' picture, and a headset frame of its own. (A full stereo frame with a menu's panel up,
    // as one opens before mono in menus takes over: shown the panel's mono way.)
    const bool mono = (vr->m_present_mono || ((vr->m_present_native || vr->m_present_aer) && panel)) && runtime->is_openxr();
    // Full stereo (OpenXR): both eyes drawn in this frame, a headset frame of its own: the left eye's picture the world's view (copied
    // as its last step drew it), the right eye's the game's picture (its view drawn last, the interface laid on it by the game).
    const bool native = vr->m_present_native && runtime->is_openxr() && !panel;
    // AER (OpenXR, fix136): this frame's eye fresh, the other eye's last picture with it, a headset frame of its own.
    const bool aer = vr->m_present_aer && runtime->is_openxr() && !panel;
    // (fix138) The frustum the game's picture was drawn with: mono's; in a full stereo frame the right eye's (its view drawn last),
    // or the left eye's when the right eye's view was not drawn in it (the world camera's); else the eye's whose turn it was.
    const Vector4f game_frustum = vr->m_present_mono     ? picture_frustum(true, 0)
                                : vr->m_present_native ? picture_frustum(false, vr->native_drawn(vr->m_presenter_frame_count.load()) ? 1 : 0)
                                                       : picture_frustum(false, present_eye == VRRuntime::Eye::LEFT ? 0 : 1);
    if ((vr->m_present_native || vr->m_present_aer) && panel && runtime->is_openxr()) {
        vr->m_openxr->mono_views = true; // (shown as mono: from the head's middle too)
    }
    // (fix148) The gun laser's beam drawn into each picture of the world handed to the headset: the frame being presented, its own camera.
    const OpenXR::Beam own_beam{true, vr->m_presenter_frame_count.load(), false, 0.0f};
    if (!aer) {
        m_aer.pose_valid[0] = m_aer.pose_valid[1] = false; // (AER again: each eye's pose from its first picture then)
    }
    const bool openxr_ready = runtime->is_openxr() && vr->m_openxr->ready();
    // (This present's own submit poses and AER mark only: a frame that could not be ended leaves none behind, fix136's review.)
    if (runtime->is_openxr()) {
        vr->m_openxr->submit_valid = false;
        vr->m_openxr->aer_frame    = false;
    }
    // World pictures are kept for the world behind a menu's panel (not on the flat screen, nor with no world shown there).
    const bool keep_history = openxr_ready && !panel && !vr->m_openxr->flat_screen_mode && vr->m_openxr->panel_enabled.load() &&
                              vr->m_openxr->panel_backdrop.load() != 0;

    // The world behind the panel: when a menu opens, the whole world picture before the newest (the newest may already show
    // the menu), kept until the menu closes; then the pictures are gathered anew.
    auto& history = m_world_history;
    // (fix192: also used when a world is allowed behind the panel only after it came up, below.)
    const auto world_before_newest = [&history]() {
        int newest = -1;
        int before = -1;
        for (int i = 0; i < 3; ++i) {
            if (history.order[i] != 0 && (newest < 0 || history.order[i] > history.order[newest])) {
                newest = i;
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (i != newest && history.order[i] != 0 && (before < 0 || history.order[i] > history.order[before])) {
                before = i;
            }
        }
        return before;
    };
    if (panel && !history.panel_was_up) {
        const int before = world_before_newest();
        history.frozen = before;
        history.world_allowed_seen = vr->m_openxr->panel_world_allowed; // (fix192)
        history.load_seen = ModSettings::g_internalSettings.menuLoadingScreen; // (fix192)
        // (fix192) This menu's own log of the layer's copy, its runs of the game's own picture, its empty see-through frames.
        m_menu_layer.stats = {};
        m_menu_layer.raw_run = false;
        m_menu_layer.raw_frames = m_menu_layer.raw_top = 0;
        m_menu_layer.raw_oldest_ms = 0;
        m_menu_layer.empty = 0;
        m_menu_layer.wanted_was = false;
        m_menu_layer.sees_was = false;
        // (Once, for the log: the world just before the menu, from the eye, to compare with the world kept drawn behind it.)
        // (fix159: not saved any more: it can only be taken as the menu opens, and the save stalled the frame then: a menu still
        // glitched for a second as it opened.)
        if (false && !m_live.before_snapped && before >= 0 && m_openvr.left_eye_tex[before].texture != nullptr) {
            m_live.before_snapped = true;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const auto path = Framework::get_persistent_dir("menu_world_before.jpg");
            m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
            const auto hr = DirectX::SaveWICTextureToFile(command_queue, m_openvr.left_eye_tex[before].texture.Get(), GUID_ContainerFormatJpeg,
                path.wstring().c_str(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            spdlog::info("[VR] Menu panel: the world just before the menu saved to {} (hr {:x})", path.string(), (uint32_t)hr);
        }
        m_menu_layer.panel_snapped = false;
        m_menu_layer.shown_since   = std::chrono::steady_clock::now(); // (fix159: the pictures for the log 3 s from here, not on this first frame)
        m_menu_layer.mode_logs = 0;
        m_live.flips_logged = 0;
        m_live.last_good_valid = false;
        m_live.was_live = false;
        vr->m_openxr->last_live_valid = false;
        spdlog::info("[VR] Menu panel: up ({}; behind it {}; the runtime {} curved panels)", vr->m_openxr->panel_curved.load() ? "curved" : "flat",
            !vr->m_openxr->panel_world_allowed ? "darkness (no world: main menu or loading)"
            : before >= 0                       ? (history.mono[before] ? "the world as it was (a mono picture)" : "the world as it was")
                                                : "darkness (no world picture kept yet)",
            runtimes::OpenXR::ext_cylinder ? "offers" : "does not offer");
    } else if (!panel && history.panel_was_up) {
        auto& xr = vr->m_openxr;
        spdlog::info("[VR] Menu panel: down (frames with it {}, curved {}, with the world as it was behind it {}, as it is now {}{}{}); "
                     "the live world shown from this frame's picture {} times, the frame before's {}, none {}; the 3D pictures cut out by depth {}, by colour {}",
            xr->stat_panel_frames, xr->stat_cylinder_frames, xr->stat_frozen_frames, xr->stat_live_frames,
            xr->panel_cylinder_refused ? "; curved panels refused by the runtime" : "", xr->panel_refused ? "; the panel refused by the runtime" : "",
            m_live.world_this, m_live.world_previous, m_live.world_missing, m_live.depth_cut, m_live.color_cut);
        m_live.world_this = m_live.world_previous = m_live.world_missing = 0;
        m_live.depth_cut = m_live.color_cut = 0;
        history.clear();
        history.slot = 0;
        m_menu_layer.was_used = false;
        m_menu_layer.good_valid = false; // (fix173: the last see-through picture never shown again for the next menu)
        m_menu_layer.holding = false;
        if (m_menu_layer.held > 0 || m_menu_layer.fallbacks > 0) {
            spdlog::info("[VR] (fix173) Menu panel: with this menu the last see-through picture was shown again {} frames, the game's whole picture "
                         "{} frames while the game showed a see-through menu", m_menu_layer.held, m_menu_layer.fallbacks);
        }
        m_menu_layer.held = m_menu_layer.fallbacks = 0;
        // (fix192) A run of the game's own picture still going as the panel went down: its end logged.
        {
            auto& L = m_menu_layer;
            if (L.raw_run) {
                L.raw_run = false;
                if (L.raw_logs < 120) {
                    ++L.raw_logs;
                    spdlog::info("[VR] (fix192) Menu panel: the game's own picture shown (opaque) until the panel went down: {} frames, {} ms ({} of them "
                                 "with the menu's box taken from the top rows; the oldest copy of the layer then {} ms)",
                        L.raw_frames, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - L.raw_since).count(),
                        L.raw_top, L.raw_oldest_ms);
                }
            }
            // (fix192) The layer's copy this menu: how often a picture was made from it, and how old the copies were (those over 60 ms were
            // refused before fix192: the game's own picture on the panel after 6 frames, the menu's top half cut, not see-through).
            const auto& s = L.stats;
            if ((s.made > 0 || s.refused > 0 || L.empty > 0) && L.stats_logs < 60) {
                ++L.stats_logs;
                spdlog::info("[VR] (fix192) Menu panel: the interface layer's copy with this menu: {} see-through pictures made from it; {} of them "
                             "from a copy over 60 ms old (before fix192 each refused), {} from a copy older than the frame before (used while the game "
                             "still showed a see-through menu); the oldest used {} ms, at most {} frames behind; refused {} times; the empty see-through "
                             "picture (no menu yet) {} frames; the mod's own work on the present thread before the check {:.1f} ms on average, the "
                             "longest {:.1f} ms",
                    s.made, s.over_60, s.older_used, s.oldest_ms, s.most_behind, s.refused, L.empty,
                    s.before_n > 0 ? s.before_sum_ms / (double)s.before_n : 0.0, s.before_max_ms);
            }
            L.stats = {};
            L.empty = 0;
            L.wanted_was = false;
            L.sees_was = false;
        }
        vr->m_openxr->panel_image_alpha = false;
        m_live.down_at = std::chrono::steady_clock::now();
        m_menu_release_pending = true; // (fix163)
        m_live.down_presents = presents_now();
        m_live.previews_logged = false; // (the next menu's 3D pictures saved too, the first few menus)
    } else if (!panel && !keep_history && history.order != std::array<uint64_t, 3>{}) {
        // (Not kept now, so what is kept would only grow old: gathered anew when they are kept again.)
        history.clear();
        history.slot = 0;
    }
    // (fix192) A loading screen shown on the panel since it came up: the pictures kept are of before the load (the panel stayed up into a
    // menu or pop-up after it, never down between to clear them): not taken for the world behind it.
    if (panel && ModSettings::g_internalSettings.menuLoadingScreen) {
        history.load_seen = true;
    }
    // A loading screen (or the main menu) while the panel is up: the world kept is not where you will be.
    if (panel && !vr->m_openxr->panel_world_allowed) {
        history.frozen = -1;
    } else if (panel && !history.world_allowed_seen && !history.load_seen && history.frozen < 0) {
        // (fix192) A world allowed behind the panel only after it came up (a menu opened in the 1.5 s after a loading screen): the world as
        // it was worked out again now, from the pictures kept since that loading screen (each panel going down clears them, and none are
        // kept while it is up). It stayed unknown for the whole menu before, and with no world to show behind it the panel showed the
        // game's own picture, opaque (10:16:10 in fix191's test: 100 frames). Only the first time a world is allowed in this panel's time,
        // and never after a loading screen shown on this panel (load_seen): the pictures kept would be of before the load, not where you are.
        const int before = world_before_newest();
        if (before >= 0) {
            history.frozen = before;
            static int told = 0;
            if (told++ < 20) {
                spdlog::info("[VR] (fix192) Menu panel: a world allowed behind it after it came up: the world as it was taken now ({})",
                    history.mono[before] ? "a mono picture" : "both eyes");
            }
        }
    }
    if (panel && vr->m_openxr->panel_world_allowed) {
        history.world_allowed_seen = true;
    }
    // (The live world's pictures, a few seconds after the panel went down: let go, their video memory with them.)
    // (And as long past the game's last copy into them: a frame in flight as the panel went down may have made one.)
    // (fix163) With full stereo on, its left eye keeps the world's copies (release_live_pictures(native)): only the menu's own copies
    // waited for, the left eye's each frame never growing old.
    const auto copies_done = [this](bool world_kept) {
        std::scoped_lock _{m_live.mtx};
        const auto& last = world_kept ? m_live.last_menu_copy : m_live.last_copy;
        return last.at == std::chrono::steady_clock::time_point{} || copy_done(last, std::chrono::steady_clock::now());
    };
    if (!panel && m_live.down_at != std::chrono::steady_clock::time_point{} &&
        copy_done(RetiredCopy{nullptr, m_live.down_at, m_live.down_presents}, std::chrono::steady_clock::now()) && copies_done(native)) {
        m_live.down_at = {};
        m_menu_release_pending = false; // (fix163)
        bool kept = m_live.eye[0].texture != nullptr || m_live.eye[1].texture != nullptr;
        {
            std::scoped_lock _{m_live.mtx};
            for (const auto& kind : m_live.slots) {
                for (const auto& slot : kind) {
                    kept = kept || slot.copy != nullptr;
                }
            }
            for (const auto& kind : m_live.depth) {
                for (const auto& slot : kind) {
                    kept = kept || slot.copy != nullptr;
                }
            }
            kept = kept || !m_live.retired.empty();
        }
        if (kept) {
            release_live_pictures(native);
        }
    }
    // (Full stereo stopped a few seconds ago, no menu up: its left eye's image and the world's copies let go, their video memory with
    // them.)
    // (fix142: not while it stands down only for the hand scanner, nor for 3 s after it closes (full stereo is back by then): let
    // go and made again around each scan, a hitch each way.)
    if (vr->m_native_brief_pause.load() && m_native.last_at != std::chrono::steady_clock::time_point{}) {
        m_native.last_at = std::chrono::steady_clock::now();
    }
    if (!native && !panel && m_native.left.texture != nullptr && m_native.last_at != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - m_native.last_at > std::chrono::seconds(3) && copies_done(false)) {
        // (fix181) Let go without waiting for the graphics card: freed once it has passed the image's last commands and the headset copies.
        RetiredGpu retired{.what = "full stereo's left eye image (stopped a few seconds ago)"};
        retire_texture(retired, m_native.left);
        note_copy_fences(retired);
        retire(std::move(retired));
        m_native.size[0] = m_native.size[1] = 0;
        m_native.kept_valid = false; // (fix147: its last picture gone with it)
        release_live_pictures();
        spdlog::info("[VR] full stereo: its pictures let go (stopped a few seconds ago)");
    } else if (!native && panel && vr->m_present_mono && m_native.left.texture != nullptr &&
               m_native.last_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - m_native.last_at > std::chrono::seconds(1)) {
        // (fix173) A menu shown in mono, full stereo stopped for it a second ago: its left eye's image let go now, not kept the whole menu
        // (an eye-sized picture: video memory the menu's own pictures and views need as it opens). Made again when full stereo is back. The
        // world's copies stay (the menu uses them). (Not while the game still draws both eyes behind the menu, MonoInMenus off: the review.)
        // (fix181) Without waiting for the graphics card (fix180's test: 490 ms waited here 1 s into the Data menu, every copy queued
        // behind the game's work, the game held up with it): freed once the graphics card has passed its last commands and the copies.
        RetiredGpu retired{.what = "full stereo's left eye image (a menu shown in mono)"};
        retire_texture(retired, m_native.left);
        note_copy_fences(retired);
        retire(std::move(retired));
        m_native.size[0] = m_native.size[1] = 0;
        m_native.kept_valid = false;
        static int told = 0;
        if (told++ < 20) {
            spdlog::info("[VR] (fix173) full stereo: its left eye's image let go (a menu shown in mono)");
        }
    }
    history.panel_was_up = panel;
    if (!panel) {
        m_ambient.was_on = false; // (fix164: the soft light round the panel started anew, faded in again, as the panel comes up)
    }
    // (Behind a menu: its frames' times and the video memory, for the log.)
    if (openxr_ready) {
        note_menu_frame(panel);
        // (The gun's laser sight: where its line meets what is drawn, read from the game's depth: LaserSight.)
        auto& laser = LaserSight::get();
        if (laser.wanted()) {
            ensure_custom_batches();
        }
        laser.on_present(m_laser_batch.get(), vr->m_presenter_frame_count.load());
    }

    if (panel) {
        // The game's picture goes to the panel's own image; the eye images get the world as it was (or nothing). See-through
        // (a menu drawn by the interface alone, with the world kept behind it): the game's interface layer over a see-through
        // dark instead (compose_menu_layer), blended over that world. With the world kept drawn behind it (LiveWorld, the game
        // side), each frame's world goes into its eye image as in play, and the panel is over the world as it is now.
        const uint32_t present_frame = vr->m_presenter_frame_count.load();
        const bool live_wanted = openxr_ready && vr->m_openxr->panel_live_world.load() && vr->m_openxr->panel_see_through.load() &&
                                 vr->m_openxr->panel_world_allowed && vr->m_openxr->panel_backdrop.load() != 0 &&
                                 ModSettings::g_internalSettings.menuLiveWorld && vr->m_present_index_ok.load();
        const bool live_now = live_wanted && present_live_world(vr, present_frame, mono, present_eye == VRRuntime::Eye::LEFT);
        if (!mono && present_eye == VRRuntime::Eye::LEFT) {
            m_live.left_ok = live_now;
            m_live.left_frame = present_frame;
        }
        if ((mono || present_eye == VRRuntime::Eye::RIGHT) && openxr_ready) {
            // (Both eyes' this pair: the left one the frame before this one.)
            const bool live_pair = live_now && (mono || (m_live.left_ok && m_live.left_frame + 1 == present_frame));
            m_live.left_ok = false;
            // (A pair whose world did not come, shortly after one that did: that one shown again, from where it was seen.)
            // (Only while both eye images still have that pair: one drawn since with a later frame's world would not match the other
            // eye or where it was seen; then the world as it was is shown for the moment.)
            const bool eyes_have_pair = m_live.eye_has[0] && m_live.eye_frame[0] == m_live.last_good_left &&
                                        (m_live.last_good_mono || (m_live.eye_has[1] && m_live.eye_frame[1] == m_live.last_good_frame));
            const bool live_hold = live_wanted && !live_pair && m_live.last_good_valid && present_frame - m_live.last_good_frame <= 45 &&
                                   vr->m_openxr->last_live_valid && m_live.eye[0].texture != nullptr && m_live.eye[1].texture != nullptr &&
                                   eyes_have_pair;
            ID3D12Resource* panel_source = eye_texture.Get();
            bool see_through = false;
            // (Only with the world shown behind the panel: with darkness there, or no world kept yet, its 3D views would only be lost.)
            const bool layer_wanted = vr->m_openxr->panel_see_through.load() && vr->m_openxr->panel_world_allowed &&
                                      vr->m_openxr->panel_backdrop.load() != 0 && (history.frozen >= 0 || live_pair || live_hold);
            bool held_layer = false;
            bool empty_layer = false; // (fix192)
            // (fix192) Since when the game shows a see-through menu (with the panel set see-through): each such spell its own, its first
            // moments and its pictures shown again only its own.
            const bool see_through_spell = ModSettings::g_internalSettings.menuSeeThrough && vr->m_openxr->panel_see_through.load();
            if (see_through_spell && !m_menu_layer.wanted_was) {
                m_menu_layer.wanted_since = std::chrono::steady_clock::now();
            }
            m_menu_layer.wanted_was = see_through_spell;
            const bool sees_through_now = layer_wanted && see_through_spell;
            // (fix192) The empty see-through picture's half second counted from when the see-through picture became possible (a world to
            // show behind the panel too): a world that comes late still gets its first moments without the game's own picture.
            if (sees_through_now && !m_menu_layer.sees_was) {
                m_menu_layer.empty_since = std::chrono::steady_clock::now();
            }
            m_menu_layer.sees_was = sees_through_now;
            // (fix192) A see-through picture made from the layer since this spell began (not a menu's before it), or less than 0.3 s before
            // it began (a see-through menu's flags dropped for a frame or two between two see-through menus: the same spell, in effect).
            const bool good_this_spell = m_menu_layer.good_valid && (m_menu_layer.good_at >= m_menu_layer.wanted_since ||
                                                                     m_menu_layer.wanted_since - m_menu_layer.good_at < std::chrono::milliseconds(300));
            if (layer_wanted && compose_menu_layer(vr, eye_texture.Get(), command_queue, present_frame)) {
                panel_source = m_menu_layer.composed.texture.Get();
                see_through = true;
                m_menu_layer.good_valid = true;
                m_menu_layer.good_at = std::chrono::steady_clock::now();
                m_menu_layer.good_presents = presents_now();
            } else if (sees_through_now && good_this_spell && eye_texture != nullptr && m_menu_layer.composed.texture != nullptr &&
                       m_menu_layer.composed_size[0] == (uint32_t)eye_texture->GetDesc().Width &&
                       m_menu_layer.composed_size[1] == eye_texture->GetDesc().Height &&
                       std::chrono::steady_clock::now() - m_menu_layer.good_at < std::chrono::milliseconds(2000)) {
                // (fix173) Not made this frame (its layer's copy late after a stall, the picture changing size as a menu opens over another),
                // while the game still shows a see-through menu: the last see-through picture shown again, not the game's raw picture (the
                // flash as the pause menu opened over the Data menu: 23:00:24.749, 21:09:54.038).
                // (fix192) As long as needed, up to 2 s, not 6 frames: after those the game's own picture came, which while the frame is
                // kept full size has the menu in its top rows and the 3D pictures stretched to the whole height; the panel's centred rows of
                // it showed only the menu's lower half, blown up, not see-through (fix191's test: the Data menu at 10:02:59-10:03:05 and
                // the Inventory at 10:18:07-10:18:20, the frames 70-100 ms long). Only a picture of this spell of see-through menus.
                panel_source = m_menu_layer.composed.texture.Get();
                see_through = true;
                held_layer = true;
                ++m_menu_layer.held;
            } else if (sees_through_now && !good_this_spell &&
                       std::chrono::steady_clock::now() - m_menu_layer.empty_since < std::chrono::milliseconds(500) &&
                       compose_menu_empty(vr, eye_texture.Get())) {
                // (fix192) The first moments of a see-through menu, no picture made from its layer yet (its first copy not made or not yet
                // this menu's): the panel's dark alone, see-through, for at most half a second (a frame or three), not the game's own
                // picture, cut wrong while the frame is kept full size. (Not "no panel image": then the game's picture goes into the eye
                // images as the old screen that moves with the head.)
                panel_source = m_menu_layer.composed.texture.Get();
                see_through = true;
                empty_layer = true;
                ++m_menu_layer.empty;
            }
            // (fix192) The game's own picture on the panel: where the menu's box is in it. While the frame is kept full size behind the menu
            // (FullFrame), the game draws its interface layer the picture's whole height with the menu in its top rows (its movies draw from
            // their viewport's top) and lays it over its picture as it is: the box is the top rows then, not the centred ones the game's own
            // 16:9 box has. Known from the layer the game drew in the frame shown (or the one before): as tall as the picture, or not.
            // (Only in play with a world behind the menu, as FullFrame only is, and never on a loading screen: its photo band is centred in
            // a layer of the whole height, which full stereo's interface copy also notes.)
            int box_top = -1;
            uint32_t seen_layer_h = 0;
            const bool layer_seen = !see_through && menu_layer_height_of(present_frame, seen_layer_h);
            const uint32_t picture_w = eye_texture != nullptr ? (uint32_t)eye_texture->GetDesc().Width : 0;  // (fix192: none if setup failed)
            const uint32_t picture_h = eye_texture != nullptr ? eye_texture->GetDesc().Height : 0;
            const int box_rows = vr->m_openxr->panel_box_rows;
            if (!see_through && layer_seen && picture_h > 0 && box_rows > 0 && (uint32_t)box_rows + 2 < picture_h &&
                std::abs((int64_t)seen_layer_h - (int64_t)picture_h) <= 2 && vr->m_openxr->panel_world_allowed &&
                !ModSettings::g_internalSettings.menuLoadingScreen) {
                box_top = 0;
            }
            vr->m_openxr->panel_box_top = box_top;
            // (fix173) Each fall back to the game's whole picture while the game shows a see-through menu, and each run of pictures shown again,
            // logged with why (at most a line a second, 60 a session; the frames counted between lines).
            {
                auto& L = m_menu_layer;
                const bool fallback = !see_through && ModSettings::g_internalSettings.menuSeeThrough && vr->m_openxr->panel_see_through.load();
                L.fallbacks += fallback ? 1 : 0;
                const auto now = std::chrono::steady_clock::now();
                const char* why = !layer_wanted ? (!vr->m_openxr->panel_world_allowed ? "no world behind the menu allowed"
                                                   : vr->m_openxr->panel_backdrop.load() == 0 ? "the backdrop set to darkness"
                                                   : "no world picture to show behind it (none kept, none drawn this frame or held)")
                                                : L.fail_reason != nullptr ? L.fail_reason : "?";
                if ((fallback || (held_layer && !L.holding)) && L.fallback_logs < 60 && now - L.fallback_logged_at >= std::chrono::seconds(1)) {
                    L.fallback_logged_at = now;
                    ++L.fallback_logs;
                    spdlog::info("[VR] (fix173) Menu panel: {} ({}{}; video memory {} of {} MB{}; so far {} frames shown again, {} of the game's whole picture)",
                        held_layer ? "the last see-through picture shown again" : "the game's whole picture shown (opaque), not the see-through one", why,
                        layer_wanted ? std::format("; the layer's copy {} ms old, {}x{}, the eye's picture {}x{}", L.fail_age_ms, L.fail_layer[0],
                                           L.fail_layer[1], L.fail_eye[0], L.fail_eye[1])
                                     : std::string{},
                        m_mem_used_mb.load(), m_mem_budget_mb.load(), m_mem_over_since_ms.load() != 0 ? ", OVER its budget" : "", L.held, L.fallbacks);
                }
                L.holding = held_layer;
                // (fix192) Each run of the game's own picture while the game shows a see-through menu: at its start, why and where the menu's
                // box is taken from (the picture's layout); at its end, how long it was. (Never a line a frame: 120 lines a session at most.)
                if (fallback) {
                    if (!L.raw_run) {
                        L.raw_run = true;
                        L.raw_since = now;
                        L.raw_frames = L.raw_top = 0;
                        L.raw_oldest_ms = 0;
                        if (L.raw_logs < 120) {
                            ++L.raw_logs;
                            const int centred = box_rows > 0 && (uint32_t)box_rows < picture_h ? ((int)picture_h - box_rows) / 2 : 0;
                            const std::string layout =
                                box_top == 0 ? std::format("the frame kept full size behind the menu (the game's interface layer {} rows: the menu in its "
                                                           "top {} rows): the menu's box taken from rows 0 to {}, not the centred {} to {}",
                                                   seen_layer_h, box_rows, box_rows, centred, centred + box_rows)
                                : box_rows <= 0 || (uint32_t)box_rows + 2 >= picture_h
                                    ? std::string{"no menu box known: the whole height shown"}
                                : layer_seen ? std::format("the game's own 16:9 box, centred (the interface layer {} rows): rows {} to {}", seen_layer_h, centred,
                                                   centred + box_rows)
                                             : std::format("the interface layer not seen in this frame: the centred rows {} to {} shown", centred,
                                                   centred + box_rows);
                            spdlog::info("[VR] (fix192) Menu panel: the game's own picture shown (opaque) from here, not the see-through one ({}{}); "
                                         "its layout: {}; the picture {}x{}; video memory {} of {} MB{}",
                                why, layer_wanted ? std::format("; the layer's copy {} ms and {} frames old", L.fail_age_ms, L.fail_behind) : std::string{},
                                layout, picture_w, picture_h, m_mem_used_mb.load(), m_mem_budget_mb.load(),
                                m_mem_over_since_ms.load() != 0 ? ", OVER its budget" : "");
                        }
                    }
                    ++L.raw_frames;
                    L.raw_top += box_top == 0 ? 1 : 0;
                    if (layer_wanted) {
                        L.raw_oldest_ms = std::max<int64_t>(L.raw_oldest_ms, L.fail_age_ms);
                    }
                } else if (L.raw_run) {
                    L.raw_run = false;
                    if (L.raw_logs < 120) {
                        ++L.raw_logs;
                        spdlog::info("[VR] (fix192) Menu panel: {} after {} frames ({} ms) of the game's own picture ({} of them with the menu's box "
                                     "taken from the top rows; the oldest copy of the layer then {} ms)",
                            see_through ? (held_layer ? "the last see-through picture shown again" : empty_layer ? "the empty see-through picture"
                                                                                                                  : "see-through again")
                                        : "the game shows no see-through menu now",
                            L.raw_frames, std::chrono::duration_cast<std::chrono::milliseconds>(now - L.raw_since).count(), L.raw_top, L.raw_oldest_ms);
                    }
                }
            }
            if (see_through != m_menu_layer.was_used) {
                // (fix181) From see-through to the game's whole picture with the menu still up: the game side's flags then (each one a reason;
                // the stall-proof menu ages and LiveWorld's hold keep them through the game's long frames), at most 40 lines a session.
                if (!see_through && m_menu_layer.was_used) {
                    static int told = 0;
                    if (told++ < 40) {
                        const auto& s = ModSettings::g_internalSettings;
                        spdlog::info("[VR] (fix181) Menu panel: see-through lost with the menu up: a see-through menu {}, the world behind it {}, kept "
                                     "drawn {}; the picture's layer wanted {}, the live world this pair {}, the last pair held {}, the world as it "
                                     "was kept {}; the see-through picture {}",
                            s.menuSeeThrough ? "yes" : "no", s.menuWorldBehind ? "yes" : "no", s.menuLiveWorld ? "yes" : "no", layer_wanted ? "yes" : "no",
                            live_pair ? "yes" : "no", live_hold ? "yes" : "no", history.frozen >= 0 ? "yes" : "no",
                            !layer_wanted ? "not tried"
                            : m_menu_layer.fail_reason != nullptr ? std::format("not made: {}", m_menu_layer.fail_reason)
                                                                  : std::string{"not made"});
                    }
                }
                m_menu_layer.was_used = see_through;
                m_menu_layer.used_since = std::chrono::steady_clock::now();
                if (m_menu_layer.mode_logs++ < 8) {
                    spdlog::info("[VR] Menu panel: {}", see_through ? "see-through (the game's interface layer over the world behind the menu)"
                                                                    : "the game's whole picture (opaque)");
                }
            }
            const bool panel_ok = m_openxr.copy_panel(panel_source);
            vr->m_openxr->panel_image_ready = panel_ok;
            vr->m_openxr->panel_image_alpha = panel_ok && see_through;
            // A menu shown anew (opened, or another of its tabs: the game's count, ModSettings): its pictures saved again (fix128: the
            // Missions and Ships tabs, opened from the Data menu, had none).
            if (const unsigned serial = ModSettings::g_internalSettings.menuShownSerial; serial != m_menu_layer.shown_serial) {
                m_menu_layer.shown_serial = serial;
                m_menu_layer.shown_since = std::chrono::steady_clock::now();
                m_menu_layer.panel_snapped = false;
                m_live.previews_logged = false;
            }
            // The first menus: the panel's picture (see-through ones) and the game's whole picture saved, for the log (a moment's
            // pause), a second after the menu became see-through or opaque, or was shown anew (its first frames are only the HUD).
            // (In the game only: not the main menu or a loading screen, which would use them up.)
            if (panel_ok && !m_menu_layer.panel_snapped && m_menu_layer.snapshots < 8 && ModSettings::g_internalSettings.menuWorldBehind &&
                std::chrono::steady_clock::now() - std::max(m_menu_layer.used_since, m_menu_layer.shown_since) > std::chrono::seconds(3)) { // (fix159: 3 s, not 1: a save stalls the frame)
                m_menu_layer.panel_snapped = true;
                const int n = ++m_menu_layer.snapshots;
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const auto layer_path = Framework::get_persistent_dir(std::format("menu_seethrough_{}.png", n));
                const auto picture_path = Framework::get_persistent_dir(std::format("menu_picture_{}.jpg", n));
                m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
                HRESULT hr_layer = S_FALSE;
                if (see_through) {
                    hr_layer = DirectX::SaveWICTextureToFile(command_queue, m_menu_layer.composed.texture.Get(), GUID_ContainerFormatPng,
                        layer_path.wstring().c_str(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                } else {
                    std::error_code ignored{};
                    std::filesystem::remove(layer_path, ignored); // (an earlier game's of this number: not taken for this menu's)
                }
                const auto hr_picture = DirectX::SaveWICTextureToFile(command_queue, eye_texture.Get(), GUID_ContainerFormatJpeg,
                    picture_path.wstring().c_str(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                spdlog::info("[VR] Menu panel ({}): {}the game's whole picture saved to {} (hr {:x})", see_through ? "see-through" : "opaque",
                    see_through ? std::format("see-through picture saved to {} (hr {:x}), ", layer_path.string(), (uint32_t)hr_layer) : std::string{},
                    picture_path.string(), (uint32_t)hr_picture);
            }
            bool frozen_ok = false;
            const bool live_ok = panel_ok && see_through && live_pair;
            const bool live_shown = live_ok || (panel_ok && see_through && live_hold);
            if (live_shown != m_live.was_live) {
                m_live.was_live = live_shown;
                if (m_live.flips_logged++ < 8) {
                    spdlog::info("[VR] Menu panel: behind it {}", live_shown ? "the world as it is now (the game keeps drawing it)"
                                                                             : "the world as it was (or darkness)");
                }
            }
            m_live.live_pairs += live_ok ? 1 : 0;
            if (live_ok) {
                m_live.last_good_valid = true;
                m_live.last_good_frame = present_frame;
                m_live.last_good_left = mono ? present_frame : m_live.left_frame;
                m_live.last_good_mono = mono;
                // (The eye images have this pair's world already. The first time, a second on: its picture saved for the log.)
                if (!m_live.live_snapped && m_live.live_pairs > 72 && m_live.eye[0].texture != nullptr) {
                    m_live.live_snapped = true;
                    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    const auto path = Framework::get_persistent_dir("menu_live_world.jpg");
                    m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
                    const auto hr = DirectX::SaveWICTextureToFile(command_queue, m_live.eye[0].texture.Get(), GUID_ContainerFormatJpeg,
                        path.wstring().c_str(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                    spdlog::info("[VR] Menu panel: the live world behind it saved to {} (hr {:x})", path.string(), (uint32_t)hr);
                }
                // (Once, with your head turned well away from the menu: the live world then, to compare with the world just before the
                // menu (menu_world_before.jpg): drawn from where your head is, it is turned as far as the menu drawn into it is.)
                if (!m_live.turned_snapped && m_live.live_pairs > 72 && m_live.eye[0].texture != nullptr) {
                    if (const float turn = vr->m_openxr->head_turn_from_panel(); turn >= 15.0f) {
                        m_live.turned_snapped = true;
                        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        const auto path = Framework::get_persistent_dir("menu_live_world_turned.jpg");
                        m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
                        const auto hr = DirectX::SaveWICTextureToFile(command_queue, m_live.eye[0].texture.Get(), GUID_ContainerFormatJpeg,
                            path.wstring().c_str(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                        spdlog::info("[VR] Menu panel: the live world behind it saved with your head turned {:.0f} deg from the menu, to {} (hr {:x})",
                            turn, path.string(), (uint32_t)hr);
                    }
                }
            } else if (panel_ok && see_through && live_hold) {
                // The last pair shown again, from where it was seen (as the world as it was is).
                frozen_ok = m_openxr.copy(0, m_live.eye[0].texture.Get(), picture_frustum(m_live.last_good_mono, 0)) &&
                            m_openxr.copy(1, m_live.eye[m_live.last_good_mono ? 0 : 1].texture.Get(), picture_frustum(m_live.last_good_mono, 1));
                if (frozen_ok) {
                    vr->m_openxr->frozen_pose = vr->m_openxr->last_live_pose;
                    vr->m_openxr->frozen_fov = vr->m_openxr->last_live_fov;
                    vr->m_openxr->frozen_has_menu = m_live.eye_menu[0] && m_live.eye_menu[m_live.last_good_mono ? 0 : 1];
                    ++m_live.held_frames;
                }
            } else if (panel_ok) {
                const int f = history.frozen;
                // (fix183) Not behind the star map: opened in space, the world as it was is mostly black space (black showed outside of
                // the background); the soft light round the panel instead (below).
                if (f >= 0 && history.order[f] != 0 && vr->m_openxr->panel_backdrop.load() != 0 && vr->m_openxr->panel_world_allowed &&
                    !vr->m_openxr->panel_glow_when_empty) {
                    auto& left = m_openvr.left_eye_tex[f];
                    auto& right = history.mono[f] ? m_openvr.left_eye_tex[f] : m_openvr.right_eye_tex[f];
                    if (left.texture != nullptr && right.texture != nullptr) {
                        frozen_ok = m_openxr.copy(0, left.texture.Get(), picture_frustum(history.mono[f], 0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                    m_openxr.copy(1, right.texture.Get(), picture_frustum(history.mono[f], 1), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    }
                    if (frozen_ok) {
                        vr->m_openxr->frozen_has_menu = false;
                        vr->m_openxr->frozen_pose = history.pose[f];
                        vr->m_openxr->frozen_fov = history.fov[f];
                    }
                }
            } else {
                // (No panel image: the old screen that moves with your head.)
                m_openxr.copy(0, eye_texture.Get(), game_frustum);
                m_openxr.copy(1, eye_texture.Get(), game_frustum);
            }
            // (fix164) Nothing behind the panel (no world: the main menu, the intro, a loading screen; or none kept yet): a soft light round
            // it in its picture's colours (behind an opaque panel only: the game's whole picture). Not where "Around the panel" was set to
            // darkness in the game.
            // (Only where there is no world at all: never in play behind an opaque menu with no world kept; off at strength 0; the review.)
            // (fix183) And the star map (its surface map too): the world as it was is not shown behind it (above), the light instead.
            const bool ambient_wanted = panel_ok && !see_through && !live_shown && !frozen_ok && vr->m_openxr->panel_ambient.load() &&
                                        !vr->m_openxr->ambient_refused &&
                                        (!vr->m_openxr->panel_world_allowed || vr->m_openxr->panel_glow_when_empty) &&
                                        vr->m_openxr->panel_ambient_strength.load() >= 0.01f;
            vr->m_openxr->ambient_ready = ambient_wanted && present_ambient(vr, eye_texture.Get(), command_queue);
            if (!ambient_wanted) {
                m_ambient.was_on = false; // (started anew, faded in again, when it comes back)
            }
            vr->m_openxr->frozen_ready = frozen_ok;
            vr->m_openxr->live_world_ready = live_ok;
        }
    } else if (aer) {
        if (openxr_ready) {
            // AER: this frame's eye into its image, kept with the pose it was drawn with (this frame's); the other eye's image as it was
            // (its last picture, submitted again with its own pose). Not had one yet (AER just on, or new images): this picture for both,
            // from the head's middle as in mono.
            auto&          xr     = vr->m_openxr;
            const uint32_t pf     = vr->m_presenter_frame_count.load();
            const int      fresh  = present_eye == VRRuntime::Eye::LEFT ? 0 : 1;
            runtimes::OpenXR::FramePoses fp{};
            const bool     posed  = xr->frame_poses_of(pf, fp);
            // (Its pose known only with its frame's: else this picture is not kept as that eye's, fix136's review.)
            if (m_openxr.copy(fresh, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam)) {
                m_aer.pose[fresh]       = fp.stage[fresh];
                m_aer.pose_valid[fresh] = posed;
            }
            const int  other       = 1 - fresh;
            const bool one_picture = !xr->image_ever[other] || !m_aer.pose_valid[other] || !posed;
            // (The flat screen shows the right eye's image: given this picture every frame, fix136's review.)
            if (one_picture || (xr->flat_screen_mode && fresh == 0)) {
                // (The right eye's image now this picture: its own last one, and its pose, no longer there.)
                if (!one_picture) {
                    m_aer.pose_valid[other] = false;
                }
                m_openxr.copy(other, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam);
            }
            if (one_picture) {
                xr->mono_views = true;
                ++m_aer.one_picture;
            } else {
                ++m_aer.two_pictures;
            }
            if (keep_history) {
                // (One picture for both: kept as mono, from the left eye's copy, as mono keeps it.)
                write_world_history(eye_texture.Get(), one_picture || fresh == 0, one_picture);
            }
            xr->aer_frame = true;
            if (posed) {
                xr->submit_valid   = true;
                xr->submit_pose[0] = m_aer.pose_valid[0] ? m_aer.pose[0] : fp.stage[0];
                xr->submit_pose[1] = m_aer.pose_valid[1] ? m_aer.pose[1] : fp.stage[1];
                xr->submit_head    = fp.head;
            }
            if (const auto now = std::chrono::steady_clock::now(); now - m_aer.stats_at >= std::chrono::seconds(10)) {
                m_aer.stats_at = now;
                spdlog::info("[VR] AER: the last 10 s, {} frames shown with the other eye's last picture, {} with one picture for both",
                    m_aer.two_pictures, m_aer.one_picture);
                m_aer.two_pictures = m_aer.one_picture = 0;
            }
        }
    } else if (native) {
        if (openxr_ready) {
            // The left eye: this frame's world view with the interface over it (present_native_left), the right eye's the game's
            // picture. A frame with one picture only (its right eye's view not drawn: the game's picture is the left eye's; or the left
            // eye's copy not there: it is the right eye's): that picture for both eyes, shown from the middle of the head as in mono
            // (half an eye's distance off for each, not a whole one for one eye).
            // (fix147) The left eye's copy not there but its last picture recent (a frame or three before, its pose known): that one
            // shown again with the pose it was drawn with, as AER shows an eye's last picture, the right eye's this frame's with its
            // own: both eyes keep their own places. Only else the one picture for both from the head's middle (a flat frame, half an
            // eye's distance off: in turn with stereo frames it looked like stutter in one eye, fix146's test).
            const uint32_t present_frame = vr->m_presenter_frame_count.load();
            const bool right_drawn = vr->native_drawn(present_frame);
            const bool left_ok = right_drawn && present_native_left(present_frame);
            const bool left_again = !left_ok && right_drawn && present_native_previous_left(vr, present_frame);
            // (fix148) The game's picture's beam: the right eye's, its camera the left eye's moved by the eyes' distance (the frame's camera,
            // the depth's, is the left eye's); a frame with the right eye's view not drawn has the left eye's camera's picture. Only when
            // the left eye's picture got one too (both eyes or neither, the review: the left eye's last picture again needs its own frame's
            // depth as well).
            auto&      laser          = vrmod::LaserSight::get();
            const bool beam_both      = laser.beam_ready(present_frame) && (!left_again || laser.beam_ready(m_native.kept_frame));
            const OpenXR::Beam game_picture_beam{beam_both, present_frame, right_drawn, right_drawn ? vr->m_stereo_eyes_apart.load() : 0.0f};
            if (!left_ok && !left_again) {
                m_openxr.copy(0, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, game_picture_beam);
                vr->m_openxr->mono_views = true;
                ++m_native.one_picture;
                vr->m_native_one_picture_run.fetch_add(1);
            } else if (left_ok) {
                vr->m_native_one_picture_run.store(0);
            }
            // (A frame with the left eye's last picture again neither ends nor adds to the run of one-picture frames StereoView counts:
            // a copy missing again and again still comes to one-picture frames three frames on, and is counted then.)
            m_openxr.copy(1, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, game_picture_beam);
            m_native.last_at = std::chrono::steady_clock::now();
            if (keep_history) {
                write_world_history((left_ok || left_again) ? m_native.left.texture.Get() : eye_texture.Get(), true, false);
                write_world_history(eye_texture.Get(), false, false);
            }
            // (Once, about 10 s in: both eyes saved for the log, to compare the two views: native_left.jpg and native_right.jpg.)
            if (left_ok && !m_native.snapped && std::chrono::steady_clock::now() - m_native.first_at > std::chrono::seconds(10)) {
                m_native.snapped = true;
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                for (int e = 0; e < 2; ++e) {
                    const auto path = Framework::get_persistent_dir(e == 0 ? "native_left.jpg" : "native_right.jpg");
                    const auto hr = DirectX::SaveWICTextureToFile(command_queue, e == 0 ? m_native.left.texture.Get() : eye_texture.Get(),
                        GUID_ContainerFormatJpeg, path.wstring().c_str(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                    spdlog::info("[VR] full stereo: the {} eye's picture saved to {} (hr {:x})", e == 0 ? "left" : "right", path.string(), (uint32_t)hr);
                }
            }
        }
    } else if (mono) {
        if (openxr_ready) {
            m_openxr.copy(0, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam);
            m_openxr.copy(1, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam);
            if (keep_history) {
                write_world_history(eye_texture.Get(), true, true);
            }
        }
    } else if (present_eye == VRRuntime::Eye::LEFT) {
        // OpenXR texture
        if (openxr_ready) {
            m_openxr.copy(0, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam);
            if (keep_history) {
                write_world_history(eye_texture.Get(), true, false);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        // OpenXR texture
        if (openxr_ready) {
            m_openxr.copy(1, eye_texture.Get(), game_frustum, D3D12_RESOURCE_STATE_PRESENT, own_beam);
            if (keep_history) {
                write_world_history(eye_texture.Get(), false, false);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            m_openvr.copy_right(eye_texture.Get());

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    // The strip of small pictures (the gun sight's, the menu panel's veil, beam and dot), made the first time it is shown:
    // copied into the runtime's images as the eye pictures just were, at the same point of the frame (begun, the eye images
    // filled).
    if ((mono || native || aer || present_eye == VRRuntime::Eye::RIGHT) && openxr_ready && vr->m_openxr->frame_began &&
        (vr->m_openxr->reticle_visible.load() || panel)) {
        m_openxr.ensure_reticle();
    }
    // (fix145) The conversation's panel's image (record_conversation's cut), as the eye images were just filled; not with the menu panel up.
    if ((mono || native || aer || present_eye == VRRuntime::Eye::RIGHT) && openxr_ready) {
        if (panel) {
            vr->m_openxr->conv_image_ready = false;
        } else {
            present_conversation(vr);
        }
        // (fix177) Build mode's console's picture into its swapchain (not with the menu panel up: then only let go when old).
        present_console(vr, !panel);
        // (fix171) The favorites wheel's newest picture into its swapchain (only when one came while it is open: the game side closes it as a
        // menu opens, and end_frame never shows it with the menu panel up).
        present_wheel(vr);
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (mono || native || aer || present_eye == VRRuntime::Eye::RIGHT) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        // Pipelined (fix136): the pictures submitted with the poses they were drawn with (their frame's, kept by the game thread), not the
        // latest located (the game thread may be a frame ahead by now). (AER set its own above.) No frame begun here: the present thread
        // opens them after ending the one before (VR::on_post_present); none open (one could not be): nothing to end this time.
        const bool pipelined = runtime->is_openxr() && vr->pipelined();
        if (pipelined && !aer && !vr->m_openxr->submit_valid) {
            runtimes::OpenXR::FramePoses fp{};
            if (vr->m_openxr->frame_poses_of(vr->m_presenter_frame_count.load(), fp)) {
                vr->m_openxr->submit_valid   = true;
                vr->m_openxr->submit_pose[0] = fp.stage[0];
                vr->m_openxr->submit_pose[1] = fp.stage[1];
                vr->m_openxr->submit_head    = fp.head;
            }
        }
        if (runtime->is_openxr() && vr->m_openxr->ready() && !(pipelined && !vr->m_openxr->frame_began)) {
            if (!pipelined && (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began)) {
                vr->m_openxr->begin_frame();
            }

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;

            // The world picture just shown, kept with the views it was shown with (a whole one: both eyes written since its slot
            // was started); otherwise its slot is written again.
            if (keep_history) {
                auto& h = m_world_history;
                bool advanced = false;
                if (vr->m_openxr->last_world_views_valid && h.written[h.slot] == 3) {
                    h.order[h.slot] = h.next_order++;
                    h.pose[h.slot] = vr->m_openxr->last_world_pose;
                    h.fov[h.slot] = vr->m_openxr->last_world_fov;
                    h.slot = (h.slot + 1) % (int)h.order.size();
                    advanced = true;
                }
                // (AER: one eye a frame, so a slot half written is kept until its other eye comes, fix136's review.)
                if (advanced || !aer) {
                    h.written[h.slot] = 0;
                    h.order[h.slot] = 0;
                    h.mono[h.slot] = false;
                }
            }
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded (mono: every frame is the same view anyway).
        if (vr->m_desktop_fix->value() && !mono && !native && !aer) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::on_reset(VR* vr) {
    auto runtime = vr->get_runtime();
    // (fix181, the review) The pictures let go earlier: past first (waited for), before the drawing tools are made again below.
    free_retired(true);
    // (fix164) The soft light's last commands done first: they read the game's picture (or its converted copy, let go below; the review).
    m_ambient.commands.wait(INFINITE);

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();
    m_world_history.clear(); // (its textures are made anew)
    // (The see-through picture and its views of the interface layer: made anew; the layer's copy is the game side's to replace.)
    m_menu_layer.composed.reset();
    // (Full stereo's left eye image: made again at the new size.)
    m_native.left.reset();
    m_native.size[0] = m_native.size[1] = 0;
    m_native.kept_valid = false; // (fix147: its last picture gone with it)
    m_menu_layer.composed_size[0] = m_menu_layer.composed_size[1] = 0;
    for (auto& source : m_menu_layer.source) {
        source.reset();
    }
    m_menu_layer.source_of = {};
    // (The live world's eye images and its pictures' views: made anew; the copies are the game side's to replace.)
    for (auto& e : m_live.eye) {
        e.reset();
    }
    m_live.eye_size[0] = m_live.eye_size[1] = 0;
    m_live.eye_has[0] = m_live.eye_has[1] = false;
    m_live.last_good_valid = false;
    for (auto& kind : m_live.source) {
        for (auto& source : kind) {
            source.reset();
        }
    }
    m_live.source_of = {};
    m_live.left_ok = false;
    // (fix164) The soft light's last commands done (they read the game's picture, made anew); it starts anew.
    m_ambient.commands.wait(INFINITE);
    m_ambient.was_on = false;

    if (runtime->is_openxr() && runtime->loaded) {
        // OpenXR swapchains follow the game's backbuffer size; OpenXR::copy recreates them
        // when the backbuffer size changes, so nothing to do here.

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::setup() {
    spdlog::info("[VR] Setting up d3d12 textures...");
    // (fix181, the review) The pictures let go earlier and their command lists: past first (waited for here, a rare moment), before the
    // drawing tools those lists were recorded with are made again below.
    free_retired(true);

    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    const auto backbuffer_desc = backbuffer->GetDesc();

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    auto rt_desc = backbuffer_desc;

    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}", backbuffer_desc.Width, backbuffer_desc.Height);

    // Create converted eye texture
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> eye_tex{};
        // (Kept in the present state between frames: see on_frame.)
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create left eye texture.");
            return;
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }
 
    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create right eye texture.");
            return;
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    // (The sprite batches are made again: the last pictures drawn with them done first, the panel's and the live world's eye images'.)
    if (m_menu_layer.composed.texture != nullptr) {
        m_menu_layer.composed.commands.wait(INFINITE);
    }
    for (auto& e : m_live.eye) {
        if (e.texture != nullptr) {
            e.commands.wait(INFINITE);
        }
    }
    setup_sprite_batch_pso(rt_desc.Format);
    // (This mod's own pixel shaders' sprite batches: made again when next needed.)
    LaserSight::get().release(); // (its readings and copies, before the batch they are drawn with)
    m_laser_batch.reset();
    m_keyed_batch.reset();
    m_backing_batch.reset();
    m_depth_keyed_batch.reset();
    m_depth_pairs.reset();
    m_depth_pairs_of = {}; // (their views are made again in the new table)
    m_depth_keyed_root.Reset();
    m_panel_ray_batch.reset();
    m_panel_ray_source.reset();
    m_panel_ray_source_of = nullptr;
    // (fix164) The soft light's pictures and batches too (its last commands and copies done first).
    release_ambient();
    m_ambient_down_batch.reset();
    m_ambient_smooth_batch.reset();
    m_ambient_light_batch.reset();
    m_custom_batches_tried = false;

    m_backbuffer_size[0] = rt_desc.Width;
    m_backbuffer_size[1] = rt_desc.Height;

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state, const RECT* dest,
    DirectX::DX12::SpriteBatch* with, const float* color, const RECT* source) {
    const auto dst_desc = dst.texture->GetDesc();
    const auto src_desc = src.texture->GetDesc();
    
    DirectX::DX12::SpriteBatch* batch = with != nullptr ? with : m_sprite_batch.get();

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dst_desc.Width;
    scissor_rect.bottom = (LONG)dst_desc.Height;

    // Transition dst to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst.texture.Get();

    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { dst.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };
    if (dest != nullptr) {
        dest_rect = *dest;
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { src.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(src.get_srv_gpu(),
        DirectX::XMUINT2{ (uint32_t)src_desc.Width, (uint32_t)src_desc.Height },
        dest_rect, source,
        color != nullptr ? DirectX::XMVectorSet(color[0], color[1], color[2], color[3]) : DirectX::XMVECTOR(DirectX::Colors::White));

    batch->End();

    // Transition dst to dst_state
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = dst_state;
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Component::render_pair_to_rtv(ID3D12GraphicsCommandList* command_list, D3D12_GPU_DESCRIPTOR_HANDLE pair, uint32_t width, uint32_t height,
    const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES dst_state, const RECT& dest, const float* color) {
    const auto dst_desc = dst.texture->GetDesc();
    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    m_depth_keyed_batch->SetViewport(viewport);
    const D3D12_RECT scissor{0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height};
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = dst_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list->ResourceBarrier(1, &barrier);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[] = {dst.get_rtv()};
    command_list->OMSetRenderTargets(1, rtv, FALSE, nullptr);
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    // (The batch's own root signature takes the table at the pair's first view: the colour, then the depth.)
    ID3D12DescriptorHeap* heaps[] = {m_depth_pairs->Heap()};
    command_list->SetDescriptorHeaps(1, heaps);
    m_depth_keyed_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
    m_depth_keyed_batch->Draw(pair, DirectX::XMUINT2{width, height}, dest, DirectX::XMVectorSet(color[0], color[1], color[2], color[3]));
    m_depth_keyed_batch->End();
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = dst_state;
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains(uint32_t width, uint32_t height) {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    // (New images: none released yet, AER resends an eye's only once it has one.)
    VR::get()->m_openxr->image_ever = {};
    VR::get()->m_openxr->image_frustum_set = {};
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    if (width == 0 || height == 0) {
        width = vr->get_hmd_width();
        height = vr->get_hmd_height();
    }

    openxr->image_ready = {};

    this->contexts.clear();
    this->contexts.resize(openxr->views.size());

    // A failure part way leaves nothing behind (destroy_swapchains pairs each context with a swapchain), and is remembered
    // so the same size is not tried again every frame.
    auto fail = [&](const char* why) -> std::optional<std::string> {
        for (auto& made : openxr->swapchains) {
            xrDestroySwapchain(made.handle);
        }
        openxr->swapchains.clear();
        this->contexts.clear();
        this->last_resolution   = {};
        this->failed_resolution = {width, height};
        this->failed_time       = std::chrono::steady_clock::now();
        spdlog::error("[VR] D3D12: {} ({}x{})", why, width, height);
        return std::string{why};
    };
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", width);
        spdlog::info("[VR] Height: {}", height);

        backbuffer_desc.Width = width;
        backbuffer_desc.Height = height;
        backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            return fail("Failed to create swapchain.");
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            return fail("Failed to enumerate swapchain images.");
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            return fail("Failed to enumerate swapchain images after texture creation.");
        }
    }

    this->last_resolution   = {width, height};
    this->failed_resolution = {};

    return std::nullopt;
}

void D3D12Component::OpenXR::ensure_reticle() {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    if (this->reticle_failed || this->reticle_blocked || openxr == nullptr || openxr->reticle_swapchain != XR_NULL_HANDLE ||
        openxr->session == XR_NULL_HANDLE) {
        return;
    }

    // Freeze guard: a file stands while the sight is being set up. Found at the first try of a session, the game stopped
    // during the setup last time: the sight stays off (until the setting is turned off and on: reticle_retry).
    const auto marker = Framework::get_persistent_dir("gun_sight_starting.txt");
    std::error_code fs_error{};
    if (!this->reticle_marker_checked) {
        this->reticle_marker_checked = true;
        if (std::filesystem::exists(marker, fs_error)) {
            this->reticle_blocked = true;
            spdlog::warn("[VR] Gun sight: the game stopped while the sight was being set up last time ({} was left behind), so it "
                         "stays off; turn the gun sight setting off and on again to try once more",
                marker.string());
            return;
        }
    }
    {
        std::ofstream out{marker, std::ios::trunc};
        out << "The gun sight is being set up. If this file is still here when the game starts, the game stopped during the setup, "
               "and the sight stays off.\n";
    }
    utility::ScopeGuard remove_marker{[&] { std::filesystem::remove(marker, fs_error); }};

    const auto fail = [&](const char* what, XrResult result) {
        spdlog::error("[VR] Gun sight: {} failed ({}); the sight stays hidden", what, openxr->get_result_string(result));
        this->reticle_failed = true;
    };

    // A ring with a dot in the middle, on a clear square (about as many pixels as it covers in the headset at its usual size).
    // Nothing is drawn on the GPU (no render target views, no clears: the setup stopped the game at that point three times):
    // the pixels are made here, put in an upload buffer and copied into each of the runtime's images, exactly as the mod
    // menu's own font texture is uploaded (ImGui's D3D12 backend: an upload buffer, CopyTextureRegion between two barriers, on
    // the game's queue, waited for with a fence), which already works on this machine. Each step is logged.
    // A strip of square pictures side by side (the layer shows one of them: OpenXR::reticle_sprite).
    constexpr uint32_t size = 48; // one picture's side
    constexpr uint32_t sprites = (uint32_t)runtimes::OpenXR::reticle_sprites;
    constexpr uint32_t width = size * sprites;
    constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    auto device = g_framework->get_d3d12_hook()->get_device();
    auto queue = g_framework->get_d3d12_hook()->get_command_queue();
    if (device == nullptr || queue == nullptr) {
        spdlog::error("[VR] Gun sight: no D3D12 device or queue yet; the sight stays hidden");
        this->reticle_failed = true;
        return;
    }
    spdlog::info("[VR] Gun sight: setting up its image (step 1: the headset's images for it)");

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.createFlags = 0;
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    create_info.format = format;
    create_info.sampleCount = 1;
    create_info.width = width;
    create_info.height = size;
    create_info.faceCount = 1;
    create_info.arraySize = 1;
    create_info.mipCount = 1;

    XrSwapchain handle{XR_NULL_HANDLE};
    if (auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
        fail("xrCreateSwapchain", result);
        return;
    }
    uint32_t image_count{};
    xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (auto result = image_count > 0 && image_count <= 8 ? xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)images.data())
                                                         : XR_ERROR_RUNTIME_FAILURE;
        result != XR_SUCCESS) {
        fail("xrEnumerateSwapchainImages", result);
        xrDestroySwapchain(handle);
        return;
    }
    // Each must be the size asked for, one layer, one level, 8-bit RGBA (or BGRA: the bytes are swapped to match), all alike.
    D3D12_RESOURCE_DESC image_desc{};
    bool bgra = false;
    for (uint32_t i = 0; i < image_count; ++i) {
        const auto image = images[i].texture;
        const auto d = image != nullptr ? image->GetDesc() : D3D12_RESOURCE_DESC{};
        const bool rgba8 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra8 = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        spdlog::info("[VR] Gun sight: headset image {} of {}: {}x{}, format {}, array {}, mips {}, flags {:x}", i + 1, image_count, (uint64_t)d.Width,
            d.Height, (int)d.Format, d.DepthOrArraySize, d.MipLevels, (int)d.Flags);
        if (image == nullptr || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != width || d.Height != size || d.DepthOrArraySize != 1 ||
            d.MipLevels != 1 || d.SampleDesc.Count != 1 || !(rgba8 || bgra8) || (i > 0 && d.Format != image_desc.Format)) {
            spdlog::error("[VR] Gun sight: the headset's image is not the size and kind asked for; the sight stays hidden");
            this->reticle_failed = true;
            xrDestroySwapchain(handle);
            return;
        }
        image_desc = d;
        bgra = bgra8;
    }

    // The pictures: premultiplied colour (as the layer is drawn) in sRGB bytes (the images are sRGB: a copy does not convert),
    // soft edges (each pixel's coverage, 8 x 8 samples):
    // 0 the gun sight: a red ring and centre dot (like a red-dot sight);
    // 1 the gravity gloves' target: a light blue ring in four arcs, around the object they will pull;
    // 2 locked on: a soft, faint white glow over the object (a highlight);
    // 3 the gloves' pointing dot (nothing found yet): a small light blue dot (also the menu pointer's dot);
    // 4, 5 the menu panel's veils: an even black, half and three quarters opaque (the world behind a menu dimmed);
    // 6 the menu pointer's beam: a soft light blue line down the middle (the picture is stretched along the beam).
    // 7 the gun's laser beam: as 6, red and thinner; 8 its dot where it hits: a small bright red dot with a soft edge.
    std::vector<uint32_t> pixels(width * size, 0u);
    {
        struct Look {
            float color[3]; // linear
            float opacity;
        };
        static_assert(runtimes::OpenXR::reticle_sprites == 9);
        constexpr Look looks[9]{{{1.0f, 0.18f, 0.12f}, 0.9f}, {{0.35f, 0.8f, 1.0f}, 0.85f}, {{1.0f, 1.0f, 1.0f}, 0.3f}, {{0.35f, 0.8f, 1.0f}, 0.75f},
            {{0.0f, 0.0f, 0.0f}, 0.5f}, {{0.0f, 0.0f, 0.0f}, 0.75f}, {{0.55f, 0.85f, 1.0f}, 0.8f}, {{1.0f, 0.06f, 0.04f}, 0.7f}, {{1.0f, 0.12f, 0.08f}, 1.0f}};
        constexpr float centre = (float)size * 0.5f;
        const auto inside = [](uint32_t sprite, float dx, float dy) -> float {
            const float r = std::sqrt(dx * dx + dy * dy) / (float)size; // in picture widths
            switch (sprite) {
            case 0:
                return r <= 0.085f || (r >= 0.37f && r <= 0.47f) ? 1.0f : 0.0f;
            case 1: {
                if (r < 0.40f || r > 0.47f) {
                    return 0.0f;
                }
                float degrees = std::atan2(dy, dx) * 57.29578f;
                degrees = std::fmod(degrees + 360.0f, 90.0f); // arcs centred on the diagonals, gaps on the axes
                return degrees >= 15.0f && degrees <= 75.0f ? 1.0f : 0.0f;
            }
            case 2: {
                // Even over the middle, fading smoothly to nothing at the edge.
                const float t = std::clamp((0.49f - r) / (0.49f - 0.28f), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            }
            case 4:
            case 5:
                return 1.0f;
            case 6: {
                // Across the beam only: a core, fading smoothly to nothing well before the edges.
                const float across = std::abs(dx) / (float)size;
                const float t = std::clamp((0.40f - across) / (0.40f - 0.12f), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            }
            case 7: {
                // The laser: a thin bright core, fading to nothing a little way out; (fix148) and along it, from bright at the gun (the
                // picture's bottom, the strip's start) to nothing at its end (the top), as the beam drawn into the pictures does.
                const float across = std::abs(dx) / (float)size;
                const float t = std::clamp((0.30f - across) / (0.30f - 0.06f), 0.0f, 1.0f);
                const float along = std::clamp(dy / (float)size + 0.5f, 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t) * std::pow(along, 1.5f);
            }
            case 8: {
                // Its dot: whole in the middle, a soft edge.
                const float t = std::clamp((0.30f - r) / (0.30f - 0.16f), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            }
            default:
                return r <= 0.22f ? 1.0f : 0.0f;
            }
        };
        const auto to_srgb = [](float c) {
            c = std::clamp(c, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        };
        const auto byte = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        for (uint32_t sprite = 0; sprite < sprites; ++sprite) {
            const Look& look = looks[std::min<uint32_t>(sprite, 8)];
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    float covered = 0.0f;
                    for (int sy = 0; sy < 8; ++sy) {
                        for (int sx = 0; sx < 8; ++sx) {
                            const float dx = (float)x + ((float)sx + 0.5f) * 0.125f - centre;
                            const float dy = (float)y + ((float)sy + 0.5f) * 0.125f - centre;
                            covered += inside(sprite, dx, dy);
                        }
                    }
                    const float alpha = look.opacity * covered / 64.0f;
                    uint32_t r = byte(to_srgb(look.color[0] * alpha)), g = byte(to_srgb(look.color[1] * alpha)), b = byte(to_srgb(look.color[2] * alpha));
                    if (bgra) {
                        std::swap(r, b);
                    }
                    pixels[y * width + sprite * size + x] = r | (g << 8) | (b << 16) | (byte(alpha) << 24);
                }
            }
        }
    }

    // The upload buffer laid out as the device wants it for these images.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    device->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
    if (rows != size || row_bytes < (UINT64)width * 4 || total_bytes == 0 || total_bytes > (1u << 20) || footprint.Footprint.RowPitch < width * 4) {
        spdlog::error("[VR] Gun sight: unexpected copy layout ({} rows of {} bytes, {} in all); the sight stays hidden", rows, row_bytes, total_bytes);
        this->reticle_failed = true;
        xrDestroySwapchain(handle);
        return;
    }
    spdlog::info("[VR] Gun sight: step 2: its picture ({} bytes, rows of {})", total_bytes, footprint.Footprint.RowPitch);
    ComPtr<ID3D12Resource> upload{};
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = total_bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        const D3D12_RANGE nothing_read{0, 0};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
            upload == nullptr || FAILED(upload->Map(0, &nothing_read, &mapped)) || mapped == nullptr) {
            spdlog::error("[VR] Gun sight: could not make its upload buffer; the sight stays hidden");
            this->reticle_failed = true;
            xrDestroySwapchain(handle);
            return;
        }
        upload->SetName(L"Gun sight upload");
        std::memset(mapped, 0, (size_t)total_bytes);
        for (uint32_t y = 0; y < size; ++y) {
            std::memcpy((uint8_t*)mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, &pixels[y * width], width * 4);
        }
        upload->Unmap(0, nullptr);
    }

    // Commands of its own (as the font upload makes them), run on the game's queue and waited for.
    ComPtr<ID3D12CommandAllocator> allocator{};
    ComPtr<ID3D12GraphicsCommandList> list{};
    ComPtr<ID3D12Fence> fence{};
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    utility::ScopeGuard close_event{[&] {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }};
    if (event == nullptr || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        spdlog::error("[VR] Gun sight: could not make its commands; the sight stays hidden");
        this->reticle_failed = true;
        xrDestroySwapchain(handle);
        return;
    }
    list->Close(); // made recording: closed, and reset for each image below

    UINT64 fence_value = 0;
    for (uint32_t filled = 0; filled < image_count; ++filled) {
        uint32_t index{};
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (auto result = xrAcquireSwapchainImage(handle, &acquire_info, &index); result != XR_SUCCESS || index >= image_count) {
            fail("xrAcquireSwapchainImage", result);
            xrDestroySwapchain(handle);
            return;
        }
        // Not forever: a runtime that never hands the image over leaves the sight off instead of stopping the game.
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = 500'000'000; // 0.5 s
        if (auto result = xrWaitSwapchainImage(handle, &wait_info); result != XR_SUCCESS) {
            fail("xrWaitSwapchainImage", result);
            xrDestroySwapchain(handle);
            return;
        }
        spdlog::info("[VR] Gun sight: step 3: copying into headset image {}", index + 1);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) {
            spdlog::error("[VR] Gun sight: could not start its commands; the sight stays hidden");
            this->reticle_failed = true;
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(handle, &release_info);
            xrDestroySwapchain(handle);
            return;
        }
        // The image is handed over in the render target state (XR_KHR_D3D12_enable) and must go back in it.
        const auto image = images[index].texture;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = image;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = image;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) {
            spdlog::error("[VR] Gun sight: its copy could not be recorded; the sight stays hidden");
            this->reticle_failed = true;
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(handle, &release_info);
            xrDestroySwapchain(handle);
            return;
        }
        ID3D12CommandList* const lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fence_value);
        bool done = fence->GetCompletedValue() >= fence_value;
        if (!done && SUCCEEDED(fence->SetEventOnCompletion(fence_value, event))) {
            done = WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= fence_value;
        }
        if (!done) {
            // The GPU did not finish the copy: nothing it may still use is freed (left as it is), and the sight stays off.
            spdlog::error("[VR] Gun sight: the GPU did not finish copying its picture in 2 s; the sight stays hidden");
            this->reticle_failed = true;
            upload.Detach();
            allocator.Detach();
            list.Detach();
            fence.Detach();
            event = nullptr;
            return;
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (auto result = xrReleaseSwapchainImage(handle, &release_info); result != XR_SUCCESS) {
            fail("xrReleaseSwapchainImage", result);
            xrDestroySwapchain(handle);
            return;
        }
        spdlog::info("[VR] Gun sight: step 4: headset image {} handed back", index + 1);
    }

    openxr->reticle_swapchain = handle;
    openxr->reticle_width = (int32_t)width;
    openxr->reticle_height = (int32_t)size;
    openxr->reticle_ready = true;
    spdlog::info("[VR] Gun sight image ready ({} images)", image_count);
}

bool D3D12Component::OpenXR::reticle_is_blocked() {
    return this->reticle_blocked.load();
}

void D3D12Component::OpenXR::reticle_retry() {
    std::scoped_lock _{this->mtx};
    if (!this->reticle_blocked && !this->reticle_failed) {
        return;
    }
    std::error_code fs_error{};
    std::filesystem::remove(Framework::get_persistent_dir("gun_sight_starting.txt"), fs_error);
    this->reticle_blocked = false;
    this->reticle_failed = false;
    spdlog::info("[VR] Gun sight: trying again");
}

void D3D12Component::OpenXR::destroy_reticle() {
    std::scoped_lock _{this->mtx};

    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        openxr->reticle_ready = false;
    }
    for (auto& ctx : this->reticle_contexts) {
        ctx->commands.wait(2000);
    }
    this->reticle_contexts.clear();
    if (openxr != nullptr && openxr->reticle_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(openxr->reticle_swapchain);
        openxr->reticle_swapchain = XR_NULL_HANDLE;
    }
    // A new session may succeed where this one failed.
    this->reticle_failed = false;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    auto& swapchains = VR::get()->m_openxr->swapchains;
    for (size_t i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear(); // (waits for each image's last copy)
        ctx.textures.clear();
    }
    for (size_t i = 0; i < swapchains.size(); ++i) {
        auto result = xrDestroySwapchain(swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

bool D3D12Component::OpenXR::copy(uint32_t swapchain_idx, ID3D12Resource* resource, const Vector4f& frustum, D3D12_RESOURCE_STATES src_state,
    const Beam& beam) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        ++vr->m_stats.copy_skipped_should_render;
        return false;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            if (vr->m_stats.copy_skipped_not_begun++ < 20) {
                spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            }
            return false;
        }
    }

    // The swapchains must match the game's backbuffer for CopyResource. The game is asked to
    // resize its window to the headset resolution, but if it doesn't (or until it does), the
    // swapchains follow the backbuffer instead and the compositor scales the image.
    {
        const auto src_desc = resource->GetDesc();
        const auto src_w = (uint32_t)src_desc.Width;
        const auto src_h = (uint32_t)src_desc.Height;

        if (this->contexts.empty() || this->last_resolution[0] != src_w || this->last_resolution[1] != src_h) {
            if (this->failed_resolution[0] == src_w && this->failed_resolution[1] == src_h &&
                std::chrono::steady_clock::now() - this->failed_time < std::chrono::seconds(3)) {
                return false; // failed at this size a moment ago (logged then)
            }
            spdlog::info("[VR] Resizing OpenXR swapchains to the backbuffer size {}x{}", src_w, src_h);

            if (auto err = this->create_swapchains(src_w, src_h)) {
                spdlog::error("[VR] Failed to resize swapchains: {}", *err);
                return false;
            }
        }
    }

    if (swapchain_idx >= this->contexts.size() || swapchain_idx >= vr->m_openxr->swapchains.size()) {
        return false;
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    if (ctx.num_textures_acquired > 0) {
        // (fix189) An image left acquired by last time: given back first, so this frame's acquire, wait and release pair up again (the
        // runtime waits on and releases the oldest acquired image). One whose wait failed is waited for again (a tenth of a second at most;
        // still not: no new picture this frame, tried again the next); one waited for whose release failed is released. Told the first 20.
        static int told_left = 0;
        XrResult waited = XR_SUCCESS;
        if (ctx.wait_owed) {
            XrSwapchainImageWaitInfo again{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            again.timeout = 100'000'000; // (nanoseconds)
            waited = xrWaitSwapchainImage(swapchain.handle, &again);
            if (XR_SUCCEEDED(waited) && waited != XR_TIMEOUT_EXPIRED) {
                ctx.wait_owed = false;
            }
        }
        XrResult released = XR_ERROR_CALL_ORDER_INVALID;
        if (!ctx.wait_owed) {
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            released = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            if (XR_SUCCEEDED(released)) {
                ctx.num_textures_acquired--;
            }
        }
        if (told_left++ < 20) {
            spdlog::info("[VR] (fix189) swapchain {}: an image still acquired from before: {} (wait {}, release {})", swapchain_idx,
                XR_SUCCEEDED(released) ? "given back" : "still acquired", vr->m_openxr->get_result_string(waited),
                vr->m_openxr->get_result_string(released));
        }
        if (ctx.wait_owed) {
            return false;
        }
        if (!XR_SUCCEEDED(released)) {
            ctx.num_textures_acquired = 0; // (not given back: counted afresh, so this is not tried again every frame)
        }
    }

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        // (fix189) Each image's last copy waited for (as the release's correction below does), not reset: reset() drops a context's
        // command list, and the copy just below records into it (a crash; every later copy too).
        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.wait(INFINITE);
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
            // (fix189) A failed wait is tried once more, for a tenth of a second at most (a success code other than XR_SUCCESS,
            // XR_SESSION_LOSS_PENDING, was a wait done). Waited after all: copied as usual below. Still not: the image stays acquired
            // (OpenXR releases only an image waited for) and is waited for and given back at the next copy's start (above); this eye gets
            // no new picture this frame, as before.
            if (XR_FAILED(result)) {
                XrSwapchainImageWaitInfo retry_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                retry_info.timeout = 100'000'000; // (nanoseconds)
                result = xrWaitSwapchainImage(swapchain.handle, &retry_info);
            }
            const bool waited = XR_SUCCEEDED(result) && result != XR_TIMEOUT_EXPIRED;
            static int told = 0;
            if (told++ < 20) {
                spdlog::error("[VR] (fix189) swapchain {}'s image after the failed wait: {} ({})", swapchain_idx,
                    waited ? "waited for after all: copied" : "still acquired, given back at the next copy", vr->m_openxr->get_result_string(result));
            }
            if (!waited) {
                ctx.wait_owed = true;
                return false;
            }
            result = XR_SUCCESS;
        }
        {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);
            texture_ctx->commands.copy(
                resource,
                ctx.textures[texture_index].texture,
                src_state,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            // (fix148) The gun laser's beam into the picture now in the image (in the render target state the copy leaves it in), hidden by
            // what the game's depth has nearer.
            if (beam.on && texture_ctx->commands.cmd_list != nullptr) {
                const float tangents[4]{frustum[0], frustum[1], frustum[2], frustum[3]};
                vrmod::LaserSight::get().draw_beam(texture_ctx->commands.cmd_list.Get(), ctx.textures[texture_index].texture, beam.frame, beam.from_left,
                    tangents, beam.eyes_apart);
            }
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return false;
            }

            ctx.num_textures_acquired--;
            vr->m_openxr->image_ready[swapchain_idx] = true;
            vr->m_openxr->image_ever[swapchain_idx]  = true;
            vr->m_openxr->image_frustum[swapchain_idx]     = frustum;
            vr->m_openxr->image_frustum_set[swapchain_idx] = true;
            ++vr->m_stats.copies_ok[swapchain_idx & 1];
            return true;
        }
    }
    return false;
}
bool D3D12Component::OpenXR::copy_panel(ID3D12Resource* src) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;
    if (src == nullptr || openxr == nullptr || openxr->session == XR_NULL_HANDLE || openxr->frame_state.shouldRender != XR_TRUE) {
        return false;
    }
    if (!openxr->frame_began && openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
        return false;
    }

    // Its swapchain: the size of the game's picture (a copy needs both alike), made again when that changes.
    const auto src_desc = src->GetDesc();
    const auto width = (uint32_t)src_desc.Width;
    const auto height = (uint32_t)src_desc.Height;
    if (width == 0 || height == 0) {
        return false;
    }
    if (openxr->panel_swapchain == XR_NULL_HANDLE || this->panel_resolution[0] != width || this->panel_resolution[1] != height) {
        if (this->panel_failed_resolution[0] == width && this->panel_failed_resolution[1] == height &&
            std::chrono::steady_clock::now() - this->panel_failed_time < std::chrono::seconds(3)) {
            return false; // (failed at this size a moment ago: logged then)
        }
        this->destroy_panel();

        const auto fail = [&](const char* what, XrResult result, XrSwapchain made) {
            spdlog::error("[VR] Menu panel: {} failed ({}) for {}x{}; menus use the old screen for now", what, openxr->get_result_string(result), width, height);
            this->panel_context.texture_contexts.clear();
            this->panel_context.textures.clear();
            if (made != XR_NULL_HANDLE) {
                xrDestroySwapchain(made);
            }
            this->panel_failed_resolution = {width, height};
            this->panel_failed_time = std::chrono::steady_clock::now();
            return false;
        };

        // As the eye images are made (create_swapchains).
        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.arraySize = 1;
        create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        create_info.width = width;
        create_info.height = height;
        create_info.mipCount = 1;
        create_info.faceCount = 1;
        create_info.sampleCount = 1;
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain handle{XR_NULL_HANDLE};
        if (const auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
            return fail("xrCreateSwapchain", result, XR_NULL_HANDLE);
        }
        uint32_t image_count{};
        if (const auto result = xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr); result != XR_SUCCESS || image_count == 0 || image_count > 8) {
            return fail("xrEnumerateSwapchainImages", result, handle);
        }
        auto& ctx = this->panel_context;
        ctx.textures.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);
        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Menu Panel Commands "} + std::to_wstring(j)).c_str());
        }
        if (const auto result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)ctx.textures.data());
            result != XR_SUCCESS) {
            return fail("xrEnumerateSwapchainImages (the images)", result, handle);
        }
        ctx.num_textures_acquired = 0;
        openxr->panel_swapchain = handle;
        openxr->panel_image_width = (int32_t)width;
        openxr->panel_image_height = (int32_t)height;
        this->panel_resolution = {width, height};
        this->panel_failed_resolution = {};
        spdlog::info("[VR] Menu panel: its image is {}x{} ({} images)", width, height, image_count);
    }

    // The picture copied in, as into the eye images (copy). A step failing after an image was taken: the swapchain is made
    // again next time (so no image stays taken).
    auto& ctx = this->panel_context;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index{};
    if (const auto result = xrAcquireSwapchainImage(openxr->panel_swapchain, &acquire_info, &index); result != XR_SUCCESS) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::error("[VR] Menu panel: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        }
        return false;
    }
    if (index >= ctx.textures.size() || index >= ctx.texture_contexts.size()) {
        spdlog::error("[VR] Menu panel: the runtime gave image {} of {}; its images are made again", index, ctx.textures.size());
        this->destroy_panel();
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const auto result = xrWaitSwapchainImage(openxr->panel_swapchain, &wait_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Menu panel: xrWaitSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_panel();
        return false;
    }
    auto& texture_ctx = ctx.texture_contexts[index];
    texture_ctx->commands.wait(INFINITE);
    texture_ctx->commands.copy(src, ctx.textures[index].texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture_ctx->commands.execute();
    if (const auto result = xrReleaseSwapchainImage(openxr->panel_swapchain, &release_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Menu panel: xrReleaseSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_panel();
        return false;
    }
    return true;
}

namespace {
// The typed format the interface layer is copied into and read as: its own; 8-bit gamma-encoded colour stays as stored (not read
// as sRGB: the panel's image takes it as sRGB, as it takes the game's picture). None for a format without alpha.
DXGI_FORMAT menu_layer_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace

int D3D12Component::record_menu_layer(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state) {
    if (command_list == nullptr || layer == nullptr) {
        return -1;
    }
    const auto desc = layer->GetDesc();
    const auto format = menu_layer_format(desc.Format);
    auto& L = m_menu_layer;
    std::scoped_lock _{L.mtx};
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || format == DXGI_FORMAT_UNKNOWN || desc.Width == 0 || desc.Height == 0 ||
        desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) {
        if (!L.refused_logged) {
            L.refused_logged = true;
            spdlog::warn("[VR] Menu panel: the game's interface layer cannot be used for see-through menus ({}x{}, format {}, {} samples, {} layers)",
                (uint64_t)desc.Width, desc.Height, (int)desc.Format, desc.SampleDesc.Count, desc.DepthOrArraySize);
        }
        return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    // Only a layer as wide as the game's picture and no taller (the menu's box, or the whole picture): laid in the middle of it.
    // (Another width: another pass's, not this one's to show; skipped, not refused.)
    if (m_backbuffer_size[0] != 0 && (uint32_t)desc.Width != m_backbuffer_size[0] && desc.Height <= m_backbuffer_size[1]) {
        if (L.logged_other_size[0] != (uint32_t)desc.Width || L.logged_other_size[1] != desc.Height) {
            L.logged_other_size[0] = (uint32_t)desc.Width;
            L.logged_other_size[1] = desc.Height;
            spdlog::info("[VR] Menu panel: a {}x{} interface layer is not as wide as the game's picture ({}): not shown", (uint64_t)desc.Width,
                desc.Height, m_backbuffer_size[0]);
        }
        return 0;
    }
    if (m_backbuffer_size[0] != 0 && ((uint32_t)desc.Width > m_backbuffer_size[0] || desc.Height > m_backbuffer_size[1])) {
        if (L.logged_other_size[0] != (uint32_t)desc.Width || L.logged_other_size[1] != desc.Height) {
            L.logged_other_size[0] = (uint32_t)desc.Width;
            L.logged_other_size[1] = desc.Height;
            spdlog::info("[VR] Menu panel: a {}x{} interface layer is larger than the game's picture ({}x{}): not used", (uint64_t)desc.Width, desc.Height,
                m_backbuffer_size[0], m_backbuffer_size[1]);
        }
        return -1;
    }
    // (fix192) The layer's height in this game frame, whether a copy is made of it or not (a slot of a new size waits its turn): the panel
    // tells from it where the menu's box is in the game's own picture of the frame (menu_layer_height_of).
    const uint32_t game_frame = VR::get()->m_render_frame_count.load();
    auto& seen = L.seen[game_frame & 3];
    seen.frame = game_frame;
    seen.height = desc.Height;
    seen.has = true;
    // Its copy: the slot of its size and format, or a new one (in place of the one used longest ago; at most one made every half
    // second: each is a picture's worth of video memory).
    int slot = -1;
    for (int i = 0; i < (int)L.slots.size(); ++i) {
        const auto& sl = L.slots[i];
        if (sl.copy != nullptr && sl.width == (uint32_t)desc.Width && sl.height == desc.Height && sl.format == format) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (now - L.made_at < std::chrono::milliseconds(500)) {
            return 0; // (not now)
        }
        L.made_at = now;
        int replace = 0;
        for (int i = 0; i < (int)L.slots.size(); ++i) {
            if (L.slots[i].copy == nullptr) {
                replace = i;
                break;
            }
            if (L.slots[i].used < L.slots[replace].used) {
                replace = i;
            }
        }
        auto device = g_framework->get_d3d12_hook()->get_device();
        if (device == nullptr) {
            return 0;
        }
        D3D12_RESOURCE_DESC copy_desc{};
        copy_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        copy_desc.Width = desc.Width;
        copy_desc.Height = desc.Height;
        copy_desc.DepthOrArraySize = 1;
        copy_desc.MipLevels = 1;
        copy_desc.Format = format;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // (its views are made with the panel's texture helper)
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> made{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&made)))) {
            if (!L.refused_logged) {
                L.refused_logged = true;
                spdlog::error("[VR] Menu panel: could not make a copy of the game's interface layer ({}x{}, format {})", (uint64_t)desc.Width,
                    desc.Height, (int)format);
            }
            return -1;
        }
        made->SetName(L"Menu panel: the game's interface layer");
        auto& sl = L.slots[replace];
        if (sl.copy != nullptr) {
            L.retired.push_back(RetiredCopy{sl.copy, now, presents_now()});
        }
        sl.copy = made;
        sl.copies = 0;
        sl.width = (uint32_t)desc.Width;
        sl.height = desc.Height;
        sl.format = format;
        slot = replace;
        spdlog::info("[VR] Menu panel: the game's interface layer is {}x{} (format {}, read as {}; {} mip levels, flags {:#x}, in state {:#x} "
                     "where the game lays it over its picture): copied each frame a see-through menu is open",
            (uint64_t)desc.Width, desc.Height, (int)desc.Format, (int)format, desc.MipLevels, (uint32_t)desc.Flags, (uint32_t)layer_state);
    }
    auto& sl = L.slots[slot];
    sl.used = now;
    // (Copies replaced a while ago: no longer in use on the GPU.)
    std::erase_if(L.retired, [&](const auto& r) { return copy_done(r, now); });

    // On the game's own commands, where the layer is (the pass that lays it over the picture): into the copy, both back as they were.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    if (layer_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = layer;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[count].Transition.StateBefore = layer_state;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++count;
    }
    barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[count].Transition.pResource = sl.copy.Get();
    barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ++count;
    command_list->ResourceBarrier(count, barriers);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = sl.copy.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = layer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    for (UINT i = 0; i < count; ++i) {
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    }
    command_list->ResourceBarrier(count, barriers);
    ++L.serial;
    // (A new copy is shown from its second copy on: its first, in this frame's commands, may not have run when the panel takes it.)
    if (++sl.copies >= 2 || L.current == slot) {
        L.current = slot;
        L.at = now;
        L.frame = game_frame; // (fix192: the frame it belongs to)
    }
    return 1;
}

void D3D12Component::drop_menu_layer() {
    std::scoped_lock _{m_menu_layer.mtx};
    m_menu_layer.at = {};
}

int D3D12Component::record_conversation(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state,
    const D3D12_BOX& copy, const D3D12_BOX& clear) {
    if (command_list == nullptr || layer == nullptr || command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return -1;
    }
    const auto desc = layer->GetDesc();
    const auto format = menu_layer_format(desc.Format);
    auto& C = m_conversation;
    std::scoped_lock _{C.mtx};
    const auto refuse = [&](const char* why) {
        if (!C.refused_logged) {
            C.refused_logged = true;
            spdlog::warn("[VR] Conversation panel: {} (the game's interface layer {}x{}, format {}, flags {:#x}): the conversation stays in the picture",
                why, (uint64_t)desc.Width, desc.Height, (int)desc.Format, (uint32_t)desc.Flags);
        }
        return -1;
    };
    // (An 8-bit RGBA picture the game draws into: its part copied into the panel's sRGB image as stored, and cleared as a render target.)
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        format != DXGI_FORMAT_R8G8B8A8_UNORM || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return refuse("the game's interface layer is not a plain 8-bit RGBA picture drawn into");
    }
    const auto inside = [&](const D3D12_BOX& b) {
        return b.right > b.left && b.bottom > b.top && b.right <= desc.Width && b.bottom <= desc.Height && b.front == 0 && b.back == 1;
    };
    if (!inside(copy) || !inside(clear)) {
        return 0;
    }
    const uint32_t cw = copy.right - copy.left;
    const uint32_t ch = copy.bottom - copy.top;
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(C.retired, [&](const auto& r) { return copy_done(r, now); });
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return 0;
    }
    // Its copy, the size of the part shown (made again at another size: at most every half second), and a view to clear the layer with.
    if (C.copy == nullptr || C.copy_size[0] != cw || C.copy_size[1] != ch) {
        if (now - C.made_at < std::chrono::milliseconds(500)) {
            return 0;
        }
        C.made_at = now;
        if (C.copy != nullptr) {
            C.retired.push_back(RetiredCopy{C.copy, now, presents_now()});
            C.copy.Reset();
        }
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = cw;
        d.Height = ch;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&C.copy)))) {
            C.copy.Reset();
            return refuse("could not make its copy");
        }
        C.copy->SetName(L"Conversation panel: its part of the interface layer");
        C.copy_size[0] = cw;
        C.copy_size[1] = ch;
        C.copies = 0;
        C.format = format;
        spdlog::info("[VR] Conversation panel: its part of the game's interface layer is {}x{} (at {},{} of the {}x{} layer; the conversation's box "
                     "{}x{} at {},{} cleared there)",
            cw, ch, copy.left, copy.top, (uint64_t)desc.Width, desc.Height, clear.right - clear.left, clear.bottom - clear.top, clear.left, clear.top);
    }
    if (C.rtv_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&C.rtv_heap)))) {
            C.rtv_heap.Reset();
            return refuse("could not make a view to clear the layer with");
        }
    }
    // (The view made again each time: the game may draw into more than one layer; a clear reads it as it is recorded. In its own format
    // when the layer has a typed one: a view must match it, the review.)
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? format : desc.Format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    rtv_desc.Texture2D.PlaneSlice = 0;
    const auto rtv = C.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(layer, &rtv_desc, rtv);

    // On the game's own commands, where the layer is (the pass that lays it over the picture, before it runs): the part copied out, the
    // conversation's box cleared, the layer back as it was.
    const auto transition = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    if (layer_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count++] = transition(layer, layer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    barriers[count++] = transition(C.copy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(count, barriers);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = C.copy.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = layer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &copy);

    count = 0;
    barriers[count++] = transition(layer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers[count++] = transition(C.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    command_list->ResourceBarrier(count, barriers);
    const float see_through[4]{0.0f, 0.0f, 0.0f, 0.0f};
    const D3D12_RECT rect{(LONG)clear.left, (LONG)clear.top, (LONG)clear.right, (LONG)clear.bottom};
    command_list->ClearRenderTargetView(rtv, see_through, 1, &rect);
    if (layer_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barriers[0] = transition(layer, D3D12_RESOURCE_STATE_RENDER_TARGET, layer_state);
        command_list->ResourceBarrier(1, barriers);
    }
    ++C.copies;
    C.at = now;
    if (C.logs < 3) {
        ++C.logs;
        spdlog::info("[VR] Conversation panel: cut out of the interface layer (the layer in state {:#x} there)", (uint32_t)layer_state);
    }
    return 1;
}

int D3D12Component::record_console(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state,
    const ConsoleCut& cut) {
    if (command_list == nullptr || layer == nullptr || command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return -1;
    }
    const auto desc = layer->GetDesc();
    const auto format = menu_layer_format(desc.Format);
    auto& K = m_console;
    std::scoped_lock _{K.mtx};
    const auto refuse = [&](const char* why) {
        if (!K.refused_logged) {
            K.refused_logged = true;
            spdlog::warn("[VR] (fix177) Build console: {} (the game's interface layer {}x{}, format {}, flags {:#x}): build mode stays in the view",
                why, (uint64_t)desc.Width, desc.Height, (int)desc.Format, (uint32_t)desc.Flags);
        }
        return -1;
    };
    // (As the conversation's: an 8-bit RGBA picture the game draws into, its parts copied into the console's sRGB image as stored, and its
    // boxes cleared as a render target.)
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        format != DXGI_FORMAT_R8G8B8A8_UNORM || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return refuse("the game's interface layer is not a plain 8-bit RGBA picture drawn into");
    }
    // (Every part inside the layer and inside the picture, every box inside the layer: else not this time.)
    if (cut.parts <= 0 || cut.parts > ConsoleCut::kMax || cut.clears <= 0 || cut.clears > 3 || cut.size[0] == 0 || cut.size[1] == 0 ||
        cut.size[0] > 8192 || cut.size[1] > 8192) {
        return 0;
    }
    for (int i = 0; i < cut.parts; ++i) {
        const auto& b = cut.from[i];
        if (!(b.right > b.left && b.bottom > b.top && b.right <= desc.Width && b.bottom <= desc.Height && b.front == 0 && b.back == 1) ||
            cut.to[i][0] + (b.right - b.left) > cut.size[0] || cut.to[i][1] + (b.bottom - b.top) > cut.size[1]) {
            return 0;
        }
    }
    for (int i = 0; i < cut.clears; ++i) {
        const auto& r = cut.clear[i];
        if (!(r.right > r.left && r.bottom > r.top && r.left >= 0 && r.top >= 0 && (uint64_t)r.right <= desc.Width && (uint32_t)r.bottom <= desc.Height)) {
            return 0;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(K.retired, [&](const auto& r) { return copy_done(r, now); });
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return 0;
    }
    // Its picture, the size the parts were packed into (made again at another size: at most every half second), and a view to clear the
    // layer with.
    if (K.copy == nullptr || K.copy_size[0] != cut.size[0] || K.copy_size[1] != cut.size[1]) {
        if (now - K.made_at < std::chrono::milliseconds(500)) {
            return 0;
        }
        K.made_at = now;
        if (K.copy != nullptr) {
            K.retired.push_back(RetiredCopy{K.copy, now, presents_now()});
            K.copy.Reset();
        }
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = cut.size[0];
        d.Height = cut.size[1];
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&K.copy)))) {
            K.copy.Reset();
            return refuse("could not make its picture");
        }
        K.copy->SetName(L"Build console: the build menu's parts");
        K.copy_size[0] = cut.size[0];
        K.copy_size[1] = cut.size[1];
        K.copies = 0;
        K.format = format;
        spdlog::info("[VR] (fix177) Build console: its picture is {}x{} ({:.1f} MB; {} parts of the {}x{} interface layer packed in it, {} boxes "
                     "cleared there)",
            cut.size[0], cut.size[1], cut.size[0] * (double)cut.size[1] * 4.0 / 1048576.0, cut.parts, (uint64_t)desc.Width, desc.Height, cut.clears);
    }
    if (K.rtv_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&K.rtv_heap)))) {
            K.rtv_heap.Reset();
            return refuse("could not make a view to clear the layer with");
        }
    }
    // (The view made again each time, in the layer's own format when it has a typed one, as the conversation's.)
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? format : desc.Format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    rtv_desc.Texture2D.PlaneSlice = 0;
    const auto rtv = K.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(layer, &rtv_desc, rtv);

    // On the game's own commands, where the layer is (the pass that lays it over the picture, before it runs): the parts copied out, the
    // boxes cleared, the layer back as it was.
    const auto transition = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    if (layer_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count++] = transition(layer, layer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    barriers[count++] = transition(K.copy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(count, barriers);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = K.copy.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = layer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    for (int i = 0; i < cut.parts; ++i) {
        command_list->CopyTextureRegion(&dst, cut.to[i][0], cut.to[i][1], 0, &src, &cut.from[i]);
    }

    count = 0;
    barriers[count++] = transition(layer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers[count++] = transition(K.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    command_list->ResourceBarrier(count, barriers);
    const float see_through[4]{0.0f, 0.0f, 0.0f, 0.0f};
    command_list->ClearRenderTargetView(rtv, see_through, (UINT)cut.clears, cut.clear);
    if (layer_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barriers[0] = transition(layer, D3D12_RESOURCE_STATE_RENDER_TARGET, layer_state);
        command_list->ResourceBarrier(1, barriers);
    }
    ++K.copies;
    ++K.total_copies;
    K.at = now;
    if (cut.snap && K.snaps < 5) {
        K.snap_wanted = true;
    }
    if (K.logs < 3) {
        ++K.logs;
        spdlog::info("[VR] (fix177) Build console: cut out of the interface layer (the layer in state {:#x} there; {} parts, {} boxes cleared)",
            (uint32_t)layer_state, cut.parts, cut.clears);
    }
    return 1;
}

void D3D12Component::present_conversation(VR* vr) {
    auto& xr = vr->m_openxr;
    if (xr == nullptr) {
        return;
    }
    xr->conv_image_ready = false;
    ComPtr<ID3D12Resource> src{};
    {
        std::scoped_lock _{m_conversation.mtx};
        const auto now = std::chrono::steady_clock::now();
        // (Cut lately, and its copy's first cut surely run: from its second.)
        if (m_conversation.copy == nullptr || m_conversation.copies < 2 || now - m_conversation.at > std::chrono::milliseconds(150)) {
            return;
        }
        src = m_conversation.copy;
    }
    xr->conv_image_ready = m_openxr.copy_conversation(src.Get());
}

bool D3D12Component::OpenXR::copy_conversation(ID3D12Resource* src) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;
    if (src == nullptr || openxr == nullptr || openxr->session == XR_NULL_HANDLE || openxr->frame_state.shouldRender != XR_TRUE) {
        return false;
    }
    if (!openxr->frame_began && openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
        return false;
    }
    // Its swapchain: the size of the part cut out (a copy needs both alike), made again when that changes (as the menu panel's).
    const auto src_desc = src->GetDesc();
    const auto width = (uint32_t)src_desc.Width;
    const auto height = (uint32_t)src_desc.Height;
    if (width == 0 || height == 0) {
        return false;
    }
    if (openxr->conv_swapchain == XR_NULL_HANDLE || this->conv_resolution[0] != width || this->conv_resolution[1] != height) {
        if (this->conv_failed_resolution[0] == width && this->conv_failed_resolution[1] == height &&
            std::chrono::steady_clock::now() - this->conv_failed_time < std::chrono::seconds(3)) {
            return false;
        }
        this->destroy_conversation();

        const auto fail = [&](const char* what, XrResult result, XrSwapchain made) {
            spdlog::error("[VR] Conversation panel: {} failed ({}) for {}x{}", what, openxr->get_result_string(result), width, height);
            this->conv_context.texture_contexts.clear();
            this->conv_context.textures.clear();
            if (made != XR_NULL_HANDLE) {
                xrDestroySwapchain(made);
            }
            this->conv_failed_resolution = {width, height};
            this->conv_failed_time = std::chrono::steady_clock::now();
            return false;
        };

        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.arraySize = 1;
        create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        create_info.width = width;
        create_info.height = height;
        create_info.mipCount = 1;
        create_info.faceCount = 1;
        create_info.sampleCount = 1;
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain handle{XR_NULL_HANDLE};
        if (const auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
            return fail("xrCreateSwapchain", result, XR_NULL_HANDLE);
        }
        uint32_t image_count{};
        if (const auto result = xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr); result != XR_SUCCESS || image_count == 0 || image_count > 8) {
            return fail("xrEnumerateSwapchainImages", result, handle);
        }
        auto& ctx = this->conv_context;
        ctx.textures.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);
        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Conversation Panel Commands "} + std::to_wstring(j)).c_str());
        }
        if (const auto result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)ctx.textures.data());
            result != XR_SUCCESS) {
            return fail("xrEnumerateSwapchainImages (the images)", result, handle);
        }
        ctx.num_textures_acquired = 0;
        openxr->conv_swapchain = handle;
        openxr->conv_image_width = (int32_t)width;
        openxr->conv_image_height = (int32_t)height;
        this->conv_resolution = {width, height};
        this->conv_failed_resolution = {};
        spdlog::info("[VR] Conversation panel: its image is {}x{} ({} images)", width, height, image_count);
    }

    auto& ctx = this->conv_context;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index{};
    if (const auto result = xrAcquireSwapchainImage(openxr->conv_swapchain, &acquire_info, &index); result != XR_SUCCESS) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::error("[VR] Conversation panel: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        }
        return false;
    }
    if (index >= ctx.textures.size() || index >= ctx.texture_contexts.size()) {
        spdlog::error("[VR] Conversation panel: the runtime gave image {} of {}; its images are made again", index, ctx.textures.size());
        this->destroy_conversation();
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const auto result = xrWaitSwapchainImage(openxr->conv_swapchain, &wait_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Conversation panel: xrWaitSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_conversation();
        return false;
    }
    auto& texture_ctx = ctx.texture_contexts[index];
    texture_ctx->commands.wait(INFINITE);
    texture_ctx->commands.copy(src, ctx.textures[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture_ctx->commands.execute();
    if (const auto result = xrReleaseSwapchainImage(openxr->conv_swapchain, &release_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Conversation panel: xrReleaseSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_conversation();
        return false;
    }
    return true;
}

void D3D12Component::OpenXR::destroy_conversation() {
    std::scoped_lock _{this->mtx};

    this->conv_context.texture_contexts.clear(); // (waits for each image's last copy)
    this->conv_context.textures.clear();
    this->conv_context.num_textures_acquired = 0;
    this->conv_resolution = {};
    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        if (openxr->conv_swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(openxr->conv_swapchain);
        }
        openxr->conv_swapchain = XR_NULL_HANDLE;
        openxr->conv_image_width = 0;
        openxr->conv_image_height = 0;
        openxr->conv_image_ready = false;
    }
}

void D3D12Component::present_console(VR* vr, bool shown) {
    auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
    if (xr == nullptr) {
        return;
    }
    xr->console_image_ready = false;
    const auto now = std::chrono::steady_clock::now();
    ComPtr<ID3D12Resource> src{};
    bool snap = false;
    bool let_go = false;
    {
        std::scoped_lock _{m_console.mtx};
        std::erase_if(m_console.retired, [&](const auto& r) { return copy_done(r, now); });
        // (Not cut for 20 s: build mode closed a while ago. Its picture let go (once the GPU is surely done with it), and its swapchain:
        // about 8 MB each, which the game is short of; made again at the next opening.)
        if (m_console.copy != nullptr && now - m_console.at > std::chrono::seconds(20)) {
            m_console.retired.push_back(RetiredCopy{m_console.copy, now, presents_now()});
            m_console.copy.Reset();
            m_console.copy_size[0] = m_console.copy_size[1] = 0;
            m_console.copies = 0;
            m_console.snap_wanted = false;
            spdlog::info("[VR] (fix177) Build console: its picture let go (not cut for 20 s)");
        }
        // (Cut lately, and its picture's first cut surely run: from its second.)
        if (!shown || m_console.copy == nullptr || m_console.copies < 2 || now - m_console.at > std::chrono::milliseconds(150)) {
            let_go = m_console.copy == nullptr && xr->console_swapchain != XR_NULL_HANDLE;
        } else {
            src = m_console.copy;
            // (Once an opening, the first few: its picture saved for the log, from its third cut on.)
            if (m_console.snap_wanted && m_console.copies >= 3) {
                m_console.snap_wanted = false;
                ++m_console.snaps;
                snap = true;
            }
        }
    }
    if (src == nullptr) {
        // (Its swapchain let go with its picture; outside the picture's lock: it waits for the images' last copies.)
        if (let_go) {
            m_openxr.destroy_console();
            spdlog::info("[VR] (fix177) Build console: its images let go");
        }
        return;
    }
    xr->console_image_ready = m_openxr.copy_console(src.Get());
    if (snap) {
        auto& hook = g_framework->get_d3d12_hook();
        auto queue = hook != nullptr ? hook->get_command_queue() : nullptr;
        if (queue != nullptr) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const auto path = Framework::get_persistent_dir("workshop_cut.png");
            m_menu_perf.saving = true; // (the frame's time is the saving's)
            const auto hr = DirectX::SaveWICTextureToFile(queue, src.Get(), GUID_ContainerFormatPng, path.wstring().c_str(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
            spdlog::info("[VR] (fix177) Build console: its picture (the build menu's parts as cut, packed) saved to {} (hr {:x})", path.string(),
                (uint32_t)hr);
        }
    }
}

bool D3D12Component::OpenXR::copy_console(ID3D12Resource* src) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;
    if (src == nullptr || openxr == nullptr || openxr->session == XR_NULL_HANDLE || openxr->frame_state.shouldRender != XR_TRUE) {
        return false;
    }
    if (!openxr->frame_began && openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
        return false;
    }
    // Its swapchain: the size of its picture (a copy needs both alike), made again when that changes (as the conversation's).
    const auto src_desc = src->GetDesc();
    const auto width = (uint32_t)src_desc.Width;
    const auto height = (uint32_t)src_desc.Height;
    if (width == 0 || height == 0) {
        return false;
    }
    if (openxr->console_swapchain == XR_NULL_HANDLE || this->console_resolution[0] != width || this->console_resolution[1] != height) {
        if (this->console_failed_resolution[0] == width && this->console_failed_resolution[1] == height &&
            std::chrono::steady_clock::now() - this->console_failed_time < std::chrono::seconds(3)) {
            return false;
        }
        this->destroy_console();

        const auto fail = [&](const char* what, XrResult result, XrSwapchain made) {
            spdlog::error("[VR] (fix177) Build console: {} failed ({}) for {}x{}", what, openxr->get_result_string(result), width, height);
            this->console_context.texture_contexts.clear();
            this->console_context.textures.clear();
            if (made != XR_NULL_HANDLE) {
                xrDestroySwapchain(made);
            }
            this->console_failed_resolution = {width, height};
            this->console_failed_time = std::chrono::steady_clock::now();
            return false;
        };

        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.arraySize = 1;
        create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        create_info.width = width;
        create_info.height = height;
        create_info.mipCount = 1;
        create_info.faceCount = 1;
        create_info.sampleCount = 1;
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain handle{XR_NULL_HANDLE};
        if (const auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
            return fail("xrCreateSwapchain", result, XR_NULL_HANDLE);
        }
        uint32_t image_count{};
        if (const auto result = xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr); result != XR_SUCCESS || image_count == 0 || image_count > 8) {
            return fail("xrEnumerateSwapchainImages", result, handle);
        }
        auto& ctx = this->console_context;
        ctx.textures.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);
        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Build Console Commands "} + std::to_wstring(j)).c_str());
        }
        if (const auto result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)ctx.textures.data());
            result != XR_SUCCESS) {
            return fail("xrEnumerateSwapchainImages (the images)", result, handle);
        }
        ctx.num_textures_acquired = 0;
        openxr->console_swapchain = handle;
        openxr->console_image_width = (int32_t)width;
        openxr->console_image_height = (int32_t)height;
        this->console_resolution = {width, height};
        this->console_failed_resolution = {};
        spdlog::info("[VR] (fix177) Build console: its image is {}x{} ({} images, {:.1f} MB)", width, height, image_count,
            width * (double)height * 4.0 * image_count / 1048576.0);
    }

    auto& ctx = this->console_context;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index{};
    if (const auto result = xrAcquireSwapchainImage(openxr->console_swapchain, &acquire_info, &index); result != XR_SUCCESS) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::error("[VR] (fix177) Build console: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        }
        return false;
    }
    if (index >= ctx.textures.size() || index >= ctx.texture_contexts.size()) {
        spdlog::error("[VR] (fix177) Build console: the runtime gave image {} of {}; its images are made again", index, ctx.textures.size());
        this->destroy_console();
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const auto result = xrWaitSwapchainImage(openxr->console_swapchain, &wait_info); result != XR_SUCCESS) {
        spdlog::error("[VR] (fix177) Build console: xrWaitSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_console();
        return false;
    }
    auto& texture_ctx = ctx.texture_contexts[index];
    texture_ctx->commands.wait(INFINITE);
    texture_ctx->commands.copy(src, ctx.textures[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture_ctx->commands.execute();
    if (const auto result = xrReleaseSwapchainImage(openxr->console_swapchain, &release_info); result != XR_SUCCESS) {
        spdlog::error("[VR] (fix177) Build console: xrReleaseSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_console();
        return false;
    }
    return true;
}

void D3D12Component::OpenXR::destroy_console() {
    std::scoped_lock _{this->mtx};

    this->console_context.texture_contexts.clear(); // (waits for each image's last copy)
    this->console_context.textures.clear();
    this->console_context.num_textures_acquired = 0;
    this->console_resolution = {};
    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        if (openxr->console_swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(openxr->console_swapchain);
        }
        openxr->console_swapchain = XR_NULL_HANDLE;
        openxr->console_image_width = 0;
        openxr->console_image_height = 0;
        openxr->console_image_ready = false;
    }
}

void D3D12Component::present_wheel(VR* vr) {
    auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
    if (xr == nullptr) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    runtimes::OpenXR::WheelLayer wheel{};
    if (!xr->get_wheel(wheel, now_ms) || xr->wheel_refused.load()) {
        // (Closed: nothing drawn, nothing copied. Its images and upload buffer let go 20 s after it was last open: about 12 MB of video
        // memory, which the game is short of; made again at the next opening.)
        if (xr->wheel_swapchain != XR_NULL_HANDLE && now - m_openxr.wheel_last_wanted > std::chrono::seconds(20)) {
            m_openxr.destroy_wheel();
            spdlog::info("[VR] (fix171) Favorites wheel: its images let go (not open for 20 s)");
        }
        return;
    }
    m_openxr.wheel_last_wanted = now;
    const auto picture = xr->get_wheel_picture();
    // (Nothing new: the swapchain's last picture stays shown. A picture of another opening is never shown for this one.)
    if (picture == nullptr || picture->open != wheel.open || picture->serial == m_openxr.wheel_uploaded_serial) {
        return;
    }
    if (m_openxr.copy_wheel(*picture)) {
        m_openxr.wheel_uploaded_serial = picture->serial;
        xr->wheel_image_open = picture->open;
    }
}

bool D3D12Component::OpenXR::copy_wheel(const runtimes::OpenXR::WheelPicture& picture) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;
    if (openxr == nullptr || openxr->session == XR_NULL_HANDLE || openxr->frame_state.shouldRender != XR_TRUE) {
        return false;
    }
    if (!openxr->frame_began && openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
        return false;
    }
    const uint32_t width = picture.width;
    const uint32_t height = picture.height;
    if (width == 0 || height == 0 || width > 2048 || height > 2048 || picture.pixels.size() != (size_t)width * height) {
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    // Its swapchain, the size of its picture (made again when that changes), its upload buffer and commands: made the first time.
    if (openxr->wheel_swapchain == XR_NULL_HANDLE || this->wheel_resolution[0] != width || this->wheel_resolution[1] != height) {
        if (now - this->wheel_failed_time < std::chrono::seconds(3)) {
            return false;
        }
        this->destroy_wheel();

        const auto fail = [&](const char* what, XrResult result, XrSwapchain made) {
            static int logged = 0;
            if (logged++ < 10) {
                spdlog::error("[VR] (fix171) Favorites wheel: {} failed ({}) for {}x{}; not shown for now", what, openxr->get_result_string(result),
                    width, height);
            }
            this->wheel_textures.clear();
            if (made != XR_NULL_HANDLE) {
                xrDestroySwapchain(made);
            }
            this->wheel_upload_mapped = nullptr;
            this->wheel_upload.Reset();
            this->wheel_failed_time = std::chrono::steady_clock::now();
            return false;
        };

        // (As the conversation's: 8-bit sRGB images, the picture's bytes copied in as they are: the colours stored as sRGB.)
        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.arraySize = 1;
        create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        create_info.width = width;
        create_info.height = height;
        create_info.mipCount = 1;
        create_info.faceCount = 1;
        create_info.sampleCount = 1;
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain handle{XR_NULL_HANDLE};
        if (const auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
            return fail("xrCreateSwapchain", result, XR_NULL_HANDLE);
        }
        uint32_t image_count{};
        if (const auto result = xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr); result != XR_SUCCESS || image_count == 0 || image_count > 8) {
            return fail("xrEnumerateSwapchainImages", result, handle);
        }
        this->wheel_textures.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if (const auto result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)this->wheel_textures.data());
            result != XR_SUCCESS) {
            return fail("xrEnumerateSwapchainImages (the images)", result, handle);
        }
        // Each must be the size asked for, one layer, one level, 8-bit RGBA (or BGRA: the bytes are swapped to match), all alike (the gun
        // sight's checks).
        D3D12_RESOURCE_DESC image_desc{};
        for (uint32_t i = 0; i < image_count; ++i) {
            const auto image = this->wheel_textures[i].texture;
            const auto d = image != nullptr ? image->GetDesc() : D3D12_RESOURCE_DESC{};
            const bool rgba8 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool bgra8 = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (image == nullptr || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != width || d.Height != height || d.DepthOrArraySize != 1 ||
                d.MipLevels != 1 || d.SampleDesc.Count != 1 || !(rgba8 || bgra8) || (i > 0 && d.Format != image_desc.Format)) {
                spdlog::error("[VR] (fix171) Favorites wheel: the headset's image {} is not the size and kind asked for ({}x{}, format {})", i + 1,
                    (uint64_t)d.Width, d.Height, (int)d.Format);
                return fail("checking the headset's images", XR_ERROR_VALIDATION_FAILURE, handle);
            }
            image_desc = d;
            this->wheel_bgra = bgra8;
        }
        if (!this->wheel_commands_ready) {
            this->wheel_commands_ready = this->wheel_commands.setup(L"OpenXR Favorites Wheel Commands");
            if (!this->wheel_commands_ready) {
                return fail("making its commands", XR_ERROR_RUNTIME_FAILURE, handle);
            }
        }
        // The upload buffer laid out as the device wants it for these images, mapped once (written only after its last copy is done).
        UINT rows = 0;
        UINT64 row_bytes = 0, total_bytes = 0;
        device->GetCopyableFootprints(&image_desc, 0, 1, 0, &this->wheel_footprint, &rows, &row_bytes, &total_bytes);
        if (rows != height || row_bytes < (UINT64)width * 4 || total_bytes == 0 || total_bytes > (64ull << 20) ||
            this->wheel_footprint.Footprint.RowPitch < width * 4) {
            spdlog::error("[VR] (fix171) Favorites wheel: unexpected copy layout ({} rows of {} bytes, {} in all)", rows, row_bytes, total_bytes);
            return fail("laying out its upload", XR_ERROR_VALIDATION_FAILURE, handle);
        }
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = total_bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* mapped = nullptr;
            const D3D12_RANGE nothing_read{0, 0};
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&this->wheel_upload))) ||
                this->wheel_upload == nullptr || FAILED(this->wheel_upload->Map(0, &nothing_read, &mapped)) || mapped == nullptr) {
                return fail("making its upload buffer", XR_ERROR_RUNTIME_FAILURE, handle);
            }
            this->wheel_upload->SetName(L"Favorites wheel upload");
            this->wheel_upload_mapped = (uint8_t*)mapped;
        }
        openxr->wheel_swapchain = handle;
        openxr->wheel_image_width = (int32_t)width;
        openxr->wheel_image_height = (int32_t)height;
        openxr->wheel_image_open = 0;
        this->wheel_resolution = {width, height};
        spdlog::info("[VR] (fix171) Favorites wheel: its image is {}x{} ({} images, {}), its upload buffer {} bytes", width, height, image_count,
            this->wheel_bgra ? "BGRA" : "RGBA", total_bytes);
    }

    // (Its last copy out of the upload buffer not done yet (queued behind the game's last frame): not waited for here, on the present
    // thread, which would stall the frame; the new picture is copied a frame later, the last one shown meanwhile. The review.)
    {
        std::scoped_lock commands_lock{this->wheel_commands.mtx};
        if (this->wheel_commands.waiting_for_fence && this->wheel_commands.fence != nullptr &&
            this->wheel_commands.fence->GetCompletedValue() < this->wheel_commands.fence_value) {
            return false;
        }
    }
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index{};
    if (const auto result = xrAcquireSwapchainImage(openxr->wheel_swapchain, &acquire_info, &index); result != XR_SUCCESS) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::error("[VR] (fix171) Favorites wheel: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        }
        return false;
    }
    if (index >= this->wheel_textures.size()) {
        spdlog::error("[VR] (fix171) Favorites wheel: the runtime gave image {} of {}; its images are made again", index, this->wheel_textures.size());
        this->destroy_wheel();
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const auto result = xrWaitSwapchainImage(openxr->wheel_swapchain, &wait_info); result != XR_SUCCESS) {
        spdlog::error("[VR] (fix171) Favorites wheel: xrWaitSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_wheel();
        return false;
    }
    // (Its last copy out of the upload buffer done (checked above: no wait): the buffer may be written again, its commands made ready.)
    this->wheel_commands.wait(INFINITE);
    const auto& fp = this->wheel_footprint.Footprint;
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = (uint32_t*)(this->wheel_upload_mapped + this->wheel_footprint.Offset + (size_t)y * fp.RowPitch);
        const uint32_t* from = picture.pixels.data() + (size_t)y * width;
        if (!this->wheel_bgra) {
            std::memcpy(row, from, (size_t)width * 4);
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t p = from[x];
                row[x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
            }
        }
    }
    {
        // The image is handed over in the render target state (XR_KHR_D3D12_enable) and must go back in it.
        std::scoped_lock commands_lock{this->wheel_commands.mtx};
        auto* list = this->wheel_commands.cmd_list.Get();
        const auto image = this->wheel_textures[index].texture;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = image;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = this->wheel_upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = this->wheel_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = image;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        this->wheel_commands.has_commands = true;
    }
    this->wheel_commands.execute();
    if (const auto result = xrReleaseSwapchainImage(openxr->wheel_swapchain, &release_info); result != XR_SUCCESS) {
        spdlog::error("[VR] (fix171) Favorites wheel: xrReleaseSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_wheel();
        return false;
    }
    return true;
}

void D3D12Component::OpenXR::destroy_wheel() {
    std::scoped_lock _{this->mtx};

    if (this->wheel_commands_ready) {
        this->wheel_commands.wait(2000); // (its last copy done before its images and buffer go)
    }
    if (this->wheel_upload != nullptr && this->wheel_upload_mapped != nullptr) {
        this->wheel_upload->Unmap(0, nullptr);
    }
    this->wheel_upload_mapped = nullptr;
    this->wheel_upload.Reset();
    this->wheel_textures.clear();
    this->wheel_resolution = {};
    this->wheel_uploaded_serial = 0;
    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        if (openxr->wheel_swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(openxr->wheel_swapchain);
        }
        openxr->wheel_swapchain = XR_NULL_HANDLE;
        openxr->wheel_image_width = 0;
        openxr->wheel_image_height = 0;
        openxr->wheel_image_open = 0;
    }
}

bool D3D12Component::compose_menu_layer(VR* vr, ID3D12Resource* eye_texture, ID3D12CommandQueue* queue, uint32_t frame) {
    auto& L = m_menu_layer;
    ComPtr<ID3D12Resource> copy{};
    DXGI_FORMAT format{};
    uint32_t width = 0, height = 0;
    int slot = -1;
    std::chrono::steady_clock::time_point at{};
    uint32_t copy_frame = 0; // (fix192)
    {
        std::scoped_lock _{L.mtx};
        slot = L.current;
        if (slot >= 0) {
            copy = L.slots[slot].copy;
            format = L.slots[slot].format;
            width = L.slots[slot].width;
            height = L.slots[slot].height;
        }
        at = L.at;
        copy_frame = L.frame;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool have_copy = slot >= 0 && copy != nullptr && at != std::chrono::steady_clock::time_point{};
    // (fix173) Why none is made, for the log (the panel's fallback line).
    L.fail_reason = nullptr;
    L.fail_age_ms = at == std::chrono::steady_clock::time_point{} ? -1 : std::chrono::duration_cast<std::chrono::milliseconds>(now - at).count();
    L.fail_behind = have_copy ? (int32_t)(frame - copy_frame) : 0; // (fix192)
    L.fail_layer[0] = width;
    L.fail_layer[1] = height;
    L.fail_eye[0] = L.fail_eye[1] = 0;
    if (eye_texture != nullptr) {
        const auto d = eye_texture->GetDesc();
        L.fail_eye[0] = (uint32_t)d.Width;
        L.fail_eye[1] = d.Height;
    }
    // (fix192) For the log: the mod's own work on the present thread before this check (its waits for the graphics card among it), while
    // the game shows a see-through menu.
    if (ModSettings::g_internalSettings.menuSeeThrough && L.present_started != std::chrono::steady_clock::time_point{}) {
        const double before_ms = std::chrono::duration<double, std::milli>(now - L.present_started).count();
        L.stats.before_sum_ms += before_ms;
        L.stats.before_max_ms = std::max(L.stats.before_max_ms, before_ms);
        ++L.stats.before_n;
    }
    // (fix192) Whether the layer's copy belongs to the picture being shown, counted in the game's frames, not in milliseconds. Before, a
    // copy over 60 ms old was refused ("copied in the last moments: the game is drawing a see-through menu now"); on slow frames (the Data
    // menu's 3D pictures, the video memory near its budget: 76-96 ms, fix191's test) that was every copy, though each was the very frame's
    // shown, measured from when the game recorded it to this check after the mod's own waits. Six frames later the panel showed the game's
    // own picture: while the frame is kept full size its menu is in the top rows and its 3D pictures stretched to the whole height, so the
    // centred rows the panel shows held only the menu's lower half, blown up, not see-through.
    // Now: copied in the frame shown or the one before (or a later one, recorded while this one was being presented), it counts. Older
    // (no copy for a frame or more, as when the copy's slot of a new size waits its turn), it still counts while the game shows a see-through
    // menu, unless it is both 3 frames and half a second old: then the menu really closed. (Its close also clears the copy's time at the
    // game's next interface pass: drop_menu_layer.)
    const int32_t behind = L.fail_behind;
    const bool fresh = have_copy && behind >= -2 && behind <= 1;
    const bool frames_far = behind > 3 || behind < -2;
    const bool see_through_menu = ModSettings::g_internalSettings.menuSeeThrough;
    const bool closed = !have_copy || (!fresh && (!see_through_menu || (frames_far && L.fail_age_ms > 500)));
    if (closed || eye_texture == nullptr || m_sprite_batch == nullptr || queue == nullptr) {
        L.fail_reason = slot < 0 || copy == nullptr ? "no copy of the interface layer"
                        : !have_copy                ? "no copy of the interface layer since the see-through menu began (or it closed)"
                        : closed && !see_through_menu
                            ? "the interface layer's last copy older than the frame before, and the game shows no see-through menu now"
                        : closed ? "the interface layer's last copy 3 frames and half a second old: the see-through menu closed"
                                 : "the eye's picture or the drawing tools missing";
        if (closed && see_through_menu) {
            ++L.stats.refused;
        }
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        L.fail_reason = "no device";
        return false;
    }
    // (The last picture's commands done before anything they use is made again: its views' heaps.)
    if (L.composed.texture != nullptr) {
        L.composed.commands.wait(INFINITE);
    }
    auto& source = L.source[slot];
    if (L.source_of[slot] != copy.Get()) {
        // (The full stereo left eye's last draw from its old views done first.)
        if (m_native.left.texture != nullptr) {
            m_native.left.commands.wait(INFINITE);
        }
        L.source_of[slot] = nullptr;
        if (!source.setup(device, copy.Get(), format, format, L"Menu panel: the interface layer's views")) {
            spdlog::error("[VR] Menu panel: could not make views of the game's interface layer");
            source.reset();
            L.fail_reason = "the interface layer's views could not be made";
            return false;
        }
        L.source_of[slot] = copy.Get();
    }
    const auto eye_desc = eye_texture->GetDesc();
    if (width > (uint32_t)eye_desc.Width || height > eye_desc.Height) {
        L.fail_reason = "the interface layer larger than the eye's picture (its size changing)";
        return false; // (a layer made for a larger picture than this one: the picture is changing size)
    }
    if (!ensure_menu_composed(device, eye_desc)) {
        return false;
    }
    // A see-through dark (its strength: the setting), and the layer over it in the middle of the picture, at its own size (the
    // game lays a menu's box in the middle of its picture; the panel shows that box). The sprite batch blends premultiplied alpha:
    // the layer's colour plus the dark where the layer is see-through.
    auto& commands = L.composed.commands;
    commands.wait(INFINITE);
    ensure_custom_batches();
    const float dark = std::clamp(vr->m_openxr->panel_see_through_darkness.load(), 0.0f, 0.9f);
    const float clear_color[4]{0.0f, 0.0f, 0.0f, dark};
    commands.clear_rtv(L.composed, clear_color, D3D12_RESOURCE_STATE_PRESENT);
    // (The frame kept full size behind the menu (the game side's FullFrame): the layer is then the whole picture's size, its menu
    // in its top rows, as its movies draw from the top of their viewport: that box taken, laid in the middle as before.)
    const int box_rows = vr->m_openxr->panel_box_rows;
    const bool tall = box_rows > 0 && height > (uint32_t)box_rows + 2;
    const RECT layer_part{0, 0, (LONG)width, (LONG)(tall ? (uint32_t)box_rows : height)};
    const uint32_t shown_height = tall ? (uint32_t)box_rows : height;
    const LONG x0 = (LONG)((eye_desc.Width - width) / 2);
    const LONG y0 = (LONG)((eye_desc.Height - shown_height) / 2);
    const RECT dest{x0, y0, x0 + (LONG)width, y0 + (LONG)shown_height};
    if (tall != L.tall_logged) {
        L.tall_logged = tall;
        spdlog::info("[VR] Menu panel: the interface layer is {}x{}, {}", width, height,
            tall ? std::format("taller than the menu's box ({} rows): its top {} rows shown", box_rows, box_rows) : std::string{"the menu's box"});
    }
    // Under the interface: the menu's 3D pictures (you, the planet, the item), each the size of the menu's box as its interface is
    // (laid on it the same way), their even background taken out. (Only while the world is kept drawn behind the menu: the game
    // draws them then; their copies are this frame's or the one before.)
    int previews = 0;
    if (vr->m_openxr->panel_previews.load() && m_keyed_batch != nullptr && ModSettings::g_internalSettings.menuLiveWorld) {
        // (Further than about 15/255 from the background's colour starts to show, 31/255 further shows whole: the background's
        // light gradient taken out, you kept.)
        const bool glow = vr->m_openxr->panel_preview_glow.load();
        const float key[4]{0.06f, 0.12f, glow ? 1.0f : 0.0f, 1.0f}; // (its third: the glow kept, or not)
        // (By depth: a depth that differs from the background's by more than about a millionth is something drawn; the colour key's
        // numbers kept for a glow over the background, and for a picture whose background has depth too.)
        // (Its sign: the light the pictures cast on their background kept over the world (a glow), or not: cut to their outline.)
        const float depth_key[4]{glow ? 1.0e-6f : -1.0e-6f, 0.06f, 0.12f, 1.0f};
        // (fix141) The pictures after the first add no light of their own, only what they draw: the Data menu's room picture holds
        // your character again, drawn there without depth, which was taken for light and added over the first picture's character
        // (its white robe blown out, its shadows orange-beige). The first picture's light (a lightsaber's blade) stays.
        const float key_later[4]{0.06f, 0.12f, 0.0f, 1.0f};
        const float depth_key_later[4]{-1.0e-6f, 0.06f, 0.12f, 3.0f}; // (its cover 3 - 2: the shader's mark for no light)
        auto device = g_framework->get_d3d12_hook()->get_device();
        for (int kind = 1; kind < (int)m_live.slots.size(); ++kind) {
            uint32_t pw = 0, ph = 0, used = 0;
            if (auto* picture = live_source(kind, frame, true, &pw, &ph, &used); picture != nullptr) {
                // Its depth of the same frame, if copied: cut out by it (the pair of views made again when either copy is another).
                ComPtr<ID3D12Resource> depth{};
                DXGI_FORMAT depth_view = DXGI_FORMAT_UNKNOWN;
                if (m_depth_keyed_batch != nullptr && m_depth_pairs != nullptr && device != nullptr && vr->m_openxr->panel_depth_cutout.load()) {
                    std::scoped_lock _{m_live.mtx};
                    const auto& ds = m_live.depth[kind][used & 1];
                    if (ds.has && ds.copy != nullptr && ds.frame == used) {
                        depth = ds.copy;
                        depth_view = ds.view_format;
                    }
                }
                if (depth != nullptr) {
                    const int index = kind * 2 + (int)(used & 1);
                    auto& of = m_depth_pairs_of[kind][used & 1];
                    if (of.first.Get() != picture->texture.Get() || of.second.Get() != depth.Get()) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC color_view{};
                        color_view.Format = picture->texture->GetDesc().Format;
                        color_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        color_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        color_view.Texture2D.MipLevels = 1;
                        device->CreateShaderResourceView(picture->texture.Get(), &color_view, m_depth_pairs->GetCpuHandle((size_t)index * 2));
                        D3D12_SHADER_RESOURCE_VIEW_DESC depth_desc = color_view;
                        depth_desc.Format = depth_view;
                        depth_desc.Texture2D.PlaneSlice = 0;
                        device->CreateShaderResourceView(depth.Get(), &depth_desc, m_depth_pairs->GetCpuHandle((size_t)index * 2 + 1));
                        of = {picture->texture, depth};
                    }
                    render_pair_to_rtv(commands.cmd_list.Get(), m_depth_pairs->GetGpuHandle((size_t)index * 2), pw, ph, L.composed,
                        D3D12_RESOURCE_STATE_PRESENT, dest, kind == 1 ? depth_key : depth_key_later);
                    ++m_live.depth_cut;
                } else {
                    render_srv_to_rtv(commands.cmd_list.Get(), *picture, L.composed, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, &dest,
                        m_keyed_batch.get(), kind == 1 ? key : key_later);
                    ++m_live.color_cut;
                }
                ++previews;
                // (fix159: 3 s after the menu was shown, not in its first moments: each save stalls the frame.)
                if (!m_live.previews_logged && kind == 1 && m_live.preview_menus < 8 &&
                    std::chrono::steady_clock::now() - m_menu_layer.shown_since > std::chrono::seconds(3)) {
                    m_live.previews_logged = true;
                    const int menu_number = ++m_live.preview_menus; // (each of the first menus: its own pictures, by its number)
                    spdlog::info("[VR] Menu panel: the menu's 3D pictures laid under its interface (the first: {}x{} over its {}x{} box)", pw, ph,
                        width, height);
                    // (Each of them saved once as it is, its alpha too, for the log.)
                    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    for (int k = 1; k < (int)m_live.slots.size(); ++k) {
                        if (auto* each = live_source(k, frame, true); each != nullptr) {
                            const auto path = Framework::get_persistent_dir(std::format("menu_3d_picture_{}_{}.png", menu_number, k));
                            m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
                            const auto hr = DirectX::SaveWICTextureToFile(queue, each->texture.Get(), GUID_ContainerFormatPng, path.wstring().c_str(),
                                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
                            spdlog::info("[VR] Menu panel: the menu's 3D picture {} saved to {} (hr {:x})", k, path.string(), (uint32_t)hr);
                        }
                    }
                }
            }
        }
    }
    // The interface; with a dark backing under its faint parts when the menu's buttons are made for a dark backdrop (the
    // Inventory's item rows): the menus' own dark blue, at most 85%.
    if (vr->m_openxr->panel_button_backing.load() && m_backing_batch != nullptr && ModSettings::g_internalSettings.menuButtonBacking) {
        const float backing[4]{0.047f, 0.106f, 0.141f, 0.85f};
        render_srv_to_rtv(commands.cmd_list.Get(), source, L.composed, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, &dest,
            m_backing_batch.get(), backing, &layer_part);
    } else {
        render_srv_to_rtv(commands.cmd_list.Get(), source, L.composed, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, &dest, nullptr,
            nullptr, &layer_part);
    }
    commands.execute();
    m_menu_layer.previews_shown = previews;
    // (fix192) This menu's copies, for the line at the panel going down.
    ++L.stats.made;
    L.stats.over_60 += L.fail_age_ms > 60 ? 1 : 0;
    L.stats.older_used += fresh ? 0 : 1;
    L.stats.oldest_ms = std::max<int64_t>(L.stats.oldest_ms, L.fail_age_ms);
    L.stats.most_behind = std::max<int32_t>(L.stats.most_behind, behind);
    return true;
}

bool D3D12Component::ensure_menu_composed(ID3D12Device* device, const D3D12_RESOURCE_DESC& eye_desc) {
    auto& L = m_menu_layer;
    if (device == nullptr) {
        L.fail_reason = "no device";
        return false;
    }
    // (The last picture's commands done before it is made again.)
    if (L.composed.texture != nullptr) {
        L.composed.commands.wait(INFINITE);
    }
    if (L.composed.texture == nullptr || L.composed_size[0] != (uint32_t)eye_desc.Width || L.composed_size[1] != eye_desc.Height) {
        L.composed.reset();
        L.composed_size[0] = L.composed_size[1] = 0;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = eye_desc.Width;
        d.Height = eye_desc.Height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // (what the sprite batch draws into, as the converted eye image)
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> made{};
        // (Kept in the present state between frames, as the converted eye image.)
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&made))) ||
            !L.composed.setup(device, made.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, L"Menu panel: the see-through picture")) {
            spdlog::error("[VR] Menu panel: could not make the see-through picture ({}x{})", (uint64_t)eye_desc.Width, eye_desc.Height);
            L.composed.reset();
            L.good_valid = false; // (fix173: no picture to show again)
            L.fail_reason = "the see-through picture could not be made";
            return false;
        }
        L.composed_size[0] = (uint32_t)eye_desc.Width;
        L.composed_size[1] = eye_desc.Height;
    }
    return true;
}

// (fix192) The first moments of a see-through menu, before a picture is made from its layer: the panel's see-through picture with its dark
// alone (no interface, no 3D pictures), so the panel shows the world behind it, dimmed where the menu will be, instead of the game's own
// picture (opaque, and while the frame is kept full size with the menu's box in the wrong rows).
bool D3D12Component::compose_menu_empty(VR* vr, ID3D12Resource* eye_texture) {
    auto& L = m_menu_layer;
    if (eye_texture == nullptr || vr == nullptr || vr->m_openxr == nullptr) {
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!ensure_menu_composed(device, eye_texture->GetDesc())) {
        return false;
    }
    auto& commands = L.composed.commands;
    commands.wait(INFINITE);
    const float dark = std::clamp(vr->m_openxr->panel_see_through_darkness.load(), 0.0f, 0.9f);
    const float clear_color[4]{0.0f, 0.0f, 0.0f, dark};
    commands.clear_rtv(L.composed, clear_color, D3D12_RESOURCE_STATE_PRESENT);
    commands.execute();
    L.previews_shown = 0;
    return true;
}

bool D3D12Component::menu_layer_height_of(uint32_t frame, uint32_t& height) {
    auto& L = m_menu_layer;
    std::scoped_lock _{L.mtx};
    for (uint32_t back = 0; back < 2; ++back) {
        const auto& s = L.seen[(frame - back) & 3];
        if (s.has && s.frame == frame - back) {
            height = s.height;
            return true;
        }
    }
    return false;
}

int D3D12Component::record_step_snapshot(ID3D12GraphicsCommandList* command_list, ID3D12Resource* picture, D3D12_RESOURCE_STATES state,
    const std::string& label) {
    if (command_list == nullptr || picture == nullptr) {
        return -1;
    }
    // (On the main kind of command list only: a compute list cannot change a render target's state.)
    if (command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return 0;
    }
    const auto desc = picture->GetDesc();
    // (A plain 2D picture of one level: with more, a step may have drawn another level, the first one in another state.)
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width == 0 || desc.Height == 0) {
        return 0;
    }
    // The kinds of colour picture kept, each copied into its typed kind (a copy within one family of formats).
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
    uint64_t pixel = 4;
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        typed = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        typed = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        typed = DXGI_FORMAT_R10G10B10A2_UNORM;
        break;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        typed = DXGI_FORMAT_R11G11B10_FLOAT;
        break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        typed = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pixel = 8;
        break;
    default:
        return 0;
    }
    const uint64_t bytes = desc.Width * desc.Height * pixel;
    constexpr size_t most_shots = 16;
    constexpr uint64_t most_bytes = 320ull * 1024 * 1024; // (video memory, for a moment)
    std::scoped_lock _{m_steps.mtx};
    if (m_steps.done || m_steps.finished || m_steps.shots.size() >= most_shots || m_steps.bytes + bytes > most_bytes) {
        return 0;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return -1;
    }
    D3D12_RESOURCE_DESC copy_desc{};
    copy_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    copy_desc.Width = desc.Width;
    copy_desc.Height = desc.Height;
    copy_desc.DepthOrArraySize = 1;
    copy_desc.MipLevels = 1;
    copy_desc.Format = typed;
    copy_desc.SampleDesc.Count = 1;
    copy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    copy_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // (its views are made with the texture helper, which makes both)
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> made{};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&made)))) {
        return -1;
    }
    made->SetName(L"Render steps test: a copy");
    // Its first level only (the state changed for that one alone), into the copy; the picture back as it was.
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = picture;
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        command_list->ResourceBarrier(1, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = made.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = picture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = picture;
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = state;
        command_list->ResourceBarrier(1, &barrier);
    }
    StepSnapshots::Shot shot{};
    shot.copy = made;
    shot.format = typed;
    shot.width = (uint32_t)desc.Width;
    shot.height = desc.Height;
    shot.label = label;
    m_steps.shots.push_back(std::move(shot));
    m_steps.bytes += bytes;
    return 1;
}

void D3D12Component::finish_step_snapshots() {
    std::scoped_lock _{m_steps.mtx};
    m_steps.finished = true;
}

void D3D12Component::process_step_snapshots(ID3D12CommandQueue* queue) {
    std::vector<StepSnapshots::Shot> shots{};
    {
        std::scoped_lock _{m_steps.mtx};
        if (m_steps.done || !m_steps.finished) {
            return;
        }
        // (A few frames on: the frame whose commands made the copies has surely run.)
        if (++m_steps.frames_since < 10) {
            return;
        }
        shots = std::move(m_steps.shots);
        m_steps.shots.clear();
        m_steps.bytes = 0;
        m_steps.done = true;
    }
    if (shots.empty()) {
        spdlog::info("[VR] Render steps test: no pictures were kept");
        return;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr || queue == nullptr || m_sprite_batch == nullptr) {
        spdlog::info("[VR] Render steps test: the pictures could not be put together (no device)");
        return;
    }
    // One picture: a grid of 4 across, each step's picture made to fit its cell (the sprite batch draws into an 8-bit picture).
    constexpr LONG columns = 4;
    constexpr LONG cell_w = 512;
    constexpr LONG cell_h = 552;
    const LONG rows = ((LONG)shots.size() + columns - 1) / columns;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT64)(columns * cell_w);
    d.Height = (UINT)(rows * cell_h);
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> made{};
    d3d12::TextureContext sheet{};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&made))) ||
        !sheet.setup(device, made.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, L"Render steps test: the picture")) {
        spdlog::error("[VR] Render steps test: could not make the picture");
        return;
    }
    std::vector<std::unique_ptr<d3d12::TextureContext>> sources{};
    auto& commands = sheet.commands;
    commands.wait(INFINITE);
    const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    commands.clear_rtv(sheet, black, D3D12_RESOURCE_STATE_PRESENT);
    std::vector<std::string> cells{};
    for (size_t i = 0; i < shots.size(); ++i) {
        const auto& shot = shots[i];
        auto source = std::make_unique<d3d12::TextureContext>();
        if (!source->setup(device, shot.copy.Get(), shot.format, shot.format, L"Render steps test: a copy's views")) {
            cells.push_back(std::format("(not shown: no views) {}", shot.label));
            continue;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shot.copy.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commands.cmd_list->ResourceBarrier(1, &barrier);
        const LONG cx = (LONG)(i % columns) * cell_w;
        const LONG cy = (LONG)(i / columns) * cell_h;
        const float scale = std::min((float)cell_w / (float)shot.width, (float)cell_h / (float)shot.height);
        const LONG w = std::max<LONG>(1, (LONG)std::lround((float)shot.width * scale));
        const LONG h = std::max<LONG>(1, (LONG)std::lround((float)shot.height * scale));
        const RECT dest{cx + (cell_w - w) / 2, cy + (cell_h - h) / 2, cx + (cell_w - w) / 2 + w, cy + (cell_h - h) / 2 + h};
        render_srv_to_rtv(commands.cmd_list.Get(), *source, sheet, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, &dest);
        cells.push_back(shot.label);
        sources.push_back(std::move(source));
    }
    commands.execute();
    commands.wait(INFINITE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto path = Framework::get_persistent_dir("menu_steps.jpg");
    m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
    const auto hr = DirectX::SaveWICTextureToFile(queue, sheet.texture.Get(), GUID_ContainerFormatJpeg, path.wstring().c_str(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
    spdlog::info("[VR] Render steps test: {} pictures put together in {} (hr {:x}); its cells, left to right and down, {} across:", cells.size(),
        path.string(), (uint32_t)hr, columns);
    for (size_t i = 0; i < cells.size(); ++i) {
        spdlog::info("[VR] Render steps test: cell {} (row {}, column {}): {}", i + 1, i / columns + 1, i % columns + 1, cells[i]);
    }
}

namespace {
// A menu's 3D picture with its even background taken away. The background's colour is that of its top corners (the picture's model
// is in its middle part); a pixel no further from either than color.r is see-through, one color.g further than that whole
// (premultiplied alpha; color.a: the most it covers). color.b above 0.5: the light over the background (a glow) kept; else no more
// light than it covers.
constexpr const char* kKeyedPictureShader = R"(
Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    float3 c  = Texture.Sample(TextureSampler, uv).rgb;
    float3 k1 = Texture.SampleLevel(TextureSampler, float2(0.004, 0.004), 0).rgb;
    float3 k2 = Texture.SampleLevel(TextureSampler, float2(0.996, 0.004), 0).rgb;
    float3 d1 = abs(c - k1);
    float3 d2 = abs(c - k2);
    float  e1 = max(d1.r, max(d1.g, d1.b));
    float  e2 = max(d2.r, max(d2.g, d2.b));
    float3 k  = e1 <= e2 ? k1 : k2;
    float  a  = saturate((min(e1, e2) - color.r) / max(color.g, 0.001));
    // (Where it is partly the background, the background's colour taken out of it: the glow around you stays a glow, not a blue haze.)
    float3 f  = max(c - (1.0 - a) * k, 0.0);
    f = color.b > 0.5 ? f : min(f, a);
    return float4(f * color.a, a * color.a);
}
)";

// A menu's 3D picture cut out by its depth (t1, the same frame's). The game's depth is reversed (1 at the near plane, 0 at the far
// one) and cleared to 0 where nothing is drawn (fix121's research); the background's depth is the nearer of the picture's top
// corners', and a pixel is drawn where its depth is nearer than that (by 5%, and more than color.x): kept whole. The game draws the
// preview through a fine dither that its anti-aliasing would smooth over frames (behind the menu its history is the world's, so it
// is not): a pixel where nothing is drawn, among drawn ones (a quarter to nearly half of the 24 around it, and more), is filled
// from them; so its silhouette's edge is soft by a pixel. Light over the background (a glow, more than color.y over the
// background's colour) is kept as light, when color.x is above 0 (the setting; its size is the depth's margin either way). If the
// picture's edges are not all at the background's depth (a background drawn with
// depth that is not even), it is cut out by colour as the keyed shader does (color.y, color.z). color.w: the most it covers.
// Light far brighter than the background drawn without depth (a lightsaber's blade) is kept as light added over the world, soft,
// with its glow a few pixels round it (Lit, NearLit; fix128 kept it as if drawn: a hard white bar, fix131; without the
// glow it was cut away). (Premultiplied alpha.)
constexpr const char* kDepthKeyedPictureShader = R"(
Texture2D<float4> Picture : register(t0);
Texture2D<float>  Depth   : register(t1);
sampler PictureSampler : register(s0);
static float  g_bg;
static float  g_eps;
static float3 g_k1;
static float3 g_k2;
static float2 g_size;
bool Drawn(int2 q, int2 dmax)
{
    const float d = Depth.Load(int3(clamp(q, int2(0, 0), dmax), 0));
    return d > g_bg * 1.05 + g_eps;
}
// Light drawn without depth (a lightsaber's blade, a planet's bright wisps): far brighter than the background (by 0.45 in one of
// its colours, over the nearer of its top corners'), added as light (main). The faint light the preview casts on its background is not.
bool Lit(float3 s)
{
    const float3 d1 = s - g_k1;
    const float3 d2 = s - g_k2;
    return min(max(d1.r, max(d1.g, d1.b)), max(d2.r, max(d2.g, d2.b))) > 0.45;
}
float3 At(int2 q, int2 dmax)
{
    return Picture.SampleLevel(PictureSampler, (float2(clamp(q, int2(0, 0), dmax)) + 0.5) / g_size, 0).rgb;
}
// A light here: bright, and nothing drawn (no depth) there or 1 and 2 pixels round it: fix132, the white robe (drawn) was taken
// for light by its colour alone, and the light round the character came back with it; the gaps of the game's dither inside the
// character, and its smoothed edge, have the robe's colour and no depth too (its anti-aliasing history kept): not lights either. A
// blade drawn without depth, away from the character, is. (Its colour first: the depth looked at only where it is bright.)
bool LitAt(int2 q, int2 dmax)
{
    bool lit = false;
    [branch] if (Lit(At(q, dmax))) {
        lit = !(Drawn(q, dmax) || Drawn(q + int2(1, 0), dmax) || Drawn(q + int2(-1, 0), dmax) || Drawn(q + int2(0, 1), dmax) ||
                Drawn(q + int2(0, -1), dmax) || Drawn(q + int2(2, 0), dmax) || Drawn(q + int2(-2, 0), dmax) || Drawn(q + int2(0, 2), dmax) ||
                Drawn(q + int2(0, -2), dmax));
    }
    return lit;
}
// A light without depth within a few pixels (1, 2, 3, 5 and 7 away, in eight directions: a blade a pixel wide is not missed
// between them): the light here is its glow (the game's bloom round it), kept.
bool NearLit(int2 p, int2 dmax)
{
    const int2 ways[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, -1), int2(1, -1), int2(-1, 1) };
    const int  steps[5] = { 1, 2, 3, 5, 7 };
    bool found = false;
    [loop] for (int i = 0; i < 8; ++i) {
        [loop] for (int j = 0; j < 5; ++j) {
            [branch] if (!found) {
                found = LitAt(p + ways[i] * steps[j], dmax);
            }
        }
    }
    return found;
}
// The light a light without depth adds: what it adds over the background; nearly white where the picture is (its core clipped
// to white, which over the background's navy would give a beige).
float3 LightOf(float3 s, float3 k)
{
    const float top = max(s.r, max(s.g, s.b));
    return lerp(max(s - k, 0.0), s, saturate((top - 0.85) / 0.15));
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    uint dw, dh;
    Depth.GetDimensions(dw, dh);
    const int2  dmax = int2((int)dw - 1, (int)dh - 1);
    const float bg1  = Depth.Load(int3(2, 2, 0));
    const float bg2  = Depth.Load(int3(dmax.x - 2, 2, 0));
    g_bg   = max(bg1, bg2);
    g_eps  = abs(color.x); // (its sign: the glow kept (+) or not (-))
    // (color.w above 1.5: a picture after the first, which adds no light of its own: its cover color.w - 2, fix141.)
    const bool  light = color.w < 1.5;
    const float cover = light ? color.w : color.w - 2.0;
    g_size = float2(dw, dh);
    float3 c  = Picture.SampleLevel(PictureSampler, uv, 0).rgb;
    float3 k1 = Picture.SampleLevel(PictureSampler, float2(0.004, 0.004), 0).rgb;
    float3 k2 = Picture.SampleLevel(PictureSampler, float2(0.996, 0.004), 0).rgb;
    g_k1 = k1;
    g_k2 = k2;
    // (The background's colour: the nearer of its top corners', as the colour key takes it.)
    float3 d1 = abs(c - k1);
    float3 d2 = abs(c - k2);
    float  e1 = max(d1.r, max(d1.g, d1.b));
    float  e2 = max(d2.r, max(d2.g, d2.b));
    float3 k  = e1 <= e2 ? k1 : k2;
    float spread = abs(bg1 - bg2);
    spread = max(spread, abs(Depth.Load(int3(2, (int)(dh >> 1), 0)) - bg1));
    spread = max(spread, abs(Depth.Load(int3(dmax.x - 2, (int)(dh >> 1), 0)) - bg1));
    if (spread > g_eps) {
        float  a0 = saturate((min(e1, e2) - color.y) / max(color.z, 0.001));
        float3 f0 = max(c - (1.0 - a0) * k, 0.0);
        f0 = color.x > 0.0 ? f0 : min(f0, a0); // (the glow not kept: no more light than it covers)
        return float4(f0 * cover, a0 * cover);
    }
    const int2 p = clamp(int2(uv * float2(dw, dh)), int2(0, 0), dmax);
    if (Drawn(p, dmax)) {
        return float4(c * cover, cover);
    }
    // Light drawn without depth (a lightsaber's blade, fix131): added over the world as the light it adds over the background,
    // soft as the game draws it, with its glow right around it (a few pixels: not the faint light the preview casts elsewhere,
    // the halo on the character that fix127 took away). A gap of the game's fine dither in it (most of its four neighbours
    // lit) is filled from them; light of its own only looked into where there is some (the rest of the picture costs little).
    float3 glow = (light && color.x > 0.0) ? max(c - k - color.y, 0.0) : float3(0.0, 0.0, 0.0);
    [branch] if (light) {
        const float3 s0 = At(p + int2( 1, 0), dmax), s1 = At(p + int2(-1, 0), dmax), s2 = At(p + int2(0, 1), dmax), s3 = At(p + int2(0, -1), dmax);
        const bool   l0 = LitAt(p + int2( 1, 0), dmax), l1 = LitAt(p + int2(-1, 0), dmax), l2 = LitAt(p + int2(0, 1), dmax), l3 = LitAt(p + int2(0, -1), dmax);
        const float  lit_near = (l0 ? 1.0 : 0.0) + (l1 ? 1.0 : 0.0) + (l2 ? 1.0 : 0.0) + (l3 ? 1.0 : 0.0);
        const float3 own = max(c - k, 0.0);
        if (LitAt(p, dmax)) {
            glow = max(glow, LightOf(c, k));
        } else if (lit_near >= 2.0) {
            const float3 sum = (l0 ? LightOf(s0, k) : 0.0) + (l1 ? LightOf(s1, k) : 0.0) + (l2 ? LightOf(s2, k) : 0.0) + (l3 ? LightOf(s3, k) : 0.0);
            glow = max(glow, sum / lit_near);
        } else {
            // (Nested: HLSL evaluates both sides of &&; the search only where there is light.)
            [branch] if (max(own.r, max(own.g, own.b)) > 0.02) {
                [branch] if (NearLit(p, dmax)) {
                    glow = max(glow, own);
                }
            }
        }
    }
    // (Nothing drawn two pixels around, as for most of the picture: background, and the light only.)
    float near = 0.0;
    near += Drawn(p + int2( 2,  0), dmax) ? 1.0 : 0.0;
    near += Drawn(p + int2(-2,  0), dmax) ? 1.0 : 0.0;
    near += Drawn(p + int2( 0,  2), dmax) ? 1.0 : 0.0;
    near += Drawn(p + int2( 0, -2), dmax) ? 1.0 : 0.0;
    near += Drawn(p + int2( 1,  1), dmax) ? 1.0 : 0.0;
    near += Drawn(p + int2(-1, -1), dmax) ? 1.0 : 0.0;
    if (near == 0.0) {
        return float4(glow * cover, 0.0);
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    float  n   = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), dmax);
            if ((x != 0 || y != 0) && Drawn(q, dmax)) {
                sum += At(q, dmax);
                n += 1.0;
            }
        }
    }
    const float  a    = saturate((n / 24.0 - 0.25) / 0.2);
    const float3 fill = n > 0.0 ? sum / n : c;
    return float4((fill * a + glow * (1.0 - a)) * cover, a * cover);
}
)";

// The sprite batch's own vertex shader, compiled here as its pixel shaders are (DirectXTK12's own shaders are Shader Model 6, and a
// pipeline cannot mix those with Shader Model 5 ones): the same as DirectXTK12's SpriteVertexShader.
constexpr const char* kSpriteVertexShader = R"(
cbuffer Parameters : register(b0)
{
    row_major float4x4 MatrixTransform;
};
void main(inout float4 color : COLOR0, inout float2 texCoord : TEXCOORD0, inout float4 position : SV_Position)
{
    position = mul(position, MatrixTransform);
}
)";

// The menu drawn into an eye picture: a sprite over the whole picture (its texture coordinates are where in the picture, 0..1 from
// the top left), each pixel's line of sight met with the panel in the room. The batch's "transform" carries the numbers instead
// (OpenXR::menu_draw_params): rows 0-2 the line of sight in the panel's own frame (A u + B v + C, from the eye at their w), row 3
// the panel (its distance, its angle (0: flat), its height and width in metres). color: the panel's picture (x, y: the part shown,
// its top and bottom; w 0), or the pointer's dot (x, y: where on the panel; z: its radius in metres; w 1).
constexpr const char* kPanelRayVertexShader = R"(
void main(inout float4 color : COLOR0, inout float2 texCoord : TEXCOORD0, inout float4 position : SV_Position)
{
    position = float4(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0, 0.0, 1.0);
}
)";
constexpr const char* kPanelRayShader = R"(
cbuffer Parameters : register(b0)
{
    row_major float4x4 P;
};
Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    float3 d = P[0].xyz * uv.x + P[1].xyz * uv.y + P[2].xyz;
    float3 o = float3(P[0].w, P[1].w, P[2].w);
    float distance = P[3].x, angle = P[3].y, height = P[3].z, width = P[3].w;
    float t = -1.0;
    if (angle > 0.0) {
        // (Inside the cylinder: the far crossing.)
        float a = dot(d.xz, d.xz);
        float b = 2.0 * dot(o.xz, d.xz);
        float c = dot(o.xz, o.xz) - distance * distance;
        float disc = b * b - 4.0 * a * c;
        if (a > 1e-6 && disc >= 0.0) {
            t = (-b + sqrt(disc)) / (2.0 * a);
        }
    } else if (d.z < -1e-4) {
        t = (-distance - o.z) / d.z;
    }
    float3 h = o + d * max(t, 0.0);
    float2 pv = float2(angle > 0.0 ? 0.5 + atan2(h.x, -h.z) / angle : 0.5 + h.x / width, 0.5 - h.y / height);
    // (Where on the panel's picture; its steps across this pixel, for four samples within it: the text stays sharp and even.)
    float2 tc = float2(pv.x, lerp(color.x, color.y, pv.y));
    float2 dx = clamp(ddx(tc), -0.01, 0.01) * 0.25;
    float2 dy = clamp(ddy(tc), -0.01, 0.01) * 0.25;
    bool inside = t > 0.0 && all(pv >= 0.0) && all(pv <= 1.0);
    if (!inside) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    if (color.w < 0.5) {
        float4 s = Texture.SampleLevel(TextureSampler, tc + dx + dy, 0) + Texture.SampleLevel(TextureSampler, tc + dx - dy, 0) +
                   Texture.SampleLevel(TextureSampler, tc - dx + dy, 0) + Texture.SampleLevel(TextureSampler, tc - dx - dy, 0);
        return s * 0.25; // (premultiplied, as the panel's picture is)
    }
    float2 m = float2((pv.x - color.x) * width, (pv.y - color.y) * height);
    float  k = (1.0 - smoothstep(color.z * 0.6, color.z, length(m))) * 0.75;
    return float4(float3(0.35, 0.8, 1.0) * k, k);
}
)";

// The gun's laser sight (LaserSight): each pixel of a small picture, the game's depth at one point of the laser's line (its pixel
// in color.xy; below 0: off the picture, 0 written). The depth's 32 bits are written as the pixel's four bytes, read back exactly.
constexpr const char* kLaserDepthShader = R"(
Texture2D<float> Depth : register(t0);
sampler TextureSampler : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    uint w, h;
    Depth.GetDimensions(w, h);
    const int2 p = int2(floor(color.xy));
    if (color.x < 0.0 || color.y < 0.0 || p.x >= (int)w || p.y >= (int)h) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const uint b = asuint(Depth.Load(int3(p, 0)));
    return float4((float)(b & 255u), (float)((b >> 8) & 255u), (float)((b >> 16) & 255u), (float)(b >> 24)) / 255.0;
}
)";

// A menu's interface (premultiplied alpha) with a dark backing (color.rgb, at most color.a) under its faint parts: where the
// interface covers a little (its buttons, a light box at a tenth of its strength), the backing shows under it, as the flat game's
// dark backdrop does; where it covers nothing it stays see-through, where it covers all it is as it is.
constexpr const char* kBackingShader = R"(
Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    float4 c = Texture.Sample(TextureSampler, uv);
    float  m = color.a * smoothstep(0.02, 0.07, c.a);
    float  k = (1.0 - c.a) * m;
    return float4(c.rgb + color.rgb * k, c.a + k);
}
)";

// (fix164) The soft light round the panel (present_ambient), pass 1: the panel's shown rows of the game's picture made small, each pixel of
// the 64x36 picture the average of 8 x 8 points spread over its own part of those rows (color.xy: that part's size, as a part of the
// picture), each point's colour taken from sRGB to linear light first (light is averaged, not its sRGB numbers).
constexpr const char* kAmbientDownShader = R"(
Texture2D<float4> Picture : register(t0);
sampler PictureSampler : register(s0);
float3 Linear(float3 s)
{
    s = saturate(s);
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (int y = 0; y < 8; ++y) {
        [unroll] for (int x = 0; x < 8; ++x) {
            const float2 at = uv + ((float2(x, y) + 0.5) / 8.0 - 0.5) * color.xy;
            sum += Linear(Picture.SampleLevel(PictureSampler, at, 0).rgb);
        }
    }
    return float4(sum / 64.0, 1.0);
}
)";

// (fix164) Pass 2: what is kept moved toward what is now (t0 now, t1 kept: out = kept + (now - kept) x k): k is color.x (a time constant of
// 0.4 s over this frame's time) or, when the picture changed a lot against what is kept (a cut: the mean change over a grid of 8 x 4
// points, in a scale nearer the eye's than light's), color.y (0.12 s); color.z above 0.5: started anew (what is now, as it is). The
// first four pixels' alpha keep the picture's black border (as Hyperion finds it: rows or columns lit nowhere, from each side, at most a
// third of the picture; as parts of it: top, bottom, left, right), kept smoothly the same way; a frame dark all over keeps the last one
// (a fade to black is no border). The other pixels' alpha is not used.
constexpr const char* kAmbientSmoothShader = R"(
Texture2D<float4> Now  : register(t0);
Texture2D<float4> Kept : register(t1);
sampler PictureSampler : register(s0);
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float Border(uint side, uint2 size)
{
    const bool rows  = side < 2u;
    const uint steps = (rows ? size.y : size.x) / 3u;
    [loop] for (uint i = 0u; i < steps; ++i) {
        float lit = 0.0;
        [unroll] for (uint j = 0u; j < 16u; ++j) { // (unrolled: its 16 reads go out together; the review)
            const uint  along = rows ? (j * size.x) / 16u + size.x / 32u : (j * size.y) / 16u + size.y / 32u;
            const uint2 q     = rows ? uint2(along, side == 0u ? i : size.y - 1u - i) : uint2(side == 2u ? i : size.x - 1u - i, along);
            lit = max(lit, dot(Now.Load(int3(q, 0)).rgb, kLuma));
        }
        if (lit > 0.004) {
            return (float)i / (float)(rows ? size.y : size.x);
        }
    }
    return 0.0; // (dark that far in, but no border: not a letterbox)
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    uint w, h;
    Now.GetDimensions(w, h);
    const uint2  size = uint2(w, h);
    const float3 c    = Now.SampleLevel(PictureSampler, uv, 0).rgb;
    const float3 kept = Kept.SampleLevel(PictureSampler, uv, 0).rgb;
    float change = 0.0;
    float brightest = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 8; ++x) {
            const float2 at = (float2(x, y) + 0.5) / float2(8.0, 4.0);
            const float3 n  = Now.SampleLevel(PictureSampler, at, 0).rgb;
            const float3 d  = abs(sqrt(max(n, 0.0)) - sqrt(max(Kept.SampleLevel(PictureSampler, at, 0).rgb, 0.0)));
            change += (d.r + d.g + d.b) / 3.0;
            brightest = max(brightest, dot(n, kLuma));
        }
    }
    change /= 32.0;
    const float  k    = lerp(color.x, color.y, smoothstep(0.06, 0.15, change));
    const bool   anew = color.z > 0.5;
    const float3 o    = anew ? c : lerp(kept, c, k);
    float a = 1.0;
    const uint2 p = uint2(uv * float2(size));
    [branch] if (p.y == 0u && p.x < 4u) {
        const float was = Kept.Load(int3(p, 0)).a;
        [branch] if (brightest > 0.004) {
            const float b = Border(p.x, size);
            a = anew ? b : lerp(was, b, k);
        } else {
            a = anew ? 0.0 : was;
        }
    }
    return float4(max(o, 0.0), a);
}
)";

// (fix164) Pass 3: the light's own image (512x256 over its cylinder, or its flat quad) from the kept picture (t0, linear light). Each pixel
// is a point of the light's surface (the sprite over the whole image, as the menu drawn into the eyes: uv is the place in it); the numbers
// come in the batch's "transform" (P): row 0 the light's shape (curved above 0.5; its arc in radians or width in metres; its height; its
// distance), row 1 the panel's (its arc or width; its height; its distance; the strength), row 2 (the fade-in, the floor, the cap, the
// saturation), row 3 (the falloff in metres, the share of the whole picture's average, the width of the fading at the light's own edges
// in metres). That point seen from the panel's middle is a place on the panel (pu, pv) or beside it, d metres (on the light's surface)
// from its edge. Its colour: a soft disc of the picture round the nearest point of what it shows (its black border taken off, as
// Hyperion does: the panel's edges are the content's, from the kept picture's first four pixels' alpha), the disc wider further out; a quarter of the content's average mixed in; its saturation x1.3; at most the cap in brightness (a tenth of white); fading
// as e^(-d / falloff); never below the floor (1% of white, in the picture's own hue); all of it times the strength and the fade-in, and
// faded out at the light's own edges (they are never seen). Stored as sRGB (as the eye images are), with half a step of noise (no bands in
// so smooth a light, through Virtual Desktop's video; color.xy: the image's size). On the panel's own part: its colour there (covered by
// the panel).
constexpr const char* kAmbientLightShader = R"(
cbuffer Parameters : register(b0)
{
    row_major float4x4 P;
};
Texture2D<float4> Light : register(t0);
sampler LightSampler : register(s0);
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float3 At(float2 q)
{
    return Light.SampleLevel(LightSampler, saturate(q), 0).rgb;
}
float3 In(float2 q, float2 lo, float2 hi)
{
    return At(clamp(q, lo, hi));
}
float3 Srgb(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0
{
    const bool  curved   = P[0].x > 0.5;
    const float span     = P[0].y;
    const float tall     = P[0].z;
    const float radius   = P[0].w;
    const float pspan    = P[1].x;
    const float ptall    = P[1].y;
    const float distance = P[1].z;
    const float strength = P[1].w;
    const float scale    = distance / radius; // (the light's surface seen at the panel's distance)
    // This point of the light: across (radians round the cylinder, or metres across the quad) and up (metres), from its middle.
    const float across = (uv.x - 0.5) * span;
    const float up     = (0.5 - uv.y) * tall;
    float pu, out_x, edge_x;
    if (curved) {
        pu     = 0.5 + across / pspan;
        out_x  = max(abs(across) - pspan * 0.5, 0.0) * radius;
        edge_x = (span * 0.5 - abs(across)) * radius;
    } else {
        pu     = 0.5 + across * scale / pspan;
        out_x  = max(abs(across) - pspan * 0.5 / scale, 0.0);
        edge_x = span * 0.5 - abs(across);
    }
    const float pv     = 0.5 - up * scale / ptall;
    const float out_y  = max(abs(up) - ptall * 0.5 / scale, 0.0);
    const float edge_y = tall * 0.5 - abs(up);
    const float d      = length(float2(out_x, out_y));
    // What the picture shows inside its black border (top, bottom, left, right: at most 40% each).
    const float4 border = float4(Light.Load(int3(0, 0, 0)).a, Light.Load(int3(1, 0, 0)).a, Light.Load(int3(2, 0, 0)).a, Light.Load(int3(3, 0, 0)).a);
    const float2 lo   = min(saturate(border.zx), 0.4);
    const float2 hi   = 1.0 - min(saturate(border.wy), 0.4);
    const float2 part = hi - lo;
    // Its average (a grid of 8 x 4 points).
    float3 average = float3(0.0, 0.0, 0.0);
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 8; ++x) {
            average += At(lo + (float2(x, y) + 0.5) / float2(8.0, 4.0) * part);
        }
    }
    average /= 32.0;
    // A soft disc round the nearest point of the panel (its edges the content's): 17 points on a golden-angle spiral, the disc 6 cm
    // across at the edge and wider further out, so the light is a blurred, enlarged copy of the picture's edges (as YouTube's ambient
    // mode; one zone per edge, sampled along it, left sharp lines from the corners where the nearest edge changes).
    const float2 c      = saturate(float2(pu, pv));
    const float  width  = curved ? pspan * distance : pspan;
    const float  disc   = 0.06 + d * 0.5;
    const float2 reach  = float2(disc / max(width, 0.1), disc / max(ptall, 0.1));
    float3       zone   = In(lo + c * part, lo, hi);
    [unroll] for (int i = 0; i < 16; ++i) {
        const float  a = (float)i * 2.39996323;
        const float  r = sqrt(((float)i + 0.5) / 16.0);
        const float2 q = saturate(c + float2(cos(a), sin(a)) * r * reach);
        zone += In(lo + q * part, lo, hi);
    }
    zone /= 17.0;
    float3 glow = lerp(zone, average, P[3].y);
    const float l0 = dot(glow, kLuma);
    glow = max(l0 + (glow - l0) * P[2].w, 0.0);
    const float l1 = dot(glow, kLuma);
    glow *= l1 > P[2].z ? P[2].z / l1 : 1.0;
    glow *= exp(-d / P[3].x);
    const float  top  = max(average.r, max(average.g, average.b));
    const float3 tint = top > 1e-4 ? average / top : float3(1.0, 1.0, 1.0);
    glow = max(glow, tint * P[2].y);
    glow *= smoothstep(0.0, P[3].z, edge_x) * smoothstep(0.0, P[3].z, edge_y) * P[2].x * strength;
    // Half a step of noise (interleaved gradient noise) on the image's own pixels.
    const float2 pixel = floor(uv * color.xy);
    const float  noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5;
    return float4(Srgb(glow) + noise / 255.0, 1.0);
}
)";

// (fix164) The soft light's pictures: what is drawn small and kept (linear light), and its own image.
constexpr uint32_t kAmbientSmallWidth  = 64;
constexpr uint32_t kAmbientSmallHeight = 36;
constexpr uint32_t kAmbientLightWidth  = 512;
constexpr uint32_t kAmbientLightHeight = 256;
}

void D3D12Component::ensure_custom_batches() {
    if (m_custom_batches_tried) {
        return;
    }
    m_custom_batches_tried = true;
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto queue = hook->get_command_queue();
    if (device == nullptr || queue == nullptr) {
        m_custom_batches_tried = false; // (again later)
        return;
    }
    const auto compile = [&](const char* source, const char* name, const char* target) -> ComPtr<ID3DBlob> {
        ComPtr<ID3DBlob> code{};
        ComPtr<ID3DBlob> errors{};
        const HRESULT hr = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &code, &errors);
        if (FAILED(hr) || code == nullptr) {
            std::string message{};
            if (errors != nullptr) {
                message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            }
            spdlog::error("[VR] Menu panel: the shader {} could not be compiled (hr {:x}): {}", name, (uint32_t)hr, message);
            return nullptr;
        }
        return code;
    };
    // (Its vertex shader compiled here too: both halves of the pipeline the same kind of shader.)
    const auto vertex = compile(kSpriteVertexShader, "vr_menu_sprite_vertex", "vs_5_0");
    // (fix164: format, what it draws into; vertex_shader, another vertex shader than the sprite batch's own.)
    const auto make = [&](const char* source, const char* name, ID3D12RootSignature* root = nullptr, bool opaque = false,
                          DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, ID3DBlob* vertex_shader = nullptr) -> std::unique_ptr<DirectX::DX12::SpriteBatch> {
        const auto code = compile(source, name, "ps_5_0");
        ID3DBlob* const vs = vertex_shader != nullptr ? vertex_shader : vertex.Get();
        if (code == nullptr || vs == nullptr) {
            return nullptr;
        }
        try {
            DirectX::ResourceUploadBatch upload{device};
            upload.Begin();
            const DirectX::RenderTargetState output_state{format, DXGI_FORMAT_UNKNOWN};
            // (Opaque: written as it is, not blended over what is there: the laser's readings, whose bytes are numbers.)
            DirectX::SpriteBatchPipelineStateDescription pd{output_state, opaque ? &DirectX::CommonStates::Opaque : nullptr};
            pd.customRootSignature = root;
            pd.customVertexShader = D3D12_SHADER_BYTECODE{vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.customPixelShader = D3D12_SHADER_BYTECODE{code->GetBufferPointer(), code->GetBufferSize()};
            auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
            upload.End(queue).wait();
            return batch;
        } catch (const std::exception& e) {
            spdlog::error("[VR] Menu panel: the sprite batch with the pixel shader {} could not be made ({}; device removed: {:x})", name, e.what(),
                (uint32_t)device->GetDeviceRemovedReason());
            return nullptr;
        } catch (...) {
            spdlog::error("[VR] Menu panel: the sprite batch with the pixel shader {} could not be made", name);
            return nullptr;
        }
    };
    m_keyed_batch = make(kKeyedPictureShader, "vr_menu_keyed_picture");
    m_backing_batch = make(kBackingShader, "vr_menu_button_backing");
    m_laser_batch = make(kLaserDepthShader, "vr_laser_depth", nullptr, true);
    // The 3D pictures cut out by their depth: the sprite batch's root signature (its table at 0, its constants at 1, its static
    // sampler: SpriteBatch's own, as CommonStates::StaticLinearClamp), with a table of two views (the colour, the depth after it).
    try {
        D3D12_DESCRIPTOR_RANGE pair{};
        pair.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        pair.NumDescriptors = 2;
        pair.BaseShaderRegister = 0;
        pair.RegisterSpace = 0;
        pair.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &pair;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.f;
        sampler.MaxAnisotropy = 16;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.MinLOD = 0.f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
        ComPtr<ID3DBlob> blob{}, errors{};
        if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
            SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_depth_keyed_root)))) {
            m_depth_keyed_batch = make(kDepthKeyedPictureShader, "vr_menu_depth_keyed_picture", m_depth_keyed_root.Get());
            m_depth_pairs_of = {}; // (a new table: none of its views made yet)
            m_depth_pairs = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 16);
        }
    } catch (...) {
        m_depth_keyed_batch.reset();
        m_depth_pairs.reset();
    }
    if (m_depth_keyed_batch == nullptr || m_depth_pairs == nullptr) {
        m_depth_keyed_batch.reset();
        m_depth_pairs.reset();
        spdlog::warn("[VR] Menu panel: the 3D pictures cannot be cut out by their depth (its shader or views could not be made): by colour");
    }
    // The menu drawn into the eye pictures: its own vertex shader (the sprite over the whole picture, whatever the "transform", which
    // carries the panel's numbers: not applied, the batch's rotation left unspecified so they reach the shaders as they are).
    {
        const auto ray_vertex = compile(kPanelRayVertexShader, "vr_menu_ray_vertex", "vs_5_0");
        const auto ray_pixel = compile(kPanelRayShader, "vr_menu_ray_pixel", "ps_5_0");
        if (ray_vertex != nullptr && ray_pixel != nullptr) {
            try {
                DirectX::ResourceUploadBatch upload{device};
                upload.Begin();
                const DirectX::RenderTargetState output_state{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN};
                DirectX::SpriteBatchPipelineStateDescription pd{output_state};
                pd.customVertexShader = D3D12_SHADER_BYTECODE{ray_vertex->GetBufferPointer(), ray_vertex->GetBufferSize()};
                pd.customPixelShader = D3D12_SHADER_BYTECODE{ray_pixel->GetBufferPointer(), ray_pixel->GetBufferSize()};
                m_panel_ray_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
                upload.End(queue).wait();
                m_panel_ray_batch->SetRotation(DXGI_MODE_ROTATION_UNSPECIFIED);
            } catch (...) {
                m_panel_ray_batch.reset();
                spdlog::error("[VR] Menu panel: the shader that draws the menu into the eye pictures could not be set up");
            }
        }
    }
    // (fix164) The soft light round the panel: its three passes, each written as it is (opaque); the first two into pictures of light (16-bit
    // floats), the second reading two pictures (the depth cut-out's root signature: a table of two views), the third with the menu-drawn-in
    // batch's vertex shader (the sprite over the whole image, its numbers in the "transform", passed as they are).
    {
        const auto light_vertex = compile(kPanelRayVertexShader, "vr_ambient_light_vertex", "vs_5_0");
        m_ambient_down_batch = make(kAmbientDownShader, "vr_ambient_down", nullptr, true, DXGI_FORMAT_R32G32B32A32_FLOAT);
        m_ambient_smooth_batch = m_depth_keyed_root != nullptr
                                     ? make(kAmbientSmoothShader, "vr_ambient_smooth", m_depth_keyed_root.Get(), true, DXGI_FORMAT_R32G32B32A32_FLOAT)
                                     : nullptr;
        m_ambient_light_batch = light_vertex != nullptr
                                    ? make(kAmbientLightShader, "vr_ambient_light", nullptr, true, DXGI_FORMAT_R8G8B8A8_UNORM, light_vertex.Get())
                                    : nullptr;
        if (m_ambient_light_batch != nullptr) {
            m_ambient_light_batch->SetRotation(DXGI_MODE_ROTATION_UNSPECIFIED);
        }
        if (m_ambient_down_batch == nullptr || m_ambient_smooth_batch == nullptr || m_ambient_light_batch == nullptr) {
            m_ambient_down_batch.reset();
            m_ambient_smooth_batch.reset();
            m_ambient_light_batch.reset();
            spdlog::warn("[VR] Ambient light: its shaders could not be made (above): no light round the panel");
        } else {
            spdlog::info("[VR] Ambient light: its shaders made");
        }
    }
    spdlog::info("[VR] Menu panel: drawing the menu into the eye pictures {}", m_panel_ray_batch != nullptr ? "ready" : "not available (its own panel is used)");
    spdlog::info("[VR] Menu panel: its own pixel shaders made (the menu's 3D pictures cut out: {}, by their depth: {}; the dark backing under faint "
                 "buttons: {})",
        m_keyed_batch != nullptr ? "yes" : "no", m_depth_keyed_batch != nullptr ? "yes" : "no", m_backing_batch != nullptr ? "yes" : "no");
}

int D3D12Component::record_live_picture(ID3D12GraphicsCommandList* command_list, ID3D12Resource* picture, D3D12_RESOURCE_STATES state, int kind,
    uint32_t frame, bool eye_sized) {
    if (command_list == nullptr || picture == nullptr || kind < 0 || kind >= (int)m_live.slots.size()) {
        return -1;
    }
    // (On the main kind of command list only: a compute list cannot change a render target's state.)
    if (command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return 0;
    }
    const auto desc = picture->GetDesc();
    // (A plain 2D picture of one level, as large as a view's picture is.)
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width < 640 || desc.Height < 360) {
        return 0;
    }
    // The views' last steps draw in 8 or 10 bits a colour (each copied into its typed kind: a copy within one family of formats).
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        typed = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        typed = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        typed = DXGI_FORMAT_R10G10B10A2_UNORM;
        break;
    default:
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& L = m_live;
    std::scoped_lock _{L.mtx};
    // (fix147) Full stereo's left eye: only a picture of the eye image's size, the game's finished picture (as the right eye's). The
    // world's view's last steps draw two of a colour kept here: HDRComposite's, 1996x2148 (two rows short), then the sharpening's
    // (ContrastAdaptiveSharpening) finished one, 1996x2150. When LiveWorld took every picture of a frame (its finished step to be found
    // again), HDRComposite's made its slot at 2148, and then every finished picture of that frame's parity was refused for half a
    // second (the wait between remakes below): no left picture on every other frame, a quarter of them (fix146's log). Another size:
    // neither copied nor its slot made again.
    const uint32_t eye_w = m_backbuffer_size[0], eye_h = m_backbuffer_size[1]; // (set on the present thread at a reset: read as they are)
    // (fix148) Not refused for those two rows any more: in space the sharpening step is a new object every frame in full stereo, never
    // known in time, and HDRComposite's picture was the only one to come (fix147's test: no left picture at all, full stereo stopping
    // itself). The left eye's copy is made at the eye image's size, once, and any picture within 64 pixels of it is copied into it (its
    // top left part; two rows of 2150 left as the frame before's); the frame's later one (the sharpened) over the earlier.
    if (eye_sized && (eye_w == 0 || eye_h == 0 || std::abs((int64_t)desc.Width - (int64_t)eye_w) > 64 ||
                         std::abs((int64_t)desc.Height - (int64_t)eye_h) > 64)) {
        ++L.eye_size_refused;
        if (L.eye_size_refused_logs < 6) {
            ++L.eye_size_refused_logs;
            spdlog::info("[VR] full stereo: a picture of the world's view {}x{} (format {}) not taken for the left eye: far from the eye image's size ({}x{})",
                (uint64_t)desc.Width, desc.Height, (int)desc.Format, eye_w, eye_h);
        }
        return 0;
    }
    const uint32_t want_w = eye_sized ? eye_w : (uint32_t)desc.Width;
    const uint32_t want_h = eye_sized ? eye_h : desc.Height;
    auto& slot = L.slots[kind][frame & 1];
    if (slot.copy == nullptr || slot.width != want_w || slot.height != want_h || slot.format != typed) {
        // (A new one: a picture's worth of video memory; not again and again when the size keeps changing, or making it failed.)
        // (fix147) Full stereo's left eye, its slot not the eye image's size yet: made at once (its size can only be that one, so it
        // cannot go back and forth); only a try that failed waits half a second. Else (the menu panel's pictures, whose size may
        // change with the menu's box; a change of format): as before.
        const bool to_eye_size = eye_sized && (slot.copy == nullptr || slot.width != want_w || slot.height != want_h);
        const auto since = to_eye_size ? slot.failed_at : slot.made_at;
        if (since != std::chrono::steady_clock::time_point{} && now - since < std::chrono::milliseconds(500)) {
            return 0;
        }
        slot.made_at = now;
        auto device = g_framework->get_d3d12_hook()->get_device();
        if (device == nullptr) {
            slot.failed_at = now;
            return 0;
        }
        D3D12_RESOURCE_DESC copy_desc{};
        copy_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        copy_desc.Width = want_w;
        copy_desc.Height = want_h;
        copy_desc.DepthOrArraySize = 1;
        copy_desc.MipLevels = 1;
        copy_desc.Format = typed;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // (its views are made with the texture helper, which makes both)
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> made{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&made)))) {
            slot.failed_at = now;
            return -1;
        }
        made->SetName(kind == 0 ? L"Menu panel: the live world" : L"Menu panel: a menu's 3D picture");
        if (eye_sized && L.eye_slot_logs < 8) {
            ++L.eye_slot_logs;
            spdlog::info("[VR] the world's copy for the frames of parity {} made at the eye's size, {}x{} (full stereo's left eye, or behind a menu: fix162; "
                         "format {}; it was {}x{}; the picture {}x{})",
                frame & 1, want_w, want_h, (int)desc.Format, slot.width, slot.height, (uint64_t)desc.Width, desc.Height);
        }
        if (slot.copy != nullptr) {
            L.retired.push_back(RetiredCopy{slot.copy, now, presents_now()});
        }
        slot.copy = made;
        slot.width = want_w;
        slot.height = want_h;
        slot.format = typed;
        slot.has = false;
        slot.copies = 0;
        if (!L.logged[kind]) {
            L.logged[kind] = true;
            spdlog::info("[VR] Menu panel: {} is {}x{} (format {}, in state {:#x} after the step that draws it): copied each frame", kind == 0
                ? std::string{"the live world's picture"} : std::format("the menu's 3D picture {}", kind), (uint64_t)desc.Width, desc.Height,
                (int)desc.Format, (uint32_t)state);
        }
    }
    // (Copies replaced a while ago: no longer in use on the GPU.)
    std::erase_if(L.retired, [&](const auto& r) { return copy_done(r, now); });

    // Its first level only (the state changed for that one alone), into the copy; the picture back as it was.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = picture;
        barriers[count].Transition.Subresource = 0;
        barriers[count].Transition.StateBefore = state;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++count;
    }
    barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[count].Transition.pResource = slot.copy.Get();
    barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ++count;
    command_list->ResourceBarrier(count, barriers);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = slot.copy.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = picture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    // (fix148) As much of it as both have: the same size but for full stereo's left eye, whose copy is the eye image's size.
    const D3D12_BOX box{0, 0, 0, std::min<UINT>((UINT)desc.Width, slot.width), std::min<UINT>(desc.Height, slot.height), 1};
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    for (UINT i = 0; i < count; ++i) {
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    }
    command_list->ResourceBarrier(count, barriers);
    slot.frame = frame;
    slot.has = true;
    slot.copied_at = now;
    ++slot.copies;
    L.last_copy = RetiredCopy{nullptr, now, presents_now()}; // (the game's last copy into them: release_live_pictures waits past it)
    if (kind != 0) {
        L.last_menu_copy = L.last_copy; // (fix163)
    }
    return 1;
}

int D3D12Component::record_live_depth(ID3D12GraphicsCommandList* command_list, ID3D12Resource* depth, int kind, uint32_t frame) {
    if (command_list == nullptr || depth == nullptr || kind < 1 || kind >= (int)m_live.depth.size()) {
        return -1;
    }
    if (command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return 0;
    }
    const auto desc = depth->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width < 640 || desc.Height < 360 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return 0;
    }
    // (A depth picture's formats a shader may read (the typeless ones: a typed depth format has no view a shader reads), and the one
    // its depth is read as: the depth part only. The game's is R32G8X24.)
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R24G8_TYPELESS:
        view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case DXGI_FORMAT_R16_TYPELESS:
        view = DXGI_FORMAT_R16_UNORM;
        break;
    default:
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& L = m_live;
    std::scoped_lock _{L.mtx};
    auto& slot = L.depth[kind][frame & 1];
    bool fresh = false;
    if (slot.copy == nullptr || slot.width != (uint32_t)desc.Width || slot.height != desc.Height || slot.format != desc.Format) {
        if (slot.made_at != std::chrono::steady_clock::time_point{} && now - slot.made_at < std::chrono::milliseconds(500)) {
            return 0;
        }
        slot.made_at = now;
        auto device = g_framework->get_d3d12_hook()->get_device();
        if (device == nullptr) {
            return 0;
        }
        // (The same kind of picture, so the whole of it can be copied as it is: a depth picture too, readable by a shader.)
        D3D12_RESOURCE_DESC copy_desc = desc;
        copy_desc.Alignment = 0;
        copy_desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> made{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&made)))) {
            return -1;
        }
        made->SetName(L"Menu panel: a menu's 3D picture's depth");
        if (slot.copy != nullptr) {
            L.retired.push_back(RetiredCopy{slot.copy, now, presents_now()});
        }
        slot.copy = made;
        slot.format = desc.Format;
        slot.view_format = view;
        slot.width = (uint32_t)desc.Width;
        slot.height = desc.Height;
        slot.has = false;
        slot.copies = 0;
        fresh = true;
        if (!L.depth_logged[kind]) {
            L.depth_logged[kind] = true;
            spdlog::info("[VR] Menu panel: the menu's 3D picture {}'s depth is {}x{} (format {}, flags {:#x}): copied each frame, the picture cut out by it",
                kind, (uint64_t)desc.Width, desc.Height, (int)desc.Format, (uint32_t)desc.Flags);
        }
    }
    std::erase_if(L.retired, [&](const auto& r) { return copy_done(r, now); });

    // (The game's picture is in the copy-source state here, as the step that just copied from it left it: read as it is. The copy
    // goes from where it waits to be read, into the copy-destination state and back.)
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = slot.copy.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!fresh) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list->ResourceBarrier(1, &barrier);
    }
    command_list->CopyResource(slot.copy.Get(), depth);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &barrier);
    slot.frame = frame;
    slot.has = true;
    ++slot.copies;
    L.last_copy      = RetiredCopy{nullptr, now, presents_now()};
    L.last_menu_copy = L.last_copy; // (fix163: the menus' 3D views only)
    return 1;
}

d3d12::TextureContext* D3D12Component::live_source(int kind, uint32_t frame, bool previous, uint32_t* width, uint32_t* height, uint32_t* used_frame) {
    if (kind < 0 || kind >= (int)m_live.slots.size()) {
        return nullptr;
    }
    ComPtr<ID3D12Resource> copy{};
    DXGI_FORMAT format{};
    uint32_t w = 0, h = 0;
    int parity = -1;
    {
        std::scoped_lock _{m_live.mtx};
        for (int back = 0; back < (previous ? 2 : 1) && parity < 0; ++back) {
            const uint32_t f = frame - (uint32_t)back;
            const auto& slot = m_live.slots[kind][f & 1];
            if (slot.has && slot.copy != nullptr && slot.frame == f) {
                copy = slot.copy;
                format = slot.format;
                w = slot.width;
                h = slot.height;
                parity = (int)(f & 1);
                if (used_frame != nullptr) {
                    *used_frame = f;
                }
            }
        }
    }
    if (parity < 0) {
        return nullptr;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return nullptr;
    }
    // (The caller has waited for the last commands that used its views: the world's, the live eye image's; the 3D pictures', the
    // panel's see-through picture's.)
    auto& source = m_live.source[kind][parity];
    if (m_live.source_of[kind][parity] != copy.Get()) {
        m_live.source_of[kind][parity] = nullptr;
        if (!source.setup(device, copy.Get(), format, format, L"Menu panel: a live picture's views")) {
            source.reset();
            return nullptr;
        }
        m_live.source_of[kind][parity] = copy.Get();
    }
    if (width != nullptr) {
        *width = w;
    }
    if (height != nullptr) {
        *height = h;
    }
    return &source;
}

bool D3D12Component::present_live_world(VR* vr, uint32_t frame, bool mono, bool left) {
    if (m_sprite_batch == nullptr || m_backbuffer_size[0] == 0 || m_backbuffer_size[1] == 0) {
        return false;
    }
    auto& L = m_live;
    auto& eye = L.eye[mono || left ? 0 : 1];
    // (The last picture's commands done before anything they use is made again: the world's views.)
    if (eye.texture != nullptr) {
        eye.commands.wait(INFINITE);
    }
    // (Mono: one picture for both eyes, so the frame before's will do when this one's is not there.)
    uint32_t used = 0;
    auto*    src  = live_source(0, frame, mono, nullptr, nullptr, &used);
    ++(src == nullptr ? L.world_missing : (used == frame ? L.world_this : L.world_previous));
    if (src == nullptr) {
        // (For the log, the first few times: the world's copies there are, by the frames they were made in.)
        static int logged = 0;
        if (logged < 6) {
            std::scoped_lock _{L.mtx};
            const auto& a = L.slots[0][0];
            const auto& b = L.slots[0][1];
            // (Only copies made just now, this menu's: older ones say nothing about this frame.)
            const auto now = std::chrono::steady_clock::now();
            const auto recent = [&](const LivePictures::Slot& slot) {
                return slot.has && now - slot.copied_at < std::chrono::milliseconds(250);
            };
            if (recent(a) || recent(b)) {
                ++logged;
                const auto newest = recent(a) && (!recent(b) || (int32_t)(a.frame - b.frame) > 0) ? a.frame : b.frame;
                spdlog::info("[VR] Menu panel: no live world picture made in frame {} (the copies are of frames {} and {}: the newest {} by {})", frame,
                    a.has ? std::to_string(a.frame) : std::string{"-"}, b.has ? std::to_string(b.frame) : std::string{"-"},
                    (int32_t)(newest - frame) > 0 ? "ahead" : "behind", std::abs((int32_t)(newest - frame)));
            }
        }
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    // The eye images are the size of the game's picture (the backbuffer); the world's picture may be smaller (the game draws its views
    // in a menu's 16:9 box while one is open): drawn over the whole eye image (it shows the eye's whole field of view, the camera's).
    if (L.eye[0].texture == nullptr || L.eye[1].texture == nullptr || L.eye_size[0] != m_backbuffer_size[0] || L.eye_size[1] != m_backbuffer_size[1]) {
        // (Not in use any more: their last draws, and the eye images' copies from them, past.) (fix181) Not waited for: the old ones kept
        // until the graphics card is past them (none the first time: nothing to wait for then, as every menu's first frame did for all copies).
        RetiredGpu retired{.what = "the live world's eye images (made again)"};
        if (L.eye[0].texture != nullptr || L.eye[1].texture != nullptr) {
            note_copy_fences(retired);
        }
        for (auto& e : L.eye) {
            retire_texture(retired, e);
        }
        retire(std::move(retired));
        L.last_good_valid = false;
        L.eye_size[0] = L.eye_size[1] = 0;
        L.eye_has[0] = L.eye_has[1] = false;
        for (auto& e : L.eye) {
            e.reset();
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = m_backbuffer_size[0];
            d.Height = m_backbuffer_size[1];
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // (as the converted eye image: what the swapchains are made like)
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> made{};
            // (Kept in the present state between frames, as the converted eye image.)
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&made))) ||
                !e.setup(device, made.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, L"Menu panel: the live world, eye-sized")) {
                spdlog::error("[VR] Menu panel: could not make the live world's eye images ({}x{})", m_backbuffer_size[0], m_backbuffer_size[1]);
                for (auto& f : L.eye) {
                    f.reset();
                }
                return false;
            }
        }
        L.eye_size[0] = m_backbuffer_size[0];
        L.eye_size[1] = m_backbuffer_size[1];
    }
    auto& commands = eye.commands;
    commands.wait(INFINITE);
    // (Cleared first: the picture's own alpha is not how much it covers; drawn over black it is the picture alone.)
    const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    commands.clear_rtv(eye, black, D3D12_RESOURCE_STATE_PRESENT);
    render_srv_to_rtv(commands.cmd_list.Get(), *src, eye, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
    // The menu drawn into it, where it is in the room (else the panel is a layer of its own, OpenXR::end_frame).
    const bool menu_in = draw_menu_into(commands.cmd_list.Get(), eye, vr, mono, left);
    commands.execute();
    const int which = mono || left ? 0 : 1;
    L.eye_menu[which] = menu_in;
    if (mono) {
        L.eye_menu[1] = menu_in;
    }
    L.eye_frame[which] = frame;
    L.eye_has[which] = true;
    if (mono) {
        return m_openxr.copy(0, eye.texture.Get(), picture_frustum(true, 0)) && m_openxr.copy(1, eye.texture.Get(), picture_frustum(true, 0));
    }
    return m_openxr.copy(left ? 0 : 1, eye.texture.Get(), picture_frustum(false, left ? 0 : 1));
}

// Full stereo: the left eye's picture of `frame`, the world's view as its last step drew it (LiveWorld copies it then, before the right
// eye's view, drawn after it, writes over the same picture), at the eye image's size, with the game's interface over it as the game
// lays it over its own picture (the layer copied at its composite this frame, MenuLayer): the HUD in both eyes. Into the left eye's
// image. False: that frame's world picture is not there (its copy not made).
bool D3D12Component::present_native_left(uint32_t frame) {
    auto& N = m_native;
    if (m_sprite_batch == nullptr || m_backbuffer_size[0] == 0 || m_backbuffer_size[1] == 0) {
        return false;
    }
    // (The last pictures' commands done before anything they use is made again: this one's, and the panel's, which read the same
    // views of the world's copies and of the interface layer, just after a menu closed.)
    if (N.left.texture != nullptr) {
        N.left.commands.wait(INFINITE);
    }
    for (auto& e : m_live.eye) {
        if (e.texture != nullptr) {
            e.commands.wait(INFINITE);
        }
    }
    if (m_menu_layer.composed.texture != nullptr) {
        m_menu_layer.composed.commands.wait(INFINITE);
    }
    auto* src = live_source(0, frame, false);
    if (src == nullptr) {
        ++N.missing;
        if (N.missing_logs < 6) {
            ++N.missing_logs;
            std::scoped_lock _{m_live.mtx};
            const auto& a = m_live.slots[0][0];
            const auto& b = m_live.slots[0][1];
            // (fix147) And the copies' sizes, against the eye image's.
            spdlog::info("[VR] full stereo: no left eye picture made in frame {} (the copies are of frames {} and {}, {}x{} and {}x{}; the eye image "
                         "{}x{}): the left eye's last picture shown again if recent, else the game's picture to both eyes",
                frame, a.has ? std::to_string(a.frame) : std::string{"-"}, b.has ? std::to_string(b.frame) : std::string{"-"}, a.width, a.height,
                b.width, b.height, m_backbuffer_size[0], m_backbuffer_size[1]);
        }
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    // (fix147) The left eye's image drawn into (or made anew) from here: no longer the last picture kept, until this one is in.
    N.kept_valid = false;
    if (N.left.texture == nullptr || N.size[0] != m_backbuffer_size[0] || N.size[1] != m_backbuffer_size[1]) {
        // (fix181) The old image, if any, kept until the graphics card is past it and its copies: not waited for (with none, as each time
        // full stereo came back after a menu, nothing to wait for).
        RetiredGpu retired{.what = "full stereo's left eye image (made again)"};
        if (N.left.texture != nullptr) {
            note_copy_fences(retired);
        }
        retire_texture(retired, N.left);
        retire(std::move(retired));
        N.size[0] = N.size[1] = 0;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = m_backbuffer_size[0];
        d.Height = m_backbuffer_size[1];
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // (as the converted eye image: what the swapchains are made like)
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> made{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&made))) ||
            !N.left.setup(device, made.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, L"Full stereo: the left eye")) {
            spdlog::error("[VR] full stereo: could not make the left eye's image ({}x{})", m_backbuffer_size[0], m_backbuffer_size[1]);
            N.left.reset();
            return false;
        }
        N.size[0] = m_backbuffer_size[0];
        N.size[1] = m_backbuffer_size[1];
    }
    auto& commands = N.left.commands;
    commands.wait(INFINITE);
    // (Over black: the picture's own alpha is not how much it covers.)
    const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    commands.clear_rtv(N.left, black, D3D12_RESOURCE_STATE_PRESENT);
    render_srv_to_rtv(commands.cmd_list.Get(), *src, N.left, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
    // The interface: its layer, copied in the last moments (this frame's), laid in the middle of the picture at its own size (the
    // whole picture in play), its premultiplied alpha blended over the world as on the panel.
    bool hud = false;
    {
        auto& L = m_menu_layer;
        ComPtr<ID3D12Resource> copy{};
        DXGI_FORMAT format{};
        uint32_t width = 0, height = 0;
        int slot = -1;
        std::chrono::steady_clock::time_point at{};
        {
            std::scoped_lock _{L.mtx};
            slot = L.current;
            if (slot >= 0) {
                copy = L.slots[slot].copy;
                format = L.slots[slot].format;
                width = L.slots[slot].width;
                height = L.slots[slot].height;
            }
            at = L.at;
        }
        if (slot >= 0 && copy != nullptr && std::chrono::steady_clock::now() - at < std::chrono::milliseconds(100) && width <= N.size[0] &&
            height <= N.size[1]) {
            auto& source = L.source[slot];
            bool ready = L.source_of[slot] == copy.Get();
            if (!ready) {
                L.source_of[slot] = nullptr;
                ready = source.setup(device, copy.Get(), format, format, L"Full stereo: the interface layer's views");
                if (ready) {
                    L.source_of[slot] = copy.Get();
                } else {
                    source.reset();
                }
            }
            if (ready) {
                // (Moved right by the HUD's stereo shift, as taking turns gave it to the left eye: the HUD a little nearer than far
                // away; the game draws the right eye's unmoved. And, each eye drawn with its own lens (fix138), from the right eye's
                // straight ahead, where the game put it, to the left eye's.)
                const float centre = std::clamp(VR::get()->m_native_hud_centre.load(), -0.5f, 0.5f);
                const LONG  x0     = (LONG)((N.size[0] - width) / 2) + (LONG)std::clamp(VR::get()->m_native_hud_shift.load(), -400, 400) +
                                (LONG)std::lround(centre * (float)width);
                const LONG y0 = (LONG)((N.size[1] - height) / 2);
                const RECT dest{x0, y0, x0 + (LONG)width, y0 + (LONG)height};
                render_srv_to_rtv(commands.cmd_list.Get(), source, N.left, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, &dest);
                hud = true;
            }
        }
    }
    commands.execute();
    ++N.shown;
    if (hud) {
        ++N.with_hud;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!N.logged) {
        N.logged = true;
        N.first_at = now;
        N.stats_at = now;
        spdlog::info("[VR] full stereo: the left eye made from the world's view ({}x{}{})", m_backbuffer_size[0], m_backbuffer_size[1],
            hud ? ", the interface laid over it" : "; no interface layer copied yet");
    }
    if (now - N.stats_at >= std::chrono::seconds(10)) {
        const double seconds = std::chrono::duration<double>(now - N.stats_at).count();
        N.stats_at = now;
        uint64_t used = 0, budget = 0;
        const bool known = video_memory(used, budget);
        uint64_t refused = 0;
        {
            std::scoped_lock _{m_live.mtx};
            refused = m_live.eye_size_refused;
        }
        // (fix147) The left picture not there: how often each stand-in was shown (the left eye's last picture with its own pose; the
        // game's picture to both eyes from the head's middle), and the world's pictures refused for not being the eye image's size.
        spdlog::info("[VR] full stereo: {:.1f} frames a second, each eye a new picture in each; left eye pictures {} (with the interface {}), "
                     "frames with the left eye's last picture again (its own pose) {}, "
                     "frames with one picture for both eyes {}, "
                     "not there {}; pictures of another size refused for the left eye {}; video memory {}",
            (double)(N.shown - N.shown_at_stats) / std::max(seconds, 0.001), N.shown, N.with_hud, N.previous_left, N.one_picture, N.missing, refused,
            known ? std::format("{} of {} MB", used >> 20, budget >> 20) : std::string{"not known"});
        N.shown_at_stats = N.shown;
    }
    if (!m_openxr.copy(0, N.left.texture.Get(), picture_frustum(false, 0), D3D12_RESOURCE_STATE_PRESENT, OpenXR::Beam{vrmod::LaserSight::get().beam_ready(frame), frame, false, 0.0f})) {
        return false;
    }
    // (fix147) The picture kept, with the pose its frame's left eye was drawn and submitted with: shown again if the next frame's left
    // picture is not there (present_native_previous_left). Its pose not known: not kept.
    runtimes::OpenXR::FramePoses fp{};
    const auto& vr = VR::get();
    if (vr != nullptr && vr->m_openxr != nullptr && vr->m_openxr->frame_poses_of(frame, fp)) {
        N.kept_valid = true;
        N.kept_frame = frame;
        N.kept_at = now;
        N.kept_pose = fp.stage[0];
    }
    return true;
}

bool D3D12Component::present_native_previous_left(VR* vr, uint32_t frame) {
    auto& N = m_native;
    const auto& xr = vr->m_openxr;
    if (xr == nullptr || !N.kept_valid || N.left.texture == nullptr || N.size[0] != m_backbuffer_size[0] || N.size[1] != m_backbuffer_size[1]) {
        return false;
    }
    // (Recent only: a frame or three before this one, within 100 ms. Older, the two eyes would show the world too far apart in time.)
    const uint32_t age = frame - N.kept_frame;
    if (age == 0 || age > 3 || std::chrono::steady_clock::now() - N.kept_at > std::chrono::milliseconds(100)) {
        return false;
    }
    // (This frame's poses known: the right eye's picture submitted with its own, as the left's is with its own.)
    runtimes::OpenXR::FramePoses fp{};
    if (!xr->frame_poses_of(frame, fp)) {
        return false;
    }
    if (!m_openxr.copy(0, N.left.texture.Get(), picture_frustum(false, 0), D3D12_RESOURCE_STATE_PRESENT, OpenXR::Beam{vrmod::LaserSight::get().beam_ready(N.kept_frame) && vrmod::LaserSight::get().beam_ready(frame), N.kept_frame, false, 0.0f})) {
        return false;
    }
    // Each eye's picture with the pose it was drawn with, as AER submits the eye shown again: the headset turns the older left picture
    // to where the head is now. (The pipelined poses below are not set over these: submit_valid.)
    xr->mono_views     = false;
    xr->submit_valid   = true;
    xr->submit_pose[0] = N.kept_pose;
    // (Not pipelined: the right eye as every other frame submits it, the latest located, not switching its pose's source frame to frame;
    // the review.)
    xr->submit_pose[1] = !vr->pipelined() && xr->stage_views.size() == 2 ? xr->stage_views[1].pose : fp.stage[1];
    xr->submit_head    = fp.head;
    ++N.previous_left;
    return true;
}

// The menu drawn into an eye picture (the live world's, in the render target state's neighbour: present), over the world: where each
// pixel's line of sight meets the panel in the room, the panel's picture there (the last one made; see-through), and the pointer's
// dot. The pose and field of view used are submitted with the picture (OpenXR::menu_draw_params). False: not drawn.
bool D3D12Component::draw_menu_into(ID3D12GraphicsCommandList* list, d3d12::TextureContext& eye, VR* vr, bool mono, bool left) {
    auto& xr = vr->m_openxr;
    auto& composed = m_menu_layer.composed;
    if (m_panel_ray_batch == nullptr || xr == nullptr || !xr->panel_draw_in.load() || !m_menu_layer.was_used || composed.texture == nullptr ||
        eye.texture == nullptr || list == nullptr) {
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    // (Views of the panel's picture, made again when it is made anew: the eyes' last draws with the old ones done first.)
    if (m_panel_ray_source_of != composed.texture.Get()) {
        for (auto& e : m_live.eye) {
            if (e.texture != nullptr && &e != &eye) {
                e.commands.wait(INFINITE);
            }
        }
        m_panel_ray_source.reset();
        m_panel_ray_source_of = nullptr;
        const auto desc = composed.texture->GetDesc();
        if (!m_panel_ray_source.setup(device, composed.texture.Get(), desc.Format, desc.Format, L"Menu panel: its picture, drawn into the eyes")) {
            m_panel_ray_source.reset();
            return false;
        }
        m_panel_ray_source_of = composed.texture.Get();
    }
    float params[16]{}, panel[4]{}, dot[4]{};
    if (!xr->menu_draw_params(mono || left ? 0 : 1, mono, params, panel, dot)) {
        return false;
    }
    const auto dst = eye.texture->GetDesc();
    const auto src = composed.texture->GetDesc();
    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)dst.Width, (float)dst.Height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH};
    D3D12_RECT scissor{0, 0, (LONG)dst.Width, (LONG)dst.Height};
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = eye.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[]{eye.get_rtv()};
    list->OMSetRenderTargets(1, rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    m_panel_ray_batch->SetViewport(viewport);
    const DirectX::XMMATRIX numbers{params};
    m_panel_ray_batch->Begin(list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate, numbers);
    ID3D12DescriptorHeap* heaps[]{m_panel_ray_source.srv_heap->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    const RECT whole{0, 0, (LONG)dst.Width, (LONG)dst.Height};
    const DirectX::XMUINT2 size{(uint32_t)src.Width, (uint32_t)src.Height};
    m_panel_ray_batch->Draw(m_panel_ray_source.get_srv_gpu(), size, whole, DirectX::XMVectorSet(panel[0], panel[1], 0.0f, 0.0f));
    if (dot[3] > 0.5f) {
        m_panel_ray_batch->Draw(m_panel_ray_source.get_srv_gpu(), size, whole, DirectX::XMVectorSet(dot[0], dot[1], dot[2], 1.0f));
    }
    m_panel_ray_batch->End();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    return true;
}

uint32_t D3D12Component::presents_now() {
    auto& vr = VR::get();
    return vr != nullptr ? vr->m_presents_done.load(std::memory_order_relaxed) : 0;
}

// (Let go 1.5 s and 8 presents after its last use: the CPU cannot be that many frames ahead of the GPU, even through long stalls
// (fix127's review: on a card out of video memory a frame can take seconds). After 10 s whatever happened: the GPU long done.)
bool D3D12Component::copy_done(const RetiredCopy& r, std::chrono::steady_clock::time_point now) {
    const auto age = now - r.at;
    return (age > std::chrono::milliseconds(1500) && presents_now() - r.presents >= 8) || age > std::chrono::seconds(10);
}

void D3D12Component::note_fence(RetiredGpu& r, d3d12::CommandContext& c) {
    std::scoped_lock _{c.mtx};
    // (Its last commands given to the graphics card: the fence's value they signal. Recorded and not given: never run.)
    if (c.fence != nullptr && c.fence_value != 0) {
        r.fences.emplace_back(c.fence, c.fence_value);
    }
}

void D3D12Component::retire_texture(RetiredGpu& r, d3d12::TextureContext& t) {
    {
        auto& c = t.commands;
        std::scoped_lock _{c.mtx};
        if (c.fence != nullptr && c.fence_value != 0) {
            r.fences.emplace_back(c.fence, c.fence_value);
        }
        // (Its command list and allocator kept too: the graphics card may still be running them.)
        if (c.cmd_allocator != nullptr) {
            r.objects.emplace_back(c.cmd_allocator);
        }
        if (c.cmd_list != nullptr) {
            r.objects.emplace_back(c.cmd_list);
        }
        if (c.fence != nullptr) {
            r.objects.emplace_back(c.fence);
        }
        if (c.fence_event != nullptr) {
            r.events.push_back(c.fence_event);
        }
        c.cmd_allocator.Reset();
        c.cmd_list.Reset();
        c.fence.Reset();
        c.fence_value       = 0;
        c.fence_event       = nullptr;
        c.waiting_for_fence = false;
        c.has_commands      = false;
    }
    if (t.texture != nullptr) {
        r.objects.emplace_back(t.texture);
    }
    t.texture.Reset();
    if (t.rtv_heap != nullptr) {
        r.heaps.push_back(std::move(t.rtv_heap));
    }
    if (t.srv_heap != nullptr) {
        r.heaps.push_back(std::move(t.srv_heap));
    }
}

void D3D12Component::note_copy_fences(RetiredGpu& r) {
    std::scoped_lock _{m_openxr.mtx};
    for (auto& ctx : m_openxr.contexts) {
        for (auto& texture_ctx : ctx.texture_contexts) {
            if (texture_ctx != nullptr) {
                note_fence(r, texture_ctx->commands);
            }
        }
    }
    for (auto& texture_ctx : m_openxr.panel_context.texture_contexts) {
        if (texture_ctx != nullptr) {
            note_fence(r, texture_ctx->commands);
        }
    }
    // (The world-history copies read full stereo's left eye too: the review.)
    for (auto& ctx : m_openvr.left_eye_tex) {
        note_fence(r, ctx.commands);
    }
    for (auto& ctx : m_openvr.right_eye_tex) {
        note_fence(r, ctx.commands);
    }
}

void D3D12Component::retire(RetiredGpu&& r) {
    if (r.objects.empty() && r.heaps.empty() && r.events.empty()) {
        return;
    }
    r.at = std::chrono::steady_clock::now();
    // (fix181, the review) The mod's own fence signalled on the queue now: it passes once everything given to the queue before it has run
    // (whichever of the mod's lists read these pictures: the headset copies, the world-history copies, the panel's, the eye images').
    auto& hook  = g_framework->get_d3d12_hook();
    auto* queue = hook != nullptr ? hook->get_command_queue() : nullptr;
    if (m_retire_fence == nullptr && hook != nullptr && hook->get_device() != nullptr) {
        if (FAILED(hook->get_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_retire_fence)))) {
            m_retire_fence.Reset();
        }
    }
    if (queue != nullptr && m_retire_fence != nullptr && SUCCEEDED(queue->Signal(m_retire_fence.Get(), m_retire_value + 1))) {
        r.queue_value = ++m_retire_value;
    }
    if (m_retired_logs < 40) {
        ++m_retired_logs;
        spdlog::info("[VR] (fix181) {} let go without waiting for the graphics card: freed once it has passed {} (checked each present)", r.what,
            r.queue_value != 0 ? std::string{"the mod's fence on the queue"} : std::format("{} command list fence(s)", r.fences.size()));
    }
    m_retired_gpu.push_back(std::move(r));
}

bool D3D12Component::retired_passed(const RetiredGpu& r) const {
    // (A lost device reports every fence passed: UINT64_MAX.)
    if (r.queue_value != 0) {
        return m_retire_fence != nullptr && m_retire_fence->GetCompletedValue() >= r.queue_value;
    }
    for (const auto& [fence, value] : r.fences) {
        if (fence->GetCompletedValue() < value) {
            return false;
        }
    }
    return true;
}

void D3D12Component::free_retired(bool drain) {
    if (m_retired_gpu.empty()) {
        return;
    }
    // (drain: before the drawing tools the retired lists were recorded with are made again, setup and on_reset: the graphics card waited
    // for, up to 5 s, past the newest one's point.)
    if (drain && m_retire_fence != nullptr && m_retire_value != 0 && m_retire_fence->GetCompletedValue() < m_retire_value) {
        if (HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr); event != nullptr) {
            if (FAILED(m_retire_fence->SetEventOnCompletion(m_retire_value, event))) {
                CloseHandle(event);
            } else if (WaitForSingleObject(event, 5000) == WAIT_OBJECT_0) {
                CloseHandle(event);
            } else {
                // (Not passed in 5 s: the event left open, as the fence may still signal it; a closed handle's number could be another's by then.)
                spdlog::warn("[VR] (fix181) the pictures let go earlier: the graphics card not past them after 5 s, before the drawing tools are made again");
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_retired_gpu, [&](RetiredGpu& r) {
        if (!retired_passed(r)) {
            // (fix181, the review) Not freed until it has passed, however long: its lists' events may still be signalled, its objects used.
            // Logged once at 10 s.
            if (!r.told_late && now - r.at > std::chrono::seconds(10)) {
                r.told_late = true;
                spdlog::warn("[VR] (fix181) {} not freed: the graphics card has not passed it in 10 s (kept until it has)", r.what);
            }
            return false;
        }
        if (m_retired_logs < 80) {
            ++m_retired_logs;
            spdlog::info("[VR] (fix181) {} freed {} ms after it was let go{}", r.what,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - r.at).count(), drain ? " (the drawing tools made again)" : "");
        }
        // (Every fence it waited on has passed: its lists' completion events have fired, no wait is left on them.)
        for (HANDLE event : r.events) {
            CloseHandle(event);
        }
        return true; // (its objects and views released with it)
    });
}

// (fix165) Once a second (the present thread): the video memory used and its budget, kept for the VR side (the status line, the guard on
// switching to two pictures, the right eye's DLSS let go in mono); going over the budget and coming back under logged.
void D3D12Component::sample_memory() {
    static auto last = std::chrono::steady_clock::time_point{};
    const auto  now  = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) {
        return;
    }
    last = now;
    uint64_t used = 0, budget = 0;
    if (!video_memory(used, budget) || budget == 0) {
        return;
    }
    m_mem_used_mb   = static_cast<uint32_t>(used >> 20);
    m_mem_budget_mb = static_cast<uint32_t>(budget >> 20);
    m_mem_samples.fetch_add(1); // (fix189)
    static auto said = std::chrono::steady_clock::time_point{};
    if (now - said >= std::chrono::seconds(10)) { // (each 10 s, in play too: before, only behind menus)
        said = now;
        spdlog::info("[VR] (fix165) video memory: {} of {} MB{}", used >> 20, budget >> 20, used > budget ? " (OVER its budget)" : "");
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // (fix173) Back under at the budget itself, once the use has stayed at or under it 2 s (three samples a second apart: a use hovering at
    // the line, each sample over it, keeps it over). fix165 waited for 95% of the budget: play sits at 95-97% (6600-6750 of 6938 MB in the
    // test's log), so once over the guard could stay on through play, every switch to stereo or AER put back to mono.
    static int64_t under_since_ms = 0;
    const bool     over           = used > budget;
    static int     tells          = 0;
    if (over) {
        under_since_ms = 0;
        if (m_mem_over_since_ms.load() == 0) {
            m_mem_over_since_ms = now_ms;
            if (tells++ < 40) {
                spdlog::info("[VR] (fix165) video memory OVER its budget: {} of {} MB (Windows moves the game's data out to system memory: frames slow down)",
                    used >> 20, budget >> 20);
            }
        }
    } else if (m_mem_over_since_ms.load() != 0) {
        if (under_since_ms == 0) {
            under_since_ms = now_ms;
        }
        if (now_ms - under_since_ms >= 1500) {
            if (tells++ < 40) {
                spdlog::info("[VR] (fix173) video memory back under its budget: {} of {} MB (over it {:.1f} s; under it {:.1f} s now)", used >> 20,
                    budget >> 20, (under_since_ms - m_mem_over_since_ms.load()) / 1000.0, (now_ms - under_since_ms) / 1000.0);
            }
            m_mem_over_since_ms = 0;
            under_since_ms      = 0;
        }
    }
}

bool D3D12Component::memory_over_for(int64_t ms) const {
    const int64_t since = m_mem_over_since_ms.load();
    if (since == 0) {
        return false;
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return now_ms - since >= ms;
}

bool D3D12Component::video_memory(uint64_t& used, uint64_t& budget) {
    if (m_adapter == nullptr && !m_adapter_tried) {
        m_adapter_tried = true;
        auto& hook = g_framework->get_d3d12_hook();
        auto device = hook->get_device();
        auto swapchain = hook->get_swap_chain();
        ComPtr<IDXGIFactory4> factory{};
        ComPtr<IDXGIAdapter3> adapter{};
        if (device != nullptr && swapchain != nullptr && SUCCEEDED(swapchain->GetParent(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
            m_adapter = adapter;
        }
    }
    ComPtr<IDXGIAdapter3> adapter{};
    if (m_adapter == nullptr || FAILED(m_adapter.As(&adapter))) {
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return false;
    }
    used = info.CurrentUsage;
    budget = info.Budget;
    return true;
}

void D3D12Component::note_menu_frame(bool panel_up) {
    auto& P = m_menu_perf;
    const auto now = std::chrono::steady_clock::now();
    // (The last present's time, menu or not: the frame the menu opens in measured too.)
    const auto previous = m_present_at;
    m_present_at = now;
    // (fix179) Where this present's time went (taken every present: the sums start over each time), and whether a picture was saved for the
    // log in it (its time is then the saving's).
    const FrameTimes times  = take_frame_times(now);
    const bool       saving = P.saving;
    const int64_t    now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto&            C      = m_menu_close;
    constexpr std::chrono::steady_clock::time_point kNone{};
    const auto slow_list = [](const std::vector<std::pair<int, int>>& slow) {
        std::string text{};
        for (const auto& [at, took] : slow) {
            text += std::format("{}+{} ms took {} ms", text.empty() ? "" : ", ", at, took);
        }
        return text.empty() ? std::string{"none"} : text;
    };
    if (!panel_up) {
        // (fix179) The menu's movies, as it closed (drawn in its last 1.5 s).
        const bool  closing = P.opened != kNone;
        std::string menus{};
        if (closing) {
            menus = menu_movies_since(now_ms - 1500);
        }
        // (The menu closed: its whole time, once; the video memory now as its copies are still kept, a moment longer.)
        if (P.menu_frames > 0 && P.logs++ < 200) {
            uint64_t used = 0, budget = 0;
            const bool known = video_memory(used, budget);
            P.peak_used = std::max(P.peak_used, used);
            spdlog::info("[VR] Menu panel: the menu closed after {} frames (on average {:.1f} ms each, the worst {:.1f} ms, {} over 50 ms, {} "
                         "stalls of 2 s or more; {} frames left out: pictures saved for the log); video memory at most {} MB while it was open{}; "
                         "(fix179) the menu: {}",
                P.menu_frames, P.menu_sum_ms / P.menu_frames, P.menu_worst_ms, P.menu_over_50, P.menu_stalls, P.menu_saving, P.peak_used >> 20,
                known ? std::format(" (a budget of {} MB)", budget >> 20) : std::string{}, menus);
        }
        P = MenuPerformance{.logs = P.logs};
        // (fix179) Its closing's first 3 s watched frame by frame too (the game makes DLSS again as a menu closes, the eyes go back to two
        // pictures), from the frame it closed in.
        if (closing) {
            C                      = MenuClose{.closed = now, .last = previous, .menus = menus};
            m_split.window_details = 0;
        }
        if (C.closed != kNone) {
            const int at = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - C.closed).count();
            if (C.last != kNone) {
                const double ms = std::chrono::duration<double, std::milli>(now - C.last).count();
                ++C.frames;
                if (ms > 40.0 && ms < 10000.0) {
                    if (!saving && C.slow.size() < 16) {
                        C.slow.emplace_back(at, (int)std::lround(ms));
                    }
                    log_slow_frame(std::format("after the menu closed ({})", C.menus), at, (int)std::lround(ms), saving, times);
                }
            }
            C.last = now;
            if (now - C.closed > std::chrono::seconds(3)) {
                if (m_split.close_logs++ < 200) { // (fix179: its own cap, the review)
                    const auto& vr = VR::get();
                    spdlog::info("[VR] (fix179) Menu panel: its first 3 s after closing ({}; now {}): {} frames; over 40 ms: {}", C.menus,
                        vr->m_present_mono ? "mono" : (vr->m_present_native ? "full stereo" : "stereo"), C.frames, slow_list(C.slow));
                }
                C = MenuClose{};
            }
        }
        return;
    }
    // (fix179) A menu (or a loading screen) up again within 3 s of one closing: that closing's line now, cut short.
    if (C.closed != kNone) {
        if (m_split.close_logs++ < 200) { // (fix179: its own cap, the review)
            spdlog::info("[VR] (fix179) Menu panel: after closing ({}), cut short at +{} ms by another menu or a loading screen: {} frames; over 40 "
                         "ms: {}",
                C.menus, (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - C.closed).count(), C.frames, slow_list(C.slow));
        }
        C = MenuClose{};
    }
    // (The opening of the menu: its first 3 s frame by frame, the slow ones kept for one line.)
    if (P.opened == kNone) {
        P.opened               = now;
        m_split.window_details = 0; // (fix179)
        // (The frame the menu opened in: from the last present before it.)
        if (previous != kNone) {
            const double first = std::chrono::duration<double, std::milli>(now - previous).count();
            ++P.opening_frames;
            if (first > 40.0 && first < 10000.0) {
                P.slow.emplace_back(0, (int)std::lround(first));
                // (fix179) Where its time went.
                log_slow_frame(std::format("as the menu opened ({})", menu_movies_since(now_ms - 300)), 0, (int)std::lround(first), saving,
                    times);
            }
        }
    } else if (!P.opening_logged && P.last != kNone) {
        const double ms = std::chrono::duration<double, std::milli>(now - P.last).count();
        const int    at = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - P.opened).count();
        const int64_t opened_ms = std::chrono::duration_cast<std::chrono::milliseconds>(P.opened.time_since_epoch()).count();
        ++P.opening_frames;
        if (ms > 40.0 && !P.saving && P.slow.size() < 16) {
            P.slow.emplace_back(at, (int)std::lround(ms));
        }
        // (fix179) Where a slow frame's time went (a frame a picture was saved for the log in is marked so).
        if (ms > 40.0 && ms < 10000.0) {
            log_slow_frame(std::format("as the menu opened ({})", menu_movies_since(opened_ms - 300)), at, (int)std::lround(ms), saving, times);
        }
        if (now - P.opened > std::chrono::seconds(3)) {
            P.opening_logged = true;
            if (P.logs++ < 200) {
                spdlog::info("[VR] Menu panel: its first 3 s ({}): {} frames; over 40 ms: {}; (fix179) the menu: {}",
                    VR::get()->m_present_mono ? "mono" : "stereo", P.opening_frames, slow_list(P.slow), menu_movies_since(opened_ms - 300));
            }
        }
    }
    if (P.last != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - P.last).count();
        if (P.saving) {
            ++P.menu_saving; // (a picture saved for the log in it: the time it took is the saving's, not the game's)
        } else if (ms >= 2000.0) {
            ++P.stalls;
            ++P.menu_stalls;
        } else {
            ++P.frames;
            P.sum_ms += ms;
            P.worst_ms = std::max(P.worst_ms, ms);
            P.over_50 += ms > 50.0 ? 1 : 0;
            ++P.menu_frames;
            P.menu_sum_ms += ms;
            P.menu_worst_ms = std::max(P.menu_worst_ms, ms);
            P.menu_over_50 += ms > 50.0 ? 1 : 0;
        }
    } else {
        P.window_start = now;
    }
    P.saving = false;
    P.last = now;
    // (Every 5 s while it is open: the frames, and the video memory.)
    if (now - P.window_start >= std::chrono::seconds(5) && (P.frames > 0 || P.stalls > 0)) {
        uint64_t used = 0, budget = 0;
        const bool known = video_memory(used, budget);
        P.peak_used = std::max(P.peak_used, used);
        if (P.logs++ < 200) {
            spdlog::info("[VR] Menu panel, last 5 s: {} frames (on average {:.1f} ms each, the worst {:.1f} ms, {} over 50 ms, {} stalls of 2 s or "
                         "more); video memory: {}",
                P.frames, P.frames > 0 ? P.sum_ms / P.frames : 0.0, P.worst_ms, P.over_50, P.stalls,
                known ? std::format("{} MB used of a budget of {} MB{}", used >> 20, budget >> 20,
                            used > budget ? " (OVER the budget: Windows moves pictures out of the card's memory)" : "")
                      : std::string{"not known"});
        }
        P.window_start = now;
        P.frames = 0;
        P.sum_ms = 0.0;
        P.worst_ms = 0.0;
        P.over_50 = 0;
        P.stalls = 0;
    }
}

D3D12Component::FrameTimes D3D12Component::take_frame_times(std::chrono::steady_clock::time_point now) {
    // (fix179) The time since the last present split by where the present thread spent it. This runs inside the present hook, before the
    // game's own present call: the hook's clock still has the last present's call, return and leaving, and this present's coming in. What
    // is not the game's present call, the headset's waits or the wait for the game's next picture is the present thread's own work (the
    // mod's: the menus', the pictures', ImGui's, and any wait for a lock).
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    const auto&   clock  = D3D12Hook::s_present_clock;
    FrameTimes    t{};
    t.from_us              = m_split.note_us;
    t.to_us                = now_us;
    const int64_t  enter   = clock.enter_us.load(std::memory_order_relaxed);
    const int64_t  call    = clock.call_us.load(std::memory_order_relaxed);
    const int64_t  back    = clock.back_us.load(std::memory_order_relaxed);
    const int64_t  leave   = clock.leave_us.load(std::memory_order_relaxed);
    const uint32_t enters  = clock.enters.load(std::memory_order_relaxed);
    t.presents             = enters - m_split.enters;
    if (m_split.note_us != 0) {
        if (call >= m_split.note_us && back >= call) {
            t.present_us = back - call;
        }
        if (leave >= m_split.note_us && enter >= leave) {
            t.game_us = enter - leave;
        }
    }
    const auto& xr = VR::get()->m_openxr;
    if (xr != nullptr) {
        const uint64_t wait = xr->stat_wait_total_us.load(std::memory_order_relaxed);
        const uint64_t end  = xr->stat_end_total_us.load(std::memory_order_relaxed);
        t.xr_wait_us        = (int64_t)(wait - m_split.xr_wait_us);
        t.xr_end_us         = (int64_t)(end - m_split.xr_end_us);
        m_split.xr_wait_us  = wait;
        m_split.xr_end_us   = end;
    }
    UpscalerAfrNvidiaModule::take_call_time(t.dlss_calls, t.dlss_us, t.dlss_longest_us, t.check_us, t.check_longest_us);
    t.gpu_wait_us = d3d12::CommandContext::s_waited_us.exchange(0, std::memory_order_relaxed); // (fix181)
    if (m_split.note_us != 0) {
        t.own_us = std::max<int64_t>(0, (now_us - m_split.note_us) - t.present_us - t.game_us - t.xr_wait_us - t.xr_end_us);
    }
    m_split.note_us = now_us;
    m_split.enters  = enters;
    return t;
}

void D3D12Component::log_slow_frame(const std::string& when, int at_ms, int took_ms, bool saving, const FrameTimes& t) {
    // (fix179) At most 12 lines in a menu's opening or closing, 400 in a session (a slow frame only: no cost in the others).
    if (m_split.window_details >= 12 || m_split.details >= 400) {
        return;
    }
    ++m_split.window_details;
    ++m_split.details;
    // The game thread's frames over this one (VR::on_wait_rendering keeps when its last 32 began): the longest that overlapped it, how many
    // began in it, and how long the one still running has run.
    std::string game_thread{};
    {
        const auto&    vr   = VR::get();
        const uint32_t n    = vr->m_game_frames.load(std::memory_order_acquire);
        const uint32_t size = (uint32_t)vr->m_game_frame_us.size();
        const uint32_t have = std::min(n, size);
        std::array<int64_t, 32> starts{};
        for (uint32_t i = 0; i < have && i < starts.size(); ++i) {
            starts[i] = vr->m_game_frame_us[(n - have + i) % size].load(std::memory_order_relaxed);
        }
        if (have == 0 || starts[have - 1] < t.to_us - 2'000'000) {
            game_thread = "the game thread's frames: none began in the last 2 s";
        } else {
            int64_t longest = 0;
            int     begun   = 0;
            for (uint32_t i = 0; i < have; ++i) {
                if (i + 1 < have && starts[i + 1] > t.from_us && starts[i] < t.to_us) {
                    longest = std::max(longest, starts[i + 1] - starts[i]);
                }
                begun += starts[i] >= t.from_us && starts[i] <= t.to_us ? 1 : 0;
            }
            const int64_t running = std::max<int64_t>(0, t.to_us - starts[have - 1]);
            game_thread = std::format("the game thread: its longest finished frame over it {:.1f} ms, {} frames began in it, the one running now {:.1f} "
                                      "ms so far{}",
                longest / 1000.0, begun, running / 1000.0, starts[0] > t.from_us && have == size ? " (its earlier frames not kept)" : "");
        }
    }
    uint64_t   used = 0, budget = 0;
    const bool known = video_memory(used, budget);
    spdlog::info("[VR] (fix179) Menu panel: a slow frame {}: +{} ms took {} ms{}{}: the mod's own work on the present thread {:.1f} ms ((fix181) "
                 "the mod's command lists waited for, the graphics card not done with them: {:.1f} ms), the game's "
                 "present call {:.1f} ms, the headset's frame wait {:.1f} ms and frame end {:.1f} ms, waiting for the game's next picture {:.1f} ms; "
                 "{}; Streamline's DLSS calls on all threads: {}, {:.1f} ms in all, the longest {:.1f} ms; the present thread's DLSS let-go check "
                 "(its wait for the DLSS lock) {:.1f} ms, the longest {:.1f} ms; video memory {}",
        when, at_ms, took_ms, t.presents > 1 ? std::format(" ({} presents in it: the split is the last one's)", t.presents) : std::string{},
        saving ? " (a picture saved for the log in it)" : "", t.own_us / 1000.0, t.gpu_wait_us / 1000.0, t.present_us / 1000.0, t.xr_wait_us / 1000.0,
        t.xr_end_us / 1000.0,
        t.game_us / 1000.0, game_thread, t.dlss_calls, t.dlss_us / 1000.0, t.dlss_longest_us / 1000.0, t.check_us / 1000.0,
        t.check_longest_us / 1000.0,
        known ? std::format("{} of {} MB{}", used >> 20, budget >> 20, used > budget ? " (OVER the budget)" : "") : std::string{"not known"});
}

void D3D12Component::note_menu_movie(uint32_t hash, std::string_view name) {
    // (fix179) The same movie as the last call, noted under 0.2 s ago: nothing new (this is called for every movie drawn, every frame).
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_movie_hash.load(std::memory_order_relaxed) == hash && ms - m_movie_ms.load(std::memory_order_relaxed) < 200) {
        return;
    }
    m_movie_hash.store(hash, std::memory_order_relaxed);
    m_movie_ms.store(ms, std::memory_order_relaxed);
    std::scoped_lock _{m_movies_mtx};
    MenuMovie* slot = nullptr;
    for (auto& m : m_movies) {
        if (m.hash == hash && !m.name.empty()) {
            slot = &m;
            break;
        }
    }
    if (slot == nullptr) {
        // (A new one: in place of the one drawn longest ago; its name without the "Interface/" folder.)
        slot = &*std::min_element(m_movies.begin(), m_movies.end(), [](const MenuMovie& a, const MenuMovie& b) { return a.last_ms < b.last_ms; });
        constexpr std::string_view kFolder = "Interface/";
        slot->hash = hash;
        slot->name = std::string{name.starts_with(kFolder) ? name.substr(kFolder.size()) : name};
    }
    slot->last_ms = ms;
}

std::string D3D12Component::menu_movies_since(int64_t since_ms) {
    std::scoped_lock _{m_movies_mtx};
    std::string      text{};
    for (const auto& m : m_movies) {
        if (!m.name.empty() && m.last_ms >= since_ms) {
            text += std::format("{}{}", text.empty() ? "" : ", ", m.name);
        }
    }
    return text.empty() ? std::string{"its movie not seen"} : text;
}

void D3D12Component::release_live_pictures(bool keep_world) {
    auto& L = m_live;
    // (fix181) Nothing waited for on the present thread: what goes is kept, with the fences of every command list that may still use it
    // (the eye images' own, full stereo's left eye's and the panel's see-through picture's, which read the copies' views and depth, and
    // the headset copies', which read the eye images), and freed once the graphics card has passed them all (free_retired).
    RetiredGpu retired{.what = keep_world ? "the menu's live pictures (the world's kept for full stereo)" : "the live world's pictures"};
    note_fence(retired, m_native.left.commands);
    note_fence(retired, m_menu_layer.composed.commands);
    note_copy_fences(retired);
    // (The panel's picture as drawn into the eyes: its views let go with it.)
    retire_texture(retired, m_panel_ray_source);
    m_panel_ray_source_of = nullptr;
    for (auto& e : L.eye) {
        retire_texture(retired, e);
    }
    L.eye_size[0] = L.eye_size[1] = 0;
    L.eye_has[0] = L.eye_has[1] = false;
    // (Full stereo: the world's copies and their views are its left eye's, in use each frame: kept. The other kinds let go.)
    const size_t first = keep_world ? 1 : 0;
    for (size_t k = first; k < L.source.size(); ++k) {
        for (auto& source : L.source[k]) {
            retire_texture(retired, source);
        }
        L.source_of[k] = {};
    }
    L.last_good_valid = false;
    // (The see-through picture's views of the 3D pictures and their depth: the pictures kept until its commands are past.)
    for (auto& kind : m_depth_pairs_of) {
        for (auto& pair : kind) {
            if (pair.first != nullptr) {
                retired.objects.emplace_back(pair.first);
            }
            if (pair.second != nullptr) {
                retired.objects.emplace_back(pair.second);
            }
        }
    }
    m_depth_pairs_of = {};
    retire(std::move(retired));
    // (The game's last copies into them have long run; unless a menu opened again meanwhile and its first copies were recorded while
    // the waits above ran: then they are let go as any copy is, once the GPU is surely done with them.)
    std::scoped_lock _{L.mtx};
    const auto now = std::chrono::steady_clock::now();
    const auto& last = keep_world ? L.last_menu_copy : L.last_copy; // (fix163: the world's kept, the menu's own copies only)
    const bool  done = last.at == std::chrono::steady_clock::time_point{} || copy_done(last, now);
    for (size_t k = first; k < L.slots.size(); ++k) {
        for (auto& slot : L.slots[k]) {
            if (!done && slot.copy != nullptr) {
                L.retired.push_back(RetiredCopy{slot.copy, now, presents_now()});
            }
            slot = LivePictures::Slot{};
        }
    }
    for (auto& kind : L.depth) {
        for (auto& slot : kind) {
            if (!done && slot.copy != nullptr) {
                L.retired.push_back(RetiredCopy{slot.copy, now, presents_now()});
            }
            slot = LivePictures::DepthSlot{};
        }
    }
    std::erase_if(L.retired, [&](const auto& r) { return copy_done(r, now); });
    spdlog::info("[VR] Menu panel: the live world's pictures let go (the panel has been down a few seconds)");
}

void D3D12Component::OpenXR::destroy_panel() {
    std::scoped_lock _{this->mtx};

    this->panel_context.texture_contexts.clear(); // (waits for each image's last copy)
    this->panel_context.textures.clear();
    this->panel_context.num_textures_acquired = 0;
    this->panel_resolution = {};
    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        if (openxr->panel_swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(openxr->panel_swapchain);
        }
        openxr->panel_swapchain = XR_NULL_HANDLE;
        openxr->panel_image_width = 0;
        openxr->panel_image_height = 0;
        openxr->panel_image_ready = false;
    }
}

bool D3D12Component::OpenXR::copy_ambient(ID3D12Resource* src) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;
    if (src == nullptr || openxr == nullptr || openxr->session == XR_NULL_HANDLE || openxr->frame_state.shouldRender != XR_TRUE) {
        return false;
    }
    if (!openxr->frame_began && openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
        return false;
    }
    // Its swapchain: the size of its image (a copy needs both alike), made the first time (and again with a new session), as the panel's.
    const auto src_desc = src->GetDesc();
    const auto width = (uint32_t)src_desc.Width;
    const auto height = (uint32_t)src_desc.Height;
    if (width == 0 || height == 0) {
        return false;
    }
    if (openxr->ambient_swapchain == XR_NULL_HANDLE || this->ambient_resolution[0] != width || this->ambient_resolution[1] != height) {
        if (this->ambient_failed_time != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - this->ambient_failed_time < std::chrono::seconds(3)) {
            return false; // (failed a moment ago: logged then)
        }
        this->destroy_ambient();

        const auto fail = [&](const char* what, XrResult result, XrSwapchain made) {
            static int logged = 0;
            if (logged++ < 10) {
                spdlog::error("[VR] Ambient light: {} failed ({}) for {}x{}; no light round the panel for now", what, openxr->get_result_string(result),
                    width, height);
            }
            this->ambient_context.texture_contexts.clear();
            this->ambient_context.textures.clear();
            if (made != XR_NULL_HANDLE) {
                xrDestroySwapchain(made);
            }
            this->ambient_failed_time = std::chrono::steady_clock::now();
            return false;
        };

        // (As the panel's: 8-bit sRGB images, its image's bytes copied in as they are: the colours stored as sRGB.)
        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.arraySize = 1;
        create_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        create_info.width = width;
        create_info.height = height;
        create_info.mipCount = 1;
        create_info.faceCount = 1;
        create_info.sampleCount = 1;
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain handle{XR_NULL_HANDLE};
        if (const auto result = xrCreateSwapchain(openxr->session, &create_info, &handle); result != XR_SUCCESS) {
            return fail("xrCreateSwapchain", result, XR_NULL_HANDLE);
        }
        uint32_t image_count{};
        if (const auto result = xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr); result != XR_SUCCESS || image_count == 0 || image_count > 8) {
            return fail("xrEnumerateSwapchainImages", result, handle);
        }
        auto& ctx = this->ambient_context;
        ctx.textures.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);
        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Ambient Light Commands "} + std::to_wstring(j)).c_str());
        }
        if (const auto result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)ctx.textures.data());
            result != XR_SUCCESS) {
            return fail("xrEnumerateSwapchainImages (the images)", result, handle);
        }
        ctx.num_textures_acquired = 0;
        openxr->ambient_swapchain = handle;
        openxr->ambient_image_width = (int32_t)width;
        openxr->ambient_image_height = (int32_t)height;
        this->ambient_resolution = {width, height};
        this->ambient_failed_time = {};
        spdlog::info("[VR] Ambient light: its image is {}x{} ({} images)", width, height, image_count);
    }

    // Its image copied in (it rests in the common state), as the panel's. A step failing after an image was taken: the swapchain is made
    // again next time (so no image stays taken).
    auto& ctx = this->ambient_context;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index{};
    if (const auto result = xrAcquireSwapchainImage(openxr->ambient_swapchain, &acquire_info, &index); result != XR_SUCCESS) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::error("[VR] Ambient light: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        }
        return false;
    }
    if (index >= ctx.textures.size() || index >= ctx.texture_contexts.size()) {
        spdlog::error("[VR] Ambient light: the runtime gave image {} of {}; its images are made again", index, ctx.textures.size());
        this->destroy_ambient();
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const auto result = xrWaitSwapchainImage(openxr->ambient_swapchain, &wait_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Ambient light: xrWaitSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_ambient();
        return false;
    }
    auto& texture_ctx = ctx.texture_contexts[index];
    texture_ctx->commands.wait(INFINITE);
    texture_ctx->commands.copy(src, ctx.textures[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture_ctx->commands.execute();
    if (const auto result = xrReleaseSwapchainImage(openxr->ambient_swapchain, &release_info); result != XR_SUCCESS) {
        spdlog::error("[VR] Ambient light: xrReleaseSwapchainImage failed: {}; its images are made again", openxr->get_result_string(result));
        this->destroy_ambient();
        return false;
    }
    return true;
}

void D3D12Component::OpenXR::destroy_ambient() {
    std::scoped_lock _{this->mtx};

    this->ambient_context.texture_contexts.clear(); // (waits for each image's last copy)
    this->ambient_context.textures.clear();
    this->ambient_context.num_textures_acquired = 0;
    this->ambient_resolution = {};
    auto& openxr = VR::get()->m_openxr;
    if (openxr != nullptr) {
        if (openxr->ambient_swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(openxr->ambient_swapchain);
        }
        openxr->ambient_swapchain = XR_NULL_HANDLE;
        openxr->ambient_image_width = 0;
        openxr->ambient_image_height = 0;
        openxr->ambient_ready = false;
    }
}

void D3D12Component::draw_ambient_pass(ID3D12GraphicsCommandList* list, DirectX::DX12::SpriteBatch* batch, D3D12_GPU_DESCRIPTOR_HANDLE srv,
    DirectX::XMUINT2 srv_size, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, const RECT* source, const float* color,
    const float* numbers) {
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH};
    const D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    batch->SetViewport(viewport);
    if (numbers != nullptr) {
        const DirectX::XMMATRIX carried{numbers};
        batch->Begin(list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate, carried);
    } else {
        batch->Begin(list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
    }
    ID3D12DescriptorHeap* heaps[]{m_ambient.views->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    const RECT whole{0, 0, (LONG)width, (LONG)height};
    batch->Draw(srv, srv_size, whole, source, DirectX::XMVectorSet(color[0], color[1], color[2], color[3]));
    batch->End();
}

bool D3D12Component::present_ambient(VR* vr, ID3D12Resource* source, ID3D12CommandQueue* queue) {
    auto& A = m_ambient;
    auto& xr = vr->m_openxr;
    // (Started anew, faded in from black, only after it was off over half a second: the panel drops for a frame or two during loading
    // screens, which would blink it to black and fade it in again; the review.)
    const bool starting = !A.was_on && (A.last_on_at == std::chrono::steady_clock::time_point{} ||
                                         std::chrono::steady_clock::now() - A.last_on_at > std::chrono::milliseconds(500));
    A.was_on = false; // (set again below once it is drawn and handed over)
    if (source == nullptr || queue == nullptr || xr == nullptr || A.failed) {
        return false;
    }
    ensure_custom_batches();
    if (m_ambient_down_batch == nullptr || m_ambient_smooth_batch == nullptr || m_ambient_light_batch == nullptr) {
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    // The game's picture as the panel shows it: 8-bit, its colours stored as sRGB.
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1) {
        if (!A.refused_logged) {
            A.refused_logged = true;
            spdlog::warn("[VR] Ambient light: the game's picture cannot be read for it ({}x{}, format {}): no light round the panel", (uint64_t)desc.Width,
                desc.Height, (int)desc.Format);
        }
        return false;
    }

    // Its pictures, made once (in the common state; the kept ones cleared, never read before they are written otherwise).
    if (!A.made) {
        const auto make_picture = [&](uint32_t w, uint32_t h, DXGI_FORMAT format, const wchar_t* name, ComPtr<ID3D12Resource>& out) {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out))) ||
                out == nullptr) {
                return false;
            }
            out->SetName(name);
            return true;
        };
        bool ok = make_picture(kAmbientSmallWidth, kAmbientSmallHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, L"Ambient light: now", A.now) &&
                  make_picture(kAmbientSmallWidth, kAmbientSmallHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, L"Ambient light: kept 0", A.kept[0]) &&
                  make_picture(kAmbientSmallWidth, kAmbientSmallHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, L"Ambient light: kept 1", A.kept[1]) &&
                  make_picture(kAmbientLightWidth, kAmbientLightHeight, DXGI_FORMAT_R8G8B8A8_UNORM, L"Ambient light: its image", A.light);
        if (ok) {
            try {
                A.views = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 6);
                A.targets = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 4);
            } catch (...) {
                ok = false;
            }
        }
        ok = ok && A.views != nullptr && A.targets != nullptr && A.commands.setup(L"Ambient light commands");
        if (!ok) {
            spdlog::error("[VR] Ambient light: its pictures could not be made: no light round the panel");
            release_ambient();
            A.failed = true;
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC small_view{};
        small_view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        small_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        small_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        small_view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(A.now.Get(), &small_view, A.views->GetCpuHandle(0));
        device->CreateShaderResourceView(A.kept[0].Get(), &small_view, A.views->GetCpuHandle(1));
        device->CreateShaderResourceView(A.now.Get(), &small_view, A.views->GetCpuHandle(2));
        device->CreateShaderResourceView(A.kept[1].Get(), &small_view, A.views->GetCpuHandle(3));
        device->CreateRenderTargetView(A.now.Get(), nullptr, A.targets->GetCpuHandle(0));
        device->CreateRenderTargetView(A.kept[0].Get(), nullptr, A.targets->GetCpuHandle(1));
        device->CreateRenderTargetView(A.kept[1].Get(), nullptr, A.targets->GetCpuHandle(2));
        device->CreateRenderTargetView(A.light.Get(), nullptr, A.targets->GetCpuHandle(3));
        const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
        A.commands.clear_rtv(A.now.Get(), A.targets->GetCpuHandle(0), black, D3D12_RESOURCE_STATE_COMMON);
        A.commands.clear_rtv(A.kept[0].Get(), A.targets->GetCpuHandle(1), black, D3D12_RESOURCE_STATE_COMMON);
        A.commands.clear_rtv(A.kept[1].Get(), A.targets->GetCpuHandle(2), black, D3D12_RESOURCE_STATE_COMMON);
        A.commands.clear_rtv(A.light.Get(), A.targets->GetCpuHandle(3), black, D3D12_RESOURCE_STATE_COMMON);
        A.commands.execute();
        A.made = true;
        A.history = false;
        A.current = 0;
        spdlog::info("[VR] Ambient light: its pictures made ({}x{} kept, its image {}x{})", kAmbientSmallWidth, kAmbientSmallHeight, kAmbientLightWidth,
            kAmbientLightHeight);
    }

    auto& commands = A.commands;
    commands.wait(INFINITE); // (its last frame's commands done: the game's picture's view, and the band, may be made again)
    auto* list = commands.cmd_list.Get();
    if (list == nullptr) {
        return false;
    }

    // The panel's shape as end_frame shows it this frame (its rows of the picture, where it is), and the light's round it.
    const auto shape = xr->panel_shape((int32_t)desc.Width, (int32_t)desc.Height);
    const auto light = runtimes::OpenXR::ambient_shape(shape);

    // What pass 1 reads: the game's picture itself, or, when it cannot be read by a shader (the swap chain's own picture, made for the
    // screen only: this game's is 8-bit, shown as it is), its shown rows copied into a band of this light's own (a copy of a few MB).
    const bool readable = (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    ID3D12Resource* read_from = source;
    RECT rows{0, (LONG)shape.rect_y, (LONG)desc.Width, (LONG)(shape.rect_y + shape.rect_h)};
    uint32_t read_size[2]{(uint32_t)desc.Width, desc.Height};
    if (!readable) {
        if (A.band == nullptr || A.band_size[0] != (uint32_t)desc.Width || A.band_size[1] != (uint32_t)shape.rect_h || A.band_format != desc.Format) {
            A.band.Reset(); // (not in use: its last commands done, above)
            A.band_size[0] = A.band_size[1] = 0;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = desc.Width;
            d.Height = (UINT)shape.rect_h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = desc.Format;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            d.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&A.band))) ||
                A.band == nullptr) {
                A.band.Reset();
                if (!A.refused_logged) {
                    A.refused_logged = true;
                    spdlog::error("[VR] Ambient light: its copy of the picture's band ({}x{}) could not be made: no light round the panel", (uint64_t)desc.Width,
                        shape.rect_h);
                }
                return false;
            }
            A.band->SetName(L"Ambient light: the picture's band");
            A.band_size[0] = (uint32_t)desc.Width;
            A.band_size[1] = (uint32_t)shape.rect_h;
            A.band_format = desc.Format;
            if (A.logs < 12) {
                ++A.logs;
                spdlog::info("[VR] Ambient light: the game's picture is not readable by a shader (flags {:x}): its shown rows copied ({}x{})",
                    (uint32_t)desc.Flags, (uint64_t)desc.Width, shape.rect_h);
            }
        }
        read_from = A.band.Get();
        rows = RECT{0, 0, (LONG)desc.Width, (LONG)shape.rect_h};
        read_size[0] = (uint32_t)desc.Width;
        read_size[1] = (uint32_t)shape.rect_h;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC picture_view{};
    picture_view.Format = desc.Format;
    picture_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    picture_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    picture_view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(read_from, &picture_view, A.views->GetCpuHandle(4));
    const bool loading = ModSettings::g_internalSettings.menuLoadingScreen;
    const auto now = std::chrono::steady_clock::now();
    if (starting) {
        // (Each time it comes on: started anew from this frame's picture, faded in over 0.4 s.)
        A.history = false;
        A.on_since = now;
        A.last_at = now;
        A.was_loading = false; // (the loading screen's picture timed from this one's start; the review)
        if (A.logs < 12) {
            ++A.logs;
            spdlog::info("[VR] Ambient light: on behind the panel (from rows {} to {} of the {}x{} picture{})", shape.rect_y, shape.rect_y + shape.rect_h,
                (uint64_t)desc.Width, desc.Height, loading ? ", a loading screen" : "");
        }
    }
    if (loading && !A.was_loading) {
        A.loading_since = now;
    }
    A.was_loading = loading;
    const float dt = std::clamp(std::chrono::duration<float>(now - A.last_at).count(), 0.0f, 0.1f);
    A.last_at = now;
    const float slow = 1.0f - std::exp(-dt / 0.4f);
    const float fast = 1.0f - std::exp(-dt / 0.12f);
    float fade = std::clamp(std::chrono::duration<float>(now - A.on_since).count() / 0.4f, 0.0f, 1.0f);
    fade = fade * fade * (3.0f - 2.0f * fade);
    const float strength = std::clamp(xr->panel_ambient_strength.load(), 0.0f, 2.0f);

    const auto transition = [](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto target = D3D12_RESOURCE_STATE_RENDER_TARGET;
    constexpr auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const int kept_from = A.current;
    const int kept_to = 1 - A.current;
    const DirectX::XMUINT2 small_size{kAmbientSmallWidth, kAmbientSmallHeight};

    // 1. The panel's shown rows (the menu's box, a loading screen's photo band) made small. (The game's picture rests in the present state,
    // the band in the common one: the same state.)
    if (!readable) {
        const D3D12_RESOURCE_BARRIER to[]{transition(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(A.band.Get(), common, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, to);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = source;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION into{};
        into.pResource = A.band.Get();
        into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        into.SubresourceIndex = 0;
        const D3D12_BOX box{0, (UINT)shape.rect_y, 0, (UINT)desc.Width, (UINT)(shape.rect_y + shape.rect_h), 1};
        list->CopyTextureRegion(&into, 0, 0, 0, &from, &box);
        const D3D12_RESOURCE_BARRIER back[]{transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
            transition(A.band.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read)};
        list->ResourceBarrier(2, back);
    } else {
        const D3D12_RESOURCE_BARRIER to[]{transition(source, D3D12_RESOURCE_STATE_PRESENT, read)};
        list->ResourceBarrier(1, to);
    }
    {
        const D3D12_RESOURCE_BARRIER b[]{transition(A.now.Get(), common, target)};
        list->ResourceBarrier(1, b);
    }
    const float part[4]{(float)(rows.right - rows.left) / (float)kAmbientSmallWidth / (float)read_size[0],
        (float)(rows.bottom - rows.top) / (float)kAmbientSmallHeight / (float)read_size[1], 0.0f, 0.0f};
    draw_ambient_pass(list, m_ambient_down_batch.get(), A.views->GetGpuHandle(4), DirectX::XMUINT2{read_size[0], read_size[1]},
        A.targets->GetCpuHandle(0), kAmbientSmallWidth, kAmbientSmallHeight, &rows, part, nullptr);
    // 2. What is kept moved toward it (or started anew from it).
    {
        const D3D12_RESOURCE_BARRIER b[]{transition(read_from, read, readable ? D3D12_RESOURCE_STATE_PRESENT : common), transition(A.now.Get(), target, read),
            transition(A.kept[kept_from].Get(), common, read), transition(A.kept[kept_to].Get(), common, target)};
        list->ResourceBarrier(4, b);
    }
    const float smooth[4]{slow, fast, A.history ? 0.0f : 1.0f, 0.0f};
    draw_ambient_pass(list, m_ambient_smooth_batch.get(), A.views->GetGpuHandle(kept_from == 0 ? 0 : 2), small_size, A.targets->GetCpuHandle(1 + kept_to),
        kAmbientSmallWidth, kAmbientSmallHeight, nullptr, smooth, nullptr);
    // 3. The light's image from it.
    {
        const D3D12_RESOURCE_BARRIER b[]{transition(A.now.Get(), read, common), transition(A.kept[kept_from].Get(), read, common),
            transition(A.kept[kept_to].Get(), target, read), transition(A.light.Get(), common, target)};
        list->ResourceBarrier(4, b);
    }
    const float numbers[16]{shape.curved ? 1.0f : 0.0f, shape.curved ? light.angle : light.width, light.height, light.radius,
        shape.curved ? shape.angle : shape.width, shape.height, shape.distance, strength,
        fade, 0.01f, 0.10f, 1.3f,
        0.35f, 0.25f, 0.45f, 0.0f};
    const float image_size[4]{(float)kAmbientLightWidth, (float)kAmbientLightHeight, 0.0f, 0.0f};
    draw_ambient_pass(list, m_ambient_light_batch.get(), A.views->GetGpuHandle(kept_to == 0 ? 1 : 3), small_size, A.targets->GetCpuHandle(3),
        kAmbientLightWidth, kAmbientLightHeight, nullptr, image_size, numbers);
    {
        const D3D12_RESOURCE_BARRIER b[]{transition(A.kept[kept_to].Get(), read, common), transition(A.light.Get(), target, common)};
        list->ResourceBarrier(2, b);
    }
    commands.has_commands = true; // (recorded here, not through its own copy or clear)
    commands.execute();
    A.current = kept_to;
    A.history = true;

    // Into its own swapchain (its commands, on the same queue, run after these).
    if (!m_openxr.copy_ambient(A.light.Get())) {
        return false;
    }
    A.was_on     = true;
    A.last_on_at = std::chrono::steady_clock::now();

    // (Once a session, a loading screen 3 s in: its whole picture and the light round it saved for the log, to check the band the panel
    // shows (its rows above) and the light. One stall, both in the same frame.)
    if (loading && !A.loading_snapped && now - A.loading_since > std::chrono::seconds(3)) {
        A.loading_snapped = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto picture_path = Framework::get_persistent_dir("menu_loading.jpg");
        const auto light_path = Framework::get_persistent_dir("menu_ambient.png");
        m_menu_perf.saving = true; // (the frame's time is the saving's: left out of the menu's frame times)
        const auto hr_picture = DirectX::SaveWICTextureToFile(queue, source, GUID_ContainerFormatJpeg, picture_path.wstring().c_str(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
        const auto hr_light = DirectX::SaveWICTextureToFile(queue, A.light.Get(), GUID_ContainerFormatPng, light_path.wstring().c_str(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        spdlog::info("[VR] Ambient light: a loading screen saved to {} (hr {:x}; the panel shows its rows {} to {}), the light round it to {} (hr {:x})",
            picture_path.string(), (uint32_t)hr_picture, shape.rect_y, shape.rect_y + shape.rect_h, light_path.string(), (uint32_t)hr_light);
    }
    return true;
}

void D3D12Component::release_ambient() {
    auto& A = m_ambient;
    A.commands.wait(INFINITE);
    // (Its image's copies into its swapchain done too: they read it.)
    {
        std::scoped_lock _{m_openxr.mtx};
        for (auto& ctx : m_openxr.ambient_context.texture_contexts) {
            if (ctx != nullptr) {
                ctx->commands.wait(INFINITE);
            }
        }
    }
    A.commands.reset();
    A.views.reset();
    A.targets.reset();
    A.now.Reset();
    A.kept[0].Reset();
    A.kept[1].Reset();
    A.light.Reset();
    A.band.Reset();
    A.band_size[0] = A.band_size[1] = 0;
    A.band_format = DXGI_FORMAT_UNKNOWN;
    A.made = false;
    A.failed = false;
    A.history = false;
    A.was_on = false;
    A.current = 0;
}

Vector4f D3D12Component::picture_frustum(bool mono, int eye) {
    auto runtime = VR::get()->get_runtime();
    std::shared_lock _{runtime->eyes_mtx};
    return mono ? runtime->mono_frustum : runtime->eye_frustums[eye & 1];
}

void D3D12Component::write_world_history(ID3D12Resource* eye_texture, bool left, bool mono) {
    auto& h = m_world_history;
    auto& ctx = (left ? m_openvr.left_eye_tex : m_openvr.right_eye_tex)[h.slot];
    if (ctx.texture == nullptr || eye_texture == nullptr) {
        return;
    }
    // (A copy needs the same size and kind; the textures are made like the backbuffer, 8-bit.)
    const auto kept = ctx.texture->GetDesc();
    const auto shown = eye_texture->GetDesc();
    if (kept.Width != shown.Width || kept.Height != shown.Height || kept.Format != shown.Format) {
        return;
    }
    ctx.commands.wait(INFINITE);
    ctx.commands.copy(eye_texture, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.commands.execute();
    if (mono) {
        h.written[h.slot] = 3;
        h.mono[h.slot] = true;
    } else {
        h.written[h.slot] |= left ? 1 : 2;
        h.mono[h.slot] = false;
    }
}
} // namespace vrmod
