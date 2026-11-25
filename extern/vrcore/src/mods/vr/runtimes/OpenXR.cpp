// Adapted from REFramework (MIT, praydog) for vrcore.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

#include <nlohmann/json.hpp>
#include <utility/String.hpp>
#include <imgui.h>

#include "Framework.hpp"

#include "mods/vr/runtimes/OpenXR.hpp"
#include "mods/vr/LaserSight.hpp"

using namespace nlohmann;

namespace runtimes {
namespace {
// (fix189) A failure that can come every frame once the session goes wrong (xrWaitFrame, xrBeginFrame, xrEndFrame, the controllers'
// state): logged its first 20 times, then one line saying the rest are not (else the log grows without end). One counter per message.
bool FirstTimes(std::atomic<int>& told, const char* what) {
    if (told.load(std::memory_order_relaxed) > 20) {
        return false;
    }
    const int n = told.fetch_add(1, std::memory_order_relaxed);
    if (n == 20) {
        spdlog::error("[VR] (fix189) {} failed 20 times: further ones are not logged", what);
    }
    return n < 20;
}
}

VRRuntime::Error OpenXR::synchronize_frame() {
    std::scoped_lock _{sync_mtx};

    // cant sync frame between begin and endframe
    if (!this->session_ready || this->frame_began) {
        return VRRuntime::Error::UNSPECIFIED;
    }

    if (this->frame_synced) {
        return VRRuntime::Error::SUCCESS;
    }

    this->begin_profile();

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    this->frame_state = {XR_TYPE_FRAME_STATE};
    auto result = xrWaitFrame(this->session, &frame_wait_info, &this->frame_state);

    this->end_profile("xrWaitFrame");

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrWaitFrame")) {
            spdlog::error("[VR] xrWaitFrame failed: {}", this->get_result_string(result));
        }
        return (VRRuntime::Error)result;
    } else {
        this->got_first_sync = true;
        this->frame_synced = true;
        // (The display time and period of the frame just waited for, for the game thread's poses: pipelined pacing.)
        this->published_period       = this->frame_state.predictedDisplayPeriod;
        this->published_display_time = this->frame_state.predictedDisplayTime;
        ++this->frames_opened;
    }

    return VRRuntime::Error::SUCCESS;
}

namespace {
// (fix163) Poses as glm, and back.
glm::quat QuatOf(const XrPosef& p) { return glm::quat{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}; }
glm::vec3 PosOf(const XrPosef& p) { return glm::vec3{p.position.x, p.position.y, p.position.z}; }
XrPosef PoseOf(const glm::quat& q, const glm::vec3& v) {
    XrPosef p{};
    p.orientation = XrQuaternionf{q.x, q.y, q.z, q.w};
    p.position    = XrVector3f{v.x, v.y, v.z};
    return p;
}
XrPosef Compose(const XrPosef& a, const XrPosef& b) { // (b in a's frame)
    const glm::quat qa = QuatOf(a);
    return PoseOf(glm::normalize(qa * QuatOf(b)), PosOf(a) + qa * PosOf(b));
}
XrPosef InFrameOf(const XrPosef& frame, const XrPosef& p) { // (p in frame's frame)
    const glm::quat inv = glm::inverse(QuatOf(frame));
    return PoseOf(glm::normalize(inv * QuatOf(p)), inv * (PosOf(p) - PosOf(frame)));
}
XrPosef Blend(const XrPosef& a, const XrPosef& b, float t) {
    const float s = t * t * (3.0f - 2.0f * t); // (smooth at both ends)
    return PoseOf(glm::normalize(glm::slerp(QuatOf(a), QuatOf(b), s)), glm::mix(PosOf(a), PosOf(b), s));
}
bool Finite(const XrPosef& p) {
    return std::isfinite(p.position.x) && std::isfinite(p.position.y) && std::isfinite(p.position.z) && std::isfinite(p.orientation.w) &&
           std::isfinite(p.orientation.x) && std::isfinite(p.orientation.y) && std::isfinite(p.orientation.z);
}
constexpr float kHoldSeconds = 1.5f;  // its last tracked pose held this long...
constexpr float kRestSeconds = 1.0f;  // ...then eased to rest over this long
constexpr float kBackSeconds = 0.5f;  // back to the tracked one over this long
// (A tiny speed while held or eased: the mod's swing and flick checks take the runtime's speed when it gives one, else the poses'.)
constexpr XrVector3f kStillSpeed{0.0f, 1e-4f, 0.0f};

// Where a hand rests (on the lap, seated): below and ahead of the eyes, to its side, turned with the head's heading, pointing ahead and
// down. Stage space: y up, -z ahead.
XrPosef RestPose(const XrPosef& head, bool left) {
    const glm::vec3 ahead3 = QuatOf(head) * glm::vec3{0.0f, 0.0f, -1.0f};
    glm::vec3       ahead{ahead3.x, 0.0f, ahead3.z};
    ahead                  = glm::length(ahead) > 1e-3f ? glm::normalize(ahead) : glm::vec3{0.0f, 0.0f, -1.0f};
    const glm::vec3 right  = glm::cross(ahead, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::vec3 at     = PosOf(head) + glm::vec3{0.0f, -0.45f, 0.0f} + ahead * 0.30f + right * (left ? -0.18f : 0.18f);
    const float     yaw    = std::atan2(-ahead.x, -ahead.z);
    const glm::quat turned = glm::angleAxis(yaw, glm::vec3{0.0f, 1.0f, 0.0f}) * glm::angleAxis(glm::radians(-35.0f), glm::vec3{1.0f, 0.0f, 0.0f});
    return PoseOf(glm::normalize(turned), at);
}
}

// (fix163) A hand the runtime does not track now: its pose held, then at rest (HandData::Hold); called after its spaces are located.
void OpenXR::hold_untracked_hand(HandData& hand, bool left) {
    constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    auto&      h       = hand.hold;
    const auto now     = std::chrono::steady_clock::now();
    const bool tracked = hand.active && (hand.location.locationFlags & valid) == valid && Finite(hand.location.pose);
    const auto seconds = [&](std::chrono::steady_clock::time_point since) { return std::chrono::duration<float>(now - since).count(); };
    const bool aim_ok  = (hand.aim_location.locationFlags & valid) == valid && Finite(hand.aim_location.pose);
    if (tracked) {
        if (!h.tracked) {
            h.tracked = true;
            // (Back over half a second only after a real loss, the hand moved to rest or held long: a moment's dropout (a fast swing, a
            // reach behind the shoulder) snaps back at once, its speed the runtime's; the review.)
            h.back_at   = seconds(h.lost_at) > kHoldSeconds ? now : std::chrono::steady_clock::time_point{};
            h.from      = h.shown;
            h.have_rest = false;
            if (h.losses <= 20) {
                spdlog::info("[VR] (fix163) the {} controller is tracked again (lost {:.1f} s): its hand eased back", left ? "left" : "right",
                    seconds(h.lost_at));
            }
        }
        h.have_good = true;
        if (aim_ok) {
            h.aim_in_grip = InFrameOf(hand.location.pose, hand.aim_location.pose);
            h.have_aim    = true;
        }
        const float back = h.back_at != std::chrono::steady_clock::time_point{} ? seconds(h.back_at) : kBackSeconds;
        if (back < kBackSeconds) {
            const XrPosef shown = Blend(h.from, hand.location.pose, back / kBackSeconds);
            hand.location.pose  = shown;
            if (h.have_aim) {
                hand.aim_location.pose          = Compose(shown, h.aim_in_grip);
                hand.aim_location.locationFlags = valid;
            }
            hand.velocity.linearVelocity  = kStillSpeed;
            hand.velocity.angularVelocity = XrVector3f{0.0f, 0.0f, 0.0f};
        }
        h.shown = hand.location.pose;
        return;
    }
    // Not tracked (never yet: left as the runtime gives it, nothing to hold).
    if (!h.have_good) {
        return;
    }
    if (h.tracked) {
        h.tracked = false;
        h.lost_at = now;
        h.from    = h.shown;
        if (++h.losses <= 20) {
            spdlog::info("[VR] (fix163) the {} controller is not tracked (still a while, or out of sight): its hand held where it was, then at rest",
                left ? "left" : "right");
        }
    }
    const float lost  = seconds(h.lost_at);
    XrPosef     shown = h.from;
    if (lost > kHoldSeconds && !h.have_rest && (this->view_space_location.locationFlags & valid) == valid) {
        h.rest      = RestPose(this->view_space_location.pose, left);
        h.have_rest = true;
    }
    if (lost > kHoldSeconds && h.have_rest) {
        shown = Blend(h.from, h.rest, std::min((lost - kHoldSeconds) / kRestSeconds, 1.0f));
    }
    hand.location.pose          = shown;
    hand.location.locationFlags = valid; // (valid, not tracked: a pose given in its place)
    if (h.have_aim) {
        hand.aim_location.pose          = Compose(shown, h.aim_in_grip);
        hand.aim_location.locationFlags = hand.location.locationFlags;
    }
    hand.velocity.linearVelocity  = kStillSpeed;
    hand.velocity.angularVelocity = XrVector3f{0.0f, 0.0f, 0.0f};
    h.shown                       = shown;
}

VRRuntime::Error OpenXR::update_poses() {
    // (Pipelined: the present thread holds sync_mtx while it waits in xrWaitFrame; the game thread does not wait for it, and locates at
    // the display time it was given (pose_display_time), not the frame state the present thread owns.)
    std::unique_lock _{ this->sync_mtx, std::defer_lock };
    if (!this->pipelined) {
        _.lock();
    }
    std::unique_lock __{ this->pose_mtx };

    if (!this->session_ready) {
        return VRRuntime::Error::SUCCESS;
    }

    /*if (!this->needs_pose_update) {
        return VRRuntime::Error::SUCCESS;
    }*/

    this->view_state = {XR_TYPE_VIEW_STATE};
    this->stage_view_state = {XR_TYPE_VIEW_STATE};

    uint32_t view_count{};

    const XrTime predicted = this->pipelined ? this->pose_display_time : this->frame_state.predictedDisplayTime;
    const XrDuration period = this->pipelined ? this->published_period.load() : this->frame_state.predictedDisplayPeriod;

    // Only signal that we got the first valid pose if the display time becomes a sane value
    if (!this->got_first_valid_poses) {
        // Seen on VDXR
        if (predicted <= period) {
            spdlog::info("[VR] Frame state predicted display time is less than predicted display period!");
            return VRRuntime::Error::SUCCESS;
        }

        // Seen on VDXR. If for some reason the above if statement doesn't work, this will catch it.
        if (predicted == 11111111) {
            spdlog::info("[VR] Frame state predicted display time is 11111111!");
            return VRRuntime::Error::SUCCESS;
        }
    }

    if (predicted <= 1000) {
        if (!this->pipelined) {
            spdlog::info("[VR] Frame state predicted display time is less than 1000!");
        }
        return VRRuntime::Error::SUCCESS;
    }

    const auto display_time = predicted + (XrDuration)(period * this->prediction_scale);

    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->view_space;

    auto result = xrLocateViews(this->session, &view_locate_info, &this->view_state, (uint32_t)this->views.size(), &view_count, this->views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    view_locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    result = xrLocateViews(this->session, &view_locate_info, &this->stage_view_state, (uint32_t)this->stage_views.size(), &view_count, this->stage_views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for stage space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    result = xrLocateSpace(this->view_space, this->stage_space, display_time, &this->view_space_location);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateSpace for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    if (this->floor_space != XR_NULL_HANDLE) {
        XrSpaceLocation floor_view{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(this->view_space, this->floor_space, display_time, &floor_view) == XR_SUCCESS &&
            (floor_view.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            this->floor_eye_height = floor_view.pose.position.y;
        }
    }

    this->update_body_hand_tracking(display_time);

    for (auto& hand : this->hands) {
        hand.location.next = &hand.velocity;
        result = xrLocateSpace(hand.space, this->stage_space, display_time, &hand.location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return (VRRuntime::Error)result;
        }

        if (hand.aim_space != XR_NULL_HANDLE) {
            hand.aim_location.next = nullptr;
            if (xrLocateSpace(hand.aim_space, this->stage_space, display_time, &hand.aim_location) != XR_SUCCESS) {
                hand.aim_location.locationFlags = 0;
            }
        }
        hold_untracked_hand(hand, &hand == &this->hands[0]); // (fix163)
    }

    if (!this->got_first_valid_poses) {
        this->got_first_valid_poses = (this->view_space_location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) != 0;
    }

    this->needs_pose_update = false;
    this->got_first_poses = true;
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::store_poses(uint32_t frame) {
    FramePoses fp{};
    {
        std::shared_lock _{this->pose_mtx};
        if (this->stage_views.size() < 2) {
            return;
        }
        fp.frame = frame;
        // (Valid once poses have really been located: the first frames may come before, with nothing located yet, fix136's review.)
        fp.valid = this->got_first_valid_poses;
        fp.stage[0] = this->stage_views[0].pose;
        fp.stage[1] = this->stage_views[1].pose;
        fp.head = this->view_space_location.pose;
        fp.display_time = this->pipelined ? this->pose_display_time : this->frame_state.predictedDisplayTime;
    }
    std::scoped_lock _{this->frame_poses_mtx};
    this->frame_poses[frame % this->frame_poses.size()] = fp;
}

bool OpenXR::frame_poses_of(uint32_t frame, FramePoses& out) {
    std::scoped_lock _{this->frame_poses_mtx};
    const auto& fp = this->frame_poses[frame % this->frame_poses.size()];
    if (!fp.valid || fp.frame != frame) {
        return false;
    }
    out = fp;
    return true;
}

XrResult OpenXR::open_frame() {
    std::scoped_lock _{sync_mtx};

    if (!this->session_ready) {
        return XR_ERROR_SESSION_NOT_READY;
    }
    if (this->frame_began) {
        return XR_SUCCESS;
    }
    // (xrWaitFrame: the headset's pace; returns at once while this frame was already waited for.)
    const auto start = std::chrono::steady_clock::now();
    if (synchronize_frame() != VRRuntime::Error::SUCCESS) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    this->stat_open_waits_us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    this->stat_wait_total_us.fetch_add(this->stat_open_waits_us.load(std::memory_order_relaxed), std::memory_order_relaxed); // (fix179)
    // (Begun once the game thread has located poses for the display times given: at the next present at the very start.)
    if (!this->got_first_poses) {
        return XR_ERROR_SESSION_NOT_READY;
    }
    return begin_frame();
}

VRRuntime::Error OpenXR::update_render_target_size() {
    uint32_t view_count{};
    auto result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, 0, &view_count, nullptr); 
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    this->view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, view_count, &view_count, this->view_configs.data());
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenXR::get_width() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * this->clamped_scale());
}

uint32_t OpenXR::get_height() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * this->clamped_scale());
}

// The resolution scale, lowered if needed so the eye image is no bigger than the runtime's largest (the game window, and so the
// swapchains, are made this size); one factor for both sides, so the shape stays the headset's.
float OpenXR::clamped_scale() const {
    const auto& view  = this->view_configs[0];
    float       scale = this->resolution_scale;
    if (view.maxImageRectWidth > 0 && view.recommendedImageRectWidth > 0) {
        scale = std::min(scale, (float)view.maxImageRectWidth / (float)view.recommendedImageRectWidth);
    }
    if (view.maxImageRectHeight > 0 && view.recommendedImageRectHeight > 0) {
        scale = std::min(scale, (float)view.maxImageRectHeight / (float)view.recommendedImageRectHeight);
    }
    return scale;
}

