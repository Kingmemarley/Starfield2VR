#include "MotionWeapon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <glm/gtc/matrix_transform.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include "NiMath.h"
#include "NodeTreeDump.h"
#include "RE/P/PlayerCamera.h"
#include "RE/N/NiCamera.h"
#include "body/BodyIK.h"
#include "vr/TrackingSpace.h"
#include <CreationEngine/memory/ScanHelper.h>
#include <mods/VR.hpp>

namespace {
    using NiMath::to_glm;

    // The first-person gun follows the controller only while the body is not shown: with the body
    // shown, the third-person gun is held by the body's hand and the first-person rig must keep its
    // animated grip (the body copies the hand's grip on the gun from it).
    // Also off while aiming with the right hand: the camera's parent then turns with the controller,
    // not the headset, and this placement assumes the headset.
    bool enabled() {
        return ModConstants::gunFollowsController && !body::IsActive() && !tracking::AimingWithHand();
    }

    struct Xform {
        glm::mat3 r{1.0f};
        glm::vec3 t{0.0f};
        float     s{1.0f};

        Xform operator*(const Xform& o) const { return {r * o.r, t + r * (o.t * s), s * o.s}; }
        Xform inverse() const {
            const float     inv_s = s != 0.0f ? 1.0f / s : 1.0f;
            const glm::mat3 rt = glm::transpose(r);
            return {rt, -(rt * t) * inv_s, inv_s};
        }
    };

    Xform to_xform(const RE::NiTransform& n) { return {to_glm(n.rotate), to_glm(n.translate), n.scale}; }

    // VR tracking space (x right, y up, z back) to the game's axes (x right, y forward, z up).
    const glm::mat3 vr_to_game{glm::vec3{1, 0, 0}, glm::vec3{0, 0, 1}, glm::vec3{0, -1, 0}};

    float angle_between(const glm::mat3& a, const glm::mat3& b) {
        const glm::mat3 d = glm::transpose(a) * b;
        const float     c = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
        return glm::degrees(std::acos(c));
    }

