//
// Created by sergp on 6/23/2024.
//

#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "CreationEngineRendererModule.h"
#include "CreationEngineSingletonManager.h"
#include "RE/P/PlayerCamera.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "MenuCamera.h"
#include "StereoView.h"
#include "UiProbe.h"
#include "WorkbenchCamera.h"
#include "LadderClimb.h"
#include "Climbing.h"
#include "ConversationPanel.h"
#include "BuildConsole.h"
#include "MessagesBlock.h"
#include "SeatView.h"
#include <CreationEngine/memory/offsets.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <REL/Relocation.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <glm/gtx/vector_angle.hpp>
#include <mods/VR.hpp>

#include "ModSettings.h"
#include "MotionWeapon.h"
#include "NiMath.h"
#include "body/BodyIK.h"
#include "TempleGestures.h"
#include "vr/TrackingSpace.h"

namespace {
    const  glm::mat4 permutation_pre = {
        1, 0, 0, 0,
        0, 0, 1, 0,
        0, -1, 0, 0,
        0, 0,  0, 1
    };
    const glm::mat4 permutation_post = glm::transpose(permutation_pre);

    glm::mat4 to_havok_space(const glm::mat4& mat) {
        return permutation_pre * mat * permutation_post;
    }

    glm::mat4 from_havok_space(const glm::mat4& mat) {
        return permutation_post * mat * permutation_pre;
    }
    float yaw_offset{0.0f};


}


void aiming_adjustments(Vector4f &frustum, float fov_adjustment);

void onSetNimFrustumDetour(RE::NiCamera *camera, RE::NiFrustum *frustum) {
    CreationEngineCameraManager::Get()->onSetNimFrustum(camera, frustum);
}

void onCalcNimFrustumDetour(RE::NiCamera *camera, float fov, float aspectRatio, float nearDist, float farDist,
                            char lodAdjust) {
    CreationEngineCameraManager::Get()->onCalcNiFrustum(camera, fov, aspectRatio, nearDist, farDist, lodAdjust);
}

void onScaleformSetViewPortDetour(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {
    CreationEngineCameraManager::Get()->onScaleformSetViewPort(thisMovie, viewport);
}

/*

void onScaleformMovieSetProjectionMatrix3DDetour(uintptr_t* thisMovie, Matrix4x4f& matrix)
{
    CreationEngineCameraManager::Get()->onScaleformMovieSetProjectionMatrix3D(thisMovie, matrix);
}
*/

bool isValidCamera(RE::NiCamera *pCamera) {
    static auto sceneGraphRoot = CreationEngineSingletonManager::GetSceneGraphRoot();
    return pCamera == sceneGraphRoot->worldCamera || pCamera == sceneGraphRoot->starfieldScene.pStarFieldCamera ||
           sceneGraphRoot->starfieldScene.pGalaxyCamera == pCamera;
}

void CreationEngineCameraManager::InstallHooks() {
    // Third-person body in first person with arm IK (SFVR's body module).
    body::InstallModelNodeHook();
    // (fix146) ...and the game's turn to third person given the bodies as in first person (its own switch: ladders climb then).
    body::InstallSwitchHook();

    REL::Relocation<uintptr_t> onNiAVObjectUpdateWorldAddr{GameStore::MemoryOffsets::BSFadeNode::vtable_UpdateWorld()};
    m_onNiAVObjectUpdateWorldHook = std::make_unique<FunctionHook>(onNiAVObjectUpdateWorldAddr.address(),
                                                                   reinterpret_cast<uintptr_t>(&onNiAVObjectUpdateWorld));
    m_onNiAVObjectUpdateWorldHook->create();

    // Motion controls: put the gun back on the controller right before the game's animation step
    // for the first-person skeleton (the reference mod's method).
    MotionWeapon::install_animation_hook();

    REL::Relocation<uintptr_t> onGetCameraRotationAddr{ GameStore::MemoryOffsets::FirstPersonState::GetRotationQuatV() };
    m_onGetCameraRotationHook = std::make_unique<FunctionHook>(onGetCameraRotationAddr.address(), reinterpret_cast<uintptr_t>(&onFPSGetCameraRotation));
    m_onGetCameraRotationHook->create();


    REL::Relocation<uintptr_t> setNimFrustumVFuncAddr{GameStore::MemoryOffsets::NiCamera::SetFrustumVfunc()};
    m_onSetNimFrustumHook = std::make_unique<FunctionHook>(setNimFrustumVFuncAddr.address(),
                                                           reinterpret_cast<uintptr_t>(&onSetNimFrustumDetour));
    m_onSetNimFrustumHook->create();

    REL::Relocation<uintptr_t> calcNimFrustumVFuncAddr{GameStore::MemoryOffsets::NiCamera::CalcFrustumVfunc()};
    m_onCalcNimFrustumHook = std::make_unique<FunctionHook>(calcNimFrustumVFuncAddr.address(),
                                                            reinterpret_cast<uintptr_t>(&onCalcNimFrustumDetour));
    m_onCalcNimFrustumHook->create();

    REL::Relocation<uintptr_t> scaleformSetViewPortAddr{GameStore::MemoryOffsets::Scaleform::Movie::SetViewportVFunc()};
    m_onScaleformSetViewPortHook = std::make_unique<FunctionHook>(scaleformSetViewPortAddr.address(),
                                                                  reinterpret_cast<uintptr_t>(&onScaleformSetViewPortDetour));
    m_onScaleformSetViewPortHook->create();
}


// Offset of the eye being rendered from the head centre, in world space (set every frame).
RE::NiPoint3 g_eye_offset_world{};

namespace {
    // The world camera (for the scene hook: its own updates counted while the world is kept behind a menu).
    std::atomic<RE::NiAVObject*> g_world_camera_node{nullptr};
    std::atomic<uint32_t>        g_world_camera_updates{0};
}

// Centre of the head in the camera parent's frame (game axes, metres), set with the camera each
// frame; the shown body is placed from it (see head_center_world).
std::mutex g_head_local_mtx;
glm::vec3  g_head_local{0.0f};
bool       g_head_local_valid{false};

bool CreationEngineCameraManager::head_center_world(glm::vec3& out) {
    glm::vec3 local{0.0f};
    {
        std::scoped_lock _{g_head_local_mtx};
        if (!g_head_local_valid) {
            return false;
        }
        local = g_head_local;
    }
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    auto camera = root ? root->worldCamera : nullptr;
    if (camera == nullptr || camera->parent == nullptr) {
        return false;
    }
    const auto& parent = camera->parent->world;
    // (fix147: the root's place this frame: the body is placed before the camera puts its root, tracking::CameraRootPlace.)
    out = tracking::CameraRootPlace(camera->parent) + NiMath::to_glm(parent.rotate) * (local * parent.scale);
    return true;
}

RE::NiAVObject *getCameraRootNode() {
    auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();

    if (!playerCamera || !playerCamera->IsInFirstPerson()) {
        return nullptr;
    }
    RE::NiAVObject *a_camera = playerCamera->pFirstPersonModeState->cameraRoot;
    while (a_camera) {
        if (a_camera->name == "Root") {
            return a_camera;
        }
        a_camera = a_camera->parent;
    }
    return nullptr;
}

// Yaw (degrees) of a rotation matrix's forward axis, for tracing.
static float yaw_deg(const RE::NiMatrix3& m) {
    return glm::degrees(std::atan2(m.entry[1].pt[0], m.entry[0].pt[0]));
}

void UpdateMesh(RE::NiAVObject* camera) {
    // (fix143: climbing a ladder as in a seat: the game's heading, the head turning the view only.)
    if (GameFlow::isImmovable() || GameFlow::isControlledByAI() || GameFlow::isOnLadder()) {
        return;
    }
    if (ModSettings::g_debug.disableFirstPersonMeshRotation) {
        return;
    }
    static auto vr_t = VR::get();
    const float yaw_before = yaw_deg(camera->world.rotate);

    // With Decoupled Pitch on the gun stays level (Mutar's behaviour). Turning it with the headset
    // anyway (fix29) made the gun disappear, so that mode is left as it was; keep Decoupled Pitch
    // off for the gun to follow the view.
    if(GameFlow::gStore.internalSettings.decoupledPitch && !(ModConstants::headTrackingType == 0 && GameFlow::isAimingDownSights())) {
        return;
    }
    auto camera_quat = RE::NiQuaternion(camera->world.rotate);
    auto glm_camera_quat = glm::normalize(glm::quat(camera_quat.w, camera_quat.x, camera_quat.y, camera_quat.z));

    // The rotation the camera's parent carries: the headset, or the right controller when aiming with it.
    auto current_hmd_rotation = tracking::AimRotation();
    auto hmd_rotation_quat = glm::normalize(
            glm::quat_cast(current_hmd_rotation));
    hmd_rotation_quat = {hmd_rotation_quat.w, hmd_rotation_quat.x, -hmd_rotation_quat.z, hmd_rotation_quat.y};
    auto quat_out = glm_camera_quat;
    {
        if (GameFlow::gStore.internalSettings.pawnControl) {
            quat_out = glm::rotate(quat_out, -yaw_offset, glm::vec3{0.0f, 0.0f, 1.0f});
        }
        quat_out = quat_out * hmd_rotation_quat;
        camera_quat = RE::NiQuaternion(quat_out.w, quat_out.x, quat_out.y, quat_out.z);
        camera_quat.ToMatrix(camera->world.rotate);
    }
    if (vr_t->tracing()) {
        spdlog::info("[VR-TRACE] fp mesh '{}' yaw {:.1f} -> {:.1f} (yaw offset {:.1f}) eye {} frame {}", camera->name.c_str() ? camera->name.c_str() : "",
            yaw_before, yaw_deg(camera->world.rotate), glm::degrees(yaw_offset),
            vr_t->get_current_render_eye() == VRRuntime::Eye::LEFT ? "L" : "R", vr_t->m_engine_frame_count.load());
    }
}

void CreationEngineCameraManager::onNiAVObjectUpdateWorld(RE::NiAVObject *obj, RE::NiUpdateData *a_data) {
    static auto instance = CreationEngineCameraManager::Get();
    using func_t = decltype(onNiAVObjectUpdateWorld);
    static auto original_func = instance->m_onNiAVObjectUpdateWorldHook->get_original<func_t>();
    static auto vr = VR::get();

    // (fix148) On a ladder, the third-person camera's root put at the body's eyes as it works out its world place (one pointer compared
    // for every other node).
    LadderClimb::BeforeNodeUpdate(obj);
    // (fix185) While climbing, the first-person camera's root put at your character's place as asked this frame plus your eyes' height
    // (after the ladder's: never both, Climbing ties nothing on a ladder; one pointer compared for every other node).
    Climbing::BeforeNodeUpdate(obj);
    // (fix195) On foot in first person, the camera root put where the body (and the gun in its hand) was placed this frame ("Tie the
    // view to the body"; after Climbing's, which it leaves alone; one pointer compared for every other node).
    tracking::BeforeNodeUpdate(obj);
    // (fix148) The camera root's place, as the game has just worked it out, noted with your character's (after the game's update, on
    // every way out below: the body is put ahead by your move since).
    struct AfterUpdate {
        const RE::NiAVObject* node;
        ~AfterUpdate() { tracking::AfterNodeUpdate(node); }
    } after_update{obj};

    // (The world camera's own update, counted: does the game still do it with a full-screen menu open?)
    if (obj == g_world_camera_node.load(std::memory_order_relaxed)) {
        g_world_camera_updates.fetch_add(1, std::memory_order_relaxed);
    }

    // Third-person body: shown (and the first-person arms and gun hidden) at its root, or put back.
    body::OnUpdateWorld(obj, (int)vr->m_engine_frame_count.load());
    // (fix174) The flashlight's light (BSLight's own update calls this one first): shown and put at your head (one pointer compared).
    TempleGestures::BeforeNodeUpdate(obj);

    // Motion controls: gun on the right controller.
    if (MotionWeapon::is_weapon_node(obj) && vr->is_hmd_active() && !ModSettings::cameraFlat()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::info("[MotionWeapon] weapon node goes through the scene update hook");
        }
        MotionWeapon::before_update(obj);
        original_func(obj, a_data);
        MotionWeapon::after_update(obj);
        return;
    }

    if (vr->is_hmd_active() && !ModSettings::cameraFlat() && !ModSettings::g_debug.disableHeadTracking) {
        auto camera_root = getCameraRootNode();
        if (obj->parent && camera_root && camera_root == obj->parent) {
            // The first-person weapon and arms sit much closer to the eyes than real objects would,
            // so with the full eye separation each eye sees them too differently to merge. Move them
            // part of the way with each eye, which reduces their stereo depth.
            // With motion controls the gun has to sit exactly where the controller is, so it keeps
            // full stereo depth.
            // (Full stereo: one update for both eyes, so the full depth.)
            const float follow = ModConstants::gunFollowsController || vr->is_native() ? 0.0f : 1.0f - std::clamp(ModConstants::weaponStereoDepth, 0.0f, 1.0f);
            const auto saved = obj->local.translate;
            if (follow > 0.0f) {
                // world = R * local, so the local offset is R^T * world offset.
                const glm::vec3 delta = glm::transpose(NiMath::to_glm(obj->parent->world.rotate)) * NiMath::to_glm(g_eye_offset_world);
                obj->local.translate.x += follow * delta.x;
                obj->local.translate.y += follow * delta.y;
                obj->local.translate.z += follow * delta.z;
            }
            original_func(obj, a_data);
            obj->local.translate = saved;
            if (vr->tracing()) {
                spdlog::info("[VR-TRACE] fp node update '{}' eye {} frame {} (turn with headset: {})", obj->name.c_str() ? obj->name.c_str() : "",
                    vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? "L" : "R", vr->m_engine_frame_count.load(),
                    !ModSettings::g_debug.disableFirstPersonMeshRotation);
            }
            if (!std::strstr(obj->name.c_str(), "Camera")) {
                UpdateMesh(obj);
            }
            return;
        }
    }
    original_func(obj, a_data);
}