VRRuntime::Error OpenXR::consume_events(std::function<void(void*)> callback) {
    std::scoped_lock _{sync_mtx};

    XrEventDataBuffer edb{XR_TYPE_EVENT_DATA_BUFFER};
    auto result = xrPollEvent(this->instance, &edb);

    const auto bh = (XrEventDataBaseHeader*)&edb;

    while (result == XR_SUCCESS) {
        spdlog::info("VR: xrEvent: {}", this->get_structure_string(bh->type));

        if (callback) {
            callback(&edb);
        }

        if (bh->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto ev = (XrEventDataSessionStateChanged*)&edb;
            this->session_state = ev->state;

            spdlog::info("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED {}", ev->state);

            if (ev->state == XR_SESSION_STATE_READY) {
                spdlog::info("VR: XR_SESSION_STATE_READY");
                
                // Begin the session
                XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                session_begin_info.primaryViewConfigurationType = this->view_config;

                result = xrBeginSession(this->session, &session_begin_info);

                if (result != XR_SUCCESS) {
                    this->error = std::string{"xrBeginSessionFailed: "} + this->get_result_string(result);
                    spdlog::error("VR: xrBeginSession failed: {}", this->get_result_string(result));
                } else {
                    this->session_ready = true;
                    synchronize_frame();
                }
            } else if (ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                spdlog::info("VR: XR_SESSION_STATE_LOSS_PENDING");
                // (fix189) The remake this asks for (VR::on_begin_rendering) is not safe while the present thread uses the runtime; left as
                // it was, since a lost session works no more without one.
                this->wants_reinitialize = true;
            } else if (ev->state == XR_SESSION_STATE_STOPPING) {
                spdlog::info("VR: XR_SESSION_STATE_STOPPING");

                if (this->ready()) {
                    xrEndSession(this->session);
                    this->session_ready = false;
                    this->frame_synced = false;
                    this->frame_began = false;

                    if (this->wants_reinitialize) {
                        //initialize_openxr();
                    }
                }
            }
        } else if (bh->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            this->wants_reset_origin = true;
            if (this->space_change_logs++ < 20) {
                const auto* change = (const XrEventDataReferenceSpaceChangePending*)bh;
                spdlog::info("[VR] the headset says a room space moves (a recenter): space type {} (1 view, 2 local, 3 stage), pose valid {}",
                    (int)change->referenceSpaceType, change->poseValid ? "yes" : "no");
            }
        }

        edb = {XR_TYPE_EVENT_DATA_BUFFER};
        result = xrPollEvent(this->instance, &edb);
    } 
    
    if (result != XR_EVENT_UNAVAILABLE) {
        spdlog::error("VR: xrPollEvent failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_matrices(float nearz, float farz) {
    if (!this->session_ready || this->views.empty()) {
        return VRRuntime::Error::SUCCESS;
    }

    std::unique_lock __{ this->eyes_mtx };
    std::unique_lock ___{ this->pose_mtx };

    for (auto i = 0; i < 2; ++i) {
        const auto& pose = this->views[i].pose;
        const auto& fov = this->views[i].fov;

        // Update projection matrix
        XrMatrix4x4f_CreateProjection((XrMatrix4x4f*)&this->projections[i], GRAPHICS_D3D, tan(fov.angleLeft), tan(fov.angleRight), tan(fov.angleUp), tan(fov.angleDown), nearz, farz);

        // Update view matrix
        this->eyes[i] = Matrix4x4f{*(glm::quat*)&pose.orientation};
        this->eyes[i][3] = Vector4f{*(Vector3f*)&pose.position, 1.0f};
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_input() {
    if (!this->ready() || this->session_state.load() != XR_SESSION_STATE_FOCUSED) {
        // (fix189) Not focused: the touchpad edges' presses worked out at the last sync are let go too (the getters give nothing held).
        this->input_synced = false;
        for (auto& hand : this->hands) {
            if (!hand.forced_actions.empty()) {
                hand.forced_actions.clear();
            }
        }
        return (VRRuntime::Error)XR_ERROR_SESSION_NOT_READY;
    }

    XrActiveActionSet active_action_set{this->action_set.handle, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;

    auto result = xrSyncActions(this->session, &sync_info);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrSyncActions")) {
            spdlog::error("[VR] Failed to sync actions: {}", this->get_result_string(result));
        }

        return (VRRuntime::Error)result;
    }
    this->input_synced = true; // (fix189) the actions' state is this focus's

    const auto current_interaction_profile = this->get_current_interaction_profile();

    for (auto i = 0; i < 2; ++i) {
        auto& hand = this->hands[i];
        hand.forced_actions.clear();

        // Update controller pose state
        XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
        get_info.subactionPath = hand.path;
        get_info.action = this->action_set.action_map["pose"];

        XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

        result = xrGetActionStatePose(this->session, &get_info, &pose_state);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

            return (VRRuntime::Error)result;
        }

        hand.active = pose_state.isActive;

        // Handle vector activator stuff
        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            if (this->is_action_active(activator, (VRRuntime::Hand)i)) {
                const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)i);
                
                for (const auto& output : it.second) {
                    const auto distance = glm::length(output.value - axis);

                    if (distance < 0.7f) {
                        hand.forced_actions[output.action] = true;
                    }
                }
            }
        }

        if (this->is_action_active_once("systembutton", (VRRuntime::Hand)i)) {
            this->handle_pause = true;
        }
    }

    // TODO: Other non-hand specific inputs
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::destroy() {
    if (!this->loaded) {
        return;
    }

    std::scoped_lock _{sync_mtx};

    if (this->session != nullptr) {
        this->destroy_body_hand_tracking();

        if (this->session_ready) {
            xrEndSession(this->session);
        }

        xrDestroySession(this->session);
    }
    // The gun sight's swapchain went with the session.
    this->reticle_ready = false;
    this->reticle_swapchain = XR_NULL_HANDLE;
    this->reticle_skip_frames = 0;
    this->reticle_suspect = false;
    this->reticle_refusals = 0;
    // So did the menu panel's (the graphics side makes a new one with the next session).
    this->panel_swapchain = XR_NULL_HANDLE;
    this->panel_image_width = 0;
    this->panel_image_height = 0;
    this->panel_image_ready = false;
    this->frozen_ready = false;
    this->live_world_ready = false;
    // (fix164) And the soft light's.
    this->ambient_swapchain = XR_NULL_HANDLE;
    this->ambient_image_width = 0;
    this->ambient_image_height = 0;
    this->ambient_ready = false;
    {
        std::scoped_lock pointer_lock{this->pointer_mtx};
        this->panel_pointer = {};
        this->hud_pointer = {};
    }

    if (this->instance != nullptr) {
        xrDestroyInstance(this->instance);
        this->instance = nullptr;
    }

    this->session = nullptr;
    this->session_ready = false;
    this->system = XR_NULL_SYSTEM_ID;
    this->frame_synced = false;
    this->frame_began = false;
}

// Body and hand tracking, after SFVR's vrframework (MIT, AetherianArtificer).
void OpenXR::initialize_body_hand_tracking() {
    const auto get_proc = [&](const char* name, auto& pfn) {
        if (xrGetInstanceProcAddr(this->instance, name, (PFN_xrVoidFunction*)&pfn) != XR_SUCCESS) {
            pfn = nullptr;
        }
    };

    if (this->body_tracking_supported && this->body_tracker == XR_NULL_HANDLE) {
        get_proc("xrCreateBodyTrackerFB", this->pfn_xrCreateBodyTrackerFB);
        get_proc("xrLocateBodyJointsFB", this->pfn_xrLocateBodyJointsFB);
        get_proc("xrDestroyBodyTrackerFB", this->pfn_xrDestroyBodyTrackerFB);
        if (this->body_fidelity_supported) {
            get_proc("xrRequestBodyTrackingFidelityMETA", this->pfn_xrRequestBodyTrackingFidelityMETA);
        }

        if (this->pfn_xrCreateBodyTrackerFB != nullptr && this->pfn_xrLocateBodyJointsFB != nullptr) {
            XrBodyTrackerCreateInfoFB create_info{vrcore_xr::TYPE_BODY_TRACKER_CREATE_INFO_FB};
            create_info.bodyJointSet = this->body_full_body_supported ? vrcore_xr::BODY_JOINT_SET_FULL_BODY : vrcore_xr::BODY_JOINT_SET_DEFAULT;

            const auto result = this->pfn_xrCreateBodyTrackerFB(this->session, &create_info, &this->body_tracker);
            if (result == XR_SUCCESS) {
                this->body_joints = {};
                this->body_joints.joint_count = this->body_full_body_supported ? vrcore_xr::FULL_BODY_JOINT_COUNT_META : vrcore_xr::BODY_JOINT_COUNT_FB;
                spdlog::info("[VR] Body tracker created ({} joints)", this->body_joints.joint_count);

                if (this->pfn_xrRequestBodyTrackingFidelityMETA != nullptr) {
                    const auto fidelity_result = this->pfn_xrRequestBodyTrackingFidelityMETA(this->body_tracker, vrcore_xr::BODY_TRACKING_FIDELITY_HIGH);
                    spdlog::info("[VR] Body tracking high fidelity request: {}", this->get_result_string(fidelity_result).c_str());
                }
            } else {
                this->body_tracker = XR_NULL_HANDLE;
                spdlog::error("[VR] xrCreateBodyTrackerFB failed: {}", this->get_result_string(result).c_str());
            }
        } else {
            spdlog::error("[VR] Body tracking functions unavailable");
        }
    }

    if (this->hand_tracking_supported) {
        get_proc("xrCreateHandTrackerEXT", this->pfn_xrCreateHandTrackerEXT);
        get_proc("xrLocateHandJointsEXT", this->pfn_xrLocateHandJointsEXT);
        get_proc("xrDestroyHandTrackerEXT", this->pfn_xrDestroyHandTrackerEXT);

        if (this->pfn_xrCreateHandTrackerEXT == nullptr || this->pfn_xrLocateHandJointsEXT == nullptr) {
            spdlog::error("[VR] Hand tracking functions unavailable");
            return;
        }

        XrHandTrackingDataSourceEXT sources[] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
        XrHandTrackingDataSourceInfoEXT source_info{vrcore_xr::TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
        source_info.requestedDataSourceCount = (uint32_t)std::size(sources);
        source_info.requestedDataSources = sources;

        for (auto i = 0; i < 2; ++i) {
            if (this->hand_trackers[i] != XR_NULL_HANDLE) {
                continue;
            }

            XrHandTrackerCreateInfoEXT create_info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            create_info.hand = i == VRRuntime::Hand::LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            create_info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            create_info.next = ext_hand_data_source ? &source_info : nullptr;

            auto result = this->pfn_xrCreateHandTrackerEXT(this->session, &create_info, &this->hand_trackers[i]);
            if (result != XR_SUCCESS && create_info.next != nullptr) {
                spdlog::warn("[VR] xrCreateHandTrackerEXT with data sources failed: {}, retrying without", this->get_result_string(result).c_str());
                create_info.next = nullptr;
                result = this->pfn_xrCreateHandTrackerEXT(this->session, &create_info, &this->hand_trackers[i]);
            }

            if (result == XR_SUCCESS) {
                this->hand_joints[i] = {};
                spdlog::info("[VR] Hand tracker created ({})", i == VRRuntime::Hand::LEFT ? "left" : "right");
            } else {
                this->hand_trackers[i] = XR_NULL_HANDLE;
                spdlog::error("[VR] xrCreateHandTrackerEXT ({}) failed: {}", i == VRRuntime::Hand::LEFT ? "left" : "right",
                    this->get_result_string(result).c_str());
            }
        }
    }
}

void OpenXR::destroy_body_hand_tracking() {
    std::unique_lock _{this->pose_mtx};

    if (this->body_tracker != XR_NULL_HANDLE && this->pfn_xrDestroyBodyTrackerFB != nullptr) {
        this->pfn_xrDestroyBodyTrackerFB(this->body_tracker);
    }
    this->body_tracker = XR_NULL_HANDLE;
    this->body_joints = {};

    for (auto i = 0; i < 2; ++i) {
        if (this->hand_trackers[i] != XR_NULL_HANDLE && this->pfn_xrDestroyHandTrackerEXT != nullptr) {
            this->pfn_xrDestroyHandTrackerEXT(this->hand_trackers[i]);
        }
        this->hand_trackers[i] = XR_NULL_HANDLE;
        this->hand_joints[i] = {};
    }
}

// Called from update_poses with pose_mtx held; failures only mark the data inactive.
void OpenXR::update_body_hand_tracking(XrTime display_time) {
    if (this->body_tracker != XR_NULL_HANDLE && this->pfn_xrLocateBodyJointsFB != nullptr) {
        auto& body = this->body_joints;

        XrBodyJointsLocateInfoFB locate_info{vrcore_xr::TYPE_BODY_JOINTS_LOCATE_INFO_FB};
        locate_info.baseSpace = this->stage_space;
        locate_info.time = display_time;

        XrBodyTrackingFidelityStatusMETA fidelity_status{vrcore_xr::TYPE_BODY_TRACKING_FIDELITY_STATUS_META};
        XrBodyJointLocationsFB locations{vrcore_xr::TYPE_BODY_JOINT_LOCATIONS_FB};
        locations.next = this->body_fidelity_supported ? &fidelity_status : nullptr;
        locations.jointCount = body.joint_count;
        locations.jointLocations = body.joints.data();

        if (this->pfn_xrLocateBodyJointsFB(this->body_tracker, &locate_info, &locations) == XR_SUCCESS) {
            body.active = locations.isActive == XR_TRUE;
            body.confidence = locations.confidence;
            body.high_fidelity = this->body_fidelity_supported && fidelity_status.fidelity == vrcore_xr::BODY_TRACKING_FIDELITY_HIGH;
        } else {
            body.active = false;
            body.confidence = 0.0f;
        }
    }

    for (auto i = 0; i < 2; ++i) {
        if (this->hand_trackers[i] == XR_NULL_HANDLE || this->pfn_xrLocateHandJointsEXT == nullptr) {
            continue;
        }

        auto& hand = this->hand_joints[i];

        XrHandJointsLocateInfoEXT locate_info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        locate_info.baseSpace = this->stage_space;
        locate_info.time = display_time;

        XrHandTrackingDataSourceStateEXT source_state{vrcore_xr::TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.next = ext_hand_data_source ? &source_state : nullptr;
        locations.jointCount = (uint32_t)hand.joints.size();
        locations.jointLocations = hand.joints.data();

        if (this->pfn_xrLocateHandJointsEXT(this->hand_trackers[i], &locate_info, &locations) == XR_SUCCESS) {
            hand.active = locations.isActive == XR_TRUE;
            hand.data_source = ext_hand_data_source && source_state.isActive == XR_TRUE ? (int)source_state.dataSource : 0;
        } else {
            hand.active = false;
            hand.data_source = 0;
        }
    }

    if (this->body_tracker == XR_NULL_HANDLE && this->hand_trackers[0] == XR_NULL_HANDLE && this->hand_trackers[1] == XR_NULL_HANDLE) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - this->last_tracking_log < std::chrono::seconds(10)) {
        return;
    }
    this->last_tracking_log = now;

    uint32_t valid_joints = 0;
    constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    for (uint32_t j = 0; j < this->body_joints.joint_count; ++j) {
        if ((this->body_joints.joints[j].locationFlags & valid) == valid) {
            ++valid_joints;
        }
    }

    const auto& l = this->hand_joints[VRRuntime::Hand::LEFT];
    const auto& r = this->hand_joints[VRRuntime::Hand::RIGHT];
    spdlog::info("[VR] Body: active {} conf {:.2f} fidelity {} valid joints {}/{} | hands L {} (source {}) R {} (source {})",
        this->body_joints.active, this->body_joints.confidence, !this->body_fidelity_supported ? "n/a" : (this->body_joints.high_fidelity ? "high" : "low"),
        this->body_joints.active ? valid_joints : 0, this->body_joints.joint_count,
        this->hand_trackers[0] == XR_NULL_HANDLE ? "none" : (l.active ? "active" : "inactive"), l.data_source,
        this->hand_trackers[1] == XR_NULL_HANDLE ? "none" : (r.active ? "active" : "inactive"), r.data_source);
}

OpenXR::FingerInput OpenXR::get_finger_input(VRRuntime::Hand hand) const {
    FingerInput out{};
    // (fix189) Not focused: no grip or trigger held either (the game side grabs and climbs by these).
    if (hand > VRRuntime::Hand::RIGHT || this->session == XR_NULL_HANDLE || !this->input_focused()) {
        return out;
    }
    const auto& map = this->action_set.action_map;
    const auto path = this->hands[hand].path;
    auto read_float = [&](const char* name, float& value) {
        if (auto it = map.find(name); it != map.end()) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
            info.action = it->second;
            info.subactionPath = path;
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            if (xrGetActionStateFloat(this->session, &info, &state) == XR_SUCCESS && state.isActive) {
                value = state.currentState;
            }
        }
    };
    auto read_bool = [&](const char* name, bool& value) {
        if (auto it = map.find(name); it != map.end()) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
            info.action = it->second;
            info.subactionPath = path;
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (xrGetActionStateBoolean(this->session, &info, &state) == XR_SUCCESS && state.isActive) {
                value = state.currentState == XR_TRUE;
            }
        }
    };
    read_float("fingertrigger", out.trigger);
    read_float("fingergrip", out.grip);
    read_bool("fingertriggertouch", out.trigger_touch);
    read_bool("fingerthumbtouch", out.thumb_touch);
    return out;
}

std::string OpenXR::get_result_string(XrResult result) const {
    std::string result_string{};
    result_string.resize(XR_MAX_RESULT_STRING_SIZE);
    xrResultToString(this->instance, result, result_string.data());

    return result_string;
}

std::string OpenXR::get_structure_string(XrStructureType type) const {
    std::string structure_string{};
    structure_string.resize(XR_MAX_STRUCTURE_NAME_SIZE);
    xrStructureTypeToString(this->instance, type, structure_string.data());

    return structure_string;
}

std::string OpenXR::get_path_string(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return "XR_NULL_PATH";
    }

    std::string path_string{};
    path_string.resize(XR_MAX_PATH_LENGTH);

    uint32_t real_size{};

    if (auto result = xrPathToString(this->instance, path, XR_MAX_PATH_LENGTH, &real_size, path_string.data()); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path string: {}", this->get_result_string(result));
        return "";
    }

    path_string.resize(real_size-1);
    return path_string;
}

XrPath OpenXR::get_path(const std::string& path) const {
    XrPath path_handle{XR_NULL_PATH};

    if (auto result = xrStringToPath(this->instance, path.c_str(), &path_handle); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path: {}", this->get_result_string(result));
        return XR_NULL_PATH;
    }

    return path_handle;
}

std::string OpenXR::get_current_interaction_profile() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return "";
    }

    return this->get_path_string(state.interactionProfile);
}

XrPath OpenXR::get_current_interaction_profile_path() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }

    return state.interactionProfile;
}

