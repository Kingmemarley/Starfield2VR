#pragma once

#include <glm/glm.hpp>

namespace RE
{
    class NiCamera;
}

// (fix199) Sitting in a chair or a seat the game shows with your own third-person body (most chairs, the pilot's seat of a landed ship):
// the view starts at that body's eyes, and your head's lean away from where you sat is softly held to about 30 cm.
//
// In such a seat the game shows its third-person body in its sitting animation and takes the first-person camera from a camera node in
// that body's skeleton (Starfield 1.16.236: 0x141A41950 sets the "third person" mark [player+0x112A] bit 3, shows the third-person body
// and hides the first-person one, then 0x141E8E3F0 -> 0x141E88300 points FirstPersonState+0xA0 at the camera node under the shown body).
// That node is placed for the flat screen, in front of the face (the game does not hide the head there): in the headset the body
// looked a little too far back. And the mod's own body is off in seats, so the seated body never follows your head: leaning far
// forward left it behind.
//
// Here: a moment after sitting down, the gap between the game's seat camera and the seated body's eyes (its head bone, as on a ladder) is
// measured once, in the body's own frame (ahead, to the right, up), and the view (with the hands' place in the world) is moved by it,
// eased in, until you get up (eased out). Your head's level lean from where it was as you sat is followed fully to 15 cm, then held
// softly, never past 30 cm. Not in flight (the ship moves) or for 3 s after it, not lying down.
namespace SeatView
{
    // (Any thread: BodyIK's model update) The seated body's head bone is wanted this frame (measuring).
    bool HeadWanted();
    // (Any thread: BodyIK's visibility) The view is at the seated body's eyes: its face is hidden meanwhile.
    bool FaceHidden();
    // (Any thread: BodyIK's model update) As FaceHidden, and the game's camera does not hang under the head bone: the whole head is
    // shrunk for the update as on a ladder (hair, a hood or a helmet with it).
    bool HeadScaleWanted();
    // (Any thread) How far the view is moved in the game's world this frame (to the seated body's eyes, eased): the hands, rays and held
    // things are placed from the moved view with it (tracking::StageToWorld, WorldToStage). Zero when off.
    glm::vec3 WorldShift();
    // (The main thread: UpdateWorldCamera, the seat branch.) `head` is the headset's place less `origin` (the room's axes, metres, before
    // the eye's own offset): its level lean softly limited in place while seated. Returns what to add to the world camera's local place
    // (in its parent's axes) this frame; zero when not seated or not measured.
    glm::vec3 Update(RE::NiCamera* world_camera, glm::vec3& head, const glm::vec3& origin);
    // (The main thread: UpdateWorldCamera, first person on foot.) Just up from a seat: the view's move eased out here too. Returns it in
    // the game's world (zero once off).
    glm::vec3 EaseOut();
    // (The main thread) No headset, the flat screen, head tracking off or camera shake: everything off at once.
    void Off();
}
