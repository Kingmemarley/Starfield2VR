// Adapted from REFramework (MIT, praydog) for vrcore.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <common/xr_linear.h>

#include "OpenXRTrackingCompat.hpp"
#include "VRRuntime.hpp"

namespace runtimes{
struct OpenXR final : public VRRuntime {
    OpenXR() {
        this->custom_stage = SynchronizeStage::EARLY;
    }
    
    virtual ~OpenXR() {
        this->destroy();
    }

    struct Swapchain {
        XrSwapchain handle;
        int32_t width;
        int32_t height;
    };

    VRRuntime::Type type() const override { 
        return VRRuntime::Type::OPENXR;
    }

    std::string_view name() const override {
        return "OpenXR";
    }

    bool ready() const override {
        return VRRuntime::ready() && this->session_ready;
    }

    VRRuntime::Error synchronize_frame() override;
    VRRuntime::Error update_poses() override;
    VRRuntime::Error update_render_target_size() override;
    uint32_t get_width() const override;
    uint32_t get_height() const override;

    VRRuntime::Error consume_events(std::function<void(void*)> callback) override;

    VRRuntime::Error update_matrices(float nearz, float farz) override;
    VRRuntime::Error update_input() override;

    void destroy() override;

public: 
    // OpenXR specific methods
    std::string get_result_string(XrResult result) const;
    std::string get_structure_string(XrStructureType type) const;
    std::string get_path_string(XrPath path) const;
    XrPath get_path(const std::string& path) const;
    std::string get_current_interaction_profile() const;
    XrPath get_current_interaction_profile_path() const;

    std::optional<std::string> initialize_actions(const std::string& json_string);

    XrResult begin_frame();
    XrResult end_frame();

    void begin_profile() {
        if (!this->profile_calls) {
            return;
        }

        this->profiler_start_time = std::chrono::high_resolution_clock::now();
    }

