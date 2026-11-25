#pragma once

#include <cstdint>
#include <string>

// (fix143, fix145) Ladders climbed in VR (grabbing a ladder glitched the game: third person, and nothing could be done; now it
// stays in first person and the ladder is climbed as in VR).
//
// Starfield (1.16.236): a ladder is a traversal on the ladder's reference. Activating it, the player's activate (0x141A38FB0) first
// turns the camera to its third-person state (0x141A390C0: SetState(20)) and then queues the activation, whose climb start
// (0x141A32870) needs your feet within 0.5 m in height and 2 m sideways of the ladder's end and plays its animation on the body that
// is shown: there are only third-person ladder animations (Ladder.agx), so in first person it is refused (fix145's research: the
// graph is chosen by [player+0x112A] bit 8, "in third person", which the third-person state's Begin sets; fix144 skipped the switch:
// 20 tries in its test, no climb). The climb (PlayerTraversalState, at player+0x10A8) moves you rung by rung while the left stick is
// pushed past half forward (up) or back (down), and gets off by itself past an end, where the game puts the camera back (0x141E91BD0).
//
// Here (fix145): the switch to third person is left to the game, so the climb is the game's own; while it lasts (and a second after
// the activation, for the climb to start) the third-person camera is put at the body's eyes (its head bone, eased) turned as the room
// was in first person, the headset's own pose on top as in play: you see from your body's eyes, the head hidden. A climb that does not
// start within a second (or one that ends and leaves the camera in third person) puts the camera back as the game does at a climb's
// end. While climbing, a hand squeezing its grip and pulling down climbs (the stick pushed up for the game), pushing up climbs down, a
// still hand stops; the left grip no longer turns the left stick into a D-pad, the hand grab is off, the body's own climbing arms show.
namespace RE
{
    class NiAVObject;
    class NiQuaternion;
}

namespace LadderClimb
{
    // Checks the game's code and hooks the turn to third person and the third-person camera's place (the mod's start).
    void Install();
    // (fix148, the game's thread: every node's world update, before the game's) The camera root the third-person camera writes this
    // frame, while the view is at the body's eyes: its place put at the eyes (its children, the world camera, follow it).
    void BeforeNodeUpdate(RE::NiAVObject* obj);
    // (The game's thread, each frame: tracking::Snapshot.) The climb's start and end, the camera put back, and the hands' pull.
    void Update();
    // (fix145, any thread) The view is at the body's head now (a ladder, the camera in its third-person state): the face is hidden.
    bool ViewAtHead();
    // (fix145) How many times the view was put at the head so far (any thread): the first-person camera, seeing it changed since its
    // last frame, keeps the room's turn as it was (no jump as first person comes back).
    uint32_t ViewFrames();
    // (fix146, any thread) A ladder was activated in the last 1.5 s and its climb has not started yet: the game's turn to third person for
    // it is being made (the shown body's bodies put as in first person for it, BodyIK).
    bool ActivatedRecently();
    // (fix148, any thread) The view is at the body's head this frame (a flag set by Update): the body's head bone read as the skeleton
    // updates (BodyIK).
    bool HeadWanted();
    // (fix150) The room's turn as the ladder's view last began (before it turned to face the ladder, or with the right stick): first
    // person goes on from the room's turn as the ladder left it (CreationEngineCameraManager). False: none yet.
    bool RoomAtLadderStart(RE::NiQuaternion& out);
    // The left stick's up and down for the game now (-1 down, 0 none, 1 up), from the hands' pull while climbing.
    int StickY();
    std::string Status();
}