void CreationEngineCameraManager::onScaleformSetViewPort(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {
    using func_t = decltype(onScaleformSetViewPortDetour);
    static auto original_func = m_onScaleformSetViewPortHook->get_original<func_t>();
    onScaleformSetViewPortInternal(thisMovie, viewport);
    BuildConsole::NoteMovie(thisMovie, viewport); // (fix177: another movie drawn inside the build console's boxes)
    MessagesBlock::NoteViewport(thisMovie, viewport); // (fix178: the messages' movie's box, tall in play, 16:9 in menus)
    original_func(thisMovie, viewport);
}

namespace {
    // (fix144) A conversation fixed in the room (after fix143 the conversation moved with the head, so it could hardly be
    // clicked, and was still too small). The head's pose (the headset's, stage space) the frame's camera was placed from (UpdateWorldCamera):
    // the conversation is drawn in each eye's picture where a point of the room, put in front of you as it opened, is seen from there.
    std::mutex g_frame_head_mtx{};
    glm::mat4  g_frame_head{1.0f};
    bool       g_frame_head_valid{false};

    void NoteFrameHead(const glm::mat4& head) {
        std::scoped_lock _{g_frame_head_mtx};
        g_frame_head       = head;
        g_frame_head_valid = true;
    }

    // Where the conversation stands (stage space: y up), put there as it opens: kConversationAhead in front of where your head faced
    // (level), kConversationUp above your eyes; its 16:9 screen kConversationWidth wide (its answers' rows then about 3 cm tall, with
    // Enhanced Dialogue Interface's text at 115). Put anew when a conversation opens after a second without one.
    constexpr float kConversationAhead = 0.45f;
    constexpr float kConversationUp    = 0.04f;
    constexpr float kConversationWidth = 1.20f;
    std::mutex g_anchor_mtx{};
    bool       g_anchor_valid{false};
    glm::vec3  g_anchor{0.0f};
    int64_t    g_anchor_last_ms{0};
    int64_t    g_anchor_seen_ms{0}; // (last seen in a picture: out of view over 2 s, it is put in front of you again)
    int        g_anchor_logs{0};
    // (Full stereo: the left eye's copy of the interface moved by this while a conversation is drawn, its other movies with it.)
    std::atomic<int> g_conversation_shift{0};

    int64_t ConversationNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A point (stage space) in an eye's picture (its pose, its frustum's tangents): across and down (0..1), and how far ahead of the eye
    // (metres); false behind it or nearer than 0.1 m.
    bool ProjectToPicture(const glm::vec3& p, const glm::mat4& eye_pose, const Vector4f& f, float& u, float& v, float& depth) {
        const glm::vec3 pe = glm::vec3{glm::inverse(eye_pose) * glm::vec4{p, 1.0f}};
        depth = -pe.z;
        if (!(depth > 0.1f) || !(f[1] > f[0]) || !(f[2] > f[3])) {
            return false;
        }
        u = (pe.x / depth - f[0]) / (f[1] - f[0]);
        v = (f[2] - pe.y / depth) / (f[2] - f[3]);
        return std::isfinite(u) && std::isfinite(v);
    }

    // (fix144) The conversation's movie placed where its point of the room is seen in the picture this call draws into (mono: the
    // head's one; full stereo: the right eye's, the game's own layer, the left eye's copy moved for it; taking turns: this frame's eye),
    // at the size it has there. Its depth and its answers' box (for the touch) from the eye whose picture the game's mouse is in.
    // False: not placed (no head pose yet, the point behind you): left to the HUD's placement.
    bool PlaceConversation(Scaleform::Gfx::Viewport* viewport, int buffer_w, int buffer_h) {
        static auto vr = VR::get();
        glm::mat4 head{1.0f};
        {
            std::scoped_lock _{g_frame_head_mtx};
            if (!g_frame_head_valid) {
                return false;
            }
            head = g_frame_head;
        }
        glm::vec3 anchor{0.0f};
        {
            std::scoped_lock _{g_anchor_mtx};
            const int64_t now = ConversationNowMs();
            // (Put anew too when out of view for 2 s: you turned away from it, or the headset was recentred: fix144's review.)
            if (!g_anchor_valid || now - g_anchor_last_ms > 1000 || now - g_anchor_seen_ms > 2000) {
                glm::vec3 ahead = -glm::vec3{head[2]};
                ahead.y         = 0.0f;
                ahead           = glm::length(ahead) > 1e-3f ? glm::normalize(ahead) : glm::vec3{0.0f, 0.0f, -1.0f};
                g_anchor        = glm::vec3{head[3]} + ahead * kConversationAhead + glm::vec3{0.0f, kConversationUp, 0.0f};
                g_anchor_valid  = true;
                g_anchor_seen_ms = now;
                if (g_anchor_logs < 20) {
                    ++g_anchor_logs;
                    spdlog::info("[Dialogue] the conversation put in the room {:.2f} m in front of you, {:.2f} m above your eyes, {:.2f} m wide (it stays "
                                 "there as your head moves)",
                        kConversationAhead, kConversationUp, kConversationWidth);
                }
            }
            g_anchor_last_ms = now;
            anchor           = g_anchor;
        }
        const bool native = vr->is_native();
        const bool mono   = vr->is_mono();
        Vector4f   fl{}, fr{}, fm{};
        {
            std::shared_lock _{vr->get_runtime()->eyes_mtx};
            fl = vr->get_runtime()->eye_frustums[0];
            fr = vr->get_runtime()->eye_frustums[1];
            fm = vr->get_runtime()->mono_frustum;
        }
        const glm::mat4 left  = head * vr->get_eye_transform(VRRuntime::Eye::LEFT);
        const glm::mat4 right = head * vr->get_eye_transform(VRRuntime::Eye::RIGHT);
        int             eye   = -1; // (-1 mono, 0 left, 1 right)
        if (!mono) {
            if (native) {
                eye = 1;
            } else {
                auto current = vr->get_current_render_eye();
                if (ModSettings::g_debug.swapHudEyes) {
                    current = current == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
                }
                eye = current == VRRuntime::Eye::LEFT ? 0 : 1;
            }
        }
        const glm::mat4& pose = eye < 0 ? head : (eye == 0 ? left : right);
        const Vector4f&  f    = eye < 0 ? fm : (eye == 0 ? fl : fr);
        const int        reference = mono ? -1 : native ? 1 : (ModConstants::dominantEye == 1 ? 0 : 1);
        const auto       xr        = vr->m_openxr; // (a copy: it may be made anew on another thread)
        if (buffer_w <= 0 || buffer_h <= 0 || !(f[1] > f[0]) || !(f[2] > f[3])) {
            return false;
        }
        const float W = (float)buffer_w;
        const float H = (float)buffer_h;
        float       u = 0.0f, v = 0.0f, depth = 0.0f;
        if (!ProjectToPicture(anchor, pose, f, u, v, depth)) {
            // Behind you, or nearer than 0.1 m: drawn nowhere (left of the picture), no answers to touch: not the HUD's place instead
            // (it would jump to the front of the view, fix144's review).
            viewport->width  = (int)(W * 0.5f);
            viewport->height = (int)(W * 0.5f * 1080.0f / 1920.0f);
            viewport->left   = -4 * buffer_w;
            viewport->top    = 0;
            if (eye == reference && xr != nullptr) {
                for (auto& b : xr->hud_touch_box) {
                    b = 2.0f;
                }
            }
            return true;
        }
        if (u > -0.1f && u < 1.1f && v > -0.1f && v < 1.1f) {
            std::scoped_lock _{g_anchor_mtx};
            g_anchor_seen_ms = ConversationNowMs();
        }
        const float width  = std::min(kConversationWidth / depth * W / (f[1] - f[0]), W);
        const float height = width * 1080.0f / 1920.0f;
        viewport->width    = (int)std::lround(width);
        viewport->height   = (int)std::lround(height);
        viewport->left     = (int)std::lround(u * W - width * 0.5f);
        viewport->top      = (int)std::lround(v * H - height * 0.5f);
        // Full stereo: the left eye's copy of the interface moved to where the left eye sees the point (less the lenses' centres' own
        // move, which D3D12Component adds).
        if (native) {
            float ul = 0.0f, vl = 0.0f, dl = 0.0f;
            if (ProjectToPicture(anchor, left, fl, ul, vl, dl)) {
                const auto  across = [](const Vector4f& g) { return (0.0f - g[0]) / (g[1] - g[0]); };
                const float centre = ModConstants::hudOnOpticalCenter && fl[1] > fl[0] && fr[1] > fr[0] ? across(fl) - across(fr) : 0.0f;
                const int   shift  = (int)std::lround((ul - u - centre) * W);
                g_conversation_shift   = shift;
                vr->m_native_hud_shift = shift;
                vr->m_native_hud_centre = centre;
            }
        }
        // The eye whose picture the game's mouse is placed in (MenuPointer's): its depth, and the answers' box (stage x 300-1620, y
        // 700-1000: the game's list at y 763-994, Enhanced Dialogue Interface's at 763-973, a margin around them; the stage fills the
        // viewport exactly).
        if (eye == reference) {
            GameFlow::dialogueDepthM = depth;
            if (xr != nullptr) {
                const float s   = width / 1920.0f;
                const float ox  = (float)viewport->left;
                const float oy  = (float)viewport->top;
                auto&       box = xr->hud_touch_box;
                box[0]          = (ox + 300.0f * s) / W;
                box[1]          = (oy + 700.0f * s) / H;
                box[2]          = (ox + 1620.0f * s) / W;
                box[3]          = (oy + 1000.0f * s) / H;
            }
        }
        return true;
    }
}

