// vrcore - engine-agnostic VR mod (alternate-frame rendering) for starfield2vr.
// Runtime, pose and input handling adapted from REFramework's VR mod (MIT, praydog).
// Frame-index driven AFR API (on_wait_rendering/on_begin_rendering/update_hmd_state with an
// engine frame index) is new and matches what starfield2vr's Creation Engine hooks call.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#include <Xinput.h>

#include <openvr.h>

#include "Framework.hpp"
#include "Mod.hpp"
#include "sdk/Math.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/runtimes/OpenVR.hpp"
#include "vr/runtimes/OpenXR.hpp"

class VR : public Mod {
public:
    static std::shared_ptr<VR>& get();

    [[nodiscard]] std::string_view get_name() const override { return "VR"; }

    std::optional<std::string> on_initialize_d3d_thread() override;
    void on_present() override;
    void on_post_present() override;
    // (fix196) The F11 menu's tabs (Mod::on_draw_main_ui): the eye mode as one choice and the sharpness on Main; the rest in Advanced and
    // Debug.
    void on_draw_main_ui(int part) override;
    void on_draw_advanced_ui(int part) override;
    void on_draw_debug_ui() override;
    int eye_mode_choice() const;          // 0 full stereo, 1 stereo with the eyes taking turns, 2 mono, 3 AER
    void set_eye_mode_choice(int choice); // (the saved switches set together: MonoMode, AlternateEyeRendering, FullStereo)
    // (fix198) The eye mode chosen in a menu (F11's, or the game's Settings > VR tab): as the eye-mode button, refused while video memory
    // has been over its budget 3 s (to a heavier mode than the one chosen now: a line for the menus, two short buzzes, the log); true when
    // set. The game thread (it sets the switches).
    bool try_eye_mode_choice(int choice, const char* where);
    // (fix198, the review) Its two halves: the refusal alone (any thread: the check reads atomics, the reason is kept under its lock, the
    // buzz asked for through an atomic), true when refused; and the choice made without the check (the game thread).
    bool eye_mode_refused(int choice, const char* where);
    void choose_eye_mode(int choice);
    bool eye_mode_refused_lately() const;
    // The refusal's line while it is recent (8 s), else empty: a copy, made under its lock.
    std::string eye_mode_refusal() const;
    // (fix198) The resolution scale set from a menu: the headset's images remade at it once it has not changed for 750 ms and no game menu
    // is shown (on_begin_rendering; inside a game menu it is applied as the menu closes). Any thread: it only notes the value and the time.
    void apply_resolution_scale();
    void on_device_reset() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    // --- Engine frame callbacks (called by the game integration) -----------------------------
    // Engine frame index parity selects the eye: even = left, odd = right.
    void on_wait_rendering(uint32_t frame_index);  // start of a game frame, before simulation
    void on_begin_rendering(uint32_t frame_index); // after the wait, before camera update
    void update_hmd_state(uint32_t frame_index);   // syncs with the runtime on left-eye frames
    void update_action_states();                   // polls controller input