std::optional<std::string> OpenXR::initialize_actions(const std::string& json_string) {
    spdlog::info("[VR] Initializing actions");

    if (auto result = xrStringToPath(this->instance, "/user/hand/left", &this->hands[VRRuntime::Hand::LEFT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (left): " + this->get_result_string(result);
    }

    if (auto result = xrStringToPath(this->instance, "/user/hand/right", &this->hands[VRRuntime::Hand::RIGHT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (right): " + this->get_result_string(result);
    }

    std::array<XrPath, 2> hand_paths{
        this->hands[VRRuntime::Hand::LEFT].path, 
        this->hands[VRRuntime::Hand::RIGHT].path
    };

    if (json_string.empty()) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating action set");

    XrActionSetCreateInfo action_set_create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_create_info.actionSetName, "defaultopenxr");
    strcpy(action_set_create_info.localizedActionSetName, "Default");
    action_set_create_info.priority = 0;

    if (auto result = xrCreateActionSet(this->instance, &action_set_create_info, &this->action_set.handle); result != XR_SUCCESS) {
        return "xrCreateActionSet failed: " + this->get_result_string(result);
    }

    // Parse the JSON string using nlohmann
    json actions_json{};

    try {
        actions_json = json::parse(json_string);
    } catch (const std::exception& e) {
        return std::string{"json parse failed: "} + e.what();
    }

    if (actions_json.count("actions") == 0) {
        return "json missing actions";
    }

    auto actions_list = actions_json["actions"];
    bool has_pose_action = false;

    std::unordered_map<std::string, std::vector<XrActionSuggestedBinding>> profile_bindings{};

    for (const auto& controller : s_supported_controllers) {
        profile_bindings[controller] = {};
    }

    auto attempt_add_binding = [&](const std::string& interaction_profile, const XrActionSuggestedBinding& binding) -> bool {
        XrPath interaction_profile_path{};
        auto result = xrStringToPath(this->instance, interaction_profile.c_str(), &interaction_profile_path);
        auto& bindings = profile_bindings[interaction_profile];

        if (result == XR_SUCCESS) {
            bindings.push_back(binding);

            XrInteractionProfileSuggestedBinding suggested_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested_bindings.interactionProfile = interaction_profile_path;
            suggested_bindings.countSuggestedBindings = (uint32_t)bindings.size();
            suggested_bindings.suggestedBindings = bindings.data();

            result = xrSuggestInteractionProfileBindings(this->instance, &suggested_bindings);

            if (result != XR_SUCCESS) {
                bindings.pop_back();
                spdlog::info("Bad binding passed to xrSuggestInteractionProfileBindings from {}: {}", interaction_profile, this->get_result_string(result));
                return false;
            }

            return true;
        } else {
            spdlog::info("Bad interaction profile passed to xrStringToPath: {}", this->get_result_string(result));
            return false;
        }
    };

    for (auto& action : actions_list) {
        XrActionCreateInfo action_create_info{XR_TYPE_ACTION_CREATE_INFO};
        auto action_name = action["name"].get<std::string>();

        if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
            action_name = action_name.substr(it + 1);
        }

        auto localized_action_name = action_name;
        std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);

        strcpy(action_create_info.actionName, action_name.c_str());
        strcpy(action_create_info.localizedActionName, localized_action_name.c_str());

        action_create_info.countSubactionPaths = (uint32_t)hand_paths.size();
        action_create_info.subactionPaths = hand_paths.data();

        if (action_name == "pose") {
            has_pose_action = true;
        }

        std::unordered_set<XrAction>* out_actions = nullptr;

        // Translate the OpenVR action types to OpenXR action types
        switch (utility::hash(action["type"].get<std::string>())) {
            case "boolean"_fnv:
                if (action["type"].get<std::string>().ends_with("/value")) {
                    action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                    out_actions = &this->action_set.float_actions;
                } else {
                    action_create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                    out_actions = &this->action_set.bool_actions;
                }
                
                break;
            case "skeleton"_fnv: // idk what this is in OpenXR
                continue;
            case "pose"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                out_actions = &this->action_set.pose_actions;
                break;
            case "vector1"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                out_actions = &this->action_set.float_actions;
                break;
            case "vector2"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                out_actions = &this->action_set.vector2_actions;
                break;
            case "vibration"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                out_actions = &this->action_set.vibration_actions;
                break;
            default:
                continue;
        }
        
        // Create the action
        XrAction xr_action{XR_NULL_HANDLE};
        if (auto result = xrCreateAction(this->action_set.handle, &action_create_info, &xr_action); result != XR_SUCCESS) {
            return "xrCreateAction failed for " + action_name + ": " + this->get_result_string(result);
        }

        if (out_actions != nullptr) {
            out_actions->insert(xr_action);
        }

        spdlog::info("[VR] Created action {} with handle {:x}", action_name, (uintptr_t)xr_action);

        this->action_set.actions.push_back(xr_action);
        this->action_set.action_map[action_name] = xr_action;
        this->action_set.action_names[xr_action] = action_name;

        // Suggest bindings
        for (const auto& map_it : OpenXR::s_bindings_map) {
            if (map_it.action_name != action_name) {
                continue;
            }

            const auto& interaction_string = map_it.interaction_path_name;

            for (auto i = 0; i < 2; ++i) {
                auto hand_string = interaction_string;
                auto it = hand_string.find('*');
                auto index = i;
                bool wildcard = false;

                if (it != std::string::npos) {
                    if (i == VRRuntime::Hand::LEFT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "left");
                    } else if (i == VRRuntime::Hand::RIGHT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "right");
                    }

                    wildcard = true;
                } else {
                    if (hand_string.find("left") != std::string::npos) {
                        index = VRRuntime::Hand::LEFT;
                    } else if (hand_string.find("right") != std::string::npos) {
                        index = VRRuntime::Hand::RIGHT;
                    }
                }

                spdlog::info("[VR] {}", hand_string);

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, hand_string.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", hand_string);

                    if (!wildcard) {
                        break;
                    }

                    continue;
                }

                if (this->action_set.action_map.contains(map_it.action_name)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[map_it.action_name], p })) {
                            this->hands[index].profiles[controller].path_map[map_it.action_name] = p;
                        }
                    }
                }

                if (!wildcard) {
                    break;
                }
            }
        }
    }

    if (!has_pose_action) {
        return "json missing pose action";
    }

    // Check for json files that will override the default suggested bindings
    for (const auto& controller : s_supported_controllers) {
        // Create default action vector associations
        for (const auto& association : s_action_vector_associations) {
            auto& hand = this->hands[association.hand];
            auto& hand_profile = hand.profiles[controller];
            auto& action_map = this->action_set.action_map;
            const auto action_activator = action_map[association.action_activator];
            const auto action_modifier = action_map[association.action_modifier];

            hand_profile.action_vector_associations[action_activator] = action_modifier;

            for (const auto& vector_activator : association.vector_activators) {
                const auto output_action = action_map[vector_activator.action_name];
                hand_profile.vector_activators[action_activator].push_back({ vector_activator.value, output_action });
            }
        }

        auto filename = controller + ".json";

        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        filename = (Framework::get_persistent_dir() / filename).string();

        // check if the file exists
        if (std::filesystem::exists(filename)) {
            spdlog::info("[VR] Loading bindings for {}", filename);

            profile_bindings[controller].clear();

            this->hands[VRRuntime::Hand::LEFT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].path_map.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].path_map.clear();

            // load the json file
            auto j = nlohmann::json::parse(std::ifstream(filename));

            for (auto it : j["bindings"]) {
                auto action_str = it["action"].get<std::string>();
                auto path_str = it["path"].get<std::string>();

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, path_str.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", path_str);
                    continue;
                }

                const auto hand_idx = path_str.find("/left/") != std::string::npos ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;

                if (this->action_set.action_map.contains(action_str)) {
                    if (attempt_add_binding(controller, { this->action_set.action_map[action_str], p })) {
                        this->hands[hand_idx].profiles[controller].path_map[action_str] = p;
                    }
                }
            }

            for (auto it : j["vector2_associations"]) {
                const auto activator_name = it["activator"].get<std::string>();
                const auto modifier_name = it["modifier"].get<std::string>();
                const auto path_name = it["path"].get<std::string>();
                const auto path = this->get_path(path_name);

                for (auto output : it["outputs"]) {
                    const auto action_name = output["action"].get<std::string>();
                    
                    auto value_json = output["value"];
                    const auto value = Vector2f{value_json["x"].get<float>(), value_json["y"].get<float>()};

                    if (this->action_set.action_map.contains(action_name)) {
                        auto& hand = path == this->hands[VRRuntime::Hand::LEFT].path ? this->hands[VRRuntime::Hand::LEFT] : this->hands[VRRuntime::Hand::RIGHT];
                        auto& hand_profile = hand.profiles[controller];

                        spdlog::info("[VR] Adding vector2 association for {} {}", controller, path_name);

                        const auto output_action = this->action_set.action_map[action_name];
                        const auto action_modifier = this->action_set.action_map[modifier_name];
                        const auto action_activator = this->action_set.action_map[activator_name];

                        hand_profile.vector_activators[action_activator].push_back({ value, output_action });
                        hand_profile.action_vector_associations[action_activator] = action_modifier;
                    }
                }
            }
        }
    }

    // Extra actions for the shown body (after SFVR's vrframework): the controllers' aim pose, and
    // analog trigger/grip plus touch sensing for finger poses. Added after any binding files so
    // they are always bound; a path a controller lacks is simply rejected for that controller.
    {
        auto create_extra = [&](const char* name, XrActionType type, std::initializer_list<const char*> components) -> XrAction {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy(info.actionName, name);
            strcpy(info.localizedActionName, name);
            info.actionType = type;
            info.countSubactionPaths = (uint32_t)hand_paths.size();
            info.subactionPaths = hand_paths.data();
            XrAction action{XR_NULL_HANDLE};
            if (auto result = xrCreateAction(this->action_set.handle, &info, &action); result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to create {} action: {}", name, this->get_result_string(result).c_str());
                return XR_NULL_HANDLE;
            }
            this->action_set.actions.push_back(action);
            this->action_set.action_map[name] = action;
            this->action_set.action_names[action] = name;
            if (type == XR_ACTION_TYPE_POSE_INPUT) {
                this->action_set.pose_actions.insert(action);
            }
            for (auto i = 0; i < 2; ++i) {
                const std::string hand = i == VRRuntime::Hand::LEFT ? "/user/hand/left/input/" : "/user/hand/right/input/";
                for (const auto* component : components) {
                    XrPath p{XR_NULL_PATH};
                    if (xrStringToPath(this->instance, (hand + component).c_str(), &p) != XR_SUCCESS) {
                        continue;
                    }
                    for (const auto& controller : s_supported_controllers) {
                        attempt_add_binding(controller, {action, p});
                    }
                }
            }
            return action;
        };
        create_extra("aimpose", XR_ACTION_TYPE_POSE_INPUT, {"aim/pose"});
        create_extra("fingertrigger", XR_ACTION_TYPE_FLOAT_INPUT, {"trigger/value"});
        create_extra("fingergrip", XR_ACTION_TYPE_FLOAT_INPUT, {"squeeze/value"});
        create_extra("fingertriggertouch", XR_ACTION_TYPE_BOOLEAN_INPUT, {"trigger/touch"});
        create_extra("fingerthumbtouch", XR_ACTION_TYPE_BOOLEAN_INPUT,
            {"thumbstick/touch", "thumbrest/touch", "a/touch", "b/touch", "x/touch", "y/touch"});
    }

    // Create the action spaces for each hand
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating action space for hand {}", i);

        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["pose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }

        if (auto it = this->action_set.action_map.find("aimpose"); it != this->action_set.action_map.end() && it->second != XR_NULL_HANDLE) {
            XrActionSpaceCreateInfo aim_space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            aim_space_info.action = it->second;
            aim_space_info.subactionPath = this->hands[i].path;
            aim_space_info.poseInActionSpace.orientation.w = 1.0f;
            if (xrCreateActionSpace(this->session, &aim_space_info, &this->hands[i].aim_space) != XR_SUCCESS) {
                this->hands[i].aim_space = XR_NULL_HANDLE;
            }
        }
    }

    // Attach the action set to the session
    spdlog::info("[VR] Attaching action set to session");

    XrSessionActionSetsAttachInfo action_sets_attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    action_sets_attach_info.countActionSets = 1;
    action_sets_attach_info.actionSets = &this->action_set.handle;

    if (auto result = xrAttachSessionActionSets(this->session, &action_sets_attach_info); result != XR_SUCCESS) {
        return "xrAttachSessionActionSets failed: " + this->get_result_string(result);
    }

    return std::nullopt;
}

bool OpenXR::is_action_active(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT || !this->input_focused()) { // (fix189: nothing held while not focused)
        return false;
    }

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    if (this->action_set.bool_actions.contains(action)) {
        XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
        auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            static std::atomic<int> told{0}; // (fix189)
            if (FirstTimes(told, "xrGetActionStateBoolean")) {
                spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            }
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
    } else if (this->action_set.float_actions.contains(action)) {
        XrActionStateFloat active{XR_TYPE_ACTION_STATE_FLOAT};
        auto result = xrGetActionStateFloat(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            static std::atomic<int> told{0}; // (fix189)
            if (FirstTimes(told, "xrGetActionStateFloat")) {
                spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            }
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState > 0.0f;
    } // idk?

    return false;
}

bool OpenXR::is_action_active(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT || !this->input_focused()) { // (fix189)
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrGetActionStateBoolean (by name)")) {
            spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        }
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
}

bool OpenXR::is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT || !this->input_focused()) { // (fix189)
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrGetActionStateBoolean (once)")) {
            spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        }
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE && active.changedSinceLastSync == XR_TRUE;
}

Vector2f OpenXR::get_action_axis(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT || !this->input_focused()) { // (fix189: no stick held while not focused)
        return Vector2f{};
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrGetActionStateVector2f")) {
            spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        }
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

std::string OpenXR::translate_openvr_action_name(std::string action_name) const {
    if (action_name.empty()) {
        return action_name;
    }

    if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
        action_name = action_name.substr(it + 1);
    }

    std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);
    return action_name;
}

Vector2f OpenXR::get_left_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick") || !this->input_focused()) { // (fix189: no stick held while not focused)
        return Vector2f{};
    }

    const auto& hand = this->hands[VRRuntime::Hand::LEFT];
    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    auto joystick_action = this->action_set.action_map.find("joystick")->second;
    auto touchpad_action = this->action_set.action_map.find("touchpad")->second;

    auto action = hand_profile.path_map.contains("joystick") ? joystick_action : touchpad_action;

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrGetActionStateVector2f (left stick)")) {
            spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        }
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

Vector2f OpenXR::get_right_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick") || !this->input_focused()) { // (fix189: no stick held while not focused)
        return Vector2f{};
    }

    const auto& hand = this->hands[VRRuntime::Hand::RIGHT];
    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    auto joystick_action = this->action_set.action_map.find("joystick")->second;
    auto touchpad_action = this->action_set.action_map.find("touchpad")->second;

    auto action = hand_profile.path_map.contains("joystick") ? joystick_action : touchpad_action;

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrGetActionStateVector2f (right stick)")) {
            spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        }
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

void OpenXR::trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = frequency;

    // cast the duration from seconds to nanoseconds
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>(duration)).count();

    auto result = xrApplyHapticFeedback(this->session, &haptic_info, (XrHapticBaseHeader*)&vibration);

    // XR_SESSION_NOT_FOCUSED is a success code (no vibration while another app has focus).
    if (XR_FAILED(result)) {
        spdlog::error("[VR] Failed to apply haptic feedback: {}", this->get_result_string(result));
    }
}

// (fix173) What a hand's controller plays now stopped (before a pattern of its own: a buzz still playing is not left to run on under it).
void OpenXR::stop_haptic_vibration(VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }
    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;
    const auto result = xrStopHapticFeedback(this->session, &haptic_info);
    if (XR_FAILED(result)) {
        static int told = 0;
        if (told++ < 5) {
            spdlog::error("[VR] (fix173) Failed to stop haptic feedback: {}", this->get_result_string(result));
        }
    }
}

void OpenXR::display_bindings_editor() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    ImGui::Text("Interaction Profile: %s", current_interaction_profile.c_str());

    if (ImGui::Button("Restore Default Bindings")) {
        auto filename = current_interaction_profile + ".json";
        
        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        if (std::filesystem::exists(filename)) {
            // Delete the file
            std::filesystem::remove(filename);
            this->wants_reinitialize = true;
        }
    }

    if (ImGui::Button("Save Bindings")) {
        this->save_bindings();
    }

    auto display_hand = [&](const std::string& name, uint32_t index) {
        if (current_interaction_profile.empty() || current_interaction_profile == "XR_NULL_PATH") {
            ImGui::Text("Interaction profile not loaded, try putting on your headset.");
            return;
        }

        if (ImGui::TreeNode(name.data())) {
            ImGui::PushID(name.data());

            auto& hand = this->hands[index];

            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                if (this->is_action_active(activator, (VRRuntime::Hand)index)) {
                    const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)index);
                    
                    for (const auto& output : it.second) {
                        const auto distance = glm::length(output.value - axis);
                        ImGui::Text("%s: %.2f", this->action_set.action_names[output.action].data(), distance);
                    }
                }
            }

            auto& path_map = hand.profiles[current_interaction_profile].path_map;

            std::vector<std::string> known_actions{};
            std::vector<const char*> known_actions_cstr{};

            std::vector<std::string> known_vector2_actions{};
            std::vector<const char*> known_vector2_actions_cstr{};

            for (auto action :this->action_set.actions) {
                known_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_actions) {
                known_actions_cstr.push_back(it.data());
            };

            for (auto action : this->action_set.vector2_actions) {
                known_vector2_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_vector2_actions) {
                known_vector2_actions_cstr.push_back(it.data());
            }
            
            for (auto& it : path_map) {
                ImGui::PushID(it.first.data());

                int current_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == it.first) {
                        current_combo_index = i;
                        break;
                    }
                }

                if (ImGui::Button("X")) {
                    path_map.erase(it.first);

                    this->save_bindings();
                    ImGui::PopID();
                    break;
                }
                
                auto combo_name = this->get_path_string(it.second) + ": " + known_actions[current_combo_index];

                ImGui::SameLine();
                if (ImGui::Combo(combo_name.c_str(), &current_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    path_map.erase(it.first);
                    path_map[known_actions[current_combo_index]] = it.second;

                    this->save_bindings();
                    ImGui::PopID();

                    break;
                }

                ImGui::PopID();
            }

            // Create a way to add a completely new binding
            // Create a textbox for inputting the path for the new binding
            ImGui::InputText("New Binding (e.g. /user/hand/left/input/trigger)", hand.ui.new_path_name, XR_MAX_PATH_LENGTH);
            ImGui::Combo("Action", &hand.ui.action_combo_index, known_actions_cstr.data(), known_actions_cstr.size());

            if (ImGui::Button("Add Binding")) {
                XrPath p{};
                if (xrStringToPath(this->instance, hand.ui.new_path_name, &p) != XR_SUCCESS) {
                    spdlog::error("[VR] Failed to convert path: {}", hand.ui.new_path_name);
                } else {
                    path_map[known_actions[hand.ui.action_combo_index]] = p;
                    this->save_bindings();
                }
            }

            ImGui::Text("Vector2 Associations");
            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                ImGui::PushID(&it.first);

                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                const auto activator_name = this->action_set.action_names[activator];
                const auto modifier_name = this->action_set.action_names[modifier];

                int activator_combo_index = 0;
                int modifier_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == activator_name) {
                        activator_combo_index = i;
                        break;
                    }
                }

                for (auto i = 0; i < known_vector2_actions.size(); i++) {
                    if (known_vector2_actions[i] == modifier_name) {
                        modifier_combo_index = i;
                        break;
                    }
                }

                ImGui::PushID(modifier_name.data());
                
                if (ImGui::Combo(modifier_name.data(), &modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size())) {
                    hand.profiles[current_interaction_profile].action_vector_associations[activator] = this->action_set.action_map[known_vector2_actions[modifier_combo_index]];
                }
                
                ImGui::PushID(activator_name.data());
                ImGui::Indent();

                if (ImGui::Combo(activator_name.data(), &activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    const auto old_outputs = hand.profiles[current_interaction_profile].vector_activators[activator];
                    const auto new_activator = this->action_set.action_map[known_actions[activator_combo_index]];

                    hand.profiles[current_interaction_profile].action_vector_associations.erase(activator);
                    hand.profiles[current_interaction_profile].action_vector_associations[new_activator] = modifier;
                    hand.profiles[current_interaction_profile].vector_activators.erase(activator);
                    hand.profiles[current_interaction_profile].vector_activators[new_activator] = old_outputs;
                }

                ImGui::Indent();
                for (auto& output : it.second) {
                    int output_combo_index = 0;
                    const auto output_name = this->action_set.action_names[output.action];

                    for (auto i = 0; i < known_actions.size(); i++) {
                        if (known_actions[i] == output_name) {
                            output_combo_index = i;
                            break;
                        }
                    }

                    ImGui::PushID(output_name.data());

                    if (ImGui::Combo(output_name.data(), &output_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                        output.action = this->action_set.action_map[known_actions[output_combo_index]];
                    }

                    ImGui::SliderFloat2("Value", &output.value[0], -1.0f, 1.0f);
                    ImGui::PopID();
                }

                if (ImGui::Button("Insert New Output")) {
                    hand.profiles[current_interaction_profile].vector_activators[activator].push_back({});
                }

                ImGui::Unindent();
                ImGui::Unindent();
                ImGui::PopID();
                ImGui::PopID();

                ImGui::PopID();
            }

            ImGui::Combo("New Vector2 Activator", &hand.ui.activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::Combo("New Vector2 Modifier", &hand.ui.modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size());
            ImGui::Combo("New Vector2 Output", &hand.ui.output_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::SliderFloat2("New Vector2 Value", &hand.ui.output_vector2[0], -1.0f, 1.0f);

            if (ImGui::Button("Add Vector2 Association")) {
                const auto activator = this->action_set.action_map[known_actions[hand.ui.activator_combo_index]];
                const auto modifier = this->action_set.action_map[known_vector2_actions[hand.ui.modifier_combo_index]];
                const auto output = this->action_set.action_map[known_actions[hand.ui.output_combo_index]];

                hand.profiles[current_interaction_profile].action_vector_associations[activator] = modifier;
                hand.profiles[current_interaction_profile].vector_activators[activator].push_back({hand.ui.output_vector2, output});
            }

            ImGui::PopID();
            ImGui::TreePop();
        }
    };

    display_hand("Left", 0);
    display_hand("Right", 1);
}

