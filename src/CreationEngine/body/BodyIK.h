#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace RE
{
    class NiAVObject;
}

// Third-person body shown in first person with arm, leg and weapon IK.
// Ported from SFVR (AetherianArtificer's fork of starfield2vr).
namespace body
{
    // Hooks BSModelNode::UpdateTransforms, where the IK is applied to the body's pose buffer.
    void InstallModelNodeHook();
    // Called for each object as it updates its world transform; toggles the body at its 3P root.
    void OnUpdateWorld(RE::NiAVObject* obj, int engine_frame);
    // True while the third-person body is shown in place of the first-person arms and gun.
    bool IsActive();
    // (fix146) The third-person body's root (the character's place, game world, at its feet) and the way it faces, as last placed; false
    // without it. For the view at the body's eyes on a ladder (LadderClimb), the game's thread.
    bool ReadRootWorld(float position[3], float forward[3]);
    // (fix148) On a ladder (LadderClimb::HeadWanted): the third-person body's head bone (C_Head) as the skeleton's update put it, game
    // world, within the last 100 ms; false without it.
    bool LadderHeadWorld(float head[3]);
    // (fix199) Seated (SeatView): `under` whether the first-person camera's nodes hang under that head bone (shrinking the head would
    // move the view then); false when the head bone or the camera is not known.
    bool SeatCameraUnderHead(bool& under);
    // (fix146) Hooks the game's switch between first and third person: the bodies put as in first person before a turn to third person
    // (the shown body made the game take it for third person already and skip its animations: no ladder climbs). (fix202) Every turn to
    // third person, not only a ladder's (seats the game plays in third person, such as the character-creation bench).
    void InstallSwitchHook();
    // How far to lower the view below the game's camera while crouching (metres, world up, <= 0).
    float ViewHeightOffset();
    // Muzzle of the weapon held by the shown body, or null when the body is not driving the weapon. (fix189) Null too unless it was found
    // under that weapon this engine frame or the last (the node is not kept alive: an older one may be freed).
    RE::NiAVObject* GetBodyMuzzle();
    // Aim direction (game world) the held weapon was posed along this frame, if any (fix189: false when GetBodyMuzzle is null).
    bool GetBodyAimForward(float out[3]);
    // Where that weapon's muzzle was (game world) at the end of the last frame, if any (any thread).
    bool GetBodyMuzzleWorld(float out[3]);
    // (fix146) Where that muzzle is drawn this frame, and its barrel's way, in the aiming hand's own axes (its controller's aim pose: x
    // right, y along its aim, z up; room metres), as the weapon was last placed in it (any thread; false: no gun placed lately). With the
    // aim pose of any frame it gives the muzzle in the room where the gun is drawn then (the laser's start).
    bool GetMuzzleInAim(float position[3], float direction[3]);
    // (fix146) The held gun's barrel is known (from its muzzle, or from a shot): the gun is drawn along your aim, not on the game's hold
    // estimate (17-21 degrees off in fix131's test, for its first moments).
    bool BarrelKnown();
    // The player fired: the weapon's pose in the first-person rig is sampled as its firing grip.
    void NotifyPlayerFired();
    // (fix206) A continuous beam (the cutter) fired from the gun in your hand this frame: a multi-beam weapon's firing hold is learned
    // only on these frames.
    void NotifyBeamFrame();
    // The left hand holds the weapon's support grip (two-handed) this frame. (fix198: drawn on it, whether or not it steers the gun.)
    bool IsSupportHandOnGun();
    // (fix198) The gun is held in both hands this frame (the left grip squeezed at its support grip, steering it): the left grip is the
    // gun's, so the gamepad's left-grip combos and the favorites wheel stand down. Any thread.
    bool GunHeldWithBothHands();
    // For the node log (NodeTreeDump): the player's first-person rig and third-person model (the body), as read now (either may
    // be null; not checked further: read them with guarded reads).
    void DebugRoots(RE::NiAVObject*& first_person, RE::NiAVObject*& third_person);

    // Where the shown hand holds things, in its controller's aim frame (x right, y up, z back; metres, from the controller's
    // grip point): the palm's surface at the base of the fingers (contact), the way the palm faces, the way the fingers
    // point, and the knuckle line (index to little finger). The body places each hand so the middle of its palm is on the
    // grip point with the fingers along the aim, so this is fixed; it is measured once from the body's own hand.
    struct HandGripFrame
    {
        float contact[3]{};
        float facing[3]{};
        float fingers[3]{};
        float knuckles[3]{};
    };
    // False until measured (or with the body never shown).
    bool GetHandGripFrame(bool left, HandGripFrame& out);
    // (fix169) The left hand's weapon's copy was just placed in your left hand (GrenadeHand, the main thread, at the frame's start) taking
    // the camera root to be at `root` (tracking::FrameStartRoot): the body's left hand is put on it this frame from the same place, so the
    // fist closes on its hilt.
    void NoteLeftWeaponRoot(const glm::vec3& root);
    // (fix158) The right hand's one-handed melee weapon's blade's way in the right controller's aim frame (its tracked axes), as last placed
    // (within 0.2 s): its hits go along it (ImpactMelee). False: none.
    bool RightBladeLocal(glm::vec3& way);

    // (fix173) A standing person held at your hand (Physical's hold): the held part bent toward your hand in that person's own model update
    // (BSModelNode::UpdateTransforms, where the body's own arms are posed): an arm by two-bone IK (shoulder, elbow, wrist), the chest, neck
    // or head by a small lean; the reach and lean capped. `model` their 3D's model node (3D root +0x180); `bone` the held bone's name;
    // `target` where the held point should come to (game world); `held_local` the held point in the bone's own frame (its scale
    // removed); `weight` 0..1 (eased in and out by the caller). Their bones are put back on their next update after it ends. Any thread.
    struct HeldPersonBend
    {
        uintptr_t model{ 0 };
        char      bone[24]{};
        float     target[3]{};
        float     held_local[3]{};
        float     weight{ 0.0f };
    };
    void SetHeldPersonBend(const HeldPersonBend& bend);
    void EndHeldPersonBend();
    // For the log: the updates of the held model seen and bent since the hold began, and the last reach asked (m) and given.
    void HeldPersonBendCounts(int& seen, int& bent, float& asked, float& given);
    // (fix182) A held forearm's or hand's elbow since the last call: its angle as animated and as bent, on average (degrees; 0 a straight
    // arm), over that many updates. False: no such updates. Any thread.
    bool HeldPersonElbow(float& animated_deg, float& bent_deg, int& updates);
}