    // Defined by the game integration (maps motion controllers onto a virtual gamepad).
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state);

    // --- Runtime access ------------------------------------------------------------------------
    template <typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    auto get_hmd_width() const { return get_runtime()->get_width(); }
    auto get_hmd_height() const { return get_runtime()->get_height(); }

    bool is_hmd_active() const { return get_runtime()->ready(); }
    bool is_openvr_loaded() const { return m_openvr != nullptr && m_openvr->loaded; }
    bool is_openxr_loaded() const { return m_openxr != nullptr && m_openxr->loaded; }

    // Size of the eye images being submitted (the game's backbuffer).
    const auto& get_backbuffer_size() const { return m_d3d12.get_backbuffer_size(); }

    // --- Eyes ----------------------------------------------------------------------------------
    // Eye being rendered for the frame the game thread is working on (engine frame parity).
    // Note: an attempt to pick the index per calling thread mixed up eyes for the world camera,
    // so everything uses the engine index; the HUD has a manual swap switch instead.
    // "Swap Eyes" (F11 > VR > Stereo) flips the eye the camera renders without changing pacing,
    // so a left/right mix-up between the camera and the submitted images can be tested live.
    VRRuntime::Eye get_current_render_eye() const {
        // (Full stereo: the world's camera is the left eye in every frame; the right eye is the game side's own camera.)
        if (m_native_active.load()) {
            return VRRuntime::Eye::LEFT;
        }
        const auto eye = eye_for_frame(m_engine_frame_count);
        if (m_swap_eyes->value()) {
            return eye == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
        }
        return eye;
    }

    // Stereo timing trace: while > 0, eye decisions are logged (see "Log Eye Timing").
    bool tracing() const { return m_trace_remaining.load() > 0; }
    void trace(std::string_view where, uint32_t frame_index, int eye);
    // Eye of the frame being presented. Follows the engine's present frame index; if the engine
    // stops reporting new indices, eyes simply alternate per present (see VR::on_present).
    VRRuntime::Eye get_present_eye() const { return m_current_present_eye; }

    VRRuntime::Eye eye_for_frame(uint32_t frame_index) const {
        return (frame_index % 2 == m_left_eye_interval) ? VRRuntime::Eye::LEFT : VRRuntime::Eye::RIGHT;
    }

    // --- Mono ----------------------------------------------------------------------------------
    // Mono: every frame is drawn once, from the middle of the head with a field of view covering both eyes, and shown to both
    // eyes (each eye gets a new picture every frame instead of every other one; no stereo depth). OpenXR only. The setting
    // takes effect at the start of the next left/right pair; each frame keeps the mode it was simulated in.
    bool is_mono() const { return m_mono_active.load(); }       // the frame the game thread is working on
    bool is_mono_frame(uint32_t frame_index) const {             // a given frame (the present thread, the render thread)
        const auto v = m_frame_modes[frame_index % m_frame_modes.size()].load();
        return (uint32_t)(v >> 3) == frame_index ? (v & 1) != 0 : m_mono_active.load();
    }
    bool mono_requested() const { return m_mono->value(); }
    void toggle_mono();

    // --- Full stereo (a test, fix134) ----------------------------------------------------------
    // Both eyes in every frame: the game draws the right eye as a second view of the world in the same frame as the left (the
    // game side's StereoView adds it), and each frame is a headset frame of its own, as in mono. Only while the setting is on, in
    // stereo, and the game side says it can (native_possible: its view made, in play); it starts at a left/right pair's start.
    // The world's camera is the left eye in these frames (get_current_render_eye), the game side's own camera the right.
    bool is_native() const { return m_native_active.load(); }
    bool is_native_frame(uint32_t frame_index) const {
        const auto v = m_frame_modes[frame_index % m_frame_modes.size()].load();
        return (uint32_t)(v >> 3) == frame_index ? (v & 2) != 0 : m_native_active.load();
    }
    // --- AER (fix136): Alternate Eye Rendering, as Luke Ross's R.E.A.L. VR -----------------------------------------------------
    // Each game frame draws one eye, as stereo does (the eye by the frame's parity, each eye's own DLSS instance and history), but each
    // is a headset frame of its own: the fresh eye with the other eye's last picture, each submitted with the pose it was drawn with
    // (the headset turns the older one to where the head is now). The headset gets a new picture every frame instead of a pair every
    // two; moving things can look doubled (one eye a frame older). The toggle (left grip + right stick click) cycles stereo, mono, AER.
    bool is_aer() const { return m_aer_active.load(); }
    bool is_aer_frame(uint32_t frame_index) const {
        const auto v = m_frame_modes[frame_index % m_frame_modes.size()].load();
        return (uint32_t)(v >> 3) == frame_index ? (v & 4) != 0 : m_aer_active.load();
    }
    // Every frame a headset frame of its own (mono, full stereo, AER): paced, posed and handed over each frame.
    bool is_single() const { return is_mono() || is_native() || is_aer(); }
    bool is_single_frame(uint32_t frame_index) const {
        return is_mono_frame(frame_index) || is_native_frame(frame_index) || is_aer_frame(frame_index);
    }
    // Pipelined pacing (fix136; setting PipelinedPacing, on): the present thread opens each headset frame after ending the one before,
    // the game thread does not wait for it (see OpenXR::pipelined). Off by the setting, or after it failed (headset frames not
    // handed over): the old way, the game thread waiting before each pair.
    bool pipelined() const { return m_pipelined_pacing->value() && !m_pipelined_failed.load() && is_openxr_loaded(); }
    bool full_stereo_requested() const { return m_full_stereo->value() && !m_mono->value() && !m_aer->value(); }
    // An eye's transform from the head (as get_current_eye_transform gives the eye being drawn), for the right eye's camera.
    Matrix4x4f get_eye_transform(VRRuntime::Eye eye) const;

    Matrix4x4f get_current_eye_transform(bool flip = false) const;
    Matrix4x4f get_current_projection_matrix(bool flip = false) const;

    // --- Poses (index 0 = HMD, 1 = left hand, 2 = right hand) ---------------------------------
    Vector4f get_position(uint32_t index) const;
    Vector4f get_velocity(uint32_t index) const;
    Vector4f get_angular_velocity(uint32_t index) const;
    Matrix4x4f get_rotation(uint32_t index) const;
    Matrix4x4f get_transform(uint32_t index) const;

    // Hand grip pose for VRRuntime::Hand::LEFT / RIGHT
    Matrix4x4f get_hand_transform(VRRuntime::Hand hand) const { return get_transform((uint32_t)hand + 1); }

    // Controller grip pose (palm): the same as get_transform for hands here.
    Matrix4x4f get_grip_transform(uint32_t index) const { return get_transform(index); }
    // Controller aim pose (pointing ray along -Z), same space as get_transform. Falls back to the
    // grip pose when the runtime does not report it.
    Matrix4x4f get_aim_transform(uint32_t index) const;
    // Eye height above the floor in metres (OpenXR stage space), or a negative value when unknown.
    float get_floor_eye_height() const;

    // Pose indices of the hands (1 = left, 2 = right; swapped with "Swap Controller Inputs").
    int get_left_controller_index() const { return m_swap_controllers->value() ? 2 : 1; }
    int get_right_controller_index() const { return m_swap_controllers->value() ? 1 : 2; }

    // Analog trigger/grip (0..1) and finger touch sensing for hand poses.
    struct FingerState {
        float trigger{0.0f};
        float grip{0.0f};
        bool trigger_touch{false};
        bool thumb_touch{false};
    };
    FingerState get_finger_state(bool left) const;

    // A tracked joint in the same stage space as get_transform.
    struct TrackedJoint {
        Vector3f position{};
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        bool position_valid{false};
        bool orientation_valid{false};
    };

    // Quest body tracking (XR_FB_body_tracking, with XR_META_body_tracking_full_body when available).
    // Joints use XrFullBodyJointMETA indices (0-69 match XrBodyJointFB; 70-83 are the legs).
    struct BodyTrackingState {
        bool supported{false};     // a body tracker exists
        bool full_body{false};     // legs available
        bool active{false};        // this frame's locations are active
        bool high_fidelity{false};
        float confidence{0.0f};
        uint32_t joint_count{0};
        std::array<TrackedJoint, 84> joints{};
    };
    // Returns false when no tracker exists; check `active` for this frame's data.
    bool get_body_tracking(BodyTrackingState& out) const;

    // Hand tracking (XR_EXT_hand_tracking), XrHandJointEXT indices.
    struct HandTrackingState {
        bool supported{false};
        bool active{false};
        int data_source{0}; // 0 unknown, 1 tracked hand, 2 derived from the controller
        std::array<TrackedJoint, 26> joints{};
    };
    bool get_hand_tracking(bool left, HandTrackingState& out) const;

    // Standing origin as a translation matrix ([3] holds the origin).
    Matrix4x4f get_transform_offset() const;
    Vector4f get_standing_origin() const;
    void set_standing_origin(const Vector4f& origin);

    glm::quat get_rotation_offset() const;
    void set_rotation_offset(const glm::quat& offset);
    void recenter_view();

    // --- Input ---------------------------------------------------------------------------------
    using ActionHandle = vr::VRActionHandle_t;
    using SourceHandle = vr::VRInputValueHandle_t;

    bool is_using_controllers() const {
        return is_using_controllers_within(std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value()));
    }

    template <typename Rep, typename Period>
    bool is_using_controllers_within(std::chrono::duration<Rep, Period> duration) const {
        return !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= duration;
    }

    bool is_action_active(ActionHandle action, SourceHandle source = vr::k_ulInvalidInputValueHandle) const;
    bool is_action_active_any_joystick(ActionHandle action) const {
        return is_action_active(action, m_left_joystick) || is_action_active(action, m_right_joystick);
    }

    Vector2f get_joystick_axis(SourceHandle handle) const;
    Vector2f get_left_stick_axis() const { return get_joystick_axis(m_left_joystick); }
    Vector2f get_right_stick_axis() const { return get_joystick_axis(m_right_joystick); }

    auto get_left_joystick() const { return m_left_joystick; }
    auto get_right_joystick() const { return m_right_joystick; }

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, SourceHandle source);

    // Gun sight: a small dot `distance_m` along a ray, `width_degrees` wide, drawn over the game image. The ray is
    // `origin` along `direction` (stage space, as the controller poses: where the app's shots really go) when given, else
    // the hand's aim. `sprite`: which picture (0 the gun sight's red dot; 1 the gravity gloves' target ring, 2 locked on,
    // 3 their pointing dot; OpenXR::sprite_laser_dot the gun laser's dot). OpenXR only.
    void set_reticle(bool visible, VRRuntime::Hand hand = VRRuntime::Hand::RIGHT, float distance_m = 15.0f, float width_degrees = 1.2f,
        const float* origin = nullptr, const float* direction = nullptr, int sprite = 0);
    // The game stopped while the gun sight was being set up last time, so it stays off (or its setup failed this session).
    bool reticle_blocked();
    // Try setting it up again (the setting turned back on).
    void retry_reticle();
    // The gun's laser sight (fix129, LaserSight): its line from your eye each frame it is shown (its start as an offset in the eye's
    // axes, x right, y up, z ahead, in the game's units; its way in them; on false: not shown,
    // the game's depth no longer copied); where it last met what is drawn (true: a reading from the last quarter second; hit false: it
    // met nothing within 80 m); its beam as seen, in the room (stage space: the gun to where it ends; not visible: none).
    void set_laser_ray(bool on, const float* origin = nullptr, const float* direction = nullptr, uint32_t frame = 0);
    bool laser_hit(bool& hit, float& distance);
    void set_laser_beam(bool visible, const float* start = nullptr, const float* end = nullptr);
    // (fix148) The beam as drawn into the pictures: its start and way in the eye's axes (as the line), how far it is seen (the game's
    // units), the frame (as the line's). Not visible: none drawn.
    void set_laser_beam_view(bool visible, const float* start = nullptr, const float* direction = nullptr, float length = 0.0f, uint32_t frame = 0);

    auto get_last_controller_update() const { return m_last_controller_update; }

    float get_resolution_scale() const { return m_resolution_scale->value(); }