    // The nearest rotation that only swaps and flips axes.
    glm::mat3 snap_to_axes(const glm::mat3& m) {
        glm::mat3 out(0.0f);
        bool      col_used[3]{}, row_used[3]{};
        int       last_col = 0;
        for (int n = 0; n < 3; ++n) {
            int   best_c = -1, best_r = -1;
            float best = -1.0f;
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    if (!col_used[c] && !row_used[r] && std::abs(m[c][r]) > best) {
                        best = std::abs(m[c][r]);
                        best_c = c;
                        best_r = r;
                    }
                }
            }
            col_used[best_c] = row_used[best_r] = true;
            out[best_c][best_r] = m[best_c][best_r] >= 0.0f ? 1.0f : -1.0f;
            last_col = best_c;
        }
        if (glm::determinant(out) < 0.0f) {
            out[last_col] = -out[last_col];
        }
        return out;
    }

    glm::mat3 rotation_about(const glm::vec3& axis, float degrees) {
        return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis));
    }

    // Fault-guarded reads/writes of node fields (nodes can go away during loads).
    bool read_transforms(RE::NiAVObject* obj, RE::NiTransform* local, RE::NiTransform* world, RE::NiAVObject** parent) {
        __try {
            if (local) *local = obj->local;
            if (world) *world = obj->world;
            if (parent) *parent = (RE::NiAVObject*)obj->parent;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool write_local(RE::NiAVObject* obj, const RE::NiTransform& local) {
        __try {
            obj->local = local;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Captured on the game thread at the start of each frame, used by the scene update later in
    // the same frame (on worker threads).
    struct FrameInput {
        uint32_t  frame{0};
        bool      have_camera{false};
        Xform     camera_parent;           // the rendered camera's parent: game camera including the headset's rotation
        glm::mat3 hmd_rotation{1.0f};      // headset rotation in tracking space, game axes
        glm::vec3 head_local{0.0f};        // headset position in the camera parent's frame (no eye offset)
        bool      have_controller{false};
        Xform     controller;              // right controller relative to the headset, game axes
        bool      have_root{false};
        Xform     root;                    // skeleton root as last drawn (for comparisons)
        bool      have_skeleton_camera{false};
        glm::vec3 skeleton_camera{0.0f};   // skeleton camera position as last drawn (for comparisons)
    };

    std::mutex   g_input_mtx;
    FrameInput   g_input;

    std::atomic<RE::NiAVObject*> g_weapon{nullptr};
    std::atomic<RE::NiAVObject*> g_hand_ik{nullptr};
    std::atomic<RE::NiAVObject*> g_fp_root{nullptr};
    std::atomic<RE::NiCamera*>   g_fp_camera{nullptr};
    uint32_t                     g_lookup_frame{0};

    // Set in before_update, used by after_update (one weapon node, updated once per frame).
    RE::NiTransform g_saved_local{};
    RE::NiTransform g_applied_local{};
    RE::NiAVObject* g_chest{nullptr};
    bool            g_have_saved{false};

    // Keeping our transform after the update (instead of putting the animated one back): parts
    // of the gun that the renderer places later from the node's local transform would otherwise
    // be drawn at the animated position, away from the rest of the gun. The reference mod also
    // leaves the controller-driven transform in place.
    RE::NiAVObject* g_kept_weapon{nullptr};
    RE::NiTransform g_animated_local{};
    bool            g_keeping{false};

    bool same_transform(const RE::NiTransform& a, const RE::NiTransform& b) {
        return std::memcmp(&a, &b, sizeof(RE::NiTransform)) == 0;
    }

    // Which way the gun's own axes point, learned from the game's normal pose relative to the head.
    RE::NiAVObject* g_axes_weapon{nullptr};
    glm::mat3       g_gun_axes{1.0f};
    bool            g_have_gun_axes{false};
    uint32_t        g_axes_attempts{0};

    // Matrix layout check (see NiMath.h).
    uint32_t g_layout_disagree{0};
    float    g_layout_err_current{0.0f};
    float    g_layout_err_other{0.0f};

    std::atomic<bool> g_report_requested{false};

    // The gun transform to put back just before the game's animation step for the first-person
    // skeleton (see install_animation_hook). Written by before_update, read on the animation thread.
    struct PendingGun {
        RE::NiAVObject* weapon{nullptr};
        RE::NiAVObject* root{nullptr};
        RE::NiTransform local{};
        uint32_t        frame{0}; // engine frame it was computed in (stale ones are not used)
    };
    std::mutex            g_pending_mtx;
    PendingGun            g_pending{};
    std::atomic<bool>     g_pending_valid{false};
    std::atomic<uintptr_t> g_pending_root{0}; // checked before taking the lock (this runs for every actor)
    std::atomic<uint32_t> g_reapply_count{0};
    std::atomic<uint32_t> g_reapply_skipped{0};
    safetyhook::MidHook   g_animation_hook{};

    void log_xform(const char* name, const Xform& x) {
        spdlog::info("[MotionWeapon]   {:<34} pos ({:+.3f} {:+.3f} {:+.3f}) x-axis ({:+.3f} {:+.3f} {:+.3f}) y-axis ({:+.3f} {:+.3f} {:+.3f}) z-axis ({:+.3f} {:+.3f} {:+.3f}) scale {:.3f}",
            name, x.t.x, x.t.y, x.t.z, x.r[0].x, x.r[0].y, x.r[0].z, x.r[1].x, x.r[1].y, x.r[1].z, x.r[2].x, x.r[2].y, x.r[2].z, x.s);
    }

    void log_raw(const char* name, const RE::NiTransform& n) {
        const auto& e = n.rotate.entry;
        spdlog::info("[MotionWeapon]   {:<34} raw rows [{:+.3f} {:+.3f} {:+.3f}] [{:+.3f} {:+.3f} {:+.3f}] [{:+.3f} {:+.3f} {:+.3f}] translate ({:+.3f} {:+.3f} {:+.3f}) scale {:.3f}",
            name, e[0].pt[0], e[0].pt[1], e[0].pt[2], e[1].pt[0], e[1].pt[1], e[1].pt[2], e[2].pt[0], e[2].pt[1], e[2].pt[2],
            n.translate.x, n.translate.y, n.translate.z, n.scale);
    }
}

void MotionWeapon::request_report() {
    g_report_requested = true;
}

void MotionWeapon::on_frame_start(RE::NiCamera* world_camera, const glm::vec3& head_local) {
    // Turned off (or the keep option turned off): give the gun its animated transform back once.
    if (g_keeping && (!enabled() || !ModConstants::keepGunTransform)) {
        if (g_kept_weapon != nullptr && g_kept_weapon == g_weapon.load()) {
            write_local(g_kept_weapon, g_animated_local);
        }
        g_keeping = false;
        g_kept_weapon = nullptr;
    }
    if (!enabled()) {
        g_weapon = nullptr;
        g_pending_valid = false;
        return;
    }
    static auto vr = VR::get();

    // Everything the scene update needs from this frame's camera and controllers.
    {
        FrameInput in{};
        in.frame = vr->m_engine_frame_count.load();
        in.head_local = head_local;

        RE::NiAVObject* camera_parent = nullptr;
        RE::NiTransform parent_world{};
        if (world_camera != nullptr && read_transforms((RE::NiAVObject*)world_camera, nullptr, nullptr, &camera_parent) && camera_parent != nullptr &&
            read_transforms(camera_parent, nullptr, &parent_world, nullptr)) {
            in.camera_parent = to_xform(parent_world);
            in.have_camera = true;
        }

        const auto hmd = vr->get_transform(0);
        const auto ctrl = vr->get_transform(2);
        in.hmd_rotation = vr_to_game * glm::mat3(hmd) * glm::transpose(vr_to_game);
        const glm::mat4 rel = glm::inverse(hmd) * ctrl;
        in.controller = {vr_to_game * glm::mat3(rel) * glm::transpose(vr_to_game), vr_to_game * glm::vec3(rel[3]), 1.0f};
        if (vr->get_runtime()->is_openxr()) {
            const auto flags = vr->m_openxr->hands[VRRuntime::Hand::RIGHT].location.locationFlags;
            in.have_controller = (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 && (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        }

        if (auto root = g_fp_root.load()) {
            RE::NiTransform root_world{};
            if (read_transforms(root, nullptr, &root_world, nullptr)) {
                in.root = to_xform(root_world);
                in.have_root = true;
            }
        }
        if (auto cam = g_fp_camera.load()) {
            RE::NiTransform cam_world{};
            if (read_transforms((RE::NiAVObject*)cam, nullptr, &cam_world, nullptr)) {
                in.skeleton_camera = to_glm(cam_world.translate);
                in.have_skeleton_camera = true;
            }
        }

        std::scoped_lock _{g_input_mtx};
        g_input = in;
    }

    // Once per session, about ten seconds after the gun first shows up, write the first-person
    // nodes (with their bounds) to the log, for the low-detail gun investigation.
    {
        static int  countdown = -1;
        static bool done = false;
        if (!done) {
            if (countdown < 0 && g_weapon.load() != nullptr) {
                countdown = 600;
            } else if (countdown > 0 && --countdown == 0) {
                done = true;
                spdlog::info("[MotionWeapon] logging the first-person nodes once (gun detail check)");
                NodeTreeDump::request();
            }
        }
    }

    // Look the nodes up again every half second (the weapon node changes when you switch guns).
    if (++g_lookup_frame % 45 != 1 && g_weapon.load() != nullptr) {
        return;
    }

    // The skeleton's own camera (its parents are an unnamed node, "Camera", then the skeleton "Root").
    RE::NiCamera* fp_camera = nullptr;
    if (auto pc = CreationEngineSingletonManager::GetPlayerCameraSingleton(); pc != nullptr && pc->pFirstPersonModeState != nullptr) {
        fp_camera = pc->pFirstPersonModeState->cameraRoot;
    }

    RE::NiAVObject* root = nullptr;
    {
        RE::NiAVObject* node = (RE::NiAVObject*)fp_camera;
        for (int i = 0; i < 8 && node != nullptr; ++i) {
            RE::NiAVObject* parent = nullptr;
            if (!read_transforms(node, nullptr, nullptr, &parent)) {
                break;
            }
            node = parent;
            if (node != nullptr && NodeTreeDump::find(node, "COM") != nullptr) {
                root = node;
                break;
            }
        }
    }
    auto weapon = root ? NodeTreeDump::find_weapon_bone(root) : nullptr;
    auto hand_ik = weapon ? NodeTreeDump::find(weapon, "R_HandIk") : nullptr;

    static RE::NiAVObject* last_logged = (RE::NiAVObject*)1;
    if (weapon != last_logged) {
        last_logged = weapon;
        spdlog::info("[MotionWeapon] world camera {} skeleton camera {} root {} weapon node {} right hand IK {}",
            fmt::ptr(world_camera), fmt::ptr(fp_camera), fmt::ptr(root), fmt::ptr(weapon), fmt::ptr(hand_ik));
    }
    g_fp_camera = fp_camera;
    g_fp_root = root;
    g_hand_ik = hand_ik;
    g_weapon = weapon;
}

bool MotionWeapon::is_weapon_node(RE::NiAVObject* obj) {
    return obj != nullptr && obj == g_weapon.load() && enabled();
}

void MotionWeapon::before_update(RE::NiAVObject* weapon) {
    g_have_saved = false;
    g_pending_valid = false; // set again below once this frame's transform is written

    static auto     vr = VR::get();
    static uint32_t why_count = 0;
    auto why = [](const char* reason) {
        if (why_count++ % 600 == 0) {
            spdlog::info("[MotionWeapon] not moving the gun: {}", reason);
        }
    };

    FrameInput in{};
    {
        std::scoped_lock _{g_input_mtx};
        in = g_input;
    }
    if (!vr->is_hmd_active() || g_fp_camera.load() == nullptr) {
        why("headset not active or no first-person camera");
        return;
    }
    if (vr->m_engine_frame_count.load() - in.frame > 3) {
        why("no camera data for this frame (not in first person?)");
        return;
    }
    if (!in.have_controller) {
        why("right controller not tracked");
        return;
    }

    // Read the skeleton. At this point the game is updating it in its own space (the skeleton
    // root sits near 0,0,0 rather than at the player's world position), so everything is built
    // from transforms read here, in that same space.
    RE::NiTransform cam_local{}, unnamed_local{}, camera_node_local{}, root_world{}, weapon_local{}, chest_world{}, ik_local{};
    RE::NiAVObject *unnamed = nullptr, *camera_node = nullptr, *root = nullptr, *chest = nullptr;
    if (!read_transforms((RE::NiAVObject*)g_fp_camera.load(), &cam_local, nullptr, &unnamed) || unnamed == nullptr ||
        !read_transforms(unnamed, &unnamed_local, nullptr, &camera_node) || camera_node == nullptr ||
        !read_transforms(camera_node, &camera_node_local, nullptr, &root) || root == nullptr ||
        !read_transforms(root, nullptr, &root_world, nullptr) ||
        !read_transforms(weapon, &weapon_local, nullptr, &chest) || chest == nullptr ||
        !read_transforms(chest, nullptr, &chest_world, nullptr)) {
        why("could not read the camera/skeleton nodes");
        return;
    }
    if (auto ik = g_hand_ik.load()) {
        read_transforms(ik, &ik_local, nullptr, nullptr);
    }

    const Xform root_x = to_xform(root_world);
    const Xform skeleton_camera = root_x * to_xform(camera_node_local) * to_xform(unnamed_local);
    const Xform chest_x = to_xform(chest_world);
    const Xform animated_weapon = chest_x * to_xform(weapon_local);

    // How much the skeleton's space is turned compared to the world (expected: about 0).
    const float space_turn = in.have_root ? angle_between(root_x.r, in.root.r) : 0.0f;

    // The head: placed at the skeleton's camera, turned like the rendered camera.
    Xform head{};
    const bool use_game_camera = ModConstants::gunHeadReference == 0 && in.have_camera;
    if (use_game_camera) {
        glm::mat3 world_to_skeleton{1.0f};
        if (space_turn > 20.0f) {
            world_to_skeleton = root_x.r * glm::transpose(in.root.r);
        }
        head.r = world_to_skeleton * in.camera_parent.r;
    } else {
        head.r = skeleton_camera.r;
    }
    head.t = skeleton_camera.t + head.r * in.head_local;

    // The controller, then the configured tilt/turn/offsets in the hand (x right, y forward, z up).
    Xform grip = head * in.controller;
    grip.r = grip.r * rotation_about({1, 0, 0}, ModConstants::gunPitchOffset) * rotation_about({0, 0, 1}, ModConstants::gunYawOffset);
    grip.t += grip.r * glm::vec3{0.0f, ModConstants::gunForwardOffset, ModConstants::gunUpOffset};

    // The gun node's own axes aren't x right / y forward / z up. Learn them from the game's pose
    // (gun held forward, upright), once per gun, rounded to whole axes so idle sway isn't copied.
    const Xform animated_from_head = head.inverse() * animated_weapon;
    if (g_axes_weapon != weapon) {
        g_axes_weapon = weapon;
        g_have_gun_axes = false;
        g_axes_attempts = 0;
    }
    if (!g_have_gun_axes) {
        const glm::mat3 snapped = snap_to_axes(animated_from_head.r);
        const float     off = angle_between(snapped, animated_from_head.r);
        if (off < 25.0f || ++g_axes_attempts > 90) {
            g_gun_axes = snapped;
            g_have_gun_axes = true;
            spdlog::info("[MotionWeapon] gun axes learned (game pose {:.1f} deg from whole axes): gun x -> ({:+.0f} {:+.0f} {:+.0f}) y -> ({:+.0f} {:+.0f} {:+.0f}) z -> ({:+.0f} {:+.0f} {:+.0f}) [hand: x right, y forward, z up]",
                off, snapped[0].x, snapped[0].y, snapped[0].z, snapped[1].x, snapped[1].y, snapped[1].z, snapped[2].x, snapped[2].y, snapped[2].z);
        } else {
            why("waiting for the gun's normal pose to learn its axes");
            return;
        }
    }

    // Put the gun's right-hand grip point (R_HandIk) on the controller.
    Xform weapon_world{};
    weapon_world.r = grip.r * g_gun_axes;
    weapon_world.t = grip.t - weapon_world.r * to_glm(ik_local.translate);
    weapon_world.s = animated_weapon.s;

    const Xform local = chest_x.inverse() * weapon_world;
    const float reach = glm::length(local.t);

    static uint32_t n = 0;
    const bool periodic = ++n % 300 == 1;
    if (periodic || g_report_requested) {
        const glm::mat3 game_camera_without_headset = in.camera_parent.r * glm::transpose(in.hmd_rotation);
        const glm::vec3 camera_gap = in.have_camera && in.have_skeleton_camera ? in.camera_parent.t - in.skeleton_camera : glm::vec3{0.0f};
        spdlog::info("[MotionWeapon] reference {} | skeleton space turned {:.1f} deg vs world | skeleton camera vs game camera {:.1f} deg, vs game camera without headset {:.1f} deg | "
                     "skeleton camera to game camera ({:+.3f} {:+.3f} {:+.3f}) m | game gun from head ({:+.3f} {:+.3f} {:+.3f}) | controller from head ({:+.3f} {:+.3f} {:+.3f}) | gun local game ({:+.3f} {:+.3f} {:+.3f}) ours ({:+.3f} {:+.3f} {:+.3f}) | layout {} (error {:.1f} mm, other {:.1f} mm)",
            use_game_camera ? "game camera" : "skeleton camera", space_turn,
            in.have_camera ? angle_between(skeleton_camera.r, in.camera_parent.r) : -1.0f,
            in.have_camera ? angle_between(skeleton_camera.r, game_camera_without_headset) : -1.0f,
            camera_gap.x, camera_gap.y, camera_gap.z,
            animated_from_head.t.x, animated_from_head.t.y, animated_from_head.t.z,
            in.controller.t.x, in.controller.t.y, in.controller.t.z,
            weapon_local.translate.x, weapon_local.translate.y, weapon_local.translate.z, local.t.x, local.t.y, local.t.z,
            NiMath::g_rows_are_axes ? "rows" : "columns", g_layout_err_current * 1000.0f, g_layout_err_other * 1000.0f);
    }
    if (g_report_requested.exchange(false)) {
        spdlog::info("[MotionWeapon] placement report (game axes: x right, y forward, z up; positions in metres)");
        log_raw("skeleton root (world, raw)", root_world);
        log_raw("chest (world, raw)", chest_world);
        log_raw("gun node (local, raw)", weapon_local);
        log_xform("skeleton root (world)", root_x);
        log_xform("skeleton root (last frame, world)", in.root);
        log_xform("'Camera' node (local)", to_xform(camera_node_local));
        log_xform("unnamed camera node (local)", to_xform(unnamed_local));
        log_xform("skeleton camera (local)", to_xform(cam_local));
        log_xform("skeleton camera parent (world)", skeleton_camera);
        log_xform("game camera (world)", in.camera_parent);
        log_xform("headset rotation (tracking)", {in.hmd_rotation, in.head_local, 1.0f});
        log_xform("head used", head);
        log_xform("controller from head", in.controller);
        log_xform("grip", grip);
        log_xform("chest (world)", chest_x);
        log_xform("game gun (world)", animated_weapon);
        log_xform("game gun from head", animated_from_head);
        log_xform("gun axes in hand", {g_gun_axes, glm::vec3{0.0f}, 1.0f});
        log_xform("our gun (world)", weapon_world);
        log_xform("our gun (local)", local);
        log_xform("right hand IK (local)", to_xform(ik_local));
    }

    if (!std::isfinite(reach) || reach > 1.5f) {
        why("computed position is too far from the body (check the log report)");
        return;
    }

    RE::NiTransform new_local = weapon_local;
    NiMath::from_glm(local.r, new_local.rotate);
    new_local.translate = NiMath::to_ni(local.t);

    // The animated transform: what the game wrote this frame, or (if the game didn't write the
    // node since our last frame) the one remembered from before.
    if (g_kept_weapon != weapon || !g_keeping || !same_transform(weapon_local, g_applied_local)) {
        g_animated_local = weapon_local;
        g_kept_weapon = weapon;
    }
    g_saved_local = g_animated_local;
    g_applied_local = new_local;
    g_chest = chest;
    if (write_local(weapon, new_local)) {
        g_have_saved = true;
        {
            std::scoped_lock _{g_pending_mtx};
            g_pending.weapon = weapon;
            g_pending.root = root;
            g_pending.local = new_local;
            g_pending.frame = vr->m_engine_frame_count.load();
        }
        g_pending_root = reinterpret_cast<uintptr_t>(root);
        g_pending_valid = true;
    }
}

void MotionWeapon::after_update(RE::NiAVObject* weapon) {
    if (!g_have_saved) {
        return;
    }
    g_have_saved = false;

    // Check the matrix layout against the game: it just placed the gun from the chest and the
    // local transform we gave it.
    RE::NiTransform weapon_world{}, chest_world{};
    if (g_chest != nullptr && read_transforms(weapon, nullptr, &weapon_world, nullptr) && read_transforms(g_chest, nullptr, &chest_world, nullptr)) {
        const bool rows = NiMath::g_rows_are_axes.load();
        const glm::vec3 local_t = to_glm(g_applied_local.translate) * chest_world.scale;
        const glm::vec3 actual = to_glm(weapon_world.translate);
        const glm::vec3 chest_t = to_glm(chest_world.translate);
        const float err_current = glm::length(chest_t + NiMath::to_glm(chest_world.rotate, rows) * local_t - actual);
        const float err_other = glm::length(chest_t + NiMath::to_glm(chest_world.rotate, !rows) * local_t - actual);
        g_layout_err_current = err_current;
        g_layout_err_other = err_other;
        if (err_other < 0.01f && err_current > 0.03f) {
            if (++g_layout_disagree >= 5) {
                NiMath::g_rows_are_axes = !rows;
                g_layout_disagree = 0;
                g_have_gun_axes = false;
                g_axes_attempts = 0;
                spdlog::info("[MotionWeapon] the game reads node rotations by {} (error {:.1f} mm vs {:.1f} mm); switching",
                    rows ? "columns" : "rows", err_other * 1000.0f, err_current * 1000.0f);
            }
        } else {
            g_layout_disagree = 0;
        }
    }

    if (ModConstants::keepGunTransform) {
        g_keeping = true;
    } else {
        write_local(weapon, g_saved_local);
        g_keeping = false;
    }
}


// --- Gun transform before the animation step -----------------------------------------------
// The reference motion-control mod (the one that shows the whole gun) hooks the game's
// AnimationManager: the first vtable the game's RTTI lists for "class AnimationManager", slot 7.
// Just before that runs for the first-person skeleton (the manager whose field at +0x330 is the
// skeleton's "Root" node), it writes the controller-driven transform back into the "Weapon" node,
// so the game's animation step works from the gun where the hand is, not where the animation had
// it. This does the same, with a mid-function hook so the call itself is left untouched.
namespace {
    bool read_pointer_at(const void* base, size_t offset, uintptr_t& out) {
        __try {
            out = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const uint8_t*>(base) + offset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool is_below(RE::NiAVObject* node, RE::NiAVObject* root) {
        __try {
            for (int i = 0; i < 128 && node != nullptr; ++i) {
                if (node == root) {
                    return true;
                }
                node = reinterpret_cast<RE::NiAVObject*>(node->parent);
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    constexpr size_t kAnimationManagerRootOffset = 0x330;

    void on_animation_manager_update(safetyhook::Context& ctx) {
        if (!enabled() || !ModConstants::gunReapplyBeforeAnimation || !g_pending_valid.load()) {
            return;
        }
        const auto manager = reinterpret_cast<const void*>(ctx.rcx);
        if (manager == nullptr) {
            return;
        }
        // This runs for every animated actor; only the first-person skeleton's manager matters.
        uintptr_t manager_root = 0;
        if (!read_pointer_at(manager, kAnimationManagerRootOffset, manager_root) || manager_root == 0 || manager_root != g_pending_root.load()) {
            return;
        }
        PendingGun gun{};
        {
            std::scoped_lock _{g_pending_mtx};
            gun = g_pending;
        }
        // Only a transform from the last few frames, for the gun and skeleton still in use (the
        // nodes are replaced when a save loads).
        static auto vr = VR::get();
        if (gun.weapon == nullptr || reinterpret_cast<uintptr_t>(gun.root) != manager_root || gun.weapon != g_weapon.load() ||
            gun.root != g_fp_root.load() || vr->m_engine_frame_count.load() - gun.frame > 3) {
            return;
        }
        if (!is_below(gun.weapon, gun.root) || !write_local(gun.weapon, gun.local)) {
            if (g_reapply_skipped.fetch_add(1) < 5) {
                spdlog::info("[MotionWeapon] gun transform not put back before the animation step (gun node no longer under the skeleton)");
            }
            return;
        }
        if (g_reapply_count.fetch_add(1) < 12) {
            spdlog::info("[MotionWeapon] gun transform put back before the animation step (manager {}, root {}, gun node {}, thread {})",
                fmt::ptr(manager), fmt::ptr(gun.root), fmt::ptr(gun.weapon), GetCurrentThreadId());
        }
    }
}

void MotionWeapon::install_animation_hook() {
    if (g_animation_hook) {
        return;
    }
    const auto vtables = utility::rtti::find_vtables(MemoryScan::mod, "class AnimationManager");
    if (vtables.empty()) {
        spdlog::error("[MotionWeapon] AnimationManager not found in the game's type info; the gun can't be put back before the animation step");
        return;
    }
    const auto target = reinterpret_cast<const uintptr_t*>(vtables[0])[7];
    g_animation_hook = safetyhook::create_mid(reinterpret_cast<void*>(target), &on_animation_manager_update);
    if (!g_animation_hook) {
        spdlog::error("[MotionWeapon] could not hook the AnimationManager update at {:x} (game offset {:x}); the gun can't be put back before the animation step",
            target, target - reinterpret_cast<uintptr_t>(MemoryScan::mod));
        return;
    }
    spdlog::info("[MotionWeapon] AnimationManager update hooked at game offset {:x} ({} vtables for the class; slot 7 of the first)",
        target - reinterpret_cast<uintptr_t>(MemoryScan::mod), vtables.size());
}


// --- Gun piece report ------------------------------------------------------------------------
// For every part of the gun (including the ones after a gap in a node's child list, which the
// earlier node dumps skipped): its flags, bounds, how far it moved since the last frame, and where
// it lands in the image of the eye just drawn and of the game's first-person reference camera,
// using each camera's own world-to-clip matrix. Fade nodes and geometry also get the raw words
// after their base class, to find the game's fade / level-of-detail state by comparing a part
// that is drawn with one that is missing. Written for the frames around a stereo screenshot.
namespace {
    struct CameraInfo {
        float         pos[3]{};
        float         rows[3][3]{};   // world rotation rows (row 0 = view direction)
        RE::NiFrustum frustum{};
        float         clip[4][4]{};   // world to clip (rows)
        float         lod_adjust{};
        uint64_t      flags{};
    };

    bool read_camera_info(RE::NiCamera* cam, CameraInfo& out) {
        __try {
            out.pos[0] = cam->world.translate.x;
            out.pos[1] = cam->world.translate.y;
            out.pos[2] = cam->world.translate.z;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    out.rows[r][c] = cam->world.rotate.entry[r].pt[c];
                }
            }
            out.frustum = cam->viewFrustum;
            std::memcpy(out.clip, cam->worldToCam, sizeof(out.clip));
            out.lod_adjust = cam->m_fLODAdjust;
            out.flags = cam->flags;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct PieceInfo {
        const char*      name{};
        const char*      type{};
        uint64_t         flags{};
        float            center[3]{};
        float            radius{};
        float            world[3]{};
        float            previous[3]{};
        RE::NiAVObject** children{};
        uint16_t         slots{};
        uint16_t         non_null{};
        bool             is_fade{};
        bool             is_geometry{};
        uint32_t         raw[32]{};
        uint64_t         after_flags[2]{}; // the two words after the flags (+0x120, +0x128)
        bool             have_raw{};
    };

    bool read_piece(RE::NiAVObject* obj, PieceInfo& out) {
        __try {
            out.name = obj->name.c_str();
            const auto ti = utility::rtti::get_type_info(obj);
            out.type = ti != nullptr ? ti->name() : "?";
            out.flags = obj->flags;
            std::memcpy(out.after_flags, (const uint8_t*)obj + 0x120, sizeof(out.after_flags));
            out.center[0] = obj->worldBound.center.x;
            out.center[1] = obj->worldBound.center.y;
            out.center[2] = obj->worldBound.center.z;
            out.radius = obj->worldBound.radius;
            out.world[0] = obj->world.translate.x;
            out.world[1] = obj->world.translate.y;
            out.world[2] = obj->world.translate.z;
            out.previous[0] = obj->previousWorld.translate.x;
            out.previous[1] = obj->previousWorld.translate.y;
            out.previous[2] = obj->previousWorld.translate.z;
            out.is_geometry = utility::rtti::derives_from(obj, "class BSGeometry");
            out.is_fade = utility::rtti::derives_from(obj, "class BSFadeNode");
            if (utility::rtti::derives_from(obj, "class NiNode")) {
                auto node = (RE::NiNode*)obj;
                out.slots = node->children.slots_to_scan();
                out.non_null = node->children.m_size;
                out.children = (RE::NiAVObject**)node->children.entries;
            }
            if (out.is_geometry || out.is_fade) {
                const auto base = (const uint8_t*)obj + (out.is_geometry ? sizeof(RE::NiAVObject) : sizeof(RE::NiNode));
                std::memcpy(out.raw, base, sizeof(out.raw));
                out.have_raw = true;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    RE::NiAVObject* read_child_at(RE::NiAVObject** children, uint16_t i) {
        __try {
            return children[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Where a bounding sphere lands for a camera: image position (0..1 across, 0..1 down), clip w
    // (about the distance in front), depth z/w, and whether any of it is inside the view.
    struct Projected {
        float u{-1.0f}, v{-1.0f}, w{0.0f}, depth{0.0f};
        bool  inside{false};
    };

    Projected project(const CameraInfo& cam, const glm::vec3& p, float radius) {
        Projected out{};
        const auto& m = cam.clip;
        const float x = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3];
        const float y = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3];
        const float z = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3];
        const float w = m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3];
        out.w = w;
        if (w <= 1e-5f) {
            // Centre behind the camera: inside only if the sphere reaches past it.
            const float wl = glm::length(glm::vec3{m[3][0], m[3][1], m[3][2]});
            out.inside = wl > 0.0f && w + radius * wl > 0.0f;
            return out;
        }
        const float nx = x / w, ny = y / w;
        out.depth = z / w;
        out.u = (nx + 1.0f) * 0.5f;
        out.v = (1.0f - ny) * 0.5f;
        const float rx = radius * glm::length(glm::vec3{m[0][0], m[0][1], m[0][2]}) / w;
        const float ry = radius * glm::length(glm::vec3{m[1][0], m[1][1], m[1][2]}) / w;
        out.inside = std::abs(nx) - rx <= 1.0f && std::abs(ny) - ry <= 1.0f;
        return out;
    }

    void log_camera(const char* what, const CameraInfo& c) {
        const auto& f = c.frustum;
        spdlog::info("[GUN] {} at ({:.3f} {:.3f} {:.3f}) view dir ({:+.3f} {:+.3f} {:+.3f}) axis1 ({:+.3f} {:+.3f} {:+.3f}) axis2 ({:+.3f} {:+.3f} {:+.3f}) "
                     "frustum {:.3f} {:.3f} {:.3f} {:.3f} near {:.3f} far {:.1f} lod adjust {:.4f} flags {:#x}",
            what, c.pos[0], c.pos[1], c.pos[2], c.rows[0][0], c.rows[0][1], c.rows[0][2], c.rows[1][0], c.rows[1][1], c.rows[1][2],
            c.rows[2][0], c.rows[2][1], c.rows[2][2], f.left, f.right, f.top, f.bottom, f._near, f._far, c.lod_adjust, c.flags);
        for (int r = 0; r < 4; ++r) {
            spdlog::info("[GUN]   {} clip row {}: {:+.5f} {:+.5f} {:+.5f} {:+.5f}", what, r, c.clip[r][0], c.clip[r][1], c.clip[r][2], c.clip[r][3]);
        }
    }

    std::atomic<int> g_piece_frames{0};

    struct ReportContext {
        CameraInfo eye{};
        CameraInfo fp{};
        bool       have_fp{false};
        int        count{0};
        int        geometry{0};
        int        geometry_in_view{0};
    };

    void report_pieces(RE::NiAVObject* obj, int depth, ReportContext& ctx) {
        if (obj == nullptr || depth > 16 || ctx.count >= 400) {
            return; // empty child slot, or too deep
        }
        PieceInfo info{};
        if (!read_piece(obj, info)) {
            spdlog::info("[GUN] {}<unreadable {}>", std::string(depth * 2, ' '), fmt::ptr(obj));
            return;
        }
        ++ctx.count;
        const glm::vec3 c{info.center[0], info.center[1], info.center[2]};
        const glm::vec3 moved = glm::vec3{info.world[0], info.world[1], info.world[2]} - glm::vec3{info.previous[0], info.previous[1], info.previous[2]};
        const Projected e = project(ctx.eye, c, info.radius);
        if (info.is_geometry) {
            ++ctx.geometry;
            ctx.geometry_in_view += e.inside ? 1 : 0;
        }
        std::string fp_text;
        if (ctx.have_fp) {
            const Projected f = project(ctx.fp, c, info.radius);
            fp_text = fmt::format(" | fp camera ({:.3f} {:.3f}) w {:.3f} {}", f.u, f.v, f.w, f.inside ? "in" : "OUT");
        }
        std::string children_text;
        if (info.children != nullptr) {
            children_text = info.slots != info.non_null ? fmt::format(" children {} in {} slots", info.non_null, info.slots)
                                                        : fmt::format(" children {}", info.non_null);
        }
        spdlog::info("[GUN] {}{} [{}] flags {:#x} +120 {:x} +128 {:x} bound ({:.3f} {:.3f} {:.3f}) r {:.3f} | eye ({:.3f} {:.3f}) w {:.3f} depth {:.5f} {}{} | moved {:.1f} mm{}",
            std::string(depth * 2, ' '), info.name ? info.name : "", info.type ? info.type : "?", info.flags, info.after_flags[0], info.after_flags[1],
            c.x, c.y, c.z, info.radius, e.u, e.v, e.w, e.depth, e.inside ? "in" : "OUT", fp_text, glm::length(moved) * 1000.0f, children_text);
        if (info.have_raw) {
            std::string words;
            for (int i = 0; i < 32; ++i) {
                if (i % 4 == 0) {
                    words += " |";
                }
                words += fmt::format(" {:08x}", info.raw[i]);
            }
            spdlog::info("[GUN] {}  raw +{:#x}:{}", std::string(depth * 2, ' '), info.is_geometry ? sizeof(RE::NiAVObject) : sizeof(RE::NiNode), words);
        }
        for (uint16_t i = 0; info.children != nullptr && i < info.slots; ++i) {
            report_pieces(read_child_at(info.children, i), depth + 1, ctx);
        }
    }
}

void MotionWeapon::request_piece_report(int frames) {
    g_piece_frames = frames;
}

void MotionWeapon::report_pieces_if_requested(RE::NiCamera* world_camera) {
    static auto vr = VR::get();
    // Report the frames the stereo screenshot actually captures: it saves the images once its 2 s
    // countdown (started when the menu closes) has run out, not when the button is pressed.
    static bool was_due = false;
    const bool  due = vr->m_screenshot_armed && vr->m_screenshot_not_before != std::chrono::steady_clock::time_point{} &&
                     std::chrono::steady_clock::now() >= vr->m_screenshot_not_before;
    if (due && !was_due) {
        g_piece_frames = 4; // two frames of each eye
    }
    was_due = due;
    if (g_piece_frames.load() <= 0 || world_camera == nullptr) {
        return;
    }
    --g_piece_frames;

    // Find the gun from the skeleton's camera.
    RE::NiAVObject* weapon = g_weapon.load();
    if (weapon == nullptr) {
        RE::NiCamera* fp_camera = nullptr;
        if (auto pc = CreationEngineSingletonManager::GetPlayerCameraSingleton(); pc != nullptr && pc->pFirstPersonModeState != nullptr) {
            fp_camera = pc->pFirstPersonModeState->cameraRoot;
        }
        RE::NiAVObject* node = (RE::NiAVObject*)fp_camera;
        for (int i = 0; i < 8 && node != nullptr && weapon == nullptr; ++i) {
            RE::NiAVObject* parent = nullptr;
            if (!read_transforms(node, nullptr, nullptr, &parent)) {
                break;
            }
            node = parent;
            if (node != nullptr) {
                weapon = NodeTreeDump::find_weapon_bone(node);
            }
        }
    }
    if (weapon == nullptr) {
        spdlog::info("[GUN] no gun found");
        return;
    }

    // The cameras as they were for the frame just drawn (their world transforms and matrices are
    // last frame's, like the gun's).
    ReportContext ctx{};
    if (!read_camera_info(world_camera, ctx.eye)) {
        spdlog::info("[GUN] camera not readable");
        return;
    }
    auto fp = CreationEngineCameraManager::first_person_reference_camera();
    ctx.have_fp = fp != nullptr && read_camera_info(fp, ctx.fp);

    const bool drawn_left = vr->get_current_render_eye() != VRRuntime::Eye::LEFT; // the previous frame's eye
    spdlog::info("[GUN] ---- frame {}, image of the {} eye (positions: 0..1 across, 0..1 down; 'OUT' = whole bound outside that camera's view) ----",
        vr->m_engine_frame_count.load(), drawn_left ? "left" : "right");
    log_camera("eye camera", ctx.eye);
    if (ctx.have_fp) {
        log_camera("fp reference camera", ctx.fp);
        const glm::vec3 a{ctx.eye.rows[0][0], ctx.eye.rows[0][1], ctx.eye.rows[0][2]};
        const glm::vec3 b{ctx.fp.rows[0][0], ctx.fp.rows[0][1], ctx.fp.rows[0][2]};
        const float     cosang = glm::length(a) > 0.0f && glm::length(b) > 0.0f ? glm::dot(a, b) / (glm::length(a) * glm::length(b)) : 1.0f;
        spdlog::info("[GUN] fp reference camera vs eye camera: {:.1f} mm apart, view directions {:.1f} deg apart",
            glm::length(glm::vec3{ctx.fp.pos[0] - ctx.eye.pos[0], ctx.fp.pos[1] - ctx.eye.pos[1], ctx.fp.pos[2] - ctx.eye.pos[2]}) * 1000.0f,
            glm::degrees(std::acos(std::clamp(cosang, -1.0f, 1.0f))));
        // The gun and arms are drawn with the fp camera's matrix: how much bigger than the eye's
        // lens would draw them (1.00 = same lens; before the lens fix it was about 1.87).
        auto row_len = [](const float* r) { return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); };
        const float e0 = row_len(ctx.eye.clip[0]), e1 = row_len(ctx.eye.clip[1]);
        if (e0 > 0.0f && e1 > 0.0f) {
            spdlog::info("[GUN] lens check: gun drawn {:.2f}x across, {:.2f}x up the size the eye's lens gives (1.00 = right size and distance)",
                row_len(ctx.fp.clip[0]) / e0, row_len(ctx.fp.clip[1]) / e1);
        }
    } else {
        spdlog::info("[GUN] no fp reference camera");
    }
    report_pieces(weapon, 0, ctx);
    spdlog::info("[GUN] ---- {} nodes, {} meshes, {} of them inside the eye's view ----", ctx.count, ctx.geometry, ctx.geometry_in_view);
}