void OpenXR::save_bindings() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    nlohmann::json j;

    for (auto& hand : this->hands) {
        for (auto& it : hand.profiles[current_interaction_profile].path_map) {
            nlohmann::json binding{};
            binding["action"] = it.first;
            binding["path"] = this->get_path_string(it.second);
            j["bindings"].push_back(binding);
        }

        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            const auto activator_name = this->action_set.action_names[activator];
            const auto modifier_name = this->action_set.action_names[modifier];

            nlohmann::json vector2_association{};
            vector2_association["path"] = this->get_path_string(hand.path);
            vector2_association["activator"] = activator_name;
            vector2_association["modifier"] = modifier_name;

            nlohmann::json outputs{};
            for (auto& output : it.second) {
                const auto output_name = this->action_set.action_names[output.action];

                nlohmann::json output_json{};
                output_json["action"] = output_name;
                output_json["value"]["x"] = output.value.x;
                output_json["value"]["y"] = output.value.y;

                outputs.push_back(output_json);
            }

            vector2_association["outputs"] = outputs;
            j["vector2_associations"].push_back(vector2_association);
        }
    }

    auto filename = current_interaction_profile + ".json";
    
    // replace the slashes with underscores
    std::replace(filename.begin(), filename.end(), '/', '_');
    std::ofstream(filename) << j.dump(4);

    this->wants_reinitialize = true;
}

XrResult OpenXR::begin_frame() {
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        //spdlog::info("VR: begin_frame: not ready");
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (this->frame_began) {
        spdlog::info("[VR] begin_frame called while frame already began");
        return XR_SUCCESS;
    }

    this->begin_profile();

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    auto result = xrBeginFrame(this->session, &frame_begin_info);

    this->end_profile("xrBeginFrame");

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrBeginFrame")) {
            spdlog::error("[VR] xrBeginFrame failed: {}", this->get_result_string(result));
        }
    }

    if (result == XR_ERROR_CALL_ORDER_INVALID) {
        synchronize_frame();
        result = xrBeginFrame(this->session, &frame_begin_info);
    }

    this->frame_began = result == XR_SUCCESS || result == XR_FRAME_DISCARDED; // discarded means endFrame was not called

    return result;
}

namespace {
    // The panel's frame (stage space): its middle (the cylinder's axis, or the point the flat panel faces) and its turn: where
    // update_panel_placement put it in the room, at its height setting.
    void panel_frame(const OpenXR& xr, glm::vec3& center, glm::quat& turn) {
        turn   = glm::angleAxis(xr.panel_yaw, glm::vec3{0.0f, 1.0f, 0.0f});
        center = glm::vec3{xr.panel_center[0], xr.panel_center[1] + std::clamp(xr.panel_height_offset.load(), -1.0f, 1.0f), xr.panel_center[2]};
    }
}

// (fix164) As end_frame's panel branch and menu_draw_params work it out (the same numbers: kept alike with them).
OpenXR::PanelShape OpenXR::panel_shape(int32_t image_width, int32_t image_height) const {
    constexpr float pi = 3.14159265f;
    PanelShape s{};
    image_width  = std::max<int32_t>(image_width, 1);
    image_height = std::max<int32_t>(image_height, 1);
    s.distance = std::clamp(this->panel_distance.load(), 0.4f, 5.0f);
    const float visible = std::clamp(this->panel_visible_height.load(), 0.3f, 1.0f);
    const int32_t box_h = this->panel_trim_to_box.load() && this->panel_box_rows > 0 && this->panel_box_rows < image_height ? this->panel_box_rows
                                                                                                                           : image_height;
    s.rect_h = std::clamp<int32_t>((int32_t)std::lround((float)box_h * visible), 1, box_h);
    s.rect_y = (image_height - s.rect_h) / 2;
    const float aspect = (float)image_width / (float)s.rect_h;
    s.curved = ext_cylinder && this->panel_curved.load() && !this->panel_cylinder_refused;
    s.width = std::clamp(this->panel_width.load(), 0.3f, 6.0f);
    if (s.curved) {
        s.angle = std::min(s.width / s.distance, 1.9f * pi);
        s.width = s.angle * s.distance;
    }
    s.height = s.width / aspect;
    return s;
}

// (fix164) The soft light behind the panel: round the same middle, a quarter of a metre further (it shows behind the panel from
// anywhere near the middle), 150 degrees of arc (at most: a seated player turns little; the runtime takes any arc) and 3 m tall, or
// taller than the panel by a metre. With a flat panel a flat quad as far, 120 degrees across (or wider than the panel by a metre).
OpenXR::AmbientShape OpenXR::ambient_shape(const PanelShape& panel) {
    AmbientShape s{};
    const float distance = std::max(panel.distance, 0.1f);
    s.radius = distance + 0.25f;
    const float seen = s.radius / distance; // (the panel's size seen at the light's distance)
    s.height = std::max(3.0f, panel.height * seen + 1.0f);
    s.angle  = 2.6f;
    s.width  = panel.curved ? s.angle * s.radius : std::max(2.0f * s.radius * std::tan(glm::radians(60.0f)), panel.width * seen + 1.0f);
    return s;
}

bool OpenXR::update_panel_placement() {
    constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const auto now = std::chrono::steady_clock::now();
    // A recenter: put in front of you again, for half a second (the room's space may move under it a moment after the headset
    // says it will; the panel then lands where you are looking once that is done).
    if (this->panel_recenter.exchange(false)) {
        this->panel_replace_until = now + std::chrono::milliseconds(500);
    }
    const bool replace = !this->panel_placed || now < this->panel_replace_until;
    if (!replace) {
        return true; // (it stays where it was put, wherever you look, until the menu closes or you recenter)
    }
    if ((this->view_space_location.locationFlags & valid) != valid) {
        return this->panel_placed; // (no head pose this frame: it stays where it is)
    }
    const auto& pose = this->view_space_location.pose;
    const glm::quat rotation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
    const glm::vec3 head{pose.position.x, pose.position.y, pose.position.z};
    const glm::vec3 forward = rotation * glm::vec3{0.0f, 0.0f, -1.0f};
    glm::vec2 level{forward.x, forward.z};
    if (glm::length(level) < 0.2f) {
        // Looking nearly straight down (or up): the top of the head points the way the face is turned (or away from it).
        const glm::vec3 up = rotation * glm::vec3{0.0f, 1.0f, 0.0f};
        level = glm::vec2{up.x, up.z} * (forward.y < 0.0f ? 1.0f : -1.0f);
    }
    if (glm::length(level) < 1e-3f) {
        return this->panel_placed;
    }
    // The heading: 0 facing -Z; a turn by it about +Y faces (-sin, 0, -cos).
    this->panel_yaw = std::atan2(-level.x, -level.y);
    for (int i = 0; i < 3; ++i) {
        this->panel_center[i] = head[i];
    }
    ++this->panel_placements;
    if (this->panel_place_logs++ < 40) {
        spdlog::info("[VR] Menu panel: put in front of you ({}): facing {:.0f} deg, at {:.2f} {:.2f} {:.2f}",
            !this->panel_placed ? "a menu opened" : "after a recenter", glm::degrees(this->panel_yaw), head.x, head.y, head.z);
    }
    this->panel_placed = true;
    return true;
}

bool OpenXR::menu_draw_params(int eye, bool mono, float params[16], float panel[4], float dot[4]) {
    constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    constexpr float pi = 3.14159265f;
    eye = std::clamp(eye, 0, 1);
    if (!this->panel_draw_in.load() || !this->panel_placed || this->panel_image_width <= 0 || this->panel_image_height <= 0 ||
        this->stage_views.size() != 2 || (mono && (this->view_space_location.locationFlags & valid) != valid)) {
        return false;
    }
    // The eye's pose and field of view, as the frame's world picture is submitted with them (add_world_views); read under the poses'
    // lock (the game thread may be locating the next frame's meanwhile).
    XrPosef pose{};
    XrFovf  fov{};
    {
        std::shared_lock _{this->pose_mtx};
        if (mono && (this->view_space_location.locationFlags & valid) != valid) {
            return false;
        }
        pose = mono ? this->view_space_location.pose : this->stage_views[eye].pose;
        fov  = this->stage_views[eye].fov;
    }
    // (Pipelined: the frame being presented's kept poses, the ones its picture was drawn with and is submitted with; the located ones
    // may be frames ahead, fix136's review.)
    if (this->pipelined) {
        FramePoses fp{};
        if (this->frame_poses_of(this->present_frame, fp)) {
            pose = mono ? fp.head : fp.stage[eye];
        }
    }
    {
        // (The frustum the picture was drawn with, mono's or the eye's own, as its copy into the eye's image gives it, fix138.)
        std::shared_lock _{this->eyes_mtx};
        const auto& f = mono ? this->mono_frustum : this->eye_frustums[eye & 1];
        if (f[1] > f[0] && f[2] > f[3]) {
            fov.angleLeft = atanf(f[0]);
            fov.angleRight = atanf(f[1]);
            fov.angleUp = atanf(f[2]);
            fov.angleDown = atanf(f[3]);
        }
    }
    // The panel, as end_frame puts it (its place, its shape, the part of its picture shown).
    glm::quat turn{};
    glm::vec3 center{};
    panel_frame(*this, center, turn);
    const float distance = std::clamp(this->panel_distance.load(), 0.4f, 5.0f);
    const float visible = std::clamp(this->panel_visible_height.load(), 0.3f, 1.0f);
    const int32_t box_h = this->panel_trim_to_box.load() && this->panel_box_rows > 0 && this->panel_box_rows < this->panel_image_height
                              ? this->panel_box_rows
                              : this->panel_image_height;
    const int32_t rect_h = std::clamp<int32_t>((int32_t)std::lround((float)box_h * visible), 1, box_h);
    const int32_t rect_y = (this->panel_image_height - rect_h) / 2;
    const float aspect = (float)this->panel_image_width / (float)rect_h;
    const bool curved = ext_cylinder && this->panel_curved.load() && !this->panel_cylinder_refused;
    float width = std::clamp(this->panel_width.load(), 0.3f, 6.0f);
    float angle = 0.0f;
    if (curved) {
        angle = std::min(width / distance, 1.9f * pi);
        width = angle * distance;
    }
    const float height = width / aspect;
    // The eye's line of sight through a point (u, v) of its picture (0..1, from the top left), in the panel's own frame:
    // origin + A u + B v + C.
    const glm::quat unturn = glm::inverse(turn);
    const glm::quat eye_rotation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
    const glm::vec3 origin = unturn * (glm::vec3{pose.position.x, pose.position.y, pose.position.z} - center);
    const glm::quat r = unturn * eye_rotation;
    const float l = std::tan(fov.angleLeft), rt = std::tan(fov.angleRight), up = std::tan(fov.angleUp), dn = std::tan(fov.angleDown);
    const glm::vec3 a = r * glm::vec3{rt - l, 0.0f, 0.0f};
    const glm::vec3 b = r * glm::vec3{0.0f, dn - up, 0.0f};
    const glm::vec3 c = r * glm::vec3{l, up, -1.0f};
    const float rows[16]{a.x, a.y, a.z, origin.x, b.x, b.y, b.z, origin.y, c.x, c.y, c.z, origin.z, distance, angle, height, width};
    std::copy(std::begin(rows), std::end(rows), params);
    const float v0 = (float)rect_y / (float)this->panel_image_height;
    const float v1 = (float)(rect_y + rect_h) / (float)this->panel_image_height;
    panel[0] = v0;
    panel[1] = v1;
    panel[2] = panel[3] = 0.0f;
    // The pointer's dot where it met the panel (the frame before's: its place on the panel's picture); not with the laser's own dot
    // layer (as at the main menu: panel_pointer_laser).
    dot[0] = dot[1] = dot[2] = dot[3] = 0.0f;
    // (Only when its layers can be shown: the strip ready and not held back after a refused frame; else the dot drawn in, as before.)
    const bool laser_layers = this->panel_pointer_laser.load() && this->reticle_ready.load() && this->reticle_swapchain != XR_NULL_HANDLE &&
                              this->reticle_height > 0 && this->reticle_skip_frames == 0;
    if (this->panel_pointer_enabled.load() && !laser_layers) {
        const auto pointer = this->get_panel_pointer();
        if (pointer.hand >= 0 && pointer.hit[pointer.hand] && std::chrono::steady_clock::now() - pointer.at < std::chrono::milliseconds(200)) {
            dot[0] = pointer.u[pointer.hand];
            dot[1] = (pointer.v[pointer.hand] - v0) / std::max(v1 - v0, 1e-4f);
            dot[2] = distance * std::tan(glm::radians(1.2f)); // (its radius, metres: as the dot layer, 2.4 degrees across at the panel)
            dot[3] = 1.0f;
        }
    }
    {
        std::scoped_lock _{this->drawn_mtx};
        for (int i = 0; i < 2; ++i) {
            if (mono || i == eye) {
                this->drawn[i] = DrawnView{pose, fov, true};
            }
        }
    }
    return true;
}

float OpenXR::head_turn_from_panel() {
    constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    constexpr float pi = 3.14159265f;
    if (!this->panel_placed) {
        return -1.0f;
    }
    XrPosef pose{};
    {
        std::shared_lock _{this->pose_mtx};
        if ((this->view_space_location.locationFlags & valid) != valid) {
            return -1.0f;
        }
        pose = this->view_space_location.pose;
    }
    // (The heading as update_panel_placement takes it: 0 facing -Z, a turn about +Y.)
    const glm::quat rotation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
    const glm::vec3 forward = rotation * glm::vec3{0.0f, 0.0f, -1.0f};
    const glm::vec2 level{forward.x, forward.z};
    if (glm::length(level) < 0.2f) {
        return -1.0f;
    }
    float turn = std::abs(std::atan2(-level.x, -level.y) - this->panel_yaw);
    if (turn > pi) {
        turn = 2.0f * pi - turn;
    }
    return glm::degrees(turn);
}