private:
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();
    std::optional<std::string> reinitialize_openvr();
    std::optional<std::string> reinitialize_openxr();

    bool detect_controllers();
    bool is_any_action_down();
    void update_frustums();

    Vector4f get_position_unsafe(uint32_t index) const;

public:
    // --- State shared with D3D12Component and the game integration ------------------------------
    // A frame index written by one engine thread; remembers which thread wrote it last.
    struct FrameIndex {
        std::atomic<uint32_t> value{0};
        std::atomic<DWORD> writer{0};

        FrameIndex& operator=(uint32_t v) {
            value = v;
            writer = GetCurrentThreadId();
            return *this;
        }

        operator uint32_t() const { return value.load(); }
        uint32_t load() const { return value.load(); }
        DWORD writer_thread() const { return writer.load(); }
    };

    // Frame indices reported by the engine (Reflex markers in Starfield).
    FrameIndex m_engine_frame_count{};    // frame currently simulated by the game thread
    FrameIndex m_render_frame_count{};    // frame whose render submission started
    FrameIndex m_presenter_frame_count{}; // frame currently being presented
    std::atomic<int64_t> m_last_present_time_ns{0};
    std::atomic<bool> m_skip_next_present{false};     // frames seen out of order (only noted now, see VR::on_present)
    std::atomic<bool> m_mono_active{false};           // mono for the frame being simulated (see is_mono)
    std::atomic<int> m_mono_toggles{0};               // toggle requests (controller, key), applied on the game thread
    std::array<std::atomic<uint64_t>, 8> m_frame_modes{}; // (frame index << 3) | (AER << 2) | (full stereo << 1) | mono, the last few frames
    std::atomic<bool> m_native_active{false};         // full stereo for the frame being simulated (see is_native)
    std::atomic<bool> m_native_possible{false};       // the game side can draw the right eye now (set by it each frame)
    std::atomic<bool> m_aer_active{false};            // AER for the frame being simulated (see is_aer)
    std::atomic<bool> m_pipelined_failed{false};      // pipelined pacing gave up (headset frames not handed over): the old way
    bool m_present_native{false};                     // the frame being presented is full stereo (present thread)
    bool m_present_aer{false};                        // the frame being presented is AER (present thread)
    std::array<std::atomic<uint32_t>, 8> m_native_drawn{}; // the last few full stereo frames whose right eye's view was drawn (+1)
    std::atomic<int> m_native_hud_shift{0};           // full stereo: the left eye's HUD moved right by this (pixels), for its depth
    std::atomic<bool> m_native_brief_pause{false};    // (fix142) full stereo stood down for a moment only (the hand scanner): its pictures kept
    std::atomic<float> m_native_hud_centre{0.0f};     // and by this share of its width: the left eye's straight ahead against the right's (fix138)
    // (fix139) Foveated shading's eye check: the frames whose world view it gave a pattern by its frame's turn (frame + 1, by frame % 8),
    // checked at that view's DLSS step against the frame DLSS is given (UpscalerAfrNvidiaModule): hits and misses, for the log.
    std::array<std::atomic<uint32_t>, 8> m_foveation_frames{};
    std::atomic<uint32_t> m_foveation_hits{0};
    std::atomic<uint32_t> m_foveation_misses{0};
    std::atomic<uint32_t> m_foveation_unmarked{0};
    std::atomic<int> m_native_one_picture_run{0};     // full stereo: presents in a row shown with one picture for both eyes
    std::atomic<float> m_stereo_eyes_apart{0.0f};     // (fix148) full stereo: the right eye's camera this far from the left's, along its right (the drawn camera's units)
    void note_native_drawn(uint32_t frame_index) { m_native_drawn[frame_index % m_native_drawn.size()] = frame_index + 1; }
    bool native_drawn(uint32_t frame_index) const { return m_native_drawn[frame_index % m_native_drawn.size()].load() == frame_index + 1; }
    // (fix138) The frustum the game's picture of a frame is drawn with: mono's; in full stereo the right eye's (the left eye's when its
    // right view was not drawn); else the eye's whose turn it is. (The menu drawn into it, Framework::render_menu.)
    Vector4f picture_frustum_of(uint32_t frame_index) const {
        const auto*       runtime = get_runtime();
        const bool        mono    = is_mono_frame(frame_index);
        const bool        native  = !mono && is_native_frame(frame_index);
        std::shared_lock  _{runtime->eyes_mtx};
        if (mono) {
            return runtime->mono_frustum;
        }
        if (native) {
            return runtime->eye_frustums[native_drawn(frame_index) ? 1 : 0];
        }
        return runtime->eye_frustums[eye_for_frame(frame_index) == VRRuntime::Eye::LEFT ? 0 : 1];
    }
    bool m_present_mono{false};                       // the frame being presented is mono (present thread)

    VRRuntime::Eye m_current_present_eye{VRRuntime::Eye::LEFT};
    uint32_t m_last_presenter_index{0xFFFFFFFF};

    // Frame index of the last present that finished (its picture handed to the runtime), and whether presents carry the
    // engine's frame index (Reflex marker 4). A new left/right pair waits for the frame before it by index (VR::on_wait_rendering).
    std::atomic<uint32_t> m_last_finished_present{0};
    std::atomic<bool> m_present_index_ok{false};

    // Diagnostics, written to the log every few seconds while a runtime is loaded.
    struct Stats {
        uint32_t presents{0};
        uint32_t left_presents{0};
        uint32_t right_presents{0};
        uint32_t stuck_present_index{0};
        uint32_t copies_ok[2]{};
        uint32_t copy_skipped_should_render{0};
        uint32_t copy_skipped_not_begun{0};
        uint32_t hmd_syncs{0};
        uint32_t hmd_sync_failures{0};
        // Pair start (game thread): how often it had to wait for the frame before it by index, how often the old
        // "presented" signal said go although that frame was not presented yet (the cause of black flicker), timeouts,
        // and the longest wait (ms).
        uint32_t pair_waits{0};
        uint32_t early_signals{0};
        uint32_t pair_wait_timeouts{0};
        uint32_t ahead_waits{0};       // (pipelined: the game thread held back, more than two frames ahead of the last one handed over)
        uint32_t pipelined_misses{0};  // (pipelined: presents that ended no headset frame though one was due)
        uint32_t longest_pair_wait_ms{0};
    } m_stats{};

    uint32_t m_frame_count{0}; // number of presents seen
    // (The same, for any thread: a copy the game's frames used is let go only some presents after its last use, as the GPU is then
    // surely done with it: the CPU cannot get more than a few frames ahead of the GPU.)
    std::atomic<uint32_t> m_presents_done{0};
    // (fix201) Diagnostics after a loading screen: when the last one ended (steady clock, milliseconds; 0 none yet) and how many have
    // ended, set by the game side (GameFlow::diagnosticsTick). For 90 s after each, the headset's layers and pictures are logged.
    std::atomic<int64_t>  m_diag_load_end_ms{0};
    std::atomic<uint32_t> m_diag_loads{0};
    static constexpr int64_t kDiagWindowMs = 90000;
    // (fix201) Milliseconds since the last loading screen ended, while that is under 90 s; else -1.
    int64_t diag_ms_since_load() const {
        const int64_t end = m_diag_load_end_ms.load(std::memory_order_relaxed);
        if (end == 0) {
            return -1;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t since = now - end;
        return since >= 0 && since < kDiagWindowMs ? since : -1;
    }
    // (fix179) When the game thread began its last frames (steady clock, microseconds; on_wait_rendering, once a game frame), and how many
    // it began: a menu's slow frames are told how long the game thread's frames around them took (D3D12Component::note_menu_frame).
    std::array<std::atomic<int64_t>, 32> m_game_frame_us{};
    std::atomic<uint32_t> m_game_frames{0};
    uint32_t m_left_eye_interval{0};
    uint32_t m_right_eye_interval{1};
    bool m_submitted{false};

    float m_nearz{0.1f};
    float m_farz{3000.0f};

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()};
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};

    vr::VRTextureBounds_t m_right_bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vr::VRTextureBounds_t m_left_bounds{0.0f, 0.0f, 1.0f, 1.0f};

    vrmod::D3D12Component m_d3d12{};

    // --- XInput spoofing state (used by on_xinput_get_state) ---------------------------------
    std::chrono::steady_clock::time_point m_last_xinput_update{};
    uint32_t m_lowest_xinput_user_index{0};
    bool m_spoofed_gamepad_connection{false};

    // --- Actions -------------------------------------------------------------------------------
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    ActionHandle m_action_pose{};
    ActionHandle m_action_trigger{};
    ActionHandle m_action_grip{};
    ActionHandle m_action_joystick{};
    ActionHandle m_action_joystick_click{};
    ActionHandle m_action_a_button_left{};
    ActionHandle m_action_a_button_right{};
    ActionHandle m_action_b_button_left{};
    ActionHandle m_action_b_button_right{};
    ActionHandle m_action_thumbrest_touch_left{};
    ActionHandle m_action_thumbrest_touch_right{};
    ActionHandle m_action_dpad_up{};
    ActionHandle m_action_dpad_right{};
    ActionHandle m_action_dpad_down{};
    ActionHandle m_action_dpad_left{};
    ActionHandle m_action_system_button{};
    ActionHandle m_action_touchpad{};
    ActionHandle m_action_touchpad_click{};
    ActionHandle m_action_haptic{};

    std::unordered_map<std::string, std::reference_wrapper<ActionHandle>> m_action_handles{
        {"/actions/default/in/Pose", m_action_pose},
        {"/actions/default/in/Trigger", m_action_trigger},
        {"/actions/default/in/Grip", m_action_grip},
        {"/actions/default/in/Joystick", m_action_joystick},
        {"/actions/default/in/JoystickClick", m_action_joystick_click},
        {"/actions/default/in/AButtonLeft", m_action_a_button_left},
        {"/actions/default/in/AButtonRight", m_action_a_button_right},
        {"/actions/default/in/BButtonLeft", m_action_b_button_left},
        {"/actions/default/in/BButtonRight", m_action_b_button_right},
        {"/actions/default/in/ThumbrestTouchLeft", m_action_thumbrest_touch_left},
        {"/actions/default/in/ThumbrestTouchRight", m_action_thumbrest_touch_right},
        {"/actions/default/in/DPad_Up", m_action_dpad_up},
        {"/actions/default/in/DPad_Right", m_action_dpad_right},
        {"/actions/default/in/DPad_Down", m_action_dpad_down},
        {"/actions/default/in/DPad_Left", m_action_dpad_left},
        {"/actions/default/in/SystemButton", m_action_system_button},
        {"/actions/default/in/Touchpad", m_action_touchpad},
        {"/actions/default/in/TouchpadClick", m_action_touchpad_click},
        {"/actions/default/out/Haptic", m_action_haptic},
    };

    SourceHandle m_left_joystick{};
    SourceHandle m_right_joystick{};

    static const std::string actions_json;

    // --- Options -------------------------------------------------------------------------------
    const ModKey::Ptr m_set_standing_key{ModKey::create(generate_name("SetStandingOriginKey"))};
    const ModKey::Ptr m_recenter_view_key{ModKey::create(generate_name("RecenterViewKey"))};
    const ModToggle::Ptr m_swap_controllers{ModToggle::create(generate_name("SwapControllerInputs"), false)};
    // Off by default (V2: a new default over saved configs): it costs a full-size copy every two frames, only for the monitor.
    const ModToggle::Ptr m_desktop_fix{ModToggle::create(generate_name("DesktopRecordingFixV2"), false)};
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 30.0f)};
    const ModSlider::Ptr m_joystick_deadzone{ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.15f)};
    // (fix198) 1.0 by default (the runtime's recommended size; 0.8 before, V2: 64% of its pixels). A config that saved the old 0.8 starts
    // at 1.0 once (m_resolution_default_1, a one-time mark: a value chosen after that, 0.8 too, is kept).
    const ModSlider::Ptr m_resolution_scale{ModSlider::create(generate_name("OpenXRResolutionScaleV2"), 0.1f, 5.0f, 1.0f)};
    const ModToggle::Ptr m_resolution_default_1{ModToggle::create(generate_name("ResolutionDefault1Applied"), false)};
    // On by default (V3, fix139: a new default over saved configs). Off (fix138's): every view drawn with both eyes' lenses together,
    // which reach less far up than down, the centred frustum's unseen top trimmed (VR::update_frustums): sharper up and down, but the
    // game's tiled lighting takes each view as centred (lights left out of blocks of the picture: fix138's test, blocky dark patches).
    const ModToggle::Ptr m_symmetric_projection{ModToggle::create(generate_name("SymmetricProjectionV3"), true)};
    const ModToggle::Ptr m_swap_eyes{ModToggle::create(generate_name("SwapEyes"), false)};
    const ModSlider::Ptr m_eye_separation_scale{ModSlider::create(generate_name("EyeSeparationScale"), 0.0f, 2.0f, 1.0f)};
    const ModCombo::Ptr m_requested_runtime{ModCombo::create(generate_name("RequestedRuntime"), {"Auto (OpenXR first)", "OpenXR", "OpenVR"}, 0)};
    const ModToggle::Ptr m_mono{ModToggle::create(generate_name("MonoMode"), false)};
    const ModKey::Ptr m_mono_key{ModKey::create(generate_name("MonoModeKey"))};
    // Mono while a game menu is shown, back as it was in play when it closes (fix128: menus run better in mono, and their 3D views
    // keep one history).
    const ModToggle::Ptr m_mono_in_menus{ModToggle::create(generate_name("MonoInMenus"), true)};
    // Full stereo (fix134; on by default from fix138): both eyes drawn in every frame, as two views of the world in one
    // frame, whenever the eyes are in stereo (not mono or AER) and it can be played; else the eyes take turns.
    const ModToggle::Ptr m_full_stereo{ModToggle::create(generate_name("FullStereo"), true)};
    // AER (fix136): one eye drawn each frame, each a headset frame of its own (the other eye's last picture with it). Off by default;
    // the toggle (left grip + right stick click) cycles stereo, mono, AER.
    const ModToggle::Ptr m_aer{ModToggle::create(generate_name("AlternateEyeRendering"), false)};
    // (fix196) AER in the eye-mode button's cycle (left grip + right stick click). Off by default: the button switches stereo and mono only
    // (AER, chosen in the menu, is then left for stereo at the next press).
    const ModToggle::Ptr m_aer_in_button{ModToggle::create(generate_name("AerInModeButton"), false)};
    // Pipelined pacing (fix136): no wait on the game thread before each frame pair; the present thread paces the headset frames.
    const ModToggle::Ptr m_pipelined_pacing{ModToggle::create(generate_name("PipelinedPacing"), true)};
    std::chrono::steady_clock::time_point m_menu_seen_at{};
    bool m_menu_mono{false};
    // (fix173) A switch the memory guard refused: its two short pulses (1: the first is due, 2: the second), when the next is (game thread).
    int m_refused_buzz_step{0};
    std::chrono::steady_clock::time_point m_refused_buzz_at{};
    // (fix198, the review) A menu's refused eye mode: its buzz asked for from any thread (started on the game thread), and its line until
    // a time (both under the lock: the menus read them on their own threads).
    std::atomic<bool> m_refused_buzz_asked{false};
    mutable std::mutex m_eye_mode_refused_mtx{};
    std::chrono::steady_clock::time_point m_eye_mode_refused_until{};
    std::string m_eye_mode_refused_why{};
    // (fix198, the review) The sharpness last asked for by a menu (negative: none yet) and when it last changed (steady clock, ms).
    std::atomic<float> m_res_asked_value{-1.0f};
    std::atomic<int64_t> m_res_asked_ms{0};

    // (fix197) VectorXR (an OpenXR API layer: its Frame Generation makes extra headset frames from the game's): its layer seen among the
    // loader's as the instance is made; its status file then read every 3 s on a thread of its own (check_vectorxr_status), read only.
    // Frame Generation makes new frames from the first projection layer with both views: AER's frames (one eye new, the other's older
    // picture with it) do not suit it, so that is warned of in the log and the menu.
    std::atomic<bool> m_vectorxr_layer{false};
    std::atomic<bool> m_vectorxr_generating{false}; // (our process's, the file current)
    std::atomic<int> m_vectorxr_multiplier{0};
    std::atomic<bool> m_vectorxr_watching{false};
    // (fix200) The session named by the last status file read (ours, current; a hash of its sessionId, 0 none), and the one named when
    // the latest instance was made: a file still naming that one is from before it, and is not taken as "Frame Generation off".
    std::atomic<uint64_t> m_vectorxr_last_session{0};
    std::atomic<uint64_t> m_vectorxr_instance_session{0};
    std::mutex m_vectorxr_mtx{};
    std::string m_vectorxr_summary{}; // (under its lock: the last state, for the log and the menu)
    bool m_vectorxr_conflict_told{false};
    void start_vectorxr_watch();
    void check_vectorxr_status();
    bool vectorxr_aer_conflict() const { return m_vectorxr_generating.load() && is_aer(); }

    ValueList m_options{
        *m_set_standing_key,
        *m_recenter_view_key,
        *m_swap_controllers,
        *m_desktop_fix,
        *m_motion_controls_inactivity_timer,
        *m_joystick_deadzone,
        *m_resolution_scale,
        *m_resolution_default_1,
        *m_requested_runtime,
        *m_swap_eyes,
        *m_symmetric_projection,
        *m_eye_separation_scale,
        *m_mono,
        *m_mono_key,
        *m_mono_in_menus,
        *m_full_stereo,
        *m_aer,
        *m_aer_in_button,
        *m_pipelined_pacing,
    };

    std::atomic<int32_t> m_trace_remaining{0};

    // Stereo screenshot request (saved by D3D12Component on the next left and right presents).
    bool m_screenshot_armed{false};
    int  m_screenshot_number{0}; // files are named stereo_<number>_left/right.jpg
    bool m_screenshot_done[2]{};
    std::chrono::steady_clock::time_point m_screenshot_not_before{};

private:
    mutable std::recursive_mutex m_openvr_mtx{};
    mutable std::shared_mutex m_rotation_mtx{};

    Vector4f m_standing_origin{0.0f, 1.5f, 0.0f, 0.0f};
    glm::quat m_rotation_offset{glm::identity<glm::quat>()};

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    std::chrono::steady_clock::time_point m_last_controller_update{};

    // Signalled after the right eye has been submitted, so the next left-eye frame
    // does not call xrWaitFrame/xrBeginFrame before xrEndFrame of the previous pair.
    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, TRUE, nullptr)};

    bool m_init_finished{false};
    bool m_wants_swapchain_rebuild{false};
};
