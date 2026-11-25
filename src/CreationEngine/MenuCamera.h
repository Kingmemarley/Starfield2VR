#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace RE
{
    class NiCamera;
}

// The camera the world is drawn with behind a see-through menu (LiveWorld keeps the world drawn there).
//
// Starfield keeps each camera in a table twice: the game's copy, which the camera's own world update writes every frame (its place
// and turn, 160 bytes per camera), and the renderer's copy, which the renderer reads when it sets up a view for the frame. Behind a
// full-screen menu the world was drawn from where you looked when the menu opened (fix116b's pictures), so in the headset it stuck to
// your face and the menu, rightly fixed in the room, seemed to move the opposite way.
//
// The logs of fix118 and fix118b showed why: in play the world's view is drawn with the head's camera [1]; when a full-screen menu
// opens the game draws it with another camera ([2]), a copy of the head's camera as it was then, which it never moves while the menu
// is open; and it moves the head's camera itself elsewhere (about 4 m away, turned about 160 degrees). So the menu's camera is moved
// here: each frame, as the renderer sets up the world's view behind a see-through menu (its function that reads the view's camera from
// the tables, watched at its start), the menu's camera is turned and moved from where it was as the menu opened by as much as your
// head has turned and moved since, written as the game writes a camera's record.
//
// Only when it is understood: its record laid out as expected, found equal to a record of the head's camera in play (which gives the
// head it was made for), and a self-test in play passed: each frame the game's thread predicts the head's camera's record from the
// head's movement a few frames before, exactly as it is done behind the menu, and checks it against the game's own record. The
// setting "MenuPanelWorldTurns" turns it off.
namespace MenuCamera
{
    // Finds the renderer's camera setup and the camera tables (by their bytes). Once, at start.
    void Install();
    // The game's thread, each frame, once the world camera's local transform is set (UpdateWorldCamera): the head (and the eye drawn)
    // the local transform is made from, as to_havok_space of the headset's pose times the eye's (no aim), the camera's first local
    // turn ("original", glm), its parent's scale, and the camera.
    void OnHeadPlaced(uint32_t frame, const glm::mat4& head, const glm::mat3& original, float parent_scale, const RE::NiCamera* camera);
    // The renderer's view-constant setup of a camera (CreationEngineRendererModule's hook of 0x1429FFFE0, given the camera's record):
    // for the world's camera behind the menu (IsWorldRecord), whether its last frame's camera was kept as its previous one, whether its
    // history was thrown away (by the game; by the mod), and whether afterwards its previous camera is its current one (no camera
    // motion seen by the anti-aliasing). Counted for the log.
    bool IsWorldRecord(uintptr_t record);
    void OnViewConstants(bool copied, bool game_reset, bool mod_reset, bool still);
    std::string Status();
}