void
CreationEngineCameraManager::onScaleformSetViewPortInternal(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {

    static auto vr = VR::get();
    auto cc = reinterpret_cast<RE::Scaleform::GFx::MovieImpl *>(thisMovie);
    auto file_url = cc->GetMovieDef()->GetFileURL();
    GameFlow::renderMenu(file_url);
    // Test build: each movie's viewport as the game sets it (and the game's mouse place in it while the menu pointer is used).
    UiProbe::OnMovieViewport(thisMovie, file_url, viewport);
    // The area each full-screen menu draws in (the menu panel shows only that box: GameFlow::menuBoxRows).
    GameFlow::noteMenuViewport(file_url, viewport->bufferWidth, viewport->bufferHeight);

    auto backbuffer_size = vr->get_backbuffer_size();
    auto viewport_buffer_width = viewport->bufferWidth;
    auto viewport_buffer_height = viewport->bufferHeight;

    int offset_left = 0;
    int offset_top = 0;

    auto settings = GameFlow::getMenuSettings(file_url);

    if (ModSettings::showFlatScreenDisplay() || !vr->is_hmd_active() || ModSettings::g_debug.disableHudViewport) {
        return;
    }
    // (fix142) The hand scanner: left over the whole picture as the game laid it out. Its marks (the brackets, names and distances
    // on things) are placed where the world camera sees their objects, in this picture; shrunk to 60% and moved like the HUD
    // (before), every mark was pulled toward the middle, off its object, and all at the HUD's one depth (flat and not correct at
    // all in fix141's test). Each eye's frame draws its own marks from its own camera (in full stereo, which draws the interface
    // once, the eyes take turns while the scanner is open: StereoView).
    if (GameFlow::isScannerMovieHash(djb2Hash(file_url))) {
        return;
    }

    // (fix143, fix144) A conversation's answers to be touched (setting DialogueTouch): fixed in the room at arm's length, as big as its
    // place there makes it (PlaceConversation: its depth and answers' box for the touch with it). In full stereo the whole interface
    // layer has one shift: while a conversation is drawn, the conversation's (its other movies at its depth).
    const uint32_t movie_hash   = djb2Hash(file_url);
    const bool     touch_on     = GameFlow::gStore.internalSettings.dialogueTouch;
    const bool     conversation = touch_on && (movie_hash == "Interface/DialogueMenu.swf"_DJB || movie_hash == "Interface/DialogueMenu_LRG.swf"_DJB);
    // (fix145) On a panel of its own in front of the person speaking (setting DialoguePanel): its movie in a fixed box of the interface
    // layer, cut out of it for the panel (ConversationPanel); the rest of the interface as in play (no conversation's shift).
    if (conversation && ConversationPanel::PlaceMovie(viewport, viewport_buffer_width, viewport_buffer_height)) {
        g_conversation_shift = 0;
        return;
    }
    if (conversation && PlaceConversation(viewport, viewport_buffer_width, viewport_buffer_height)) {
        return;
    }
    // (fix177) Build mode as a console (setting BuildConsole): its menu in a fixed 16:9 box of the interface layer, its notifications' movie in
    // one of their own below it, the HUD drawn nowhere meanwhile; the parts cut out for panels of their own (BuildConsole). False for every
    // other movie, and in play (one look at a number).
    if (BuildConsole::PlaceMovie(thisMovie, movie_hash, viewport, viewport_buffer_width, viewport_buffer_height)) {
        return;
    }
    const int  conversation_shift = g_conversation_shift.load();
    const bool in_conversation    = touch_on && conversation_shift != 0 && GameFlow::isDialogueDrawnNow();

    auto width_multiplier = settings.hud_scale;
    auto height_multiplier = settings.hud_scale;

    // Implement offset based on dominant eye and menu-specific offset_value
    auto current_eye = vr->get_current_render_eye();
    if (vr->tracing()) {
        vr->trace("hud viewport", vr->m_engine_frame_count.load(), (int)current_eye);
        TraceCameras("cams@hud");
    }
    if (ModSettings::g_debug.swapHudEyes) {
        current_eye = current_eye == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
    }
    if (vr->is_mono() || vr->is_native()) {
        // Mono: both eyes see the same picture, so no per-eye shift (it would make the HUD jump every other frame). Full stereo:
        // the HUD is drawn once a frame, for both eyes: the left eye's copy of it is moved instead (D3D12Component), as taking turns
        // moved it (the HUD's depth).
        offset_left = 0;
        if (vr->is_native()) {
            // (As taking turns placed it: the dominant eye's HUD moved by the HUD's shift, the other's not: the right eye's here (the
            // game's own layer), the left eye's copy moved right by the whole shift from it. One shift for the whole layer: the HUD's.)
            // (fix143: during a conversation to be touched, the conversation's shift, for its depth at arm's length.)
            const auto shift = in_conversation ? conversation_shift : static_cast<int>(GameFlow::gStore.hudSettings.perspective);
            offset_left             = ModConstants::dominantEye == 1 ? -shift : 0;
            vr->m_native_hud_shift = shift;
        }
    } else if (!touch_on && GameFlow::dialoguePointing.load() &&
               (movie_hash == "Interface/DialogueMenu.swf"_DJB || movie_hash == "Interface/DialogueMenu_LRG.swf"_DJB)) {
        // (fix142) A hand points at the conversation's responses: drawn at the same place in both eyes' frames (the game's mouse is
        // placed for one picture; moved in the other's frames, the response under it would change every other frame). (Not to be
        // touched only: the answers need their depth to be reached; their rows are wide, so the mouse stays on the same one.)
        offset_left = 0;
    } else {
        const int perspective = settings.perspective;
        if (ModConstants::dominantEye == 1) {
            // Dominant eye is right
            offset_left = (current_eye == VRRuntime::Eye::RIGHT) ? -perspective : 0;
        } else {
            // Dominant eye is left
            offset_left = (current_eye == VRRuntime::Eye::LEFT) ? perspective : 0;
        }
    }

    // Each eye's lens is off-centre, so "straight ahead" is not the middle of the eye's image.
    // Move the HUD (and its crosshair) so its centre sits on the straight-ahead direction,
    // which is where the gun fires.
    // (Full stereo, fix138: the game's interface layer ends up in the right eye's picture, so it goes at the right eye's straight
    // ahead; the left eye's copy of it is moved by how far the left eye's straight ahead is from the right's, m_native_hud_centre.)
    if (ModConstants::hudOnOpticalCenter) {
        const bool native    = vr->is_native();
        const int  eye_index = native ? 1 : vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
        Vector4f   f{}, left{};
        {
            std::shared_lock _{vr->get_runtime()->eyes_mtx};
            f    = native ? vr->get_runtime()->eye_frustums[1] : vr->get_runtime()->frustums[eye_index];
            left = vr->get_runtime()->eye_frustums[0];
        }
        const auto across = [](const Vector4f& g) { return (0.0f - g[0]) / (g[1] - g[0]); }; // 0..1 from the left edge
        if (f[1] > f[0] && f[2] > f[3]) {
            const float center_x = across(f);
            const float center_y = f[2] / (f[2] - f[3]);          // 0..1 from the top edge
            offset_left += (int) ((center_x - 0.5f) * (float) viewport_buffer_width);
            offset_top += (int) ((center_y - 0.5f) * (float) viewport_buffer_height);
            if (native) {
                vr->m_native_hud_centre = left[1] > left[0] ? across(left) - center_x : 0.0f;
            }
        }
    } else if (vr->is_native()) {
        vr->m_native_hud_centre = 0.0f;
    }

    auto visible_width = std::min((int) ((float) backbuffer_size[0] * width_multiplier), viewport_buffer_width);
    auto visible_height = std::min((int) ((float) backbuffer_size[1] * height_multiplier), viewport_buffer_height);
    viewport->width = visible_width;
    viewport->height = visible_height;
    viewport->left = (int) (viewport_buffer_width - visible_width) / 2 + offset_left;
    viewport->top = (int) (viewport_buffer_height - visible_height) / 2 + offset_top;
}

namespace {
    // Diagnostics for the per-eye projection (written to the log every few seconds).
    std::atomic<uint32_t> g_frustum_hook_calls{0};
    RE::NiFrustum         g_last_game_frustum{};
}

namespace {
    // Every camera that sets a frustum, for diagnostics (the first-person weapon may use its own).
    struct CameraSeen {
        std::string name;
        uint32_t    calls{0};
        uint32_t    overridden{0};
        RE::NiFrustum last{};       // last frustum the game asked for
    };
    std::mutex                                g_cameras_mtx{};
    std::unordered_map<uintptr_t, CameraSeen> g_cameras{};
    std::atomic<RE::NiCamera*>                g_first_person_camera{nullptr};

    // The gun and arms are drawn by the game's own first-person camera ('1st Person Camera - World
    // Reference'). The game sets that camera's lens itself, once when a save loads or its FOV
    // settings change, from its FOV settings rather than from the headset: about 73x76 degrees
    // against the eye's 108x110. The gun was drawn ~1.9x too big in each eye, which in stereo put
    // it at about half its real distance (squashed toward the face, so everything in front of the
    // hand seemed missing). The world camera already gets the eye's lens; this one gets it too.
    std::mutex            g_fp_lens_mtx{};
    RE::NiFrustum         g_fp_game_frustum{};       // the lens the game last asked for (put back when off)
    bool                  g_fp_have_game_frustum{false};
    std::atomic<bool>     g_fp_lens_applied{false};
    std::atomic<uint32_t> g_fp_lens_writes{0};       // per-frame writes since the last status line
    std::atomic<uint32_t> g_fp_game_sets{0};         // times the game set the lens since the last status line
    RE::NiCamera*         g_fp_lens_logged_for{nullptr};

    // Plain functions so they can use SEH: cameras can be torn down while a save is loading.
    bool same_class_guarded(void* obj, void* reference) {
        __try {
            return *(void**)obj == *(void**)reference;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool write_frustum_guarded(RE::NiFrustum* dst, const RE::NiFrustum* src) {
        __try {
            dst->left   = src->left;
            dst->right  = src->right;
            dst->top    = src->top;
            dst->bottom = src->bottom;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool read_frustum_guarded(const RE::NiFrustum* src, RE::NiFrustum* dst) {
        __try {
            *dst = *src;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // True if the camera is still the game's first-person camera (same class as the world camera
    // and the same name); the pointer goes stale when a save loads.
    bool is_first_person_camera_guarded(RE::NiCamera* cam, RE::NiCamera* world_camera) {
        __try {
            if (*(void**)cam != *(void**)world_camera) {
                return false;
            }
            const char* name = cam->name.c_str();
            return name != nullptr && std::strncmp(name, "1st Person Camera", 17) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // How much bigger the first camera draws things than the second: the lengths of the across
    // and up rows of their world-to-clip matrices (1.00 = the same lens).
    float row_length(const float* r) {
        return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    }

    bool lens_scale_guarded(RE::NiCamera* cam, RE::NiCamera* reference, float& across, float& up) {
        __try {
            const float a0 = row_length(cam->worldToCam[0]), a1 = row_length(cam->worldToCam[1]);
            const float b0 = row_length(reference->worldToCam[0]), b1 = row_length(reference->worldToCam[1]);
            if (!(b0 > 0.0f) || !(b1 > 0.0f)) {
                return false;
            }
            across = a0 / b0;
            up = a1 / b1;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Set while the game's CalcFrustum runs, so a SetFrustum it makes inside isn't recorded twice.
    thread_local RE::NiCamera* t_calc_camera{nullptr};
    thread_local bool          t_calc_saw_set{false};

    void remember_fp_game_frustum(const RE::NiFrustum& f) {
        std::scoped_lock _{g_fp_lens_mtx};
        g_fp_game_frustum = f;
        g_fp_have_game_frustum = true;
        g_fp_game_sets.fetch_add(1);
    }

    bool is_screen_like(const RE::NiFrustum& f) {
        // Symmetric perspective frustum with the same shape as the main camera's: a camera that
        // draws to the screen (e.g. a separate first-person weapon camera), not a shadow map.
        const auto& w = g_last_game_frustum;
        if (!(f.right > 0.0f && f.left < 0.0f && f.top > 0.0f && f.bottom < 0.0f) || w.top <= 0.0f || w.right <= 0.0f) {
            return false;
        }
        if (std::abs(f.left + f.right) > 1e-3f * f.right || std::abs(f.top + f.bottom) > 1e-3f * f.top) {
            return false;
        }
        const float aspect = f.right / f.top;
        const float world_aspect = w.right / w.top;
        return std::abs(aspect - world_aspect) < 0.03f * world_aspect;
    }
}

void CreationEngineCameraManager::onSetNimFrustum(RE::NiCamera *pCamera, RE::NiFrustum *pFrustum) {
    using func_t = decltype(onSetNimFrustumDetour);
    static auto original_func = m_onSetNimFrustumHook->get_original<func_t>();
    const bool valid = isValidCamera(pCamera);
    if (pFrustum != nullptr && valid) {
        ++g_frustum_hook_calls;
        g_last_game_frustum = *pFrustum;
    }

    bool extra_override = false;
    if (pFrustum != nullptr && pCamera != nullptr && !valid) {
        static auto vr = VR::get();
        extra_override = ModSettings::g_debug.eyeProjectionOtherCameras && vr->is_hmd_active() && !ModSettings::g_debug.disableFrustumOverride &&
                         !ModSettings::cameraFlat() && is_screen_like(*pFrustum);

        bool is_first_person = false;
        {
            std::scoped_lock _{g_cameras_mtx};
            // (fix189, the review) The game makes new cameras all the time (a menu's preview camera on each opening, the first-person camera on
            // each load) and never tells when one is gone: the list was never emptied and a camera's name was read only for the first 64, so
            // after that a new first-person camera was never recognised (the gun and arms drawn ~1.9x too big with the body off), and a
            // camera made where an old one was kept the old one's name. Now the list starts again when it reaches 64 (it is only counts for
            // the log), and the name is read on every call (only when it changed is it copied).
            static int s_cameras_logged = 0;
            if (g_cameras.size() >= 64 && !g_cameras.contains((uintptr_t)pCamera)) {
                g_cameras.clear();
            }
            auto [it, inserted] = g_cameras.try_emplace((uintptr_t)pCamera);
            it->second.calls++;
            it->second.last = *pFrustum;
            {
                const char*            raw_name = pCamera->name.c_str();
                const std::string_view name_now{ raw_name != nullptr ? raw_name : "" };
                if (inserted || it->second.name != name_now) {
                    it->second.name = std::string{ name_now };
                }
            }
            if (inserted && s_cameras_logged < 63) { // (the first 63 cameras seen logged, as before)
                ++s_cameras_logged;
                spdlog::info("[VR-CAM] other camera '{}' {} sets frustum {:.3f} {:.3f} {:.3f} {:.3f} near {:.3f} far {:.1f} (screen-like {})",
                    it->second.name, fmt::ptr(pCamera), pFrustum->left, pFrustum->right, pFrustum->top, pFrustum->bottom,
                    pFrustum->_near, pFrustum->_far, is_screen_like(*pFrustum));
            }
            if (it->second.name.starts_with("1st Person Camera")) {
                g_first_person_camera = pCamera;
                is_first_person = true;
            }
            if (is_first_person && first_person_lens_wanted()) {
                extra_override = true; // the gun/arms camera always gets the eye's lens
            }
            if (extra_override) {
                it->second.overridden++;
            }
        }
        if (is_first_person) {
            remember_fp_game_frustum(*pFrustum);
            if (t_calc_camera == pCamera) {
                t_calc_saw_set = true;
            }
        }
    }

    if (extra_override) {
        apply_eye_frustum_values(pFrustum);
    } else {
        onSetNiFrustumInternal(pCamera, pFrustum);
    }
    original_func(pCamera, pFrustum);
}

// Writes the eye's projection (same as the main camera's) into a frustum.
void CreationEngineCameraManager::apply_eye_frustum_values(RE::NiFrustum *pFrustum) {
    static auto vr = VR::get();
    auto eye = vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
    Vector4f frustum = vr->get_runtime()->frustums[eye];
    if (!(frustum[1] > frustum[0]) || !(frustum[2] > frustum[3])) {
        return;
    }
    aiming_adjustments(frustum, get_fov_adjustment());
    pFrustum->left = frustum[0];
    pFrustum->right = frustum[1];
    pFrustum->top = frustum[2];
    pFrustum->bottom = frustum[3];
}

void aiming_adjustments(Vector4f &frustum, float fov_adjustment) {
    frustum[0] = std::tanf(std::atanf(frustum[0]) - fov_adjustment);
    frustum[1] = std::tanf(std::atanf(frustum[1]) + fov_adjustment);
    frustum[2] = std::tanf(std::atanf(frustum[2]) + fov_adjustment);
    frustum[3] = std::tanf(std::atanf(frustum[3]) - fov_adjustment);
}

void CreationEngineCameraManager::onSetNiFrustumInternal(RE::NiCamera *pCamera, RE::NiFrustum *pFrustum) {
    if (!isValidCamera(pCamera) || ModSettings::g_debug.disableFrustumOverride || ModSettings::cameraFlat()) {
        return;
    }
    static auto vr = VR::get();
    if (!vr->is_hmd_active()) {
        return;
    }
    auto eye = vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
    if (vr->tracing()) {
        vr->trace("frustum", vr->m_engine_frame_count.load(), eye);
    }
    auto runtime = vr->get_runtime();
    Vector4f frustum = runtime->frustums[eye];

    // Not filled yet (no headset pose received) or invalid: leave the game's frustum alone.
    if (!(frustum[1] > frustum[0]) || !(frustum[2] > frustum[3])) {
        return;
    }

    aiming_adjustments(frustum, get_fov_adjustment());
    pFrustum->left = frustum[0];
    pFrustum->right = frustum[1];
    pFrustum->top = frustum[2];
    pFrustum->bottom = frustum[3];
}

void CreationEngineCameraManager::onCalcNiFrustum(RE::NiCamera *pCamera, float fov, float aspectRatio, float nearz,
                                                  float farz, char lodAdjust) {
    using func_t = decltype(onCalcNimFrustumDetour);
    static auto original_func = m_onCalcNimFrustumHook->get_original<func_t>();
    auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();

    //    spdlog::info("Setting frustum for camera [{}] {} camera[{} {}] setFov[{}]", fmt::ptr(pCamera), pCamera->name, playerCamera->fov, playerCamera->notViewFov, fov);
    static auto vr = VR::get();

    if (vr->is_hmd_active() && CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera == pCamera) {
        //        fov = fov + Constants::lodAdjustFov;
        m_fov_adjust = fov - playerCamera->fov;
        vr->m_nearz = nearz;
        vr->m_farz = farz;
    }

    // The first-person (gun/arms) camera: note the lens the game computes, then give it the eye's.
    const bool first_person = pCamera != nullptr && pCamera == g_first_person_camera.load();
    if (!first_person) {
        original_func(pCamera, fov, aspectRatio, nearz, farz, lodAdjust);
        return;
    }
    t_calc_camera = pCamera;
    t_calc_saw_set = false;
    original_func(pCamera, fov, aspectRatio, nearz, farz, lodAdjust);
    t_calc_camera = nullptr;

    RE::NiFrustum computed{};
    if (!read_frustum_guarded(&pCamera->viewFrustum, &computed)) {
        return;
    }
    if (!t_calc_saw_set) {
        remember_fp_game_frustum(computed); // (a SetFrustum inside already recorded it)
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 6) {
        spdlog::info("[VR-CAM] game computes the first-person camera lens: fov {:.2f} aspect {:.3f} near {:.3f} far {:.1f} -> {:.3f} {:.3f} {:.3f} {:.3f}",
            fov, aspectRatio, nearz, farz, computed.left, computed.right, computed.top, computed.bottom);
    }
    if (first_person_lens_wanted()) {
        apply_eye_frustum_values(&computed);
        write_frustum_guarded(&pCamera->viewFrustum, &computed);
    }
}

float CreationEngineCameraManager::get_fov_adjustment() const {
    // No aiming zoom with the body shown: you aim the gun in your hand, not through the view.
    if (GameFlow::gStore.internalSettings.preventZoom || body::IsActive()) {
        return 0.0f;
    }
    return tanf((m_fov_adjust * ModConstants::DEG_TO_RAD) / 2.0f);
}

float CreationEngineCameraManager::get_head_tracking_multiplier() const {
    static auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
    auto multiplier = (playerCamera->fov + m_fov_adjust) / playerCamera->fov;
    return multiplier * ModConstants::headTrackingMultiplier;
}


// The game only calls SetFrustum when its FOV changes, not every frame. With one eye per frame
// that left both eyes with the same (one eye's) projection, which moves the image differently
// in each eye. Re-apply the projection of the eye being rendered (ApplyEyeFrustum below).

void CreationEngineCameraManager::ApplyEyeFrustum(RE::NiCamera *pCamera) {
    if (pCamera == nullptr) {
        return;
    }
    // Only write the four tangents straight into the camera; calling the engine's setter here
    // (fix9/10) is not needed and is unsafe while a save loads.
    RE::NiFrustum frustum = pCamera->viewFrustum; // keeps near/far
    onSetNiFrustumInternal(pCamera, &frustum);
    if (!write_frustum_guarded(&pCamera->viewFrustum, &frustum)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::error("[VR-CAM] could not write the camera frustum (camera not valid)");
        }
    }
}

bool CreationEngineCameraManager::first_person_lens_wanted() {
    static auto vr = VR::get();
    return ModConstants::firstPersonEyeLens && vr->is_hmd_active() && !ModSettings::g_debug.disableFrustumOverride &&
           !ModSettings::cameraFlat();
}

// Every frame (start of the frame, before the game places its cameras): the first-person camera
// gets the same lens as the eye being drawn, so the gun and arms are drawn at their real size and
// distance. The game rebuilds the camera's matrix from this when it places the camera. When it's
// not wanted (menus on the flat screen, switched off) the game's own lens is put back.
void CreationEngineCameraManager::ApplyFirstPersonLens() {
    auto worldCamera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
    auto fp = g_first_person_camera.load();
    if (worldCamera == nullptr || fp == nullptr) {
        return;
    }
    if (!is_first_person_camera_guarded(fp, worldCamera)) {
        // Gone (a save loaded); the game registers the new one when it sets its lens.
        g_first_person_camera.compare_exchange_strong(fp, nullptr);
        g_fp_lens_applied = false;
        return;
    }

    if (first_person_lens_wanted()) {
        RE::NiFrustum lens{};
        if (!read_frustum_guarded(&fp->viewFrustum, &lens)) {
            return;
        }
        const RE::NiFrustum before = lens;
        apply_eye_frustum_values(&lens); // keeps the game's near/far

        // Did something other than SetFrustum/CalcFrustum (which are hooked) change it since our
        // last write? Then the game has another way of setting this lens that still needs handling.
        static RE::NiFrustum last_written{};
        const auto differs = [](const RE::NiFrustum& a, const RE::NiFrustum& b) {
            return std::abs(a.left - b.left) > 1e-4f || std::abs(a.right - b.right) > 1e-4f || std::abs(a.top - b.top) > 1e-4f ||
                   std::abs(a.bottom - b.bottom) > 1e-4f;
        };
        if (g_fp_lens_applied.load() && differs(before, last_written) && differs(before, lens)) {
            static int reported = 0;
            if (reported++ < 10) {
                spdlog::info("[VR-CAM] first-person camera lens was changed outside the hooked setters: {:.3f} {:.3f} {:.3f} {:.3f} (we had {:.3f} {:.3f} {:.3f} {:.3f})",
                    before.left, before.right, before.top, before.bottom, last_written.left, last_written.right, last_written.top, last_written.bottom);
            }
        }

        if (!write_frustum_guarded(&fp->viewFrustum, &lens)) {
            return;
        }
        last_written = lens;
        g_fp_lens_applied = true;
        g_fp_lens_writes.fetch_add(1);
        if (g_fp_lens_logged_for != fp) {
            g_fp_lens_logged_for = fp;
            const float scale = lens.right > lens.left && before.right > before.left ? (lens.right - lens.left) / (before.right - before.left) : 0.0f;
            spdlog::info("[VR-CAM] first-person (gun/arms) camera {} now uses the eye's lens: was {:.3f} {:.3f} {:.3f} {:.3f} near {:.3f}, now {:.3f} {:.3f} {:.3f} {:.3f} "
                         "(the gun was drawn {:.2f}x too big)",
                fmt::ptr(fp), before.left, before.right, before.top, before.bottom, before._near, lens.left, lens.right, lens.top, lens.bottom, scale);
        }
    } else if (g_fp_lens_applied.exchange(false)) {
        RE::NiFrustum game{};
        bool have_game = false;
        {
            std::scoped_lock _{g_fp_lens_mtx};
            game = g_fp_game_frustum;
            have_game = g_fp_have_game_frustum;
        }
        if (have_game) {
            write_frustum_guarded(&fp->viewFrustum, &game);
        }
        static int logged = 0;
        if (logged++ < 5) {
            spdlog::info("[VR-CAM] first-person camera: game's own lens put back ({})",
                !ModConstants::firstPersonEyeLens ? "switched off" : "flat screen / menu");
        }
    }
}

// Eye offset for the world camera. The game lines the gun sights up with the camera centre,
// which in VR is the point between the eyes, so with both eyes offset neither eye looks down
// the sights. While aiming, move both eyes so the dominant one sits on the sight line.
// Aiming down sights: the game's flag, or the game narrowing its FOV (it zooms when aiming).
bool CreationEngineCameraManager::is_aiming() const {
    return GameFlow::isAimingDownSights() || m_fov_adjust < -1.0f;
}

Matrix4x4f CreationEngineCameraManager::get_camera_eye_transform() const {
    static auto vr = VR::get();
    auto eye = vr->get_current_eye_transform();

    // With the body shown the gun is in your hand, not on the view, so both eyes stay where they are.
    if (ModConstants::sightsOnDominantEye && is_aiming() && !body::IsActive() && !vr->is_native()) {
        const int current = vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
        // The Dominant Eye setting lists "Right" first (0) and "Left" second (1).
        const int dominant_index = ModConstants::dominantEye == 1 ? 0 : 1;
        const auto dominant = current == dominant_index ? eye : vr->get_current_eye_transform(true);
        eye[3].x -= dominant[3].x;
        eye[3].y -= dominant[3].y;
        eye[3].z -= dominant[3].z;
    }

    return eye;
}

// The first-person weapon/arms are drawn by their own camera. Give it the same eye offset as
// the world camera, otherwise the gun has no stereo depth and looks doubled up close.
void CreationEngineCameraManager::UpdateFirstPersonCamera() {
    static auto vr = VR::get();
    auto worldCamera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
    auto fp = g_first_person_camera.load();

    if (worldCamera == nullptr || fp == nullptr || !vr->is_hmd_active() || ModSettings::cameraFlat() ||
        !same_class_guarded(fp, worldCamera)) {
        return;
    }

    TraceCameras("cams@sim");

    static RE::NiCamera* logged_for = nullptr;
    if (logged_for != fp) {
        logged_for = fp;
        auto name_of = [](RE::NiAVObject* o) -> std::string {
            if (o == nullptr) return "(none)";
            auto n = o->name.c_str();
            return n ? n : "";
        };
        spdlog::info("[VR-CAM] 1st person camera parent '{}' {}, world camera parent '{}' {}, fp is child of world camera {}",
            name_of((RE::NiAVObject*)fp->parent), fmt::ptr(fp->parent), name_of((RE::NiAVObject*)worldCamera->parent),
            fmt::ptr(worldCamera->parent), (void*)fp->parent == (void*)worldCamera);
    }

    static uint32_t frames = 0;
    if (++frames % 600 == 0) {
        const auto& a = fp->world.translate;
        const auto& b = worldCamera->world.translate;
        spdlog::info("[VR-CAM] 1st person camera world pos minus world camera pos: {:.4f} {:.4f} {:.4f} (eye {}), fp local {:.4f} {:.4f} {:.4f}",
            a.x - b.x, a.y - b.y, a.z - b.z, vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? "L" : "R",
            fp->local.translate.x, fp->local.translate.y, fp->local.translate.z);
    }

    if (!ModSettings::g_debug.eyeOffsetFirstPersonCamera || (void*)fp->parent == (void*)worldCamera) {
        return; // a child of the world camera already moves with it
    }

    if (fp->parent == worldCamera->parent) {
        // Same parent: use exactly the world camera's head/eye placement.
        fp->local.rotate = worldCamera->local.rotate;
        fp->local.translate = worldCamera->local.translate;
        return;
    }

    // No parent: the game places this camera itself each frame. Copy the world camera's final
    // placement (which includes the eye offset) once the game has computed it.
    // worldCamera->world is still last frame's (the other eye), so rebuild this frame's placement
    // from its parent and the local transform UpdateWorldCamera just set.
    if (fp->parent == nullptr && worldCamera->parent != nullptr) {
        // parent world * local, in the game's matrix layout (the SDK's NiTransform operator* assumes
        // the other layout and gave a rotation ~60 degrees off and a position ~20 cm off).
        const auto& parent = worldCamera->parent->world;
        const auto& local = worldCamera->local;
        const glm::mat3 parent_rotation = NiMath::to_glm(parent.rotate);
        RE::NiTransform placed{};
        NiMath::from_glm(parent_rotation * NiMath::to_glm(local.rotate), placed.rotate);
        placed.translate = NiMath::to_ni(NiMath::to_glm(parent.translate) + parent_rotation * (NiMath::to_glm(local.translate) * parent.scale));
        placed.scale = parent.scale * local.scale;
        fp->local = placed;
        fp->world = placed;
    }
}

// Called on the render side of a frame (HUD setup), after the game finished placing cameras.
void CreationEngineCameraManager::TraceCameras(const char* where) {
    static auto vr = VR::get();
    auto worldCamera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
    auto fp = g_first_person_camera.load();
    if (!vr->tracing() || worldCamera == nullptr || fp == nullptr || !same_class_guarded(fp, worldCamera)) {
        return;
    }
    const auto& a = fp->world.translate;
    const auto& b = worldCamera->world.translate;
    spdlog::info("[VR-TRACE] {:<14} fp-world {:+.4f} {:+.4f} {:+.4f} | world cam {:.3f} {:.3f} {:.3f} | eye {}", where,
        a.x - b.x, a.y - b.y, a.z - b.z, b.x, b.y, b.z, vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? "L" : "R");
}

RE::NiCamera* CreationEngineCameraManager::first_person_reference_camera() {
    return g_first_person_camera.load();
}

void CreationEngineCameraManager::recenter_head(const char* why) {
    static auto vr = VR::get();
    const auto head = vr->get_position(0);
    const auto origin = vr->get_standing_origin();
    vr->set_standing_origin(head);
    spdlog::info("[VR] view recentred ({}): headset was {:.2f} {:.2f} {:.2f} m from the old centre", why, head.x - origin.x, head.y - origin.y, head.z - origin.z);
}

void CreationEngineCameraManager::UpdateWorldCamera() {
    static auto vr = VR::get();
    auto sceneGraphRoot = CreationEngineSingletonManager::GetSceneGraphRoot();
    auto worldCamera = sceneGraphRoot->worldCamera;

    if (!worldCamera) {
        return;
    }

    // Put the view back at your head after each loading screen (the headset's starting position
    // is otherwise wherever it was when the game started).
    if (ModConstants::recenterAfterLoading && vr->is_hmd_active() && GameFlow::takeRecenterAfterLoading()) {
        recenter_head("after loading");
    }

    const RE::NiFrustum before_apply = worldCamera->viewFrustum;
    bool applied = false;

    if (vr->is_hmd_active() && ModSettings::g_debug.forceFrustumEveryFrame && !ModSettings::g_debug.disableFrustumOverride &&
        !ModSettings::cameraFlat() && CreationEngineSingletonManager::GetPlayerRef() != nullptr) {
        applied = true;
        ApplyEyeFrustum(worldCamera);
    }
    ApplyFirstPersonLens();

    {
        static uint32_t frames = 0;
        if (vr->is_hmd_active() && ++frames % 600 == 0) {
            const auto eye = vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
            const auto& f = vr->get_runtime()->frustums[eye];
            const auto& g = g_last_game_frustum;
            const auto& c = worldCamera->viewFrustum;
            const auto& b = before_apply;
            spdlog::info("[VR-CAM] SetFrustum calls in last 600 frames: {}, eye {}, headset tan(l r t b) {:.3f} {:.3f} {:.3f} {:.3f}, "
                         "game last set {:.3f} {:.3f} {:.3f} {:.3f}, camera before {:.3f} {:.3f} {:.3f} {:.3f}, after {:.3f} {:.3f} {:.3f} {:.3f} "
                         "(applied {}), zoom adjust {:.2f} deg, 1st person {}, menu/flat {}, aiming {} (game flag {}), "
                         "head offset from origin {:.2f} {:.2f} {:.2f} m",
                g_frustum_hook_calls.exchange(0), eye ? "R" : "L", f[0], f[1], f[2], f[3], g.left, g.right, g.top, g.bottom,
                b.left, b.right, b.top, b.bottom, c.left, c.right, c.top, c.bottom, applied, m_fov_adjust,
                GameFlow::isInFirstPerson(), ModSettings::showFlatScreenDisplay(), is_aiming(), GameFlow::isAimingDownSights(),
                vr->get_transform(0)[3].x - vr->get_transform_offset()[3].x, vr->get_transform(0)[3].y - vr->get_transform_offset()[3].y,
                vr->get_transform(0)[3].z - vr->get_transform_offset()[3].z);
            {
                std::scoped_lock _{g_cameras_mtx};
                for (auto& [ptr, cam] : g_cameras) {
                    if (cam.calls > 0) {
                        spdlog::info("[VR-CAM]   other camera '{}' {:x}: {} frustum sets, {} given the eye projection, game asked for {:.3f} {:.3f} {:.3f} {:.3f}",
                            cam.name, ptr, cam.calls, cam.overridden, cam.last.left, cam.last.right, cam.last.top, cam.last.bottom);
                    }
                    cam.calls = 0;
                    cam.overridden = 0;
                }
            }

            // Is the gun drawn with the eye's lens? The size ratio comes from the matrices the
            // game built from each camera's lens (1.00 = same lens; it was about 1.87).
            if (auto fp = g_first_person_camera.load(); fp != nullptr && is_first_person_camera_guarded(fp, worldCamera)) {
                RE::NiFrustum now{}, game{};
                bool have_game = false;
                {
                    std::scoped_lock _{g_fp_lens_mtx};
                    game = g_fp_game_frustum;
                    have_game = g_fp_have_game_frustum;
                }
                float across = 0.0f, up = 0.0f;
                const bool have_scale = lens_scale_guarded(fp, worldCamera, across, up);
                if (read_frustum_guarded(&fp->viewFrustum, &now)) {
                    spdlog::info("[VR-CAM]   first-person (gun/arms) camera lens: now {:.3f} {:.3f} {:.3f} {:.3f}, game's own {} | gun drawn {} the eye's size | "
                                 "headset lens {} ({} writes, game set it {} times since last line)",
                        now.left, now.right, now.top, now.bottom,
                        have_game ? fmt::format("{:.3f} {:.3f} {:.3f} {:.3f}", game.left, game.right, game.top, game.bottom) : std::string{"(not seen)"},
                        have_scale ? fmt::format("{:.2f}x across, {:.2f}x up", across, up) : std::string{"?"},
                        g_fp_lens_applied.load() ? "on" : "off", g_fp_lens_writes.exchange(0), g_fp_game_sets.exchange(0));
                }
            }
        }
    }

    static auto originalRotation = worldCamera->local.rotate;
    static auto originalPosition = worldCamera->local.translate;

    if (!vr->is_hmd_active() || ModConstants::cameraShake || ModSettings::cameraFlat() || ModSettings::g_debug.disableHeadTracking) {
        worldCamera->local.rotate = originalRotation;
        worldCamera->local.translate = originalPosition;
        WatchCameraBehindMenu(worldCamera, false);
        // (fix199) The seat view off only for good reasons (no headset, the flat screen, head tracking off, camera shake): a menu on its
        // panel (the star map from the pilot's seat) leaves it as it is.
        if (!vr->is_hmd_active() || ModConstants::cameraShake || ModSettings::g_internalSettings.forceFlatScreen ||
            ModSettings::g_debug.disableHeadTracking) {
            SeatView::Off();
        }
        std::scoped_lock _{g_head_local_mtx};
        g_head_local_valid = false;
        return;
    }

    if (vr->tracing()) {
        vr->trace(GameFlow::isInFirstPerson() ? "camera(1st)" : "camera(3rd)", vr->m_engine_frame_count.load(), (int)vr->get_current_render_eye());
    }

    // (The head and the eye drawn as the camera's local transform is made from them, before any aim: for MenuCamera, which moves the
    // menu's frozen copy of this camera as the head moves.)
    glm::mat4 head_for_menu{1.0f};
    if(!GameFlow::isImmovable() && !GameFlow::isControlledByAI() && !GameFlow::isOnLadder() && GameFlow::isInFirstPerson()) {
        auto hmd_transform = vr->get_transform(0);
        NoteFrameHead(hmd_transform); // (fix144: the conversation fixed in the room is placed from it)
        auto standing_origin = vr->get_transform_offset();
        hmd_transform[3].x -= standing_origin[3].x;
        hmd_transform[3].y -= standing_origin[3].y;
        hmd_transform[3].z -= standing_origin[3].z;
        if (ModConstants::sightsOnDominantEye && is_aiming() && !body::IsActive()) {
            // The gun is attached to the view, so leaning the head would move the eye off the
            // sights. While aiming, keep the eye exactly on the sight line. (Not with the body
            // shown: the gun is in your hand then, not on the view.)
            hmd_transform[3] = Vector4f{0.0f, 0.0f, 0.0f, 1.0f};
        }
        // Head and hand movement share one scale so the hands stay where they are relative to the
        // head ("Match body height", SFVR).
        const float tracking_scale = tracking::TrackingScale();
        hmd_transform[3] = Vector4f{glm::vec3{hmd_transform[3]} * tracking_scale, 1.0f};
        auto eye = get_camera_eye_transform();
        eye[3] = Vector4f{glm::vec3{eye[3]} * tracking_scale, 1.0f};
        head_for_menu = to_havok_space(hmd_transform * eye);
        // The camera's parent carries the aim rotation (the headset, or the right controller when
        // aiming with it); the local transform undoes it so the view stays on the head.
        const auto parent_aim = glm::inverse(tracking::CameraParentAimRotation((int)vr->m_engine_frame_count.load()));
        const auto head_only = to_havok_space(parent_aim * hmd_transform);
        // Crouching with the body shown: the view goes down to the crouched body's eyes (world up, turned
        // into the camera parent's frame). The body module lowers the hands by the same amount.
        glm::vec3 crouch_local{0.0f};
        if (const float crouch = body::ViewHeightOffset(); crouch != 0.0f && worldCamera->parent != nullptr) {
            const auto& parent_world = worldCamera->parent->world;
            const float parent_scale = parent_world.scale != 0.0f ? parent_world.scale : 1.0f;
            crouch_local = glm::transpose(NiMath::to_glm(parent_world.rotate)) * glm::vec3{0.0f, 0.0f, crouch} / parent_scale;
        }
        // (fix199) Just up from a seat: the view's move to the seated body's eyes eased out here too (SeatView; zero once off; the hands
        // take the same move, tracking::StageToWorld).
        if (const glm::vec3 seat_shift = SeatView::EaseOut(); seat_shift != glm::vec3{0.0f} && worldCamera->parent != nullptr) {
            const auto& parent_world = worldCamera->parent->world;
            const float parent_scale = parent_world.scale != 0.0f ? parent_world.scale : 1.0f;
            crouch_local += glm::transpose(NiMath::to_glm(parent_world.rotate)) * seat_shift / parent_scale;
        }
        {
            std::scoped_lock _{g_head_local_mtx};
            g_head_local = glm::vec3(head_only[3]) + crouch_local;
            g_head_local_valid = true;
        }
        MotionWeapon::on_frame_start(worldCamera, glm::vec3(head_only[3]));
        // (Full stereo: the right eye made the same way, for its own camera: how far it is from the left, in the parent's space.)
        if (vr->is_native()) {
            auto right = vr->get_eye_transform(VRRuntime::Eye::RIGHT);
            right[3] = Vector4f{glm::vec3{right[3]} * tracking_scale, 1.0f};
            const auto left_at  = to_havok_space(parent_aim * hmd_transform * eye);
            const auto right_at = to_havok_space(parent_aim * hmd_transform * right);
            StereoView::SetEyeDelta(glm::vec3(right_at[3]) - glm::vec3(left_at[3]), vr->m_engine_frame_count.load());
        }
        hmd_transform      = parent_aim * hmd_transform * eye;
        hmd_transform = to_havok_space(hmd_transform);
        // Remember this eye's offset in world space, for the first-person weapon/body (see
        // onNiAVObjectUpdateWorld).
        if (worldCamera->parent != nullptr) {
            const RE::NiPoint3 eye_parent{hmd_transform[3][0] - head_only[3][0], hmd_transform[3][1] - head_only[3][1],
                                          hmd_transform[3][2] - head_only[3][2]};
            g_eye_offset_world = NiMath::to_ni(NiMath::to_glm(worldCamera->parent->world.rotate) * NiMath::to_glm(eye_parent));
        }
        worldCamera->local.rotate = originalRotation * *(RE::NiMatrix3*) & hmd_transform;
        worldCamera->local.translate.x = hmd_transform[3][0] + crouch_local.x;
        worldCamera->local.translate.y = hmd_transform[3][1] + crouch_local.y;
        worldCamera->local.translate.z = hmd_transform[3][2] + crouch_local.z;
    } else {
        {
            std::scoped_lock _{g_head_local_mtx};
            g_head_local_valid = false;
        }
        auto head_rotation = vr->get_transform(0);
        NoteFrameHead(head_rotation); // (fix144)
        auto standing_origin = vr->get_transform_offset();
        head_rotation[3].x -= standing_origin[3].x;
        head_rotation[3].y -= standing_origin[3].y;
        head_rotation[3].z -= standing_origin[3].z;
        // (fix199) Seated with the game's own body shown: your head's lean from where it was as you sat held softly (to about 30 cm),
        // and the view moved from the game's seat camera to that body's eyes (in the camera's parent's axes, added below). Zero otherwise.
        glm::vec3 seat_head{head_rotation[3]};
        const glm::vec3 seat_offset = SeatView::Update(worldCamera, seat_head, glm::vec3{standing_origin[3]});
        head_rotation[3] = Vector4f{seat_head, 1.0f};
        auto eye = get_camera_eye_transform();
        // (Full stereo: the right eye's distance from the left, as in first person.)
        if (vr->is_native()) {
            const auto left_at  = to_havok_space(head_rotation * eye);
            const auto right_at = to_havok_space(head_rotation * vr->get_eye_transform(VRRuntime::Eye::RIGHT));
            StereoView::SetEyeDelta(glm::vec3(right_at[3]) - glm::vec3(left_at[3]), vr->m_engine_frame_count.load());
        }
        head_rotation = head_rotation * eye;
        head_rotation = to_havok_space(head_rotation);
        head_rotation[3][0] += seat_offset.x; // (fix199)
        head_rotation[3][1] += seat_offset.y;
        head_rotation[3][2] += seat_offset.z;
        head_for_menu = head_rotation;
        worldCamera->local.rotate = originalRotation * *(RE::NiMatrix3 *) &head_rotation;
        worldCamera->local.translate.x = head_rotation[3][0];
        worldCamera->local.translate.y = head_rotation[3][1];
        worldCamera->local.translate.z = head_rotation[3][2];
    }
    MenuCamera::OnHeadPlaced(vr->m_engine_frame_count.load(), head_for_menu, NiMath::to_glm(originalRotation),
        worldCamera->parent != nullptr ? worldCamera->parent->world.scale : 1.0f, worldCamera);
    WatchCameraBehindMenu(worldCamera, true);
}

// For the log, while the world is kept drawn behind a full-screen menu: does the game still update the camera itself, and does the
// camera turn as your head does? (fix112 updated the camera here, believing the paused game no longer did: fix113b's log showed it
// does, every frame. Both numbers are the camera's own, not the picture's: the renderer draws from its own copy of the camera, which
// the game leaves as it was when the menu opened; see MenuCamera.) Said when the menu closes.
void CreationEngineCameraManager::WatchCameraBehindMenu(RE::NiCamera* worldCamera, bool tracking) {
    static int       frames = 0, open_logs = 0, close_logs = 0;
    static uint32_t  game_updates_at = 0;
    static bool      active = false;
    static float     max_turn = 0.0f, max_drawn_turn = 0.0f;
    static glm::mat3 first_rotation{1.0f};
    static glm::vec3 first_forward{0.0f};
    g_world_camera_node.store(worldCamera, std::memory_order_relaxed);
    const auto angle = [](const glm::mat3& a, const glm::mat3& b) {
        const glm::mat3 d = glm::transpose(a) * b;
        return glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
    };
    // (The way the camera looks, from its own matrix: its last row is the view's forward axis, as WorldToScreen uses it.)
    const auto drawn_forward = [&]() {
        const glm::vec3 f{worldCamera->worldToCam[3][0], worldCamera->worldToCam[3][1], worldCamera->worldToCam[3][2]};
        return glm::length(f) > 1e-6f ? glm::normalize(f) : glm::vec3{0.0f};
    };
    if (!tracking || !ModSettings::g_internalSettings.showQuadDisplay || !ModSettings::g_internalSettings.menuLiveWorld || worldCamera->parent == nullptr) {
        if (active && close_logs < 12) {
            ++close_logs;
            spdlog::info("[VR-CAM] behind the menu ({} frames): the game updated the camera itself {} times; the camera turned up to {:.0f} deg "
                         "(its own matrix: {:.0f} deg; the picture is drawn from the renderer's copy, see [MenuCamera])",
                frames, g_world_camera_updates.load() - game_updates_at, max_turn, max_drawn_turn);
        }
        active = false;
        return;
    }
    if (!active) {
        active          = true;
        frames          = 0;
        max_turn = max_drawn_turn = 0.0f;
        game_updates_at = g_world_camera_updates.load();
        first_rotation  = NiMath::to_glm(worldCamera->world.rotate);
        first_forward   = drawn_forward();
        if (open_logs < 12) {
            ++open_logs;
            spdlog::info("[VR-CAM] behind the menu: watching the camera while the menu is open");
        }
    }
    ++frames;
    max_turn = std::max(max_turn, angle(first_rotation, NiMath::to_glm(worldCamera->world.rotate)));
    if (const glm::vec3 f = drawn_forward(); glm::length(f) > 0.5f && glm::length(first_forward) > 0.5f) {
        max_drawn_turn = std::max(max_drawn_turn, glm::degrees(std::acos(std::clamp(glm::dot(f, first_forward), -1.0f, 1.0f))));
    }
}


// (fix163) Frames a snap turn still has to show in (it turns the character; the camera follows from the next frame): not held back in a
// conversation.
static int g_snap_show_frames = 0;

void CreationEngineCameraManager::onFPSGetCameraRotation(RE::FirstPersonState *fps, RE::NiQuaternion *quat_out) {
    static auto instance = CreationEngineCameraManager::Get();
    static auto original_func = instance->m_onGetCameraRotationHook->get_original<decltype(onFPSGetCameraRotation)>();
    original_func(fps, quat_out);
    static auto vr = VR::get();
    if (!vr->is_hmd_active() || ModConstants::cameraShake || ModSettings::cameraFlat() || ModSettings::g_debug.disableHeadTracking) {
        yaw_offset = 0.0f;
        GameFlow::pendingSnapYaw.store(0.0f);
        GameFlow::pendingDialogueYaw.store(0.0f); // (fix173)
        return;
    }
    // (fix143: not on a ladder: the climb's animation turns you to face it; the head turns the view, as in a seat.)
    if (!GameFlow::isImmovable() && !GameFlow::isControlledByAI() && !GameFlow::isOnLadder()) {
        // order of extraction Pitch->Yaw->Roll (Havok X->Z->Y)
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();

        // Snap turn queued by the right stick: turn the character (its heading, clockwise from above).
        // The camera follows from the next frame on.
        float snap_applied = 0.0f; // (fix195: left out of the heading the room is worked out from below; it shows from the next frame)
        if (const float snap = GameFlow::pendingSnapYaw.exchange(0.0f); snap != 0.0f && p_player) {
            const float two_pi = 2.0f * glm::pi<float>();
            p_player->data.angle.z = std::fmod(p_player->data.angle.z + snap + two_pi, two_pi);
            snap_applied           = snap;
            g_snap_show_frames     = 3; // (frames: counted once a frame)
            static int logged = 0;
            if (logged < 5) {
                ++logged;
                spdlog::info("[SnapTurn] turned {:.0f} degrees (engine frame {})", glm::degrees(snap), vr->m_engine_frame_count.load());
            }
        }

        RE::NiMatrix3 havok_rotation;
        quat_out->ToMatrix(havok_rotation);
        float pitch, yaw, roll;
        havok_rotation.ToEulerAnglesXYZ(pitch, roll, yaw);

        // (fix145) Back from a ladder, where the view was at the body's head turned as the room was (LadderClimb): the climb turned the
        // body (the game's heading, this yaw) to the ladder and off it meanwhile; that turn taken into the offset, so the room's turn
        // goes on as it was and the view does not jump as first person comes back.
        {
            static float    last_raw_yaw   = 0.0f;
            static bool     have_raw       = false;
            static uint32_t ladder_seen    = 0;
            static int      ladder_logs    = 0;
            static auto     last_call      = std::chrono::steady_clock::time_point{};
            const auto      call_now       = std::chrono::steady_clock::now();
            const uint32_t  ladder_frames  = LadderClimb::ViewFrames();
            // (Only after a while without first person: the view was at the head meanwhile, not a frame of it beside first person.)
            const bool      after_gap      = call_now - last_call > std::chrono::milliseconds(100);
            last_call                      = call_now;
            if (ladder_frames != ladder_seen) {
                ladder_seen = ladder_frames;
                if (have_raw && after_gap && GameFlow::gStore.internalSettings.pawnControl) {
                    const float two_pi = 2.0f * glm::pi<float>();
                    const auto  wrap   = [&](float a) {
                        a = std::fmod(a + glm::pi<float>(), two_pi);
                        return (a < 0.0f ? a + two_pi : a) - glm::pi<float>();
                    };
                    const float turned = wrap(yaw - last_raw_yaw);
                    // (fix150) Less what the ladder's view turned the room by meanwhile (to face the ladder, the right stick), measured as this
                    // yaw is: the room goes on as the ladder left it.
                    float          room_turned = 0.0f;
                    RE::NiQuaternion at_start{};
                    if (const auto room_now = tracking::RoomQuaternion(); room_now && LadderClimb::RoomAtLadderStart(at_start)) {
                        const auto yaw_of = [](const RE::NiQuaternion& q) {
                            RE::NiMatrix3 m{};
                            q.ToMatrix(m);
                            float p = 0.0f, r = 0.0f, y = 0.0f;
                            m.ToEulerAnglesXYZ(p, r, y);
                            return y;
                        };
                        room_turned = wrap(yaw_of(*room_now) - yaw_of(at_start));
                        if (!std::isfinite(room_turned)) {
                            room_turned = 0.0f;
                        }
                    }
                    yaw_offset += turned - room_turned;
                    if (ladder_logs < 10) {
                        ++ladder_logs;
                        spdlog::info("[Ladder] first person again: the climb turned the body {:.1f} degrees, the ladder's view the room {:.1f}; the "
                                     "view goes on as it was",
                            glm::degrees(turned), glm::degrees(room_turned));
                    }
                }
            }
            last_raw_yaw = yaw;
            have_raw     = true;
        }

        auto current_hmd_rotation = vr->get_rotation(0);
        auto rotation_quat = glm::normalize(glm::quat_cast(to_havok_space(current_hmd_rotation)));
        auto ni_hmd_rotation = RE::NiQuaternion(rotation_quat.w, rotation_quat.x, rotation_quat.y, rotation_quat.z);
        // The camera's parent turns with the aim: the headset, or the right controller when aiming with
        // it (the view itself stays on the head, see UpdateWorldCamera).
        const auto aim_rotation = tracking::AimRotation();
        const auto aim_quat = glm::normalize(glm::quat_cast(to_havok_space(aim_rotation)));
        const auto ni_aim_rotation = RE::NiQuaternion(aim_quat.w, aim_quat.x, aim_quat.y, aim_quat.z);
        {
            if (GameFlow::gStore.internalSettings.pawnControl) {
                // (fix159) The heading's step worked out from level rotations (the game's yaw alone, without its pitch and roll): the game's
                // camera carries a fixed tilt (72.9 degrees with its look angle 0, LevelView's log), and with it in, the step's yaw from the
                // full difference rotation came out wrong once the head was turned more than about 18 degrees from the room's forward:
                // the heading spun about a turn every 5 frames and the body spun with it (turning the head still made the body
                // glitch; a helper's research, simulated from the log's traced frames). Without a tilt this is the same as before.
                // (fix163) In a conversation the room's turn held where it was: the game's own heading changes (its conversation camera turns
                // with the mouse, which the menu pointer moves: with the hands near the middle of the screen, around where
                // the npc's text showed in a conversation, the screen shook slightly; a helper's research) taken into the offset, so
                // only your head turns the view, as in Skyrim VR. The mod's own corrections keep yaw - yaw_offset as it was anyway; a snap turn
                // (seen from the next frame on) let through. Logged each second with how much was held back.
                {
                    static float   room_last    = 0.0f;
                    static bool    room_have    = false;
                    static float   held_back    = 0.0f;
                    static int64_t held_logged  = 0;
                    static int     held_logs    = 0;
                    static auto    held_frame   = vr->m_engine_frame_count.load() - 1;
                    static int64_t last_call_ms = 0;
                    const int64_t  now_ms       = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    // (Once a frame: the hook may run again in the same frame before the game has its heading from this one, and that call
                    // would take the mod's own correction for the game's; and anew after a gap (third person, a ladder, AI control); the review.)
                    if (now_ms - last_call_ms > 100) {
                        room_have = false;
                    }
                    last_call_ms           = now_ms;
                    const auto  frame      = vr->m_engine_frame_count.load();
                    const bool  first_call = frame != held_frame;
                    held_frame             = frame;
                    const float pi    = glm::pi<float>();
                    const float room  = yaw - yaw_offset;
                    const bool  hold  = first_call && GameFlow::isDialogueDrawnNow() && g_snap_show_frames == 0;
                    if (hold && room_have) {
                        float d = std::fmod(room - room_last + 3.0f * pi, 2.0f * pi) - pi;
                        if (std::abs(d) > 1e-5f && std::abs(d) < glm::radians(45.0f)) {
                            yaw_offset += d;
                            held_back += std::abs(d);
                        }
                    }
                    // (fix173) The right stick's turn in a conversation (ModConfig: smooth at the
                    // smooth turn's speed, whatever the turn mode). The game's own heading change is held back just above, so the stick's turn through
                    // the game was undone (only a snap's three frames passed); the room is turned here instead, as the ladder's view turns it
                    // (pendingLadderYaw). A turn to the right lowers the room's yaw, as a snap turn's heading change does from the next frame (the
                    // heading and this yaw run opposite ways: the correction below takes from one what it adds to the other). Taken into room_last
                    // below, so the hold keeps the new turn; the correction then turns the game's heading after the head as always. The conversation's
                    // panel turns with the room (ConversationPanel).
                    {
                        static float   stick_turned  = 0.0f;
                        static int64_t stick_logged  = 0;
                        static int     stick_logs    = 0;
                        if (first_call) {
                            const float stick = GameFlow::pendingDialogueYaw.exchange(0.0f);
                            if (stick != 0.0f && std::isfinite(stick) && std::abs(stick) < glm::radians(45.0f) && GameFlow::isDialogueDrawnNow()) {
                                yaw_offset += stick;
                                stick_turned += stick;
                            }
                        }
                        if (std::abs(stick_turned) > 1e-4f && now_ms - stick_logged >= 1000 && stick_logs < 30) {
                            ++stick_logs;
                            stick_logged = now_ms;
                            spdlog::info("[Dialogue] (fix173) the right stick turned you {:.1f} degrees in the conversation (the room's turn)", glm::degrees(stick_turned));
                            stick_turned = 0.0f;
                        }
                    }
                    if (first_call) {
                        g_snap_show_frames = std::max(g_snap_show_frames - 1, 0);
                        room_last          = yaw - yaw_offset;
                        room_have          = true;
                    }
                    if (hold && held_back > 0.0f && now_ms - held_logged >= 1000 && held_logs < 30) {
                        ++held_logs;
                        held_logged = now_ms;
                        spdlog::info("[Dialogue] (fix163) the game's own turn held back in the conversation: {:.2f} degrees in the last second",
                            glm::degrees(held_back));
                        held_back = 0.0f;
                    }
                }
                const float game_yaw = yaw;
                yaw -= yaw_offset;
                RE::NiMatrix3 level{};
                level.FromEulerAnglesXYZ(0.0f, 0.0f, yaw);
                auto new_quat = RE::NiQuaternion(level);

                new_quat = new_quat * ni_hmd_rotation;

                level.FromEulerAnglesXYZ(0.0f, 0.0f, game_yaw);
                const auto game_level = RE::NiQuaternion(level);
                auto       delta      = game_level.InvertVector() * new_quat;
                delta.ToMatrix(havok_rotation);
                float delta_pitch, delta_yaw, delta_roll;
                havok_rotation.ToEulerAnglesXYZ(delta_pitch, delta_roll, delta_yaw);
                yaw_offset += delta_yaw;
                auto corrected_yaw = p_player->data.angle.z - delta_yaw + 2.0f * glm::pi<float>();
                corrected_yaw = std::fmod(corrected_yaw, 2.0f * glm::pi<float>());
                p_player->data.angle.z = corrected_yaw;
            }
            if (GameFlow::gStore.internalSettings.decoupledPitch && !((ModConstants::headTrackingType == 0 && GameFlow::isAimingDownSights()) || ModConstants::headTrackingType == 2)) {
                pitch = 0.0f;
            }
            // Level view (on foot, first person): the game's own look kept level, so the view is your head's alone, whatever the
            // right stick's up and down did (fix128: it tilted the view away from your head, and a menu, put level in front of you,
            // sat below where you then looked). Its look angle levelled too each frame, so it stays so and the game aims as you
            // see (as snap turning sets its heading). The stick itself still reaches the game: menus scroll and turn with it.
            // (fix143) Also in the menus opened from what you use in the world (a container, trading, a workbench): opening a
            // container moved the view down to the container; the game's look took over as such a menu opened, the level view
            // stopping for every full-screen menu. Logged as each opens (how far the game had tilted it, its camera's state).
            // (fix160: whether a menu is open read once here: it depends on the time, and two reads could fall either side of its change, the
            // frame then levelled by neither block; the review.)
            const bool menu_open  = GameFlow::isShowingMenu();
            const bool keep_menu  = menu_open && GameFlow::menusKeepLevelView();
            const bool world_menu = GameFlow::gStore.internalSettings.levelViewMenus && keep_menu;
            {
                static bool was_world_menu = false;
                static int  world_logs     = 0;
                if (world_menu && !was_world_menu && world_logs < 20 && p_player != nullptr) {
                    ++world_logs;
                    spdlog::info("[LevelView] a container, trading or workbench menu opened: the game's look tilted {:.1f} degrees (its look angle "
                                 "{:.1f}), its camera in state {} ({}): kept level",
                        glm::degrees(pitch), glm::degrees(p_player->data.angle.x), WorkbenchCamera::CameraStateIndex(),
                        GameFlow::isInFirstPerson() ? "first person" : "not first person");
                }
                was_world_menu = world_menu;
            }
            const bool level_on_foot =
                GameFlow::gStore.internalSettings.levelView && p_player != nullptr && GameFlow::isInFirstPerson() && !GameFlow::isPilotingShip();
            // (fix160) In the other full-screen menus too (the pause menu, the Data menu and its tabs, the inventory): the room's turn
            // level, the game's look angle left as it is (nothing in the paused game changed). The game's own look carries a fixed tilt
            // (72.9 degrees here at every load, its look angle 0), and the world kept drawn behind a see-through menu is drawn from this
            // camera: unlevelled it looked up at the sky: at every pause the pause screen's background showed the view looking up
            // instead of level as before. (fix159's hold, a menu counted from its first frames, left none of its opening frames
            // levelled; before, about half the menus froze level by chance; a helper's research.)
            if (level_on_foot && menu_open && !keep_menu) { // (the container and workbench menus by their own setting: the review)
                static int menu_logs = 0;
                if (std::abs(pitch) > glm::radians(0.5f) && menu_logs < 6) {
                    ++menu_logs;
                    spdlog::info("[LevelView] (fix160) a menu: the game's look tilted {:.1f} degrees (its look angle {:.1f}): the room kept level, "
                                 "the look angle left",
                        glm::degrees(pitch), glm::degrees(p_player->data.angle.x));
                }
                pitch = 0.0f;
            }
            if (level_on_foot && (!menu_open || world_menu)) {
                static int levelled_logs = 0;
                if (std::abs(pitch) > glm::radians(0.5f) && levelled_logs < 6) {
                    ++levelled_logs;
                    spdlog::info("[LevelView] the game's look was tilted {:.1f} degrees (its look angle {:.1f}): levelled", glm::degrees(pitch),
                        glm::degrees(p_player->data.angle.x));
                }
                p_player->data.angle.x = 0.0f;
                pitch                  = 0.0f;
            }
            // (fix195) Measured only: does the game ever roll its camera here (a stagger, a hit, sprinting)? The room, and with it the
            // view and the hands, would then roll too. Counted in first person on foot, told every 10 s while it happens (20 times).
            {
                static int     rolled = 0, frames = 0, roll_logs = 0;
                static float   roll_max = 0.0f;
                static int64_t roll_at  = 0;
                const int64_t  roll_now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (GameFlow::isInFirstPerson() && !menu_open) {
                    ++frames;
                    if (std::abs(roll) > glm::radians(0.5f) && std::isfinite(roll)) {
                        ++rolled;
                        roll_max = std::max(roll_max, std::abs(roll));
                    }
                }
                if (roll_now - roll_at > 10'000) {
                    if (rolled > 0 && roll_logs < 20) {
                        ++roll_logs;
                        spdlog::info("[LevelView] (fix195) the game's camera was rolled (over 0.5 degrees) in {} of {} frames in first person, at "
                                     "most {:.1f} degrees, the last 10 s: the room (the view and the hands) rolled with it",
                            rolled, frames, glm::degrees(roll_max));
                    }
                    rolled = frames = 0;
                    roll_max        = 0.0f;
                    roll_at         = roll_now;
                }
            }
            havok_rotation.FromEulerAnglesXYZ(pitch, roll, yaw);
            const auto room_rotation = RE::NiQuaternion(havok_rotation);
            tracking::SetRoomRotation(room_rotation);
            // (fix195) The heading the room was just worked out from (the snap turn above shows from the next frame): the body's hands are
            // placed before this hook next frame, with this room turned on by your heading's change since (tracking::RoomRotationNow).
            // (Not carried on in a conversation, where the game's heading change is held back into the yaw offset (fix163, fix173) so the room
            // does not turn with the heading, nor after a snap turn, which shows from the next frame as before; the reviews.)
            if (p_player != nullptr) {
                tracking::NoteRoomHeading(p_player->data.angle.z - snap_applied, snap_applied == 0.0f && !GameFlow::isDialogueDrawnNow());
            }
            *quat_out = room_rotation * ni_aim_rotation;
            tracking::RecordAppliedAim(aim_rotation, (int)vr->m_engine_frame_count.load());
            if (vr->tracing()) {
                spdlog::info("[VR-TRACE] fps rotation: game yaw {:.1f}, yaw offset {:.1f}, player angle {:.1f}, eye {} frame {} tid {}",
                    glm::degrees(yaw), glm::degrees(yaw_offset), glm::degrees(p_player->data.angle.z),
                    vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? "L" : "R", vr->m_engine_frame_count.load(), GetCurrentThreadId());
            }
        }
    } else {
        yaw_offset = 0.0f;
        GameFlow::pendingSnapYaw.store(0.0f);
        GameFlow::pendingDialogueYaw.store(0.0f); // (fix173)
    }
}