    void end_profile(std::string_view name) {
        if (!this->profile_calls) {
            return;
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto dur = std::chrono::duration<float, std::milli>(end_time - this->profiler_start_time).count();

        spdlog::info("{} took {} ms", name, dur);
    }

    bool is_action_active(XrAction action, VRRuntime::Hand hand) const;
    bool is_action_active(std::string_view action_name, VRRuntime::Hand hand) const;
    bool is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const;
    Vector2f get_action_axis(XrAction action, VRRuntime::Hand hand) const;
    std::string translate_openvr_action_name(std::string action_name) const;

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const;
    void stop_haptic_vibration(VRRuntime::Hand source) const; // (fix173)
    void display_bindings_editor();
    void save_bindings();

    // Analog trigger/grip (0..1) and finger touch sensing, for hand poses on the shown body.
    struct FingerInput {
        float trigger{0.0f};
        float grip{0.0f};
        bool trigger_touch{false};
        bool thumb_touch{false};
    };
    FingerInput get_finger_input(VRRuntime::Hand hand) const;

    // Body and hand tracking (Quest body tracking, XR_EXT_hand_tracking). Created after the
    // session when the runtime offers them; all failures only leave the data inactive.
    void initialize_body_hand_tracking();
    void destroy_body_hand_tracking();
    void update_body_hand_tracking(XrTime display_time);

public: 
    // OpenXR specific fields
    double prediction_scale{0.0};
    bool session_ready{false};
    bool frame_began{false};
    bool frame_synced{false};
    bool profile_calls{false};

    std::chrono::high_resolution_clock::time_point profiler_start_time{};

    std::recursive_mutex sync_mtx{};

    XrInstance instance{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrSpace stage_space{XR_NULL_HANDLE};
    // Floor-level (STAGE) space, used only to measure the eye height above the floor.
    XrSpace floor_space{XR_NULL_HANDLE};
    std::atomic<float> floor_eye_height{-1.0f};
    XrSpace view_space{XR_NULL_HANDLE}; // for generating view matrices
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrFormFactor form_factor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrViewConfigurationType view_config{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrViewState stage_view_state{XR_TYPE_VIEW_STATE};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};

    // (fix189) Set where the events are polled (the present thread when pipelined), read by the controllers' getters on other threads.
    std::atomic<XrSessionState> session_state{XR_SESSION_STATE_UNKNOWN};
    // (fix189) The controllers' input is ours only while the session is focused: OpenXR stops updating the actions' state otherwise (the
    // headset taken off: Virtual Desktop's runtime goes from focused to visible), and xrSyncActions is not called then (update_input), so
    // what the runtime gives is what was held when focus went. The getters give nothing held then: no button or stick stays held.
    // (The review: focus comes back on the present thread before the input's next xrSyncActions, so for one poll the getters would give
    // what was held as focus went, a press among it: ours again only after a sync made while focused.)
    std::atomic<bool> input_synced{false};
    bool input_focused() const { return this->session_state.load() == XR_SESSION_STATE_FOCUSED && this->input_synced.load(); }

    // (fix205) The headset report: which runtime and headset this is, which controller each hand is taken as, and which physical buttons
    // the menu button's action (the game's Start: the Data menu) got. Some runtimes keep a controller's menu button for themselves (SteamVR
    // opens its dashboard with it), and the game then never gets Start; the log says so instead of leaving it to guesswork. The runtime's
    // name and version and the system's name are noted once at start (VR's OpenXR start); the report is written a moment after the first
    // sync made while focused and after each change of the controllers (the runtime sends its profile-changed events in bursts: one report
    // for each burst), on the thread that syncs the input. A few lines each time, at most kHeadsetReports times a session.
    std::string runtime_name{};
    std::string system_name{};
    // (fix205) A Pimax headset (its runtime's or system's name says so): a notice for the first 30 s on the monitor (the game side draws it,
    // CreationEngineEntry), in the log and in F11, since its passthrough can leave the headset black. Set once at start (VR's OpenXR start).
    std::atomic<bool> pimax{false};
    std::atomic<int64_t> pimax_since_ms{0}; // (steady clock)
    static constexpr const char* kPimaxNotice =
        "Pimax headset: if the headset shows a black screen, turn off passthrough in Pimax Play, then restart the game.";
    std::atomic<bool> headset_report_wanted{false};
    std::atomic<int64_t> headset_report_at_ms{0};
    bool headset_report_started{false}; // (the first report asked for after the first sync made while focused)
    int headset_reports{0};
    static constexpr int kHeadsetReports = 12;
    // (fix205) Either hand's controller is taken as the Steam Frame's now (set at each sync): its bumpers are the thumbrest actions'
    // buttons, real presses, so the game side keeps them LB and RB whatever its thumbrest setting says.
    std::atomic<bool> frame_controllers_now{false};
    XrPath frame_profile_path{XR_NULL_PATH}; // (its profile's path, made at the first sync: the input's thread only)

    // (fix207) Valve Index controllers (each hand's controller looked at each sync, on the input's thread; the game side reads the results):
    // - The grip button (the "grip" action: aiming, the left grip's combos, the favorites wheel, the grips as LB/RB). The runtime makes it
    //   from the grip's squeeze value with its own point: Pimax's runtime (built on PimaxXR) only past 0.95, with no hysteresis, so it needs
    //   an almost full squeeze and flickers near it, while the mod's grabs (HandGrab) take 0.75 and let go below 0.40. On an Index the
    //   button is made here from the same value with the grabs' points, except on SteamVR, whose own controller bindings set it (each
    //   player can tune it there).
    // - The trackpad's press (s_index_bindings): pressed past kIndexPadOn, let go below kIndexPadOff; given as that hand's thumbrest action
    //   (left: LB, right: RB on the game side, which takes them as buttons, index_pads_now).
    XrPath index_profile_path{XR_NULL_PATH}; // (made at the first sync: the input's thread only)
    XrAction grip_action{XR_NULL_HANDLE};            // (the actions' handles, set as they are made)
    XrAction trigger_action{XR_NULL_HANDLE};
    XrAction finger_grip_action{XR_NULL_HANDLE};
    XrAction finger_trigger_action{XR_NULL_HANDLE};
    XrAction trackpad_force_action{XR_NULL_HANDLE};
    XrAction thumbrest_actions[2]{XR_NULL_HANDLE, XR_NULL_HANDLE}; // (left, right)
    std::array<std::atomic<bool>, 2> index_hand{};      // the hand's controller is taken as an Index now
    std::array<std::atomic<bool>, 2> grip_from_value{}; // its grip button is made here from the value...
    std::array<std::atomic<bool>, 2> grip_value_down{}; // ...and is down
    std::array<std::atomic<bool>, 2> index_pad_live{};  // its trackpad's press is read (bound, and an Index)...
    std::array<std::atomic<bool>, 2> index_pad_down{};  // ...and is pressed
    std::atomic<bool> index_pads_now{false};            // either hand's trackpad press is read now
    // (fix207, the review) get_finger_input's button-as-value fallback only for a hand whose controller has given no real value (over 0.1)
    // since it was taken as its current profile: a runtime that gives values never has it (a value of 0 with the button down is then just
    // that). Reset as the hand's profile changes. Vive wands and Windows Mixed Reality controllers give their grip as a click only: there the
    // grip's click counts as a full squeeze (grabs and climbing work with it).
    XrPath hand_profile_now[2]{XR_NULL_PATH, XR_NULL_PATH}; // (the input's thread only)
    std::array<std::atomic<bool>, 2> trigger_value_seen{};
    std::array<std::atomic<bool>, 2> grip_value_seen{};
    static constexpr float kIndexGripOn  = 0.75f; // (HandGrab's kGripOn and kGripOff)
    static constexpr float kIndexGripOff = 0.40f;
    static constexpr float kIndexPadOn   = 0.50f;
    static constexpr float kIndexPadOff  = 0.30f;
    int index_hand_logs{0};
    // (fix207) Each 10 s while a trigger or grip was used: the highest value each hand's trigger, grip (and an Index's trackpad press) gave,
    // and how many syncs the runtime's own trigger and grip buttons were down: a runtime that gives the buttons but no values shows as a
    // button down with its value 0 (get_finger_input then takes the button as the whole value). The input's thread only.
    struct InputPeaks {
        float trigger{0.0f};
        float grip{0.0f};
        float pad{0.0f};
        int trigger_down{0};
        int grip_down{0};
    };
    InputPeaks input_peaks[2]{};
    int64_t input_peaks_since_ms{0};
    int input_peak_logs{0};
    static constexpr int kInputPeakLogs = 30;
    // The report itself (the input's thread, after a successful xrSyncActions); `why` names what asked for it.
    void report_headset(const char* why);
    // Which physical buttons an action got now ("none" when it got none; "?" when the runtime could not say), and how many.
    std::string bound_sources(XrAction action, uint32_t& count, bool localized) const; // (localized: also the runtime's own names)

    XrSpaceLocation view_space_location{XR_TYPE_SPACE_LOCATION};

    std::vector<XrViewConfigurationView> view_configs{};
    std::vector<Swapchain> swapchains{};
    std::vector<XrView> views{};
    std::vector<XrView> stage_views{};

    // (fix201) Diagnostics: milliseconds since the last loading screen ended while under 90 s (else -1) and its number, set by the present
    // thread each present (D3D12Component::on_frame from VR::diag_ms_since_load); and end_frame's record of the layers it handed over, one
    // line each time they change (the present thread only).
    int64_t diag_since_load_ms{-1};
    uint32_t diag_load{0};
    uint64_t diag_layers_sig{0};        // this frame's layers
    uint64_t diag_layers_logged_sig{0}; // the ones last logged
    std::chrono::steady_clock::time_point diag_layers_logged_at{};
    uint32_t diag_layers_changes{0};    // changes since the last line
    uint32_t diag_layers_load{0};       // the load the lines below are counted for
    int diag_layers_lines_load{0};      // lines in this load's 90 s
    int diag_layers_lines_other{0};     // lines outside them, this session

    float resolution_scale{1.0f};
    float clamped_scale() const; // resolution_scale, no bigger than the runtime's largest eye image allows (view_configs not empty)
    // Mono (VR::is_mono): the frame being ended was drawn once from the middle of the head; both views are submitted from there.
    bool mono_views{false};

    // vrcore: when set, the last eye image is shown on a head-locked virtual screen
    // (menus, flat-screen mode) instead of as a stereo projection layer.
    bool flat_screen_mode{false};

    // vrcore: set when an image was copied into (and released from) swapchain i during the
    // current frame. end_frame only submits layers whose images are ready, otherwise the
    // runtime rejects the whole frame with XR_ERROR_LAYER_INVALID.
    std::array<bool, 2> image_ready{};
    // (fix136) Each eye's swapchain has had an image released since its swapchains were made: AER submits the eye not drawn this
    // frame with its last image (the spec: the most recently released image is used), once it has one.
    std::array<bool, 2> image_ever{};
    // (fix138) The frustum the picture in each eye's swapchain was drawn with (VRRuntime::eye_frustums or mono_frustum, set as it is
    // copied in): submitted with it, as a picture shown to the other eye too, or kept for a later frame (AER), may have been drawn
    // with another eye's lens, or mono's, than the eye's own now.
    std::array<Vector4f, 2> image_frustum{};
    std::array<bool, 2>     image_frustum_set{};

    // Pipelined pacing (fix136): the present thread opens each headset frame (xrWaitFrame, xrBeginFrame) right after ending the one
    // before, so a begin never comes before the end of the frame before it (the old black screen) and the game thread does not wait
    // for it. The game thread locates each frame's poses for the display time it will be shown at (the last one the present thread
    // was given, plus the headset frames between), kept per game frame (frame_poses); each eye's picture is then submitted with the
    // pose it was drawn with (submit_*), not the latest located.
    bool pipelined{false};
    struct FramePoses {
        uint32_t frame{0};
        bool valid{false};
        XrPosef stage[2]{};
        XrPosef head{};
        XrTime display_time{0};
    };
    std::array<FramePoses, 8> frame_poses{};
    std::mutex frame_poses_mtx{};
    std::atomic<XrTime> published_display_time{0}; // (the present thread's last xrWaitFrame: its display time and period)
    std::atomic<XrDuration> published_period{0};
    std::atomic<uint64_t> frames_opened{0};
    XrTime pose_display_time{0};                   // (the game thread: the display time update_poses locates at, pipelined)
    uint32_t present_frame{0};                     // (the present thread: the game frame being presented, set before its pictures are handled)
    // Kept for a game frame, from what update_poses just located (the game thread).
    void store_poses(uint32_t frame);
    bool frame_poses_of(uint32_t frame, FramePoses& out);
    // The present thread: a headset frame opened if none is (xrWaitFrame, xrBeginFrame); its display time published.
    XrResult open_frame();
    // Set by the present thread before end_frame: each eye's picture's pose (from its frame's poses), the head's for mono views.
    bool submit_valid{false};
    XrPosef submit_pose[2]{};
    XrPosef submit_head{};
    // AER (fix136): this frame shows one fresh eye and the other's last picture, each with its own pose.
    bool aer_frame{false};
    // For the log: xrWaitFrame's time on the present thread, and the display period.
    std::atomic<uint32_t> stat_open_waits_us{0};
    // (fix179) xrWaitFrame's time (open_frame) and xrEndFrame's on the present thread, added up (microseconds): for the menus' slow frames.
    std::atomic<uint64_t> stat_wait_total_us{0};
    std::atomic<uint64_t> stat_end_total_us{0};

    // vrcore diagnostics
    uint32_t stat_end_frames{0};
    uint32_t stat_end_frames_with_layers{0};
    XrResult stat_last_end_result{XR_SUCCESS};
    float flat_screen_distance{1.6f}; // meters
    float flat_screen_width{1.8f};    // meters

    struct Action {
        std::vector<XrAction> action_collection{};
    };

    struct ActionSet {
        XrActionSet handle;
        std::vector<XrAction> actions{};
        std::unordered_map<std::string, XrAction> action_map{}; // XrActions are handles so it's okay.
        std::unordered_map<XrAction, std::string> action_names{};

        std::unordered_set<XrAction> float_actions{};
        std::unordered_set<XrAction> vector2_actions{};
        std::unordered_set<XrAction> bool_actions{};
        std::unordered_set<XrAction> pose_actions{};
        std::unordered_set<XrAction> vibration_actions{};
    } action_set;

    struct VectorActivator {
        Vector2f value{};
        std::string action_name{};
    };

    struct VectorActivatorTrue {
        Vector2f value{};
        XrAction action{};
    };

    struct HandData {
        XrSpace space{XR_NULL_HANDLE};          // grip pose (the "pose" action)
        XrPath path{XR_NULL_PATH};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};

        // Aim pose: origin at the controller's pointing tip, -Z along the pointing ray.
        XrSpace aim_space{XR_NULL_HANDLE};
        XrSpaceLocation aim_location{XR_TYPE_SPACE_LOCATION};

        // interaction profile -> action -> path map
        struct InteractionProfile {
            std::unordered_map<std::string, XrPath> path_map{};
            std::unordered_map<XrAction, std::vector<VectorActivatorTrue>> vector_activators{};
            std::unordered_map<XrAction, XrAction> action_vector_associations{};
        };

        std::unordered_map<std::string, InteractionProfile> profiles{};
        std::unordered_map<XrAction, bool> forced_actions{};
        std::unordered_map<XrAction, bool> prev_action_states{};

        bool active{false};

        // (fix163) A controller the runtime no longer tracks (the Quest turns a still controller's tracking off after a while; Virtual
        // Desktop's runtime then gives it no valid pose and an identity one, at the room's origin: your eyes, so a hand
        // kept still for a while seemed to jump to the face): its last tracked pose held 1.5 s, then eased over a second
        // to rest on the lap; back to the tracked one over half a second when it is tracked again. Meanwhile its speed is a tiny one, so
        // nothing reads the easing as a swing or a flick.
        struct Hold {
            bool     tracked{true};
            bool     have_good{false};
            XrPosef  shown{};       // the grip pose given last
            XrPosef  from{};        // where the hold (or the blend back) started
            XrPosef  aim_in_grip{}; // its aim pose in its grip pose's frame, as last tracked
            XrPosef  rest{};        // where it rests, worked out once as the ease starts (the review: not swung round with the head)
            bool     have_rest{false};
            bool     have_aim{false};
            std::chrono::steady_clock::time_point lost_at{}, back_at{};
            int      losses{0};
        } hold{};

        struct UI {
            char new_path_name[XR_MAX_PATH_LENGTH]{};
            uint32_t new_path_name_length{0};
            int action_combo_index{0};

            int activator_combo_index{0};
            int modifier_combo_index{0};
            int output_combo_index{0};
            Vector2f output_vector2{};
        } ui;
    };

    std::array<HandData, 2> hands{};
    // (fix163) A hand the runtime does not track now: its last pose held, then at rest on the lap (HandData::Hold).
    void hold_untracked_hand(HandData& hand, bool left);

    // Body/hand tracking. Extension flags are static like the instance they were enabled on.
    static inline bool ext_body_tracking{false};
    static inline bool ext_body_full_body{false};
    static inline bool ext_body_fidelity{false};
    static inline bool ext_hand_tracking{false};
    static inline bool ext_hand_data_source{false};

    // Gun sight: a small dot along a ray, drawn over the game image by end_frame (the game's own crosshair stays in the
    // middle of the view; shots follow the hand while aiming with it). The ray is the one the app gives (stage space: where
    // its shots really go), else the hand's aim. The swapchain is made and filled by the graphics side (D3D12Component) when
    // first needed.
    std::atomic<bool> reticle_visible{false};
    std::atomic<int> reticle_hand{VRRuntime::Hand::RIGHT};
    std::atomic<float> reticle_distance{15.0f}; // metres along the ray
    std::atomic<float> reticle_degrees{1.2f};   // its width, as an angle seen from the ray's start
    std::mutex reticle_ray_mtx{};
    bool reticle_has_ray{false};
    float reticle_origin[3]{};
    float reticle_direction[3]{0.0f, 0.0f, -1.0f};
    // Its image is a strip of square pictures side by side (reticle_sprites of them, each reticle_height wide): the one
    // shown (reticle_ray_mtx). 0: the gun sight's red dot; 1: the gravity gloves' target ring; 2: the same, locked on; 3:
    // the gloves' pointing dot; 7 the gun's laser beam (red), 8 its dot where it hits (fix129). The menu panel uses 4 to 6
    // itself: 4 and 5 two veils (an even black, partly
    // see-through: the world behind a menu dimmed), 6 the pointer's beam (a soft line along the picture's height).
    static constexpr int reticle_sprites = 9;
    static constexpr int reticle_game_sprites = 4; // the ones the game side may ask for (set_reticle)
    static constexpr int sprite_veil_dim = 4;
    static constexpr int sprite_veil_dark = 5;
    static constexpr int sprite_beam = 6;
    static constexpr int sprite_pointer_dot = 3;
    static constexpr int sprite_laser_beam = 7;
    static constexpr int sprite_laser_dot = 8;
    // The gun's laser beam (reticle_ray_mtx): from the gun to where it ends, in the room (stage space).
    bool laser_beam_visible{false};
    float laser_beam_start[3]{};
    float laser_beam_end[3]{};
    int reticle_sprite{0};
    XrSwapchain reticle_swapchain{XR_NULL_HANDLE};
    int32_t reticle_width{0};  // the whole strip
    int32_t reticle_height{0}; // (one picture's side)
    std::atomic<bool> reticle_ready{false};
    // A frame the runtime refused with the sight in it: the next few go without it; if those are taken, the sight was
    // the trouble (after a few such times it stays hidden).
    int reticle_skip_frames{0};
    bool reticle_suspect{false};
    int reticle_refusals{0};

    // ---- Menu panel (as Skyrim VR shows its menus) ----
    // While a game menu is open (the game's menu flag: flat_screen_mode without the player's own flat-screen switch), the
    // game's picture is shown on a panel that stays where it was put in the room, in front of where you looked when the
    // menu opened, curved around you (a cylinder layer, XR_KHR_composition_layer_cylinder, when the runtime offers it; a
    // flat quad otherwise). It moves in front of you again when you have looked well away from it for a moment, or walked
    // away from it. Behind it: the world as it last was before the menu (both eyes' pictures, from where they were seen,
    // so it stays put as you look around), dimmed by a veil, or darkness. The controllers point at it (a beam and a dot);
    // where a beam meets it is given to the game side (panel_pointer) as a place on the game's picture.
    static inline bool ext_cylinder{false};
    // (fix205) The Steam Frame's controllers (XR_VALVE_frame_controller_interaction, SteamVR): their own interaction profile, when the
    // runtime offers it. Without it SteamVR remaps the Touch bindings onto them (the Touch menu button as the View button, B/Y as all three
    // top buttons), and their bumpers and D-pad reach nothing.
    static inline bool ext_frame_controller{false};
    static constexpr const char* kFrameControllerExtension = "XR_VALVE_frame_controller_interaction";
    static constexpr const char* kFrameControllerProfile = "/interaction_profiles/valve/frame_controller_valve";
    // Settings (VRConfig).
    std::atomic<bool> panel_enabled{true};
    std::atomic<bool> panel_curved{true};
    // Virtual Desktop: an empty layer with this frame's head position sent first (VDFix in the log; as UEVR's "Virtual Desktop Fix").
    std::atomic<bool> vd_fix{true};
    bool vd_fix_logged{false};
    // (fix197) In play its empty layer is left out when the world's picture is already first with the same poses; counted for the log.
    bool vd_fix_skip_logged{false};
    // (fix200) Behind a menu too, when the world as it is now is first with this frame's poses; and never before the world's picture while
    // VectorXR's Frame Generation may run (vectorxr_framegen): logged once each.
    bool vd_fix_menu_skip_logged{false};
    bool vd_fix_vectorxr_logged{false};
    // (fix200) Which way each frame went, counted for the minute's log line (VdFixWay). The order matters: the ways from
    // vd_sent_menu_no_world on send the empty layer, those up to vd_sent_menu_pose are behind a menu.
    enum VdFixWay : int {
        vd_left_out_play = 0,      // left out: the world first with this frame's poses (in play, or before a menu's panel is ready, fix197)
        vd_left_out_menu,          // left out: behind a menu, the world as it is now first with this frame's poses
        vd_left_out_vectorxr_menu, // left out for VectorXR's Frame Generation: behind a menu (the last live picture again, drawn in, a pose differed)
        vd_left_out_vectorxr_play, // left out for VectorXR's Frame Generation: elsewhere (a pose differed, or the world not first)
        vd_sent_menu_no_world,     // sent: behind a menu with no world (the main menu, a loading screen, darkness)
        vd_sent_menu_world_left_out, // sent: behind a menu, the world as it was left out for VectorXR's Frame Generation (the main menu's layout)
        vd_sent_menu_as_it_was,    // sent: behind a menu, the world as it was
        vd_sent_menu_drawn_in,     // sent: the menu drawn into the world's pictures
        vd_sent_menu_pose,         // sent: behind a menu, the world as it is now, a pose differed
        vd_sent_flat_screen,       // sent: the flat screen (no world's picture)
        vd_sent_play_not_first,    // sent: the world not first, or none
        vd_sent_play_pose,         // sent: the world first, a pose differed
        vd_ways
    };
    std::array<uint32_t, vd_ways> vd_fix_ways{};
    int vd_fix_count_logs{0};
    // (fix200) VectorXR's layer is loaded and its Frame Generation may run this session. Set as each instance is made, from whether its
    // layer is there (VR::initialize_openxr: the loader's list, or its library loaded in the game); cleared only when its status file (ours,
    // current, of a session first seen after that instance was made) says Frame Generation is off for this game; set again when one says
    // it is on in any state. Static: kept while the instance and its sessions are made anew.
    static inline std::atomic<bool> vectorxr_framegen{false};
    // (fix200) This frame (the graphics side): the world as it was (an opaque menu's, from before it opened) was not copied into the eye
    // images because VectorXR's Frame Generation may run: behind the panel is then what the main menu has (no world's picture).
    bool frozen_left_out{false};
    bool frozen_left_out_logged{false};
    std::atomic<float> panel_distance{1.5f};       // metres from your head to the panel
    std::atomic<float> panel_width{1.8f};          // metres
    std::atomic<float> panel_height_offset{-0.1f}; // its middle against your eyes' height, metres
    std::atomic<float> panel_visible_height{1.0f}; // the part of the shown height kept (centred), 0.4..1
    std::atomic<bool> panel_trim_to_box{true};     // show only the box the menu draws in (panel_box_rows), not the empty bars
    std::atomic<int> panel_backdrop{2};            // 0 darkness, 1 the world as it was, 2 dimmed, 3 dark
    std::atomic<bool> panel_pointer_enabled{true};
    // See-through menus: menus whose content is drawn by the interface itself (not the game's 3D views) are shown from the
    // game's interface layer alone, over the world kept behind the panel (D3D12Component::record_menu_layer), darkened by a
    // see-through dark this strong (0..0.9) under it.
    std::atomic<bool> panel_see_through{true};
    std::atomic<float> panel_see_through_darkness{0.05f};
    // Behind a see-through menu, the world as it is now (the game side keeps the game drawing it: LiveWorld; its picture is
    // copied each frame, record_live_picture, and shown in the eye images as in play) instead of the world as it was.
    std::atomic<bool> panel_live_world{true};
    // On a see-through menu's panel, the menu's 3D pictures (you in the Data menu, the planet, the item) cut out of their own
    // pictures (their even background taken away) under its interface.
    std::atomic<bool> panel_previews{true};
    // ...cut out by their depth (what is drawn kept, the gaps of their dither filled; else by their background's colour).
    std::atomic<bool> panel_depth_cutout{true};
    // ...the light they cast on their background (a glow: the lightsaber's, the menu's own lamp behind you) kept over the world too
    // (off: cut to their outline, fix127; the menu's orange lamp showed as a halo on the character).
    std::atomic<bool> panel_preview_glow{false};
    // The pointer's laser and dot as at the main menu (their own layers) also over a menu drawn into the world picture (fix128: the
    // main menu's pointer wanted everywhere; drawn in, only a dot was drawn with the menu since fix116).
    std::atomic<bool> panel_pointer_laser{true};
    // Every menu with a 3D view of its own drawn as the Data menu is: the world at full size with TAA, not DLSS's size copied up
    // (MenuTaa: the Missions menu's shimmer, fix128).
    std::atomic<bool> panel_menu_taa{true};
    // Under the faint buttons of a menu made for a dark backdrop (the Inventory's item rows), a dark backing.
    std::atomic<bool> panel_button_backing{true};
    // With the world kept drawn behind a see-through menu: its own last-frame pictures (anti-aliasing history, previous depth, ...)
    // kept apart from the menu's 3D views', which the game draws with the same ones (the game side: ViewHistory).
    std::atomic<bool> panel_world_history{true};
    // ...each of the menu's 3D views' own ones kept apart too (else they read the world's: the player's preview shows the world in
    // it, shiny).
    std::atomic<bool> panel_view_histories{true};
    // ...what was learned for a menu's views used again when they are seen again (going back to a menu; else learned again each time).
    std::atomic<bool> panel_history_plans{true};
    // With the world kept drawn behind a see-through menu: the frame kept full size (the game would make it the menu's 16:9 box; the
    // game side: FullFrame), the menu's 3D pictures laid in the box.
    std::atomic<bool> panel_full_frame{true};
    // With the world kept drawn behind a see-through menu: drawn from where your head is now (the game side: MenuCamera brings the
    // renderer's copy of the world's camera up to date; the game leaves it as it was when the menu opened).
    std::atomic<bool> panel_world_turns{true};
    // This frame (the graphics side): the panel's image is see-through (premultiplied alpha: blended over what is behind it).
    std::atomic<bool> panel_image_alpha{false};
    std::atomic<int> panel_preferred_hand{VRRuntime::Hand::RIGHT}; // (the game side: the hand that last clicked)
    // This frame (VR::on_present): the panel is wanted; the world behind the menu is the game's world (not the main menu
    // or a loading screen) and may be shown.
    bool panel_mode{false};
    bool panel_world_allowed{false};
    bool panel_glow_when_empty{false}; // (fix183) the star map: the soft light round the panel instead of the world as it was (or darkness)
    int panel_box_rows{0}; // the height of the box the open menus draw in, centred (0: the whole picture)
    // (fix192) This frame (the graphics side): where the menu's box is in the panel's image. -1: centred, as the game lays its own 16:9 box
    // (and as the see-through picture lays it); 0: at the top, as the game's own picture has it while the frame is kept full size behind
    // the menu (FullFrame: the menu drawn in the top rows), when that picture must be shown. The pointer keeps the centred box's places.
    int panel_box_top{-1};
    int32_t panel_logged_image_y{-1}; // (fix192: the top rows' part last logged; -1 the centred rows in use)
    int panel_top_logs{0};
    int32_t panel_logged_rect[2]{-1, -1}; // (the part shown, as last logged)
    // This frame (the graphics side): the panel's image got the game's picture; the eye images got the frozen world, seen
    // from frozen_pose with frozen_fov.
    bool panel_image_ready{false};
    bool frozen_ready{false};
    // This frame (the graphics side): the eye images got the world as it is now (panel_live_world), each eye's from its own
    // frame: shown from where the eyes are, as in play.
    bool live_world_ready{false};
    // The views the last live world pair was shown with (the graphics side shows that pair again, from there, when a frame's world
    // did not come: no pop back to the world as it was for a frame).
    bool last_live_valid{false};
    std::array<XrPosef, 2> last_live_pose{};
    std::array<XrFovf, 2> last_live_fov{};
    std::array<XrPosef, 2> frozen_pose{};
    std::array<XrFovf, 2> frozen_fov{};
    // The panel's own swapchain (made and filled by the graphics side, the size of the game's picture).
    XrSwapchain panel_swapchain{XR_NULL_HANDLE};
    int32_t panel_image_width{0};
    int32_t panel_image_height{0};
    // Turned off for the session after the runtime refused frames with it (the old head-locked screen then).
    bool panel_cylinder_refused{false};
    bool panel_refused{false};
    int panel_cylinder_refusals{0};
    int panel_refusals{0};
    // (fix164) The panel's shape as end_frame shows it, for a picture of this size: the rows of it shown (the menu's box, then the part the
    // setting keeps, centred), its distance, and its arc (curved: radians) or none, width and height (metres).
    struct PanelShape {
        int32_t rect_y{0};
        int32_t rect_h{1};
        float   distance{1.5f};
        bool    curved{false};
        float   angle{0.0f};
        float   width{1.8f};
        float   height{1.0f};
    };
    PanelShape panel_shape(int32_t image_width, int32_t image_height) const;
    // (fix164) Soft light round the panel (an ambient background for the 2D screens such as the intro, the startup menu and loading
    // screens, as Ambilight does, relaxed and subtle): while the panel is up with nothing behind it (the main menu, the intro, a loading screen:
    // no world to show), a larger cylinder behind it, opaque, lit by the picture's own colours near each of its edges, at most a tenth of
    // white (Strength 1), fading with the distance from the panel (the graphics side draws its image: D3D12Component::present_ambient).
    // Its shape: round the panel's middle, a quarter of a metre further than the panel, 150 degrees wide, 3 m tall (a flat quad, 120
    // degrees wide, with a flat panel). Settings (VRConfig).
    std::atomic<bool>  panel_ambient{true};
    std::atomic<float> panel_ambient_strength{1.0f}; // 0..2 (1: at most a tenth of white)
    struct AmbientShape {
        float radius{1.75f}; // (or, flat, its distance)
        float angle{2.6f};   // (curved: the arc it spans, radians)
        float width{4.55f};  // metres (flat: the quad's width)
        float height{3.0f};
    };
    static AmbientShape ambient_shape(const PanelShape& panel);
    // Its own swapchain (made and filled by the graphics side), and this frame: its image filled (ambient_ready).
    XrSwapchain ambient_swapchain{XR_NULL_HANDLE};
    int32_t ambient_image_width{0};
    int32_t ambient_image_height{0};
    bool ambient_ready{false};
    // A frame the runtime refused with it: the next few go without it (the panel and the strip are not blamed for that frame); twice in a
    // row, not shown again this session.
    int  ambient_skip_frames{0};
    int  ambient_refusals{0};
    bool ambient_refused{false};
    bool ambient_logged{false};
    // Where it is (stage space): the middle of the cylinder (or the point the flat panel faces), turned about the vertical.
    // Put in front of you when a menu opens, and then it stays there until the menu closes or you recenter (panel_recenter).
    bool panel_placed{false};
    float panel_yaw{0.0f};
    float panel_center[3]{};
    // A recenter (the headset's own, or the mod's key or buttons): the panel put in front of you again at the next frame.
    std::atomic<bool> panel_recenter{false};
    std::chrono::steady_clock::time_point panel_replace_until{};
    // (For the log: where it was put and where your head is, every few seconds while it is up.)
    int panel_placements{0};
    int panel_place_logs{0};
    int space_change_logs{0};
    std::chrono::steady_clock::time_point panel_diag_at{};
    // The views of the last world picture submitted (both eyes: where each was seen from, its field of view), kept by the
    // graphics side with its copy of that picture for the frozen world.
    bool last_world_views_valid{false};
    std::array<XrPosef, 2> last_world_pose{};
    std::array<XrFovf, 2> last_world_fov{};
    // Where each controller's aim meets the panel (u across, v down, 0..1 over the picture), and the hand pointing (the
    // preferred one when it meets the panel, else the other one when it does): pointer_mtx.
    struct PanelPointer {
        bool shown{false};    // the panel was shown on the last frame
        bool hit[2]{false, false};
        float u[2]{}, v[2]{};
        int hand{-1};         // the pointing hand (-1: none meets the panel)
        uint32_t frame{0};    // counts frames with the panel
        std::chrono::steady_clock::time_point at{}; // when this was found (an old one: the frames stopped)
    };
    std::mutex pointer_mtx{};
    PanelPointer panel_pointer{};
    PanelPointer get_panel_pointer() {
        std::scoped_lock _{this->pointer_mtx};
        return this->panel_pointer;
    }
    // (fix142) The interface drawn over the world pointed at (a conversation's responses, pressed
    // with the hand): while wanted (the game side, each poll: a conversation shown, no panel), where each controller's aim points on the
    // game's picture as seen from the eye whose picture the interface's places are in (hud_pointer_eye: 0 left, 1 right, -1 the head's
    // middle, mono), with a short beam from the pointing hand and a dot where it points, at the interface's depth (hud_pointer_shift:
    // how far the other eye's interface is moved, in its lens's tangent units; 0: no depth, the dot 20 m away). Kept as panel_pointer is.
    std::atomic<bool> hud_pointer_wanted{false};
    std::atomic<int64_t> hud_pointer_ms{0}; // (steady clock, ms: when it was last set; an old one is not wanted, the polls stopped)
    std::atomic<int> hud_pointer_eye{-1};
    std::atomic<float> hud_pointer_shift{0.0f};
    int hud_pointer_logs{0};
    PanelPointer hud_pointer{};
    PanelPointer get_hud_pointer() {
        std::scoped_lock _{this->pointer_mtx};
        return this->hud_pointer;
    }
    // (fix143) The conversation's answers touched with a finger (the shown hand put on the answer, and touching
    // the answer with the hand counted as a press): while wanted (the game side, with hud_pointer_wanted), each hand's index fingertip
    // (the runtime's finger joints, or the controller's front) is seen from the interface's eye: where it lies on the picture (u, v)
    // and how far in front of the interface's depth it is (gap, metres: through it below 0). Within the answers' box
    // (hud_touch_box: u0, v0, u1, v1 over that eye's picture) and a pointing finger (off the trigger): hover from 0.10 m (left
    // beyond 0.13 m); a press when it crosses 5 mm through, having come from 1 cm in front (Meta's poke: hover, then select on
    // crossing between frames; FRIK's Pip-Boy press), again only after it is back 2 cm in front and 0.35 s on; its place held from
    // 1 cm in front. presses counts each hand's presses (the game side clicks on a new one). Kept as hud_pointer is.
    struct HudTouch {
        bool hover[2]{false, false};
        float u[2]{}, v[2]{};
        float gap[2]{};
        uint32_t presses[2]{0, 0};
        // (fix146) The conversation's panel dragged: each hand's mouse-wheel notches so far (up above 0), the game side scrolls by new ones.
        int32_t scroll[2]{0, 0};
    };
    std::atomic<bool> hud_touch_wanted{false};
    std::atomic<bool> hud_touch_finger[2]{false, false}; // (the game side: each physical hand's finger off the trigger, pointing)
    std::atomic<float> hud_touch_box[4]{0.0f, 0.0f, 1.0f, 1.0f};
    HudTouch hud_touch{};
    HudTouch get_hud_touch() {
        std::scoped_lock _{this->pointer_mtx};
        return this->hud_touch;
    }
    // (The present thread's own: each hand's touch as it goes.)
    struct TouchState {
        bool hovering{false};
        bool armed{false};
        bool down{false};
        bool frozen{false};
        float frozen_u{0.0f}, frozen_v{0.0f};
        int64_t last_press_ms{0};
        // (fix146, the conversation's panel) pressed through and moved: a drag (no tap at the let-go); where it was pressed, how far scrolled.
        bool dragged{false};
        float down_y{0.0f};
        float scrolled_y{0.0f};
        float down_gap{0.0f};      // (how deep it was then: a push deeper at an angle is no drag, the review)
        int64_t hold_until_ms{0};  // (a tap's place held for the click until then, the review)
        float hold_u{0.0f}, hold_v{0.0f};
        int64_t seen_ms{0};        // (last looked at: a press left from before a gap is dropped)
    };
    TouchState touch_state[2]{};
    int hud_touch_logs{0};
    // (fix143) The eyes' distance apart (metres), as the runtime gives the eyes' poses: the game side places the conversation's depth
    // by it.
    std::atomic<float> ipd_m{0.064f};
    // (fix145) The conversation on a panel of its own, fixed in the room (after fix144 the text box still moved with the head slightly;
    // the conversation is meant to stay locked in front of the npc: fix144 placed its movie in the game's picture where a point of the room
    // was seen, so only that point stayed, the rest a flat card turning with the head). The game side cuts the conversation out of the
    // game's interface layer (D3D12Component::record_conversation) and says where its panel goes (set_conversation: its pose in stage
    // space, its front, +z, toward you; its size in metres; crop: the part of the game's picture it shows, u0 v0 u1 v1, so a place on the
    // panel is a place in that picture, where the game's mouse goes); the graphics side fills conv_swapchain each frame the cut came
    // (conv_image_ready); the quad is laid over the world, and the pointer and the touch meet the panel itself (hud_pointer and hud_touch
    // as before, in the picture's places).
    struct Conversation {
        XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        float size[2]{0.0f, 0.0f};
        float crop[4]{0.0f, 0.0f, 1.0f, 1.0f};
        int64_t ms{0}; // (steady clock: when it was set; the game side sets it each frame the conversation is drawn)
    };
    void set_conversation(const Conversation& c) {
        std::scoped_lock _{this->conv_mtx};
        this->conversation = c;
    }
    // (Wanted: set within the last 200 ms, with a size.)
    bool get_conversation(Conversation& c, int64_t now_ms) {
        std::scoped_lock _{this->conv_mtx};
        c = this->conversation;
        return c.ms != 0 && now_ms - c.ms < 200 && c.size[0] > 0.01f && c.size[1] > 0.01f;
    }
    std::mutex conv_mtx{};
    Conversation conversation{};
    XrSwapchain conv_swapchain{XR_NULL_HANDLE};
    int32_t conv_image_width{0};
    int32_t conv_image_height{0};
    bool conv_image_ready{false}; // (the graphics side, this frame: its image filled)
    int conv_logs{0};
    // When the panel was last shown (a frame taken with it: steady clock, ms): the game side puts the conversation back in the picture
    // (fix144's way) when it has not been shown for a moment (the review: any failure on the way would leave it shown nowhere).
    std::atomic<int64_t> conv_shown_ms{0};
    // Frames refused with it (three: not shown again this session).
    int conv_refusals{0};
    bool conv_refused{false};
    // (fix171) The favorites wheel (the game side: FavoritesWheel, a Spell Wheel VR-style wheel of your 12 favorites, opened in front of the
    // left hand): a square quad of its own, fixed in the room while it is open. The game side says where it is each frame it is open
    // (set_wheel: its pose in stage space, its front, +z, toward you; its width in metres; open: which opening it is, 0 closed) and draws its
    // picture on a thread of its own (premultiplied colour in sRGB bytes, as the gun sight's picture is made) whenever what it shows changes
    // (set_wheel_picture). The graphics side copies the newest picture into wheel_swapchain (once per new picture, not every frame; nothing
    // at all while it is closed), and end_frame lays the quad over the world while the swapchain holds a picture of this opening.
    struct WheelLayer {
        XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        // (fix174) The same wheel shown a second time at the other hand (one wheel shown at both hands).
        XrPosef pose2{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        bool second{false};
        float size{0.0f};
        uint32_t open{0};
        int64_t ms{0}; // (steady clock: when it was set; the game side sets it each frame the wheel is open)
    };
    struct WheelPicture {
        std::vector<uint32_t> pixels{}; // (width x height, rows top to bottom, each pixel R G B A from its lowest byte)
        uint32_t width{0};
        uint32_t height{0};
        uint32_t open{0};   // (the opening it was drawn for)
        uint64_t serial{0}; // (one more each picture)
    };
    void set_wheel(const WheelLayer& w) {
        std::scoped_lock _{this->wheel_mtx};
        this->wheel = w;
    }
    // (Wanted: open, set within the last 250 ms, with a size.)
    bool get_wheel(WheelLayer& w, int64_t now_ms) {
        std::scoped_lock _{this->wheel_mtx};
        w = this->wheel;
        return w.open != 0 && w.ms != 0 && now_ms - w.ms < 250 && w.size > 0.05f;
    }
    void set_wheel_picture(std::shared_ptr<const WheelPicture> picture) {
        std::scoped_lock _{this->wheel_mtx};
        this->wheel_picture = std::move(picture);
    }
    std::shared_ptr<const WheelPicture> get_wheel_picture() {
        std::scoped_lock _{this->wheel_mtx};
        return this->wheel_picture;
    }
    std::mutex wheel_mtx{};
    WheelLayer wheel{};
    std::shared_ptr<const WheelPicture> wheel_picture{};
    XrSwapchain wheel_swapchain{XR_NULL_HANDLE};
    int32_t wheel_image_width{0};
    int32_t wheel_image_height{0};
    std::atomic<uint32_t> wheel_image_open{0}; // (the graphics side: the opening whose picture the swapchain holds; 0 none)
    std::atomic<int64_t> wheel_shown_ms{0};    // (a frame taken with it, last: steady clock, ms)
    // Frames refused with it (three: not shown again this session; the game side then gives the favorites back to the game).
    int wheel_refusals{0};
    std::atomic<bool> wheel_refused{false};
    int wheel_logs{0};
    // (fix177) Build mode's console (the game side: BuildConsole). The build menu and its notifications are drawn in fixed boxes of the
    // game's interface layer, their parts copied out once a frame into one picture of their own (D3D12Component::record_console: the parts
    // packed side by side) and the boxes cleared there; each part is shown by a quad of its own, a crop of that one picture (subImage's
    // imageRect, as the favorites wheel shows one picture twice): the item card, the list and the button hints and footer low in front of
    // your seat (the room: stage space), the action cards, warnings and notifications near the middle of your view (your head: view space).
    // The game side says where each goes each frame building mode is drawn (set_console); end_frame lays them over the world while the
    // console's swapchain holds this frame's picture (console_image_ready).
    struct ConsoleQuad {
        uint8_t space{0}; // 0 the room (stage space), 1 your head (view space)
        XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // (its front, +z, toward you)
        float size[2]{0.0f, 0.0f};                                 // (metres)
        int32_t rect[4]{0, 0, 0, 0};                               // its part of the picture: x, y, width, height (pixels)
    };
    static constexpr int kConsoleQuads = 12;
    struct Console {
        ConsoleQuad quads[kConsoleQuads]{};
        int count{0};
        int64_t ms{0}; // (steady clock: when it was set; the game side sets it each frame build mode is drawn on the console)
    };
    void set_console(const Console& c) {
        std::scoped_lock _{this->console_mtx};
        this->console = c;
    }
    // (Wanted: set within the last 200 ms, with a quad.)
    bool get_console(Console& c, int64_t now_ms) {
        std::scoped_lock _{this->console_mtx};
        c = this->console;
        return c.count > 0 && c.ms != 0 && now_ms - c.ms < 200;
    }
    std::mutex console_mtx{};
    Console console{};
    XrSwapchain console_swapchain{XR_NULL_HANDLE};
    int32_t console_image_width{0};
    int32_t console_image_height{0};
    bool console_image_ready{false}; // (the graphics side, this frame: its image filled)
    // When it was last shown (a frame taken with it: steady clock, ms): the game side puts the build menu back in the view (fix175's way)
    // when it has not been shown for a moment.
    std::atomic<int64_t> console_shown_ms{0};
    // Frames refused with it (three in a row: not shown again this session).
    int console_refusals{0};
    std::atomic<bool> console_refused{false};
    int console_logs{0};
    std::atomic<uint32_t> console_dropped{0}; // (its quads left out of frames: the runtime's layer limit)
    // (fix196) The mod's own menu (F11) on a panel of its own: the framework draws it into a picture of its own (not the game's), the
    // graphics side copies that into this swapchain each frame the menu is open (fw_menu_image_ready), and end_frame shows it as a quad
    // fixed in the room (stage space), over everything else, in both eyes whatever the eye mode. It is put in front of you each time the
    // menu opens (fw_menu_place_serial, from the framework), when you recenter (fw_menu_recenter), and stays there while it is open; its
    // size in metres (its height; its width from the picture's shape) and its distance come from the framework's settings.
    XrSwapchain fw_menu_swapchain{XR_NULL_HANDLE};
    int32_t fw_menu_image_width{0};
    int32_t fw_menu_image_height{0};
    bool fw_menu_image_ready{false}; // (the graphics side, this frame: its image filled)
    std::atomic<float> fw_menu_height_m{1.1f};
    std::atomic<float> fw_menu_distance_m{1.75f};
    std::atomic<uint32_t> fw_menu_place_serial{0};
    std::atomic<bool> fw_menu_recenter{false};
    // (The hand the framework takes as pointing, for its beam: -1 none yet.)
    std::atomic<int> fw_menu_hand{-1};
    bool fw_menu_placed{false};
    uint32_t fw_menu_placed_serial{0};
    float fw_menu_yaw{0.0f};
    float fw_menu_head[3]{};
    std::chrono::steady_clock::time_point fw_menu_replace_until{};
    std::chrono::steady_clock::time_point fw_menu_last_shown{}; // (not shown for 2 s: put in front of you again when it next shows)
    int fw_menu_place_logs{0};
    int fw_menu_logs{0};
    // Frames refused with it three times (without the strip's layers: then it is what was refused): not shown again this session (the
    // framework then draws the menu into the game's picture, as before fix196).
    int fw_menu_refusals{0};
    std::atomic<bool> fw_menu_refused{false};
    // Shown in the last frame submitted (for the graphics side: the strip's pictures made for its beam).
    std::atomic<bool> fw_menu_visible{false};
    // When a frame with it was last taken by the runtime (steady clock, ms): the framework draws the menu into the game's picture again
    // for the rest of an opening in which it has not reached the headset for a second.
    std::atomic<int64_t> fw_menu_submitted_ms{0};
    // Where each controller's aim meets it (u across, v down, 0..1 over its picture), for the framework's mouse: pointer_mtx.
    struct FwMenuPointer {
        bool shown{false}; // shown on the last frame
        bool hit[2]{false, false};
        float u[2]{}, v[2]{};
        std::chrono::steady_clock::time_point at{};
    };
    FwMenuPointer fw_menu_pointer{};
    FwMenuPointer get_fw_menu_pointer() {
        std::scoped_lock _{this->pointer_mtx};
        return this->fw_menu_pointer;
    }
    // Puts it in front of you when needed (false: not placed, no head pose yet).
    bool place_fw_menu();
    // (fix177) The most layers the runtime takes in a frame (its system properties; 16 at least by the standard).
    uint32_t max_layers{16};
    // Diagnostics.
    uint32_t stat_panel_frames{0};
    uint32_t stat_frozen_frames{0};
    uint32_t stat_live_frames{0};
    uint32_t stat_cylinder_frames{0};

    // (end_frame's parts for the panel.) Moves the panel in front of you when needed (false: not placed: no pose yet).
    bool update_panel_placement();

    // The menu drawn into the live world's eye pictures (D3D12Component: a shader finds, for each pixel, where that eye's line of
    // sight meets the panel in the room), instead of a panel of its own: Virtual Desktop moved the panel with the head whenever the
    // world's picture was shown with it, while the world itself stayed still. The panel is then where the world is.
    std::atomic<bool> panel_draw_in{true};
    struct DrawnView {
        XrPosef pose{};
        XrFovf fov{};
        bool valid{false};
    };
    std::mutex drawn_mtx{};
    DrawnView drawn[2]{}; // the pose and field of view each eye picture was drawn with (submitted with them)
    uint64_t stat_drawn_frames{0};
    bool frozen_has_menu{false}; // the pictures shown again (the last live pair, held) have the menu drawn into them
    bool drawn_logged{false};
    // For an eye picture about to be drawn (mono: both eyes' one): the shader's numbers (16: the eye's line of sight in the panel's
    // own frame, the panel's shape) and the panel's and pointer dot's (each 4); the pose and field of view used are kept for the
    // frame's submission. False: no panel to draw (not placed, no picture, no head pose).
    bool menu_draw_params(int eye, bool mono, float params[16], float panel[4], float dot[4]);
    // How far your head is turned from the way the panel faces, about the vertical (degrees; -1: no panel placed, no head pose, or
    // looking nearly straight up or down). The present thread, as end_frame.
    float head_turn_from_panel();

    bool body_tracking_supported{false};
    bool body_full_body_supported{false};
    bool body_fidelity_supported{false};
    bool hand_tracking_supported{false};

    PFN_xrCreateBodyTrackerFB pfn_xrCreateBodyTrackerFB{nullptr};
    PFN_xrLocateBodyJointsFB pfn_xrLocateBodyJointsFB{nullptr};
    PFN_xrDestroyBodyTrackerFB pfn_xrDestroyBodyTrackerFB{nullptr};
    PFN_xrRequestBodyTrackingFidelityMETA pfn_xrRequestBodyTrackingFidelityMETA{nullptr};
    PFN_xrCreateHandTrackerEXT pfn_xrCreateHandTrackerEXT{nullptr};
    PFN_xrLocateHandJointsEXT pfn_xrLocateHandJointsEXT{nullptr};
    PFN_xrDestroyHandTrackerEXT pfn_xrDestroyHandTrackerEXT{nullptr};

    XrBodyTrackerFB body_tracker{XR_NULL_HANDLE};
    std::array<XrHandTrackerEXT, 2> hand_trackers{XR_NULL_HANDLE, XR_NULL_HANDLE};

    struct BodyJoints {
        bool active{false};
        bool high_fidelity{false};
        float confidence{0.0f};
        uint32_t joint_count{0};
        std::array<XrBodyJointLocationFB, vrcore_xr::FULL_BODY_JOINT_COUNT_META> joints{};
    } body_joints{};

    struct HandJoints {
        bool active{false};
        int data_source{0};
        std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
    };
    std::array<HandJoints, 2> hand_joints{};

    std::chrono::steady_clock::time_point last_tracking_log{};

public:
    struct InteractionBinding {
        std::string interaction_path_name{};
        std::string action_name{};
    };

    struct ActionVectorAssociation {
        VRRuntime::Hand hand{};
        std::string action_modifier{};
        std::string action_activator{};
        std::vector<VectorActivator> vector_activators{};
    };

    // vrcore: hand-specific face buttons so the game integration can map X/Y and A/B separately.
    static inline std::vector<InteractionBinding> s_bindings_map {
        {"/user/hand/*/input/grip/pose", "pose"},
        {"/user/hand/*/input/trigger", "trigger"},
        {"/user/hand/*/input/squeeze", "grip"},
        {"/user/hand/left/input/x/click", "abuttonleft"},   // oculus touch
        {"/user/hand/left/input/y/click", "bbuttonleft"},   // oculus touch
        {"/user/hand/left/input/a/click", "abuttonleft"},   // index
        {"/user/hand/left/input/b/click", "bbuttonleft"},   // index
        {"/user/hand/right/input/a/click", "abuttonright"},
        {"/user/hand/right/input/b/click", "bbuttonright"},
        {"/user/hand/*/input/thumbstick", "joystick"},
        {"/user/hand/*/input/thumbstick/click", "joystickclick"},
        {"/user/hand/left/input/thumbrest/touch", "thumbresttouchleft"},
        {"/user/hand/right/input/thumbrest/touch", "thumbresttouchright"},
        {"/user/hand/*/input/system/click", "systembutton"},
        {"/user/hand/*/input/menu/click", "systembutton"},
        {"/user/hand/*/input/trackpad", "touchpad"},
        {"/user/hand/*/input/trackpad/click", "touchpadclick"},
        {"/user/hand/*/output/haptic", "haptic"},
    };

    // Touchpad controllers (Vive wands): clicking an edge of the pad acts as a face button.
    static inline std::vector<ActionVectorAssociation> s_action_vector_associations {
        {
            VRRuntime::Hand::LEFT, "touchpad", "touchpadclick", {
            { {0.0f, -1.0f}, "abuttonleft" },
            { {1.0f, 0.0f}, "bbuttonleft" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
        {
            VRRuntime::Hand::RIGHT, "touchpad", "touchpadclick", {
            { {0.0f, -1.0f}, "abuttonright" },
            { {-1.0f, 0.0f}, "bbuttonright" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
    };

    static inline std::vector<std::string> s_supported_controllers {
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/oculus/go_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/microsoft/motion_controller",
        "/interaction_profiles/htc/vive_controller",
        // (fix205) Only suggested when the runtime offers its extension (ext_frame_controller; else skipped, not even tried).
        "/interaction_profiles/valve/frame_controller_valve",
    };

    // (fix205) The Steam Frame's own buttons, suggested for its profile alone (after the table above, whose shared paths it also takes:
    // poses, triggers, squeeze, thumbsticks and their clicks, right A and B, the right menu button, the haptics). All four letters are on
    // its right controller: each goes to the action of the Touch button with the same letter (X and Y are Touch's left-hand buttons, so the
    // game gets the letter printed whatever Swap Controller Inputs says, as fix194 has it). It has no thumbrests: its bumpers (the shoulder
    // buttons above the triggers) stand in for them (the left one's LB tap, the right one's RB held while pressed, as a resting thumb's).
    // Its left D-pad gives the game's D-pad; its left View button the game's View (Back: first / third person), as on a gamepad.
    // ("bumper" is the name in Valve's documentation; "shoulder" is suggested too, for SteamVR versions that renamed it: a path a runtime
    // does not know is left out, the rest kept.)
    static inline std::vector<InteractionBinding> s_frame_bindings {
        {"/user/hand/right/input/x/click", "abuttonleft"},
        {"/user/hand/right/input/y/click", "bbuttonleft"},
        {"/user/hand/left/input/bumper/click", "thumbresttouchleft"},
        {"/user/hand/right/input/bumper/click", "thumbresttouchright"},
        {"/user/hand/left/input/shoulder/click", "thumbresttouchleft"},
        {"/user/hand/right/input/shoulder/click", "thumbresttouchright"},
        {"/user/hand/left/input/dpad_up/click", "dpad_up"},
        {"/user/hand/left/input/dpad_right/click", "dpad_right"},
        {"/user/hand/left/input/dpad_down/click", "dpad_down"},
        {"/user/hand/left/input/dpad_left/click", "dpad_left"},
        {"/user/hand/left/input/view/click", "backbutton"},
    };

    // (fix207) The Valve Index controllers' own extra input, suggested for their profile alone: each trackpad's press (its force, 0 to 1),
    // read as a value (the mod's own press and release points, update_input) so it works the same on every runtime (a runtime that turns
    // a value into a button may want it pressed almost all the way: PimaxXR's 0.95). The Index has no thumbrests: the left pad's press is
    // the left thumbrest's LB, the right pad's the right one's RB (as the Steam Frame's bumpers, real presses).
    static constexpr const char* kIndexControllerProfile = "/interaction_profiles/valve/index_controller";
    static inline std::vector<InteractionBinding> s_index_bindings {
        {"/user/hand/left/input/trackpad/force", "trackpadforce"},
        {"/user/hand/right/input/trackpad/force", "trackpadforce"},
    };
};
}