XrResult OpenXR::end_frame() {
    std::scoped_lock _{sync_mtx};

    this->last_world_views_valid = false; // (set again below once a world picture has been shown)

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (!this->frame_began) {
        spdlog::info("[VR] end_frame called while frame not begun");
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    constexpr float pi = 3.14159265f;

    std::vector<XrCompositionLayerBaseHeader*> layers{};
    layers.reserve(10);
    std::vector<XrCompositionLayerProjectionView> projection_layer_views{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cylinder_layer{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerQuad veil_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad beam_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad dot_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad reticle_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad laser_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // (fix164) The soft light behind the panel (curved, or flat with a flat panel).
    XrCompositionLayerCylinderKHR ambient_cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerQuad ambient_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool ambient_submitted = false;

    // we CANT push the layers every time, it cause some layer error
    // in xrEndFrame, so we must only do it when shouldRender is true
    const bool render = this->frame_state.shouldRender == XR_TRUE;
    // (AER: one eye fresh this frame, the other's last image submitted again: both once each has had one, fix136.)
    const bool both_images_ready = this->swapchains.size() >= 2 && ((this->image_ready[0] && this->image_ready[1]) ||
                                   (this->aer_frame && (this->image_ready[0] || this->image_ready[1]) && this->image_ever[0] && this->image_ever[1]));
    const bool last_image_ready = this->swapchains.size() >= 2 && this->image_ready[1];
    if (!this->panel_mode) {
        this->panel_placed = false; // (the next menu's panel is put in front of you)
    }
    const bool panel_ready = render && this->panel_mode && !this->panel_refused && this->panel_image_ready &&
                             this->panel_swapchain != XR_NULL_HANDLE && this->panel_image_width > 0 && this->panel_image_height > 0 &&
                             this->update_panel_placement();
    // (The panel's image without a place for it yet, no head pose: on the old screen, not the eye images, which hold the world.)
    const bool panel_image_only = render && !panel_ready && this->panel_mode && this->panel_image_ready && this->panel_swapchain != XR_NULL_HANDLE &&
                                  this->panel_image_width > 0 && this->panel_image_height > 0;
    const bool has_picture = panel_ready || (render && this->flat_screen_mode && (last_image_ready || panel_image_only)) || (render && both_images_ready);

    // The strip's pictures (the gun sight, the panel's veil, beam and dot). A frame the runtime refused with them in it: the
    // next few go without them (see below).
    const bool strip_skipped = this->reticle_skip_frames > 0 && has_picture;
    if (strip_skipped) {
        --this->reticle_skip_frames; // (counted on frames with a picture: a frame without one tells nothing)
    }
    const bool strip_usable = has_picture && !strip_skipped && this->reticle_ready.load() && this->reticle_swapchain != XR_NULL_HANDLE &&
                              this->reticle_height > 0;
    bool strip_submitted = false;
    // One picture of the strip (square cells side by side), less a margin (so its neighbours never blend in at its edges).
    const auto strip_cell = [&](int sprite, int32_t inset_x, int32_t inset_y, XrSwapchainSubImage& sub) {
        const int32_t cell = this->reticle_height;
        const int32_t cells = std::max(this->reticle_width / std::max(cell, 1), 1);
        sprite = std::clamp(sprite, 0, cells - 1);
        sub.swapchain = this->reticle_swapchain;
        sub.imageRect.offset = {sprite * cell + inset_x, inset_y};
        sub.imageRect.extent = {std::max(cell - 2 * inset_x, 1), std::max(cell - 2 * inset_y, 1)};
        sub.imageArrayIndex = 0;
    };
    // A quad facing the head, kept upright (as the gun sight's).
    const auto facing_head = [&](const glm::vec3& center, const glm::vec3& head, const glm::vec3& fallback_normal) {
        glm::vec3 normal = head - center;
        normal = glm::length(normal) > 1e-4f ? glm::normalize(normal) : fallback_normal;
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        up -= normal * glm::dot(up, normal);
        up = glm::length(up) > 1e-3f ? glm::normalize(up) : glm::normalize(glm::cross(normal, glm::vec3{1.0f, 0.0f, 0.0f}));
        const glm::vec3 side = glm::cross(up, normal);
        return glm::normalize(glm::quat_cast(glm::mat3{side, up, normal}));
    };

    int panel_kind = 0; // the panel submitted: 1 flat, 2 curved
    bool world_submitted = false;
    PanelPointer pointer{};

    // The eye images as in play: each seen from where its eye is now (mono: the head's pose for both, which then see the same
    // picture, turned only as the head turns), with the field of view the game actually rendered with (VR::update_frustums),
    // which differs from the lens's when the symmetric projection is on.
    // (The menu drawn into this frame's eye pictures: the poses and fields of view they were drawn with. Taken once a frame.)
    DrawnView drawn_now[2]{};
    {
        std::scoped_lock _{this->drawn_mtx};
        for (int i = 0; i < 2; ++i) {
            drawn_now[i] = this->drawn[i];
            this->drawn[i].valid = false;
        }
    }
    bool menu_drawn_in = false;

    // (use: the menu was drawn into the eye pictures with these poses and fields of view: submitted with exactly those.)
    const auto add_world_views = [&](const DrawnView* use) {
        projection_layer_views.resize(this->stage_views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        for (auto i = 0; i < projection_layer_views.size(); ++i) {
            const auto& swapchain = this->swapchains[i];

            projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // (Pipelined or AER: the pose each eye's picture was drawn with, kept for its game frame, fix136.)
            if (this->submit_valid) {
                projection_layer_views[i].pose = this->mono_views ? this->submit_head : this->submit_pose[i < 2 ? i : 0];
            } else {
                projection_layer_views[i].pose = this->mono_views ? this->view_space_location.pose : this->stage_views[i].pose;
            }
            projection_layer_views[i].fov = this->stage_views[i].fov;

            if (use != nullptr && i < 2 && use[i].valid) {
                projection_layer_views[i].pose = use[i].pose;
                projection_layer_views[i].fov = use[i].fov;
            } else {
                // (The frustum the picture in this eye's image was drawn with, fix138: another eye's or mono's when it is one picture
                // for both eyes, or one kept from an earlier frame.)
                std::shared_lock _{this->eyes_mtx};
                const auto& f = i < 2 && this->image_frustum_set[i] ? this->image_frustum[i] : this->frustums[i];
                if (f[1] > f[0] && f[2] > f[3]) {
                    projection_layer_views[i].fov.angleLeft = atanf(f[0]);
                    projection_layer_views[i].fov.angleRight = atanf(f[1]);
                    projection_layer_views[i].fov.angleUp = atanf(f[2]);
                    projection_layer_views[i].fov.angleDown = atanf(f[3]);
                }
            }
            projection_layer_views[i].subImage.swapchain = swapchain.handle;
            projection_layer_views[i].subImage.imageRect.offset = {0, 0};
            projection_layer_views[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
        }

        layer.space = this->stage_space;
        layer.viewCount = (uint32_t)projection_layer_views.size();
        layer.views = projection_layer_views.data();
        layers.push_back((XrCompositionLayerBaseHeader*)&layer);
    };

    if (panel_ready) {
        // The panel: the game's picture (this frame's), where it was put in the room.
        glm::quat turn{};
        glm::vec3 center{};
        panel_frame(*this, center, turn);
        const float distance = std::clamp(this->panel_distance.load(), 0.4f, 5.0f);
        // The part of the picture shown: the box the menu draws in (the game draws menus in a 16:9 box, centred, and nothing
        // around it), then the part of that the setting keeps (centred).
        const float visible = std::clamp(this->panel_visible_height.load(), 0.3f, 1.0f);
        const int32_t box_h = this->panel_trim_to_box.load() && this->panel_box_rows > 0 && this->panel_box_rows < this->panel_image_height
                                  ? this->panel_box_rows
                                  : this->panel_image_height;
        const int32_t rect_h = std::clamp<int32_t>((int32_t)std::lround((float)box_h * visible), 1, box_h);
        const int32_t rect_y = (this->panel_image_height - rect_h) / 2;
        if (rect_y != this->panel_logged_rect[0] || rect_h != this->panel_logged_rect[1]) {
            this->panel_logged_rect[0] = rect_y;
            this->panel_logged_rect[1] = rect_h;
            spdlog::info("[VR] Menu panel: showing rows {} to {} of the {}x{} picture ({})", rect_y, rect_y + rect_h, this->panel_image_width,
                this->panel_image_height, box_h < this->panel_image_height ? fmt::format("the menu's box is {} rows", box_h) : std::string{"the whole height"});
        }
        const float aspect = (float)this->panel_image_width / (float)rect_h;
        const bool curved = ext_cylinder && this->panel_curved.load() && !this->panel_cylinder_refused;
        float width = std::clamp(this->panel_width.load(), 0.3f, 6.0f);
        float angle = 0.0f;
        if (curved) {
            angle = std::min(width / distance, 1.9f * pi); // (an arc of at most most of the way around)
            width = angle * distance;
        }
        const float height = width / aspect;

        // (fix164) With nothing behind the panel (the main menu, the intro, a loading screen: no world), its soft light first: layers are
        // drawn back to front, so it is behind the panel, the pointer's beam and its dot. Opaque (its image is the light alone). Never
        // with a world behind the panel (live or as it was), nor with the menu drawn into the world's pictures (only done with a world).
        if (this->ambient_ready && !this->live_world_ready && !this->frozen_ready) {
            if (this->ambient_skip_frames > 0) {
                --this->ambient_skip_frames;
            } else if (!this->ambient_refused && this->panel_ambient.load() && this->ambient_swapchain != XR_NULL_HANDLE &&
                       this->ambient_image_width > 0 && this->ambient_image_height > 0) {
                const PanelShape shape = this->panel_shape(this->panel_image_width, this->panel_image_height);
                const AmbientShape light = ambient_shape(shape);
                const XrSwapchainSubImage light_image{this->ambient_swapchain, {{0, 0}, {this->ambient_image_width, this->ambient_image_height}}, 0};
                if (shape.curved) {
                    ambient_cylinder.layerFlags = 0;
                    ambient_cylinder.space = this->stage_space;
                    ambient_cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    ambient_cylinder.subImage = light_image;
                    ambient_cylinder.pose.orientation = {turn.x, turn.y, turn.z, turn.w};
                    ambient_cylinder.pose.position = {center.x, center.y, center.z};
                    ambient_cylinder.radius = light.radius;
                    ambient_cylinder.centralAngle = light.angle;
                    ambient_cylinder.aspectRatio = light.radius * light.angle / light.height; // (its height: radius x angle / aspect)
                    layers.push_back((XrCompositionLayerBaseHeader*)&ambient_cylinder);
                } else {
                    const glm::vec3 at = center + turn * glm::vec3{0.0f, 0.0f, -light.radius};
                    ambient_quad.layerFlags = 0;
                    ambient_quad.space = this->stage_space;
                    ambient_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    ambient_quad.subImage = light_image;
                    ambient_quad.pose.orientation = {turn.x, turn.y, turn.z, turn.w};
                    ambient_quad.pose.position = {at.x, at.y, at.z};
                    ambient_quad.size = {light.width, light.height};
                    layers.push_back((XrCompositionLayerBaseHeader*)&ambient_quad);
                }
                ambient_submitted = true;
                if (!this->ambient_logged) {
                    this->ambient_logged = true;
                    spdlog::info("[VR] Ambient light: shown behind the panel ({}, {:.2f} m away, {:.0f} deg wide, {:.1f} m tall; the panel {:.2f} m)",
                        shape.curved ? "curved" : "flat", light.radius, shape.curved ? glm::degrees(light.angle)
                                                                                    : glm::degrees(2.0f * std::atan(light.width * 0.5f / light.radius)),
                        light.height, shape.distance);
                }
            }
        }

        // Behind it: the world as it is now (the game keeps drawing it behind a see-through menu), shown as in play; else the world
        // as it last was before the menu, from where each eye saw it (so it stays put as you look around).
        if (this->live_world_ready && both_images_ready && this->stage_views.size() == 2) {
            // (The menu drawn into both eye pictures of this frame: no panel of its own, the pictures submitted as drawn.)
            // (One eye only, as the first frame of a menu in stereo: that eye as drawn, the other as before; no layer either way, so
            // the menu is never shown twice in an eye.)
            menu_drawn_in = this->panel_draw_in.load() && (drawn_now[0].valid || drawn_now[1].valid);
            add_world_views(menu_drawn_in ? drawn_now : nullptr);
            if (menu_drawn_in) {
                ++this->stat_drawn_frames;
                if (!this->drawn_logged) {
                    this->drawn_logged = true;
                    spdlog::info("[VR] Menu panel: drawn into the world's eye pictures (no panel layer of its own)");
                }
            }
            ++this->stat_live_frames;
            if (projection_layer_views.size() == 2) {
                for (size_t i = 0; i < 2; ++i) {
                    this->last_live_pose[i] = projection_layer_views[i].pose;
                    this->last_live_fov[i] = projection_layer_views[i].fov;
                }
                this->last_live_valid = true;
            }
        } else if (this->frozen_ready && both_images_ready && this->stage_views.size() == 2) {
            // (The last live pair shown again, the menu drawn into it: no panel of its own either.)
            menu_drawn_in = this->panel_draw_in.load() && this->frozen_has_menu;
            projection_layer_views.resize(2, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            for (size_t i = 0; i < 2; ++i) {
                const auto& swapchain = this->swapchains[i];
                projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projection_layer_views[i].pose = this->frozen_pose[i];
                projection_layer_views[i].fov = this->frozen_fov[i];
                projection_layer_views[i].subImage.swapchain = swapchain.handle;
                projection_layer_views[i].subImage.imageRect.offset = {0, 0};
                projection_layer_views[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
            }
            layer.space = this->stage_space;
            layer.viewCount = 2;
            layer.views = projection_layer_views.data();
            layers.push_back((XrCompositionLayerBaseHeader*)&layer);
            ++this->stat_frozen_frames;

            // Dimmed by a veil kept in front of your eyes, under the panel (the strip's even black, partly see-through). Not behind a
            // see-through menu: the world is to be seen there (its own slight darkness is in the panel's picture,
            // panel_see_through_darkness).
            const int backdrop = this->panel_backdrop.load();
            if (strip_usable && (backdrop == 2 || backdrop == 3) && !this->panel_image_alpha.load()) {
                veil_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                veil_layer.space = this->view_space;
                veil_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                strip_cell(backdrop == 3 ? sprite_veil_dark : sprite_veil_dim, 8, 8, veil_layer.subImage);
                veil_layer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                veil_layer.pose.position = {0.0f, 0.0f, -0.3f};
                veil_layer.size = {6.0f, 6.0f}; // (wider than any headset's view at that distance)
                layers.push_back((XrCompositionLayerBaseHeader*)&veil_layer);
                strip_submitted = true;
            }
        }

        // The panel itself: opaque (the game's picture's alpha is not how much it covers), or, see-through, the game's interface
        // layer over a see-through dark (premultiplied alpha: blended over the world behind it).
        const XrCompositionLayerFlags panel_flags = this->panel_image_alpha.load() ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        // (fix192) The rows taken from the image: the centred box's (rect_y), or, when the image is the game's own picture with the frame
        // kept full size (the menu in its top rows: panel_box_top 0), the same part of the box counted from the top. The centred rows of
        // that picture held only the menu's lower half, the rest the 3D pictures stretched to the whole height: the menu cut and blown
        // up. (The pointer's places below stay the centred box's: the menu's own, as the game side reads them.)
        int32_t image_y = rect_y;
        if (this->panel_box_top >= 0 && box_h < this->panel_image_height) {
            image_y = std::clamp<int32_t>(rect_y - (this->panel_image_height - box_h) / 2 + this->panel_box_top, 0, this->panel_image_height - rect_h);
        }
        // (fix192) The rows really shown, logged when the top rows are taken (each time they start being used, 40 a session).
        if (image_y == rect_y) {
            this->panel_logged_image_y = -1;
        } else if (image_y != this->panel_logged_image_y) {
            this->panel_logged_image_y = image_y;
            if (this->panel_top_logs++ < 40) {
                spdlog::info("[VR] (fix192) Menu panel: showing rows {} to {} of the {}x{} picture (the game's own picture with the frame kept full "
                             "size: the menu's box from its top rows, not the centred {} to {})",
                    image_y, image_y + rect_h, this->panel_image_width, this->panel_image_height, rect_y, rect_y + rect_h);
            }
        }
        const XrSwapchainSubImage panel_image{this->panel_swapchain, {{0, image_y}, {this->panel_image_width, rect_h}}, 0};
        if (curved) {
            // A cylinder around its middle (at your head when it was put there); the part shown faces its -Z.
            cylinder_layer.layerFlags = panel_flags;
            cylinder_layer.space = this->stage_space;
            cylinder_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cylinder_layer.subImage = panel_image;
            cylinder_layer.pose.orientation = {turn.x, turn.y, turn.z, turn.w};
            cylinder_layer.pose.position = {center.x, center.y, center.z};
            cylinder_layer.radius = distance;
            cylinder_layer.centralAngle = angle;
            cylinder_layer.aspectRatio = aspect; // (its height: radius x angle / aspect)
            if (!menu_drawn_in) {
                layers.push_back((XrCompositionLayerBaseHeader*)&cylinder_layer);
            }
            panel_kind = 2;
            ++this->stat_cylinder_frames;
        } else {
            const glm::vec3 at = center + turn * glm::vec3{0.0f, 0.0f, -distance};
            quad_layer.layerFlags = panel_flags;
            quad_layer.space = this->stage_space;
            quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad_layer.subImage = panel_image;
            quad_layer.pose.orientation = {turn.x, turn.y, turn.z, turn.w};
            quad_layer.pose.position = {at.x, at.y, at.z};
            quad_layer.size = {width, height};
            if (!menu_drawn_in) {
                layers.push_back((XrCompositionLayerBaseHeader*)&quad_layer);
            }
            panel_kind = 1;
        }
        ++this->stat_panel_frames;

        // (For the log, every 2 seconds while it is up: where it is, where your head is and looks, and the world picture's view.)
        if (const auto now = std::chrono::steady_clock::now(); now - this->panel_diag_at > std::chrono::seconds(2)) {
            this->panel_diag_at = now;
            const auto yaw_of = [](const XrQuaternionf& q) {
                const glm::vec3 f = glm::quat{q.w, q.x, q.y, q.z} * glm::vec3{0.0f, 0.0f, -1.0f};
                return glm::degrees(std::atan2(-f.x, -f.z));
            };
            const auto& head = this->view_space_location.pose;
            std::string world{"none"};
            if (!projection_layer_views.empty() && !layers.empty() && layers.front() == (XrCompositionLayerBaseHeader*)&layer) {
                const auto& p = projection_layer_views[0].pose;
                world = fmt::format("{} {} views, the first facing {:.0f} deg at {:.2f} {:.2f} {:.2f}",
                    this->live_world_ready ? "live" : "as it was", projection_layer_views.size(), yaw_of(p.orientation), p.position.x,
                    p.position.y, p.position.z);
            }
            spdlog::info("[VR] Menu panel now: {} facing {:.0f} deg at {:.2f} {:.2f} {:.2f} (put there {} times so far); your head facing {:.0f} deg at "
                         "{:.2f} {:.2f} {:.2f} (flags {:#x}); the world behind: {}; layers {}",
                panel_kind == 2 ? "curved" : "flat", glm::degrees(this->panel_yaw), center.x, center.y, center.z, this->panel_placements,
                yaw_of(head.orientation), head.position.x, head.position.y, head.position.z, (uint32_t)this->view_space_location.locationFlags,
                world, layers.size());
        }

        // Where each controller's aim meets it (in the panel's own frame: the cylinder's axis is its Y, the flat panel at -Z).
        pointer.shown = true;
        const glm::quat unturn = glm::inverse(turn);
        glm::vec3 ray_origin[2]{}, ray_direction[2]{}, hit_point[2]{};
        bool has_ray[2]{false, false};
        for (int h = 0; h < 2; ++h) {
            const auto& hand = this->hands[h];
            if (hand.aim_space == XR_NULL_HANDLE || (hand.aim_location.locationFlags & valid) != valid) {
                continue;
            }
            const auto& aim = hand.aim_location.pose;
            const glm::quat aim_rotation{aim.orientation.w, aim.orientation.x, aim.orientation.y, aim.orientation.z};
            ray_origin[h] = glm::vec3{aim.position.x, aim.position.y, aim.position.z};
            ray_direction[h] = glm::normalize(aim_rotation * glm::vec3{0.0f, 0.0f, -1.0f});
            has_ray[h] = true;
            const glm::vec3 o = unturn * (ray_origin[h] - center);
            const glm::vec3 d = unturn * ray_direction[h];
            float t = -1.0f;
            if (curved) {
                // The side shown faces the axis: where the ray leaves the cylinder (the far crossing; from inside, the only one
                // ahead). A crossing outside the part shown is not a hit (below).
                const float a = d.x * d.x + d.z * d.z;
                const float b = 2.0f * (o.x * d.x + o.z * d.z);
                const float c = o.x * o.x + o.z * o.z - distance * distance;
                const float disc = b * b - 4.0f * a * c;
                if (a > 1e-6f && disc >= 0.0f) {
                    t = (-b + std::sqrt(disc)) / (2.0f * a);
                }
            } else if (d.z < -1e-4f) {
                t = (-distance - o.z) / d.z;
            }
            if (!(t > 0.0f && t < 20.0f)) {
                continue;
            }
            const glm::vec3 p = o + d * t;
            const float u = curved ? 0.5f + std::atan2(p.x, -p.z) / angle : 0.5f + p.x / width;
            const float v = 0.5f - p.y / height;
            if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
                continue;
            }
            pointer.hit[h] = true;
            pointer.u[h] = u;
            pointer.v[h] = ((float)rect_y + v * (float)rect_h) / (float)this->panel_image_height; // (over the whole picture)
            hit_point[h] = center + turn * p;
        }
        const int preferred = std::clamp(this->panel_preferred_hand.load(), 0, 1);
        pointer.hand = pointer.hit[preferred] ? preferred : (pointer.hit[1 - preferred] ? 1 - preferred : -1);

        // The pointing hand's beam to where it meets the panel and a dot there; with neither meeting it, a short beam from the
        // preferred hand (so you can see you can point).
        const int beam_hand = pointer.hand >= 0 ? pointer.hand : preferred;
        if (strip_usable && this->panel_pointer_enabled.load() && has_ray[beam_hand] && (this->view_space_location.locationFlags & valid) == valid) {
            const auto& head_pose = this->view_space_location.pose;
            const glm::vec3 head{head_pose.position.x, head_pose.position.y, head_pose.position.z};
            const glm::vec3 start = ray_origin[beam_hand];
            const glm::vec3 end = pointer.hand >= 0 ? hit_point[beam_hand] : start + ray_direction[beam_hand] * 0.3f;
            const float length = glm::length(end - start);
            if (length > 0.03f) {
                // Along the beam its height; turned about it to face the head as much as it can.
                const glm::vec3 middle = (start + end) * 0.5f;
                const glm::vec3 along = (end - start) / length;
                glm::vec3 normal = head - middle;
                normal -= along * glm::dot(normal, along);
                if (glm::length(normal) > 1e-3f) {
                    normal = glm::normalize(normal);
                    const glm::vec3 side = glm::cross(along, normal);
                    const glm::quat orientation = glm::normalize(glm::quat_cast(glm::mat3{side, along, normal}));
                    beam_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    beam_layer.space = this->stage_space;
                    beam_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_beam, 3, 0, beam_layer.subImage);
                    beam_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                    beam_layer.pose.position = {middle.x, middle.y, middle.z};
                    beam_layer.size = {0.012f, length};
                    // (Drawn in: the laser as at the main menu, if set; else none, the dot drawn into the pictures with the menu.)
                    if (!menu_drawn_in || this->panel_pointer_laser.load()) {
                        layers.push_back((XrCompositionLayerBaseHeader*)&beam_layer);
                        strip_submitted = true;
                    }
                }
            }
            if (pointer.hand >= 0) {
                const glm::vec3 hit = hit_point[beam_hand];
                const float to_head = glm::length(head - hit);
                if (to_head > 0.05f) {
                    const glm::vec3 at = hit + (head - hit) / to_head * 0.003f; // (a little in front of the panel)
                    const glm::quat orientation = facing_head(at, head, -ray_direction[beam_hand]);
                    const float size = 2.0f * to_head * std::tan(glm::radians(2.4f) * 0.5f);
                    dot_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    dot_layer.space = this->stage_space;
                    dot_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_pointer_dot, 1, 1, dot_layer.subImage);
                    dot_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                    dot_layer.pose.position = {at.x, at.y, at.z};
                    dot_layer.size = {size, size};
                    if (!menu_drawn_in || this->panel_pointer_laser.load()) {
                        layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer);
                        strip_submitted = true;
                    }
                }
            }
        }
    } else if (render && this->flat_screen_mode && (last_image_ready || panel_image_only)) {
        // Show the most recent image (right eye slot; or the menu panel's, when the panel could not be placed) on a head-locked
        // virtual screen.
        const Swapchain swapchain = panel_image_only ? Swapchain{this->panel_swapchain, this->panel_image_width, this->panel_image_height}
                                                     : this->swapchains[1];
        const auto aspect = swapchain.width > 0 ? (float)swapchain.height / (float)swapchain.width : 1.0f;

        quad_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad_layer.space = this->view_space;
        quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad_layer.subImage.swapchain = swapchain.handle;
        quad_layer.subImage.imageRect.offset = {0, 0};
        quad_layer.subImage.imageRect.extent = {swapchain.width, swapchain.height};
        quad_layer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        quad_layer.pose.position = {0.0f, 0.0f, -this->flat_screen_distance};
        quad_layer.size = {this->flat_screen_width, this->flat_screen_width * aspect};
        layers.push_back((XrCompositionLayerBaseHeader*)&quad_layer);
    } else if (render && both_images_ready) {
        add_world_views(nullptr);

        // Kept for the frozen world behind a menu's panel: what this picture is, as it is submitted.
        if (projection_layer_views.size() == 2) {
            for (size_t i = 0; i < 2; ++i) {
                this->last_world_pose[i] = projection_layer_views[i].pose;
                this->last_world_fov[i] = projection_layer_views[i].fov;
            }
            world_submitted = true;
        }
    }

    // Gun sight: a dot along the app's ray (where its shots go) or else the hand's aim ray, facing the head and kept
    // upright, its size a fixed angle.
    const bool laser_shown = [&] {
        std::scoped_lock _{this->reticle_ray_mtx};
        return this->laser_beam_visible;
    }();
    if (strip_usable && !this->flat_screen_mode && (this->reticle_visible.load() || laser_shown)) {
        const auto& hand = this->hands[std::clamp(this->reticle_hand.load(), 0, 1)];
        bool have_ray = false;
        glm::vec3 ray_origin{0.0f}, forward{0.0f, 0.0f, -1.0f};
        int sprite = 0;
        {
            std::scoped_lock _{this->reticle_ray_mtx};
            sprite = this->reticle_sprite;
            if (this->reticle_has_ray) {
                ray_origin = glm::vec3{this->reticle_origin[0], this->reticle_origin[1], this->reticle_origin[2]};
                forward = glm::vec3{this->reticle_direction[0], this->reticle_direction[1], this->reticle_direction[2]};
                have_ray = true;
            }
        }
        if (!have_ray && hand.aim_space != XR_NULL_HANDLE && (hand.aim_location.locationFlags & valid) == valid) {
            const auto& aim = hand.aim_location.pose;
            const glm::quat aim_rotation{aim.orientation.w, aim.orientation.x, aim.orientation.y, aim.orientation.z};
            ray_origin = glm::vec3{aim.position.x, aim.position.y, aim.position.z};
            forward = glm::normalize(aim_rotation * glm::vec3{0.0f, 0.0f, -1.0f});
            have_ray = true;
        }
        // The gun's laser beam (fix129): from the gun to where it ends, a thin red line under its dot, as the menu pointer's beam.
        bool laser = false;
        glm::vec3 laser_start{0.0f}, laser_end{0.0f};
        {
            std::scoped_lock _{this->reticle_ray_mtx};
            laser = this->laser_beam_visible;
            laser_start = glm::vec3{this->laser_beam_start[0], this->laser_beam_start[1], this->laser_beam_start[2]};
            laser_end = glm::vec3{this->laser_beam_end[0], this->laser_beam_end[1], this->laser_beam_end[2]};
        }
        // (fix148) Only when the beam was not drawn into the pictures (no depth: no DLSS), as before: the strip over everything.
        if (laser && !vrmod::LaserSight::get().beam_drawn_lately() && (this->view_space_location.locationFlags & valid) == valid) {
            const auto& head = this->view_space_location.pose;
            const glm::vec3 head_position{head.position.x, head.position.y, head.position.z};
            const float length = glm::length(laser_end - laser_start);
            if (length > 0.05f && length < 200.0f) {
                const glm::vec3 middle = (laser_start + laser_end) * 0.5f;
                const glm::vec3 along = (laser_end - laser_start) / length;
                glm::vec3 normal = head_position - middle;
                normal -= along * glm::dot(normal, along);
                if (glm::length(normal) > 1e-3f) {
                    normal = glm::normalize(normal);
                    const glm::vec3 side = glm::cross(along, normal);
                    const glm::quat orientation = glm::normalize(glm::quat_cast(glm::mat3{side, along, normal}));
                    laser_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    laser_layer.space = this->stage_space;
                    laser_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_laser_beam, 3, 0, laser_layer.subImage);
                    laser_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                    laser_layer.pose.position = {middle.x, middle.y, middle.z};
                    laser_layer.size = {0.006f, length};
                    layers.push_back((XrCompositionLayerBaseHeader*)&laser_layer);
                    strip_submitted = true;
                }
            }
        }
        if (this->reticle_visible.load() && have_ray && (this->view_space_location.locationFlags & valid) == valid) {
            const auto& head = this->view_space_location.pose;
            const glm::vec3 head_position{head.position.x, head.position.y, head.position.z};
            const float distance = std::clamp(this->reticle_distance.load(), 0.2f, 100.0f);
            const glm::vec3 center = ray_origin + forward * distance;
            const glm::quat orientation = facing_head(center, head_position, -forward);
            const float width = 2.0f * distance * std::tan(glm::radians(std::clamp(this->reticle_degrees.load(), 0.2f, 40.0f)) * 0.5f);

            reticle_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            reticle_layer.space = this->stage_space;
            reticle_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            strip_cell(sprite == sprite_laser_dot ? sprite : std::clamp(sprite, 0, reticle_game_sprites - 1), 1, 1, reticle_layer.subImage);
            reticle_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
            reticle_layer.pose.position = {center.x, center.y, center.z};
            reticle_layer.size = {width, width};
            layers.push_back((XrCompositionLayerBaseHeader*)&reticle_layer);
            strip_submitted = true;
        }
    }

    // (fix142) The interface drawn over the world (a conversation) pointed at: where each controller's aim points on the picture the
    // interface's places are in, seen from that eye (a direction seen from the eye is a place in its picture, whatever the depth it
    // shows at); a short beam from the pointing hand, and a dot along that eye's line of sight at the interface's depth (it lies on
    // what is clicked, in both eyes). Not with the panel up (its own pointer).
    // (fix143) The eyes' distance apart, for the game side (the conversation's depth).
    if (this->stage_views.size() == 2) {
        const auto& e0 = this->stage_views[0].pose.position;
        const auto& e1 = this->stage_views[1].pose.position;
        const float apart = glm::length(glm::vec3{e1.x - e0.x, e1.y - e0.y, e1.z - e0.z});
        if (apart > 0.04f && apart < 0.09f) {
            this->ipd_m = apart;
        }
    }
    // (fix145) The conversation on its own panel, fixed in the room (the game side: set_conversation; its image filled this frame:
    // conv_image_ready), see-through where the interface is (the game's premultiplied picture), laid over the world before the pointer's
    // beam and dot (they show on it).
    XrCompositionLayerQuad conv_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    Conversation conv{};
    const int64_t conv_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // (Wanted: the game side placed it lately; then the old interface touch is not used: its box is the panel's layout, the review.)
    const bool conv_wanted = this->get_conversation(conv, conv_now_ms);
    const bool conv_shown = conv_wanted && render && !panel_ready && !this->conv_refused && this->conv_image_ready && this->conv_swapchain != XR_NULL_HANDLE &&
                            this->conv_image_width > 0 && this->conv_image_height > 0;
    if (conv_shown) {
        conv_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        conv_layer.space = this->stage_space;
        conv_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        conv_layer.subImage.swapchain = this->conv_swapchain;
        conv_layer.subImage.imageRect.offset = {0, 0};
        conv_layer.subImage.imageRect.extent = {this->conv_image_width, this->conv_image_height};
        conv_layer.subImage.imageArrayIndex = 0;
        conv_layer.pose = conv.pose;
        conv_layer.size = {conv.size[0], conv.size[1]};
        layers.push_back((XrCompositionLayerBaseHeader*)&conv_layer);
        if (this->conv_logs < 6) {
            ++this->conv_logs;
            const auto& head = this->view_space_location.pose.position;
            spdlog::info("[VR] Conversation panel shown: {:.2f} x {:.2f} m at {:.2f},{:.2f},{:.2f} (your head at {:.2f},{:.2f},{:.2f}), the picture's "
                         "{:.3f},{:.3f}-{:.3f},{:.3f} on it",
                conv.size[0], conv.size[1], conv.pose.position.x, conv.pose.position.y, conv.pose.position.z, head.x, head.y, head.z, conv.crop[0],
                conv.crop[1], conv.crop[2], conv.crop[3]);
        }
    }
    // (fix171) The favorites wheel (the game side: set_wheel, each frame it is open), fixed in the room, over the world and the gun sight:
    // only while its swapchain holds a picture of this opening (the graphics side copies one in as it comes: present_wheel). Never with the
    // menu panel up (the game side closes it as a menu opens).
    XrCompositionLayerQuad wheel_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad wheel_layer2{XR_TYPE_COMPOSITION_LAYER_QUAD}; // (fix174: the same picture at the other hand)
    WheelLayer wheel_now{};
    const bool wheel_wanted = this->get_wheel(wheel_now, conv_now_ms);
    const bool wheel_shown = wheel_wanted && render && !panel_ready && !this->panel_mode && !this->wheel_refused.load() &&
                             this->wheel_swapchain != XR_NULL_HANDLE && this->wheel_image_width > 0 && this->wheel_image_height > 0 &&
                             this->wheel_image_open.load() == wheel_now.open;
    if (wheel_shown) {
        wheel_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        wheel_layer.space = this->stage_space;
        wheel_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        wheel_layer.subImage.swapchain = this->wheel_swapchain;
        wheel_layer.subImage.imageRect.offset = {0, 0};
        wheel_layer.subImage.imageRect.extent = {this->wheel_image_width, this->wheel_image_height};
        wheel_layer.subImage.imageArrayIndex = 0;
        wheel_layer.pose = wheel_now.pose;
        wheel_layer.size = {wheel_now.size, wheel_now.size * (float)this->wheel_image_height / (float)this->wheel_image_width};
        layers.push_back((XrCompositionLayerBaseHeader*)&wheel_layer);
        if (wheel_now.second) {
            wheel_layer2      = wheel_layer;
            wheel_layer2.pose = wheel_now.pose2;
            layers.push_back((XrCompositionLayerBaseHeader*)&wheel_layer2);
        }
        if (this->wheel_logs < 6) {
            ++this->wheel_logs;
            const auto& head = this->view_space_location.pose.position;
            spdlog::info("[VR] (fix171) Favorites wheel shown: {:.2f} m wide at {:.2f},{:.2f},{:.2f} (your head at {:.2f},{:.2f},{:.2f}), {} layers this frame",
                wheel_now.size, wheel_now.pose.position.x, wheel_now.pose.position.y, wheel_now.pose.position.z, head.x, head.y, head.z, layers.size());
        }
    }

    PanelPointer hud{};
    // (fix143) The answers touched (HudTouch): its press counts carried on from frame to frame.
    HudTouch touch{};
    {
        std::scoped_lock _{this->pointer_mtx};
        touch.presses[0] = this->hud_touch.presses[0];
        touch.presses[1] = this->hud_touch.presses[1];
        touch.scroll[0] = this->hud_touch.scroll[0];
        touch.scroll[1] = this->hud_touch.scroll[1];
    }
    const int64_t hud_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (conv_shown && this->hud_pointer_wanted.load() && hud_now_ms - this->hud_pointer_ms.load() < 200 &&
        (this->view_space_location.locationFlags & valid) == valid) {
        // (fix145) The conversation's own panel pointed at and touched: each hand's aim and index fingertip met with the panel itself (its
        // plane in the room), the place found on it given as the place in the game's picture it shows (its crop), as before.
        const glm::quat conv_rotation{conv.pose.orientation.w, conv.pose.orientation.x, conv.pose.orientation.y, conv.pose.orientation.z};
        const glm::vec3 conv_center{conv.pose.position.x, conv.pose.position.y, conv.pose.position.z};
        const glm::quat to_conv = glm::inverse(conv_rotation);
        const glm::vec3 conv_front = conv_rotation * glm::vec3{0.0f, 0.0f, 1.0f};
        // (A place on the panel, metres from its middle with x right and y up, as a place in the game's picture; false off the panel.)
        const auto to_picture = [&](const glm::vec3& q, float& u, float& v) {
            const float pu = q.x / conv.size[0] + 0.5f;
            const float pv = 0.5f - q.y / conv.size[1];
            u = conv.crop[0] + pu * (conv.crop[2] - conv.crop[0]);
            v = conv.crop[1] + pv * (conv.crop[3] - conv.crop[1]);
            return pu >= 0.0f && pu <= 1.0f && pv >= 0.0f && pv <= 1.0f;
        };
        glm::vec3 start[2]{}, direction[2]{}, at[2]{};
        bool has_ray[2]{false, false};
        for (int h = 0; h < 2; ++h) {
            const auto& hand = this->hands[h];
            if (hand.aim_space == XR_NULL_HANDLE || (hand.aim_location.locationFlags & valid) != valid) {
                continue;
            }
            const auto& aim = hand.aim_location.pose;
            const glm::quat aim_rotation{aim.orientation.w, aim.orientation.x, aim.orientation.y, aim.orientation.z};
            start[h] = glm::vec3{aim.position.x, aim.position.y, aim.position.z};
            direction[h] = glm::normalize(aim_rotation * glm::vec3{0.0f, 0.0f, -1.0f});
            has_ray[h] = true;
            const float toward = glm::dot(direction[h], conv_front);
            if (toward > -0.05f) {
                continue; // (not toward its front)
            }
            const float t = glm::dot(conv_center - start[h], conv_front) / toward;
            if (!(t > 0.0f) || t > 20.0f) {
                continue; // (the hand past it, or too far)
            }
            const glm::vec3 hit = start[h] + direction[h] * t;
            float u = 0.0f, v = 0.0f;
            if (!to_picture(to_conv * (hit - conv_center), u, v)) {
                continue;
            }
            hud.hit[h] = true;
            hud.u[h] = u;
            hud.v[h] = v;
            at[h] = hit;
        }
        // The answers touched: each hand's index fingertip (the runtime's finger joint; else a little in front of the controller) against
        // the panel: how far in front of it (gap) and where on it.
        int touching = -1;
        glm::vec3 touch_at{};
        if (this->hud_touch_wanted.load()) {
            const float box[4]{this->hud_touch_box[0].load(), this->hud_touch_box[1].load(), this->hud_touch_box[2].load(), this->hud_touch_box[3].load()};
            float closest = 1e9f;
            for (int h = 0; h < 2; ++h) {
                auto& ts = this->touch_state[h];
                glm::vec3 tip{};
                bool have = false;
                const auto& joints = this->hand_joints[h];
                const auto& tip_joint = joints.joints[XR_HAND_JOINT_INDEX_TIP_EXT];
                if (joints.active && (tip_joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                    tip = glm::vec3{tip_joint.pose.position.x, tip_joint.pose.position.y, tip_joint.pose.position.z};
                    have = true;
                } else if (has_ray[h]) {
                    tip = start[h] + direction[h] * 0.05f;
                    have = true;
                }
                // (Only a pointing finger, off the trigger: the game side, hud_touch_finger; a hand holding the trigger points the beam.)
                if (!have || !this->hud_touch_finger[h].load()) {
                    ts.hovering = ts.armed = ts.down = ts.frozen = ts.dragged = false;
                    continue;
                }
                // (A press left from before the panel went for a moment is dropped, not a tap at its old place: the review.)
                if (ts.seen_ms != 0 && hud_now_ms - ts.seen_ms > 200) {
                    ts.hovering = ts.armed = ts.down = ts.frozen = ts.dragged = false;
                    ts.hold_until_ms = 0;
                }
                ts.seen_ms = hud_now_ms;
                const glm::vec3 q = to_conv * (tip - conv_center);
                const float gap = q.z; // (in front of the panel above 0, through it below)
                float u = 0.0f, v = 0.0f;
                const bool on_panel = to_picture(q, u, v);
                const bool in_box = on_panel && u >= box[0] && u <= box[2] && v >= box[1] && v <= box[3];
                // (The finger's place for the game side, and the nearest finger's dot.)
                const auto report = [&](float ru, float rv) {
                    touch.hover[h] = true;
                    touch.u[h] = ru;
                    touch.v[h] = rv;
                    touch.gap[h] = gap;
                    if (gap < closest) {
                        closest = gap;
                        touching = h;
                        touch_at = conv_center + conv_rotation * glm::vec3{q.x, q.y, 0.002f};
                    }
                };
                // (fix146) Pressed through (below): let go (back 2 cm in front, or off the panel) without having moved, a tap: the answer
                // chosen, counted then (as a phone's: a drag is no choice), where it was pressed; moved up or down 2 cm while through, a
                // drag: the answers scrolled a mouse-wheel notch each 2.5 cm (the finger up, the list up, as a phone's), no tap.
                if (ts.down) {
                    if (gap < 0.02f && on_panel) {
                        const float dy = q.y - ts.down_y;
                        // (Along the panel more than deeper into it: a push at an angle moves the tip along it too, no drag; the review.)
                        if (!ts.dragged && std::abs(dy) > 0.025f && std::abs(dy) > std::abs(gap - ts.down_gap) + 0.005f) {
                            ts.dragged = true;
                            ts.scrolled_y = 0.0f;
                            if (this->hud_touch_logs < 60) {
                                ++this->hud_touch_logs;
                                spdlog::info("[VR] The conversation's panel dragged with the {} hand: scrolling", h == 0 ? "left" : "right");
                            }
                        }
                        if (ts.dragged) {
                            const int notches = (int)((dy - ts.scrolled_y) / 0.025f);
                            if (notches != 0) {
                                touch.scroll[h] -= notches; // (the finger up: the wheel down, the list moving up)
                                ts.scrolled_y += (float)notches * 0.025f;
                            }
                        }
                        report(ts.frozen_u, ts.frozen_v);
                        continue;
                    }
                    const bool tap = !ts.dragged && hud_now_ms - ts.last_press_ms > 350;
                    ts.down = false;
                    ts.dragged = false;
                    if (tap) {
                        ts.last_press_ms = hud_now_ms;
                        ++touch.presses[h];
                        // (Its place held a moment, for the click: the game side clicks where the cursor is as it arrives; the review.)
                        ts.hold_until_ms = hud_now_ms + 300;
                        ts.hold_u = ts.frozen_u;
                        ts.hold_v = ts.frozen_v;
                        if (this->hud_touch_logs < 60) {
                            ++this->hud_touch_logs;
                            spdlog::info("[VR] Answer tapped with the {} hand at {:.3f},{:.3f} of the game's picture, on the conversation's panel",
                                h == 0 ? "left" : "right", ts.frozen_u, ts.frozen_v);
                        }
                        ts.hovering = true;
                        ts.armed = false;
                        report(ts.frozen_u, ts.frozen_v);
                        continue;
                    }
                    ts.frozen = false;
                }
                if (hud_now_ms < ts.hold_until_ms) {
                    ts.hovering = true;
                    if (gap >= 0.01f) {
                        ts.armed = true;
                    }
                    report(ts.hold_u, ts.hold_v);
                    continue;
                }
                const bool hover = in_box && gap >= -0.08f && gap <= (ts.hovering ? 0.13f : 0.10f);
                if (!hover) {
                    ts.hovering = ts.armed = ts.frozen = false;
                    continue;
                }
                ts.hovering = true;
                if (gap >= 0.01f) {
                    ts.armed = true;
                    ts.frozen = false;
                } else if (!ts.frozen) {
                    ts.frozen = true; // (its place held from here: the finger's wobble as it pushes does not move it)
                    ts.frozen_u = u;
                    ts.frozen_v = v;
                }
                if (ts.frozen) {
                    u = ts.frozen_u;
                    v = ts.frozen_v;
                }
                if (ts.armed && gap <= -0.005f) {
                    ts.down = true;
                    ts.armed = false;
                    ts.dragged = false;
                    ts.down_y = q.y;
                    ts.down_gap = gap;
                    ts.scrolled_y = 0.0f;
                    if (!ts.frozen) {
                        ts.frozen = true;
                        ts.frozen_u = u;
                        ts.frozen_v = v;
                    }
                    if (this->hud_touch_logs < 60) {
                        ++this->hud_touch_logs;
                        spdlog::info("[VR] Answer pressed with the {} hand at {:.3f},{:.3f} of the game's picture, on the conversation's panel (the "
                                     "finger {:.3f} m through)",
                            h == 0 ? "left" : "right", u, v, -gap);
                    }
                }
                report(u, v);
            }
        } else {
            for (auto& ts : this->touch_state) {
                ts.hovering = ts.armed = ts.down = ts.frozen = false;
            }
        }
        hud.shown = true;
        const int preferred = std::clamp(this->panel_preferred_hand.load(), 0, 1);
        hud.hand = hud.hit[preferred] ? preferred : (hud.hit[1 - preferred] ? 1 - preferred : -1);
        const int beam_hand = hud.hand >= 0 ? hud.hand : preferred;
        const auto& head_pose = this->view_space_location.pose;
        const glm::vec3 head{head_pose.position.x, head_pose.position.y, head_pose.position.z};
        if (touching >= 0 && strip_usable && this->panel_pointer_enabled.load()) {
            // A finger at the answers: a small dot where it touches the panel, no beam.
            const float to_head = glm::length(head - touch_at);
            if (to_head > 0.05f) {
                const float size = 2.0f * to_head * std::tan(glm::radians(1.0f) * 0.5f);
                dot_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                dot_layer.space = this->stage_space;
                dot_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                strip_cell(sprite_pointer_dot, 1, 1, dot_layer.subImage);
                dot_layer.pose.orientation = conv.pose.orientation;
                dot_layer.pose.position = {touch_at.x, touch_at.y, touch_at.z};
                dot_layer.size = {size, size};
                layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer);
                strip_submitted = true;
            }
        } else if (strip_usable && this->panel_pointer_enabled.load() && has_ray[beam_hand]) {
            // The beam: a short one from the hand, along its aim; the dot where it meets the panel.
            const glm::vec3 b0 = start[beam_hand];
            const glm::vec3 b1 = b0 + direction[beam_hand] * 0.25f;
            const glm::vec3 middle_point = (b0 + b1) * 0.5f;
            const glm::vec3 along = direction[beam_hand];
            glm::vec3 normal = head - middle_point;
            normal -= along * glm::dot(normal, along);
            if (glm::length(normal) > 1e-3f) {
                normal = glm::normalize(normal);
                const glm::vec3 side = glm::cross(along, normal);
                const glm::quat orientation = glm::normalize(glm::quat_cast(glm::mat3{side, along, normal}));
                beam_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                beam_layer.space = this->stage_space;
                beam_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                strip_cell(sprite_beam, 3, 0, beam_layer.subImage);
                beam_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                beam_layer.pose.position = {middle_point.x, middle_point.y, middle_point.z};
                beam_layer.size = {0.012f, 0.25f};
                layers.push_back((XrCompositionLayerBaseHeader*)&beam_layer);
                strip_submitted = true;
            }
            if (hud.hand >= 0) {
                const glm::vec3 dot_at = at[beam_hand] + conv_front * 0.002f;
                const float to_head = glm::length(head - dot_at);
                if (to_head > 0.05f) {
                    const float size = 2.0f * to_head * std::tan(glm::radians(1.6f) * 0.5f);
                    dot_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    dot_layer.space = this->stage_space;
                    dot_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_pointer_dot, 1, 1, dot_layer.subImage);
                    dot_layer.pose.orientation = conv.pose.orientation;
                    dot_layer.pose.position = {dot_at.x, dot_at.y, dot_at.z};
                    dot_layer.size = {size, size};
                    layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer);
                    strip_submitted = true;
                }
            }
        }
        if (this->hud_pointer_logs < 6 && hud.hand >= 0) {
            ++this->hud_pointer_logs;
            spdlog::info("[VR] Interface pointer: the {} hand points at {:.3f},{:.3f} of the game's picture, on the conversation's panel",
                hud.hand == 0 ? "left" : "right", hud.u[hud.hand], hud.v[hud.hand]);
        }
    } else if (render && !panel_ready && this->hud_pointer_wanted.load() && hud_now_ms - this->hud_pointer_ms.load() < 200 && this->stage_views.size() == 2 &&
        (this->view_space_location.locationFlags & valid) == valid) {
        const int eye = this->hud_pointer_eye.load();
        const bool middle = eye < 0 || eye > 1;
        const XrPosef from = middle ? this->view_space_location.pose : this->stage_views[eye].pose;
        Vector4f f{};
        {
            std::shared_lock _{this->eyes_mtx};
            f = middle ? this->mono_frustum : this->eye_frustums[eye];
        }
        if (f[1] > f[0] && f[2] > f[3]) {
            const glm::quat eye_rotation{from.orientation.w, from.orientation.x, from.orientation.y, from.orientation.z};
            const glm::vec3 eye_position{from.position.x, from.position.y, from.position.z};
            const glm::quat to_eye = glm::inverse(eye_rotation);
            const auto& p0 = this->stage_views[0].pose.position;
            const auto& p1 = this->stage_views[1].pose.position;
            const float ipd = glm::length(glm::vec3{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z});
            const float shift = this->hud_pointer_shift.load();
            // (No shift: the interface at no depth, as far as the eye can tell: the dot far away too, not doubled against it.)
            const float depth = shift > 1e-4f && ipd > 0.01f ? std::clamp(ipd / shift, 0.2f, 20.0f) : 20.0f;
            glm::vec3 start[2]{}, direction[2]{}, at[2]{};
            bool has_ray[2]{false, false};
            for (int h = 0; h < 2; ++h) {
                const auto& hand = this->hands[h];
                if (hand.aim_space == XR_NULL_HANDLE || (hand.aim_location.locationFlags & valid) != valid) {
                    continue;
                }
                const auto& aim = hand.aim_location.pose;
                const glm::quat aim_rotation{aim.orientation.w, aim.orientation.x, aim.orientation.y, aim.orientation.z};
                start[h] = glm::vec3{aim.position.x, aim.position.y, aim.position.z};
                direction[h] = glm::normalize(aim_rotation * glm::vec3{0.0f, 0.0f, -1.0f});
                has_ray[h] = true;
                const glm::vec3 d = to_eye * direction[h];
                if (d.z > -0.05f) {
                    continue; // (not ahead of the eye)
                }
                // (fix144) Where the hand's ray meets the interface's plane (its depth ahead of the eye), seen from the eye: up close (a
                // conversation at arm's length) the hand's own place counts; far off, or the hand past the plane, its direction alone.
                glm::vec3 meet = d * (depth / -d.z);
                if (depth < 10.0f) {
                    const glm::vec3 o = to_eye * (start[h] - eye_position);
                    if (o.z > -depth) {
                        const float t = (-depth - o.z) / d.z;
                        if (t > 0.0f) {
                            meet = o + d * t;
                        }
                    }
                }
                const float tx = meet.x / -meet.z;
                const float ty = meet.y / -meet.z;
                const float u = (tx - f[0]) / (f[1] - f[0]);
                const float v = (f[2] - ty) / (f[2] - f[3]);
                if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
                    continue;
                }
                hud.hit[h] = true;
                hud.u[h] = u;
                hud.v[h] = v;
                at[h] = eye_position + eye_rotation * meet; // (on the interface's plane, straight ahead of the eye at its depth)
            }
            // (fix143) The answers touched: each hand's index fingertip (the runtime's finger joint; else a little in front of the
            // controller) seen from the interface's eye, against the interface's depth (no depth, mono: 0.45 m, within reach).
            int touching = -1;
            glm::vec3 touch_at{};
            if (this->hud_touch_wanted.load() && !conv_wanted) {
                const float plane = shift > 1e-4f && ipd > 0.01f ? std::clamp(ipd / shift, 0.2f, 2.0f) : 0.45f;
                const float box[4]{this->hud_touch_box[0].load(), this->hud_touch_box[1].load(), this->hud_touch_box[2].load(), this->hud_touch_box[3].load()};
                float closest = 1e9f;
                for (int h = 0; h < 2; ++h) {
                    auto& ts = this->touch_state[h];
                    glm::vec3 tip{};
                    bool have = false;
                    const auto& joints = this->hand_joints[h];
                    const auto& tip_joint = joints.joints[XR_HAND_JOINT_INDEX_TIP_EXT];
                    if (joints.active && (tip_joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                        tip = glm::vec3{tip_joint.pose.position.x, tip_joint.pose.position.y, tip_joint.pose.position.z};
                        have = true;
                    } else if (has_ray[h]) {
                        tip = start[h] + direction[h] * 0.05f;
                        have = true;
                    }
                    // (Only a pointing finger, off the trigger: the game side, hud_touch_finger; a hand holding the trigger points the beam.)
                    const glm::vec3 pe = have ? to_eye * (tip - eye_position) : glm::vec3{0.0f};
                    if (!have || !this->hud_touch_finger[h].load() || pe.z > -0.05f) {
                        ts.hovering = ts.armed = ts.down = ts.frozen = false;
                        continue;
                    }
                    const float gap = plane + pe.z; // (in front of the interface above 0, through it below)
                    float u = (pe.x / -pe.z - f[0]) / (f[1] - f[0]);
                    float v = (f[2] - pe.y / -pe.z) / (f[2] - f[3]);
                    const bool in_box = u >= box[0] && u <= box[2] && v >= box[1] && v <= box[3];
                    const bool hover = in_box && gap >= -0.08f && gap <= (ts.hovering ? 0.13f : 0.10f);
                    if (!hover) {
                        ts.hovering = ts.armed = ts.frozen = false;
                        if (gap >= 0.02f) {
                            ts.down = false;
                        }
                        continue;
                    }
                    ts.hovering = true;
                    if (gap >= 0.01f) {
                        ts.armed = true;
                        ts.frozen = false;
                    } else if (!ts.frozen) {
                        ts.frozen = true; // (its place held from here: the finger's wobble as it pushes does not move it)
                        ts.frozen_u = u;
                        ts.frozen_v = v;
                    }
                    if (ts.frozen) {
                        u = ts.frozen_u;
                        v = ts.frozen_v;
                    }
                    if (ts.down && gap >= 0.02f) {
                        ts.down = false;
                    }
                    if (ts.armed && !ts.down && gap <= -0.005f && hud_now_ms - ts.last_press_ms > 350) {
                        ts.down = true;
                        ts.armed = false;
                        ts.last_press_ms = hud_now_ms;
                        ++touch.presses[h];
                        if (this->hud_touch_logs < 20) {
                            ++this->hud_touch_logs;
                            spdlog::info("[VR] Answer touched with the {} hand at {:.3f},{:.3f} of the {} picture (the answers {:.2f} m away, the finger "
                                         "{:.3f} m through)",
                                h == 0 ? "left" : "right", u, v, middle ? "head's" : (eye == 0 ? "left eye's" : "right eye's"), plane, -gap);
                        }
                    }
                    touch.hover[h] = true;
                    touch.u[h] = u;
                    touch.v[h] = v;
                    touch.gap[h] = gap;
                    if (gap < closest) {
                        closest = gap;
                        touching = h;
                        touch_at = eye_position + eye_rotation * (pe * (plane / -pe.z));
                    }
                }
            } else {
                for (auto& ts : this->touch_state) {
                    ts.hovering = ts.armed = ts.down = ts.frozen = false;
                }
            }
            hud.shown = true;
            const int preferred = std::clamp(this->panel_preferred_hand.load(), 0, 1);
            hud.hand = hud.hit[preferred] ? preferred : (hud.hit[1 - preferred] ? 1 - preferred : -1);
            const int beam_hand = hud.hand >= 0 ? hud.hand : preferred;
            if (touching >= 0 && strip_usable && this->panel_pointer_enabled.load()) {
                // (fix143) A finger at the answers: a small dot where it touches them, no beam.
                const auto& head_pose = this->view_space_location.pose;
                const glm::vec3 head{head_pose.position.x, head_pose.position.y, head_pose.position.z};
                const float to_head = glm::length(head - touch_at);
                if (to_head > 0.05f) {
                    const glm::quat orientation = facing_head(touch_at, head, glm::vec3{0.0f, 0.0f, 1.0f});
                    const float size = 2.0f * to_head * std::tan(glm::radians(1.0f) * 0.5f);
                    dot_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    dot_layer.space = this->stage_space;
                    dot_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_pointer_dot, 1, 1, dot_layer.subImage);
                    dot_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                    dot_layer.pose.position = {touch_at.x, touch_at.y, touch_at.z};
                    dot_layer.size = {size, size};
                    layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer);
                    strip_submitted = true;
                }
            } else if (strip_usable && this->panel_pointer_enabled.load() && has_ray[beam_hand]) {
                const auto& head_pose = this->view_space_location.pose;
                const glm::vec3 head{head_pose.position.x, head_pose.position.y, head_pose.position.z};
                // The beam: a short one from the hand, along its aim (the dot, far or near, shows where it points).
                const glm::vec3 b0 = start[beam_hand];
                const glm::vec3 b1 = b0 + direction[beam_hand] * 0.25f;
                const glm::vec3 middle_point = (b0 + b1) * 0.5f;
                const glm::vec3 along = direction[beam_hand];
                glm::vec3 normal = head - middle_point;
                normal -= along * glm::dot(normal, along);
                if (glm::length(normal) > 1e-3f) {
                    normal = glm::normalize(normal);
                    const glm::vec3 side = glm::cross(along, normal);
                    const glm::quat orientation = glm::normalize(glm::quat_cast(glm::mat3{side, along, normal}));
                    beam_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    beam_layer.space = this->stage_space;
                    beam_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    strip_cell(sprite_beam, 3, 0, beam_layer.subImage);
                    beam_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                    beam_layer.pose.position = {middle_point.x, middle_point.y, middle_point.z};
                    beam_layer.size = {0.012f, 0.25f};
                    layers.push_back((XrCompositionLayerBaseHeader*)&beam_layer);
                    strip_submitted = true;
                }
                if (hud.hand >= 0) {
                    const glm::vec3 dot_at = at[beam_hand];
                    const float to_head = glm::length(head - dot_at);
                    if (to_head > 0.05f) {
                        const glm::quat orientation = facing_head(dot_at, head, -direction[beam_hand]);
                        const float size = 2.0f * to_head * std::tan(glm::radians(1.6f) * 0.5f);
                        dot_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        dot_layer.space = this->stage_space;
                        dot_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        strip_cell(sprite_pointer_dot, 1, 1, dot_layer.subImage);
                        dot_layer.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
                        dot_layer.pose.position = {dot_at.x, dot_at.y, dot_at.z};
                        dot_layer.size = {size, size};
                        layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer);
                        strip_submitted = true;
                    }
                }
            }
            if (this->hud_pointer_logs < 6 && hud.hand >= 0) {
                ++this->hud_pointer_logs;
                spdlog::info("[VR] Interface pointer: the {} hand points at {:.3f},{:.3f} of the {} picture (the interface's depth {:.2f} m)",
                    hud.hand == 0 ? "left" : "right", hud.u[hud.hand], hud.v[hud.hand], middle ? "head's" : (eye == 0 ? "left eye's" : "right eye's"), depth);
            }
        }
    }

    // (fix177) Build mode's console (the game side: set_console, each frame build mode is drawn on it; its picture filled this frame:
    // console_image_ready): each part a quad of its own, a crop of the one picture, in the room (stage space) or at your head (view space),
    // see-through where the menu is (the game's premultiplied picture), over the world. Never with the menu panel up. Only as many as keep
    // the frame within the runtime's layer limit (one kept free for the Virtual Desktop fix's layer, put first below).
    std::array<XrCompositionLayerQuad, kConsoleQuads> console_layers{};
    Console console_now{};
    const bool console_wanted = this->get_console(console_now, conv_now_ms);
    const bool console_shown = console_wanted && render && !panel_ready && !this->panel_mode && !this->console_refused.load() && this->console_image_ready &&
                               this->console_swapchain != XR_NULL_HANDLE && this->console_image_width > 0 && this->console_image_height > 0;
    int console_submitted = 0;
    if (console_shown) {
        const size_t limit = this->max_layers > 1 ? this->max_layers - 1 : 1;
        int left_out = 0;
        for (int i = 0; i < console_now.count && i < kConsoleQuads; ++i) {
            const auto& q = console_now.quads[i];
            // (Its crop inside the picture, a real size, a finite pose: else left out, not refused by the runtime.)
            const bool inside = q.rect[0] >= 0 && q.rect[1] >= 0 && q.rect[2] > 0 && q.rect[3] > 0 && q.rect[0] + q.rect[2] <= this->console_image_width &&
                                q.rect[1] + q.rect[3] <= this->console_image_height;
            const bool finite = std::isfinite(q.pose.position.x) && std::isfinite(q.pose.position.y) && std::isfinite(q.pose.position.z) &&
                                std::isfinite(q.pose.orientation.w) && std::abs(q.pose.orientation.x * q.pose.orientation.x + q.pose.orientation.y * q.pose.orientation.y +
                                                                               q.pose.orientation.z * q.pose.orientation.z + q.pose.orientation.w * q.pose.orientation.w - 1.0f) < 0.01f;
            if (!inside || !finite || !(q.size[0] > 0.005f) || !(q.size[1] > 0.005f) || q.size[0] > 5.0f || q.size[1] > 5.0f) {
                continue;
            }
            if (layers.size() >= limit) {
                ++left_out;
                continue;
            }
            auto& l = console_layers[console_submitted++];
            l = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            l.space = q.space == 1 ? this->view_space : this->stage_space;
            l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            l.subImage.swapchain = this->console_swapchain;
            l.subImage.imageRect.offset = {q.rect[0], q.rect[1]};
            l.subImage.imageRect.extent = {q.rect[2], q.rect[3]};
            l.subImage.imageArrayIndex = 0;
            l.pose = q.pose;
            l.size = {q.size[0], q.size[1]};
            layers.push_back((XrCompositionLayerBaseHeader*)&l);
        }
        if (left_out > 0) {
            this->console_dropped += (uint32_t)left_out;
        }
        if (this->console_logs < 6) {
            ++this->console_logs;
            const auto& head = this->view_space_location.pose.position;
            spdlog::info("[VR] (fix177) Build console shown: {} quads of its {}x{} picture ({} left out: the runtime's limit of {} layers), {} layers "
                         "this frame (your head at {:.2f},{:.2f},{:.2f})",
                console_submitted, this->console_image_width, this->console_image_height, left_out, this->max_layers, layers.size(), head.x, head.y,
                head.z);
        }
    }

    // Where the controllers point at the panel, for the game side (its mouse).
    {
        std::scoped_lock _{this->pointer_mtx};
        pointer.frame = this->panel_pointer.frame + (pointer.shown ? 1u : 0u);
        pointer.at = std::chrono::steady_clock::now();
        this->panel_pointer = pointer;
        hud.frame = this->hud_pointer.frame + (hud.shown ? 1u : 0u);
        hud.at = pointer.at;
        this->hud_pointer = hud;
        this->hud_touch = touch;
    }

    // Virtual Desktop puts all the layers together using the first layer's head position (its developer's explanation, quoted in
    // UEVR's "Virtual Desktop Fix", which does the same). The world picture behind a menu carries the head position it was drawn
    // from (the world as it was: from before the menu opened), so the panel, though put in the room, moved with the head; at the
    // main menu, with no world picture, it stayed. So first an empty layer (a fully see-through corner of the strip's first
    // picture) with this frame's head position, as located for its display time: the rest is put together from there.
    XrCompositionLayerProjection vd_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> vd_views{};
    if (this->vd_fix.load() && strip_usable && !layers.empty() && this->stage_views.size() == 2) {
        // (Its head position this frame's: the frame's kept poses (pipelined: the located ones may be frames ahead of it by now), else
        // the latest located; not the world layer's, which behind a menu is the world as it was, fix136's review.)
        std::shared_lock pose_lock{this->pose_mtx};
        for (size_t i = 0; i < 2; ++i) {
            auto& view = vd_views[i];
            view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            if (this->submit_valid) {
                view.pose = this->mono_views ? this->submit_head : this->submit_pose[i];
            } else {
                view.pose = this->stage_views[i].pose;
            }
            view.fov = this->stage_views[i].fov;
            view.subImage.swapchain = this->reticle_swapchain;
            view.subImage.imageRect.offset = {0, 0};
            view.subImage.imageRect.extent = {2, 2};
            view.subImage.imageArrayIndex = 0;
        }
        vd_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        vd_layer.space = this->stage_space;
        vd_layer.viewCount = 2;
        vd_layer.views = vd_views.data();
        layers.insert(layers.begin(), (XrCompositionLayerBaseHeader*)&vd_layer);
        strip_submitted = true;
        if (!this->vd_fix_logged) {
            this->vd_fix_logged = true;
            spdlog::info("[VR] Virtual Desktop fix: an empty layer with this frame's head position is sent first ({} layers this frame)", layers.size());
        }
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = (uint32_t)layers.size();
    frame_end_info.layers = layers.data();

    //spdlog::info("[VR] Ending frame, {} layers", frame_end_info.layerCount);
    //spdlog::info("[VR] Ending frame, layer ptr: {:x}", (uintptr_t)frame_end_info.layers);

    this->begin_profile();
    const auto end_start = std::chrono::steady_clock::now(); // (fix179: its time added up, for the menus' slow frames)
    auto result = xrEndFrame(this->session, &frame_end_info);
    this->stat_end_total_us.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - end_start).count(), std::memory_order_relaxed);
    this->end_profile("xrEndFrame");

    if (result != XR_SUCCESS) {
        static std::atomic<int> told{0}; // (fix189)
        if (FirstTimes(told, "xrEndFrame")) {
            spdlog::error("[VR] xrEndFrame failed: {}", this->get_result_string(result));
        }
    }

    // (Only errors about the layers count against them: not the session ending, the headset going to sleep...)
    const bool layers_refused = result == XR_ERROR_LAYER_INVALID || result == XR_ERROR_SWAPCHAIN_RECT_INVALID || result == XR_ERROR_POSE_INVALID ||
                                result == XR_ERROR_VALIDATION_FAILURE || result == XR_ERROR_LAYER_LIMIT_EXCEEDED ||
                                result == XR_ERROR_HANDLE_INVALID || result == XR_ERROR_FEATURE_UNSUPPORTED;

    // (fix164) A frame refused with the soft light behind the panel in it: the light, the newest layer, is what is held back first (the
    // next three frames go without it, and the strip and the panel are not blamed for this frame); refused again the same way (twice in a
    // row), it is not shown again this session.
    if (ambient_submitted) {
        if (layers_refused) {
            this->ambient_skip_frames = 3;
            if (++this->ambient_refusals >= 2 && !this->ambient_refused) {
                this->ambient_refused = true;
                spdlog::warn("[VR] Ambient light: the runtime refused frames with it twice ({}); it is off from now on", this->get_result_string(result));
            }
        } else if (XR_SUCCEEDED(result)) {
            this->ambient_refusals = 0;
        }
    }

    // A runtime that refuses the strip's layers would otherwise lose every frame while they are shown: after a refused frame
    // with them, the next ones go without them; if those are taken, it counts against them (three times: they stay hidden).
    if (layers_refused && ambient_submitted) {
        // (fix164: the soft light held back first, above: the strip neither blamed nor cleared by this frame.)
    } else if (layers_refused && strip_submitted) {
        this->reticle_skip_frames = 2;
        this->reticle_suspect = true;
    } else if (strip_submitted) {
        this->reticle_refusals = 0;
        this->reticle_suspect = false;
    } else if (this->reticle_suspect && !layers.empty()) {
        this->reticle_suspect = false;
        if (XR_SUCCEEDED(result) && ++this->reticle_refusals >= 3) {
            this->reticle_ready = false;
            spdlog::warn("[VR] The runtime refused frames with the gun sight's layer three times; the sight stays hidden");
        }
    }

    // The same for the panel (only frames without the strip's layers: then the panel is what was refused): curved twice in a
    // row, it is flat from then on; flat three times, menus go back to the old screen that moves with your head.
    if (panel_kind != 0 && !strip_submitted && !(ambient_submitted && layers_refused)) { // (fix164: not for a frame the light is held back for)
        if (layers_refused) {
            if (panel_kind == 2 && ++this->panel_cylinder_refusals >= 2 && !this->panel_cylinder_refused) {
                this->panel_cylinder_refused = true;
                spdlog::warn("[VR] Menu panel: the runtime refused the curved panel ({}); it is flat from now on", this->get_result_string(result));
            } else if (panel_kind == 1 && ++this->panel_refusals >= 3 && !this->panel_refused) {
                this->panel_refused = true;
                spdlog::warn("[VR] Menu panel: the runtime refused the panel ({}); menus are on the old screen from now on",
                    this->get_result_string(result));
            }
        } else if (XR_SUCCEEDED(result) && panel_kind == 2) {
            this->panel_cylinder_refusals = 0;
        } else if (XR_SUCCEEDED(result)) {
            this->panel_refusals = 0;
        }
    }

    // (fix145) The conversation's panel likewise (frames without the strip's layers: then it is what was refused): three times, not shown
    // again this session (the game side then puts the conversation back in the picture, conv_shown_ms no longer moving on).
    if (conv_shown) {
        if (layers_refused && !strip_submitted) {
            if (++this->conv_refusals >= 3 && !this->conv_refused) {
                this->conv_refused = true;
                spdlog::warn("[VR] Conversation panel: the runtime refused it ({}); conversations are in the picture from now on",
                    this->get_result_string(result));
            }
        } else if (XR_SUCCEEDED(result)) {
            this->conv_refusals = 0;
            this->conv_shown_ms = conv_now_ms;
        }
    }
    // (fix171) The favorites wheel likewise (frames without the strip's layers: then it is what was refused): three times, not shown again
    // this session (the game side then gives the favorites back to the game's own wheel).
    if (wheel_shown) {
        if (layers_refused && !strip_submitted) {
            if (++this->wheel_refusals >= 3 && !this->wheel_refused.load()) {
                this->wheel_refused = true;
                spdlog::warn("[VR] (fix171) Favorites wheel: the runtime refused it ({}); the game's own favorites from now on",
                    this->get_result_string(result));
            }
        } else if (XR_SUCCEEDED(result)) {
            this->wheel_refusals = 0;
            this->wheel_shown_ms = conv_now_ms;
        }
    }
    // (fix177) Build mode's console: frames refused with its quads in them three times in a row (whatever else was in them: the console is
    // the newest), not shown again this session (the game side then puts the build menu back in the view, console_shown_ms no longer
    // moving on).
    if (console_submitted > 0) {
        if (layers_refused) {
            if (++this->console_refusals >= 3 && !this->console_refused.exchange(true)) {
                spdlog::warn("[VR] (fix177) Build console: the runtime refused frames with it three times ({}); build mode is in the view from now on",
                    this->get_result_string(result));
            }
        } else if (XR_SUCCEEDED(result)) {
            this->console_refusals = 0;
            this->console_shown_ms = conv_now_ms;
        }
    }

    this->last_world_views_valid = world_submitted && XR_SUCCEEDED(result);

    ++this->stat_end_frames;
    this->stat_end_frames_with_layers += layers.empty() ? 0 : 1;
    this->stat_last_end_result = result;
    // (Pipelined: until the next xrWaitFrame (right after this present), the next frame's display time is this one's plus a period: the
    // game thread, woken as this present finishes, locates for it, not for this one's, fix136's review.)
    if (this->pipelined && XR_SUCCEEDED(result) && this->frame_state.predictedDisplayPeriod > 0) {
        this->published_display_time = this->frame_state.predictedDisplayTime + this->frame_state.predictedDisplayPeriod;
    }

    this->frame_began = false;
    this->frame_synced = false;
    this->image_ready = {};
    this->submit_valid = false;
    this->aer_frame = false;
    this->panel_image_ready = false;
    this->frozen_ready = false;
    this->live_world_ready = false;
    this->ambient_ready = false; // (fix164)
    this->console_image_ready = false; // (fix177)

    return result;
}
}
