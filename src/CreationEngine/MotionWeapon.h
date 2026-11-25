#pragma once

#include <glm/glm.hpp>

namespace RE {
    class NiAVObject;
    class NiCamera;
}

// Puts the first-person "Weapon" node (the gun and the hand IK targets under it) on the right
// controller. The node's local transform is replaced just before the game updates it, so the
// gun parts under it (and the arms, which reach for the hand IK targets) follow; it is restored
// afterwards so the animation keeps owning it.
//
// Settings live in ModConstants (CreationEngineConstants.h) and are edited under Quick tests.
namespace MotionWeapon {
    // Game thread, start of frame (UpdateWorldCamera, first-person branch). head_local is the
    // headset position (without the eye offset) in the world camera's parent frame.
    void on_frame_start(RE::NiCamera* world_camera, const glm::vec3& head_local);

    // Called from the scene update hook.
    bool is_weapon_node(RE::NiAVObject* obj);
    void before_update(RE::NiAVObject* weapon);
    void after_update(RE::NiAVObject* weapon);

    // Write every transform used for the placement to the log on the next frame.
    void request_report();

    // Hook the game's animation step for the first-person skeleton (AnimationManager vtable slot 7)
    // and put the controller-driven gun transform back just before it runs, like the reference mod.
    void install_animation_hook();

    // After a stereo screenshot (or when requested), log every part of the gun with its flags and
    // where it should appear in the eye's image. Game thread, after the cameras are placed.
    void request_piece_report(int frames);
    void report_pieces_if_requested(RE::NiCamera* world_camera);
}
