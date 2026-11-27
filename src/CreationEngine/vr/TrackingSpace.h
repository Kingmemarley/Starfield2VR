#pragma once

#include <optional>

#include <glm/glm.hpp>
#include <RE/N/NiQuaternion.h>

namespace RE
{
    class NiAVObject;
}

// Tracked poses sampled once per engine frame and the mapping from OpenXR stage space into the game.
// Ported from SFVR (AetherianArtificer's fork of starfield2vr).
namespace tracking
{
    // Samples the aim rotation and the controller poses for this engine frame.
    void Snapshot();

    // True when this frame's aim follows the right controller instead of the headset.
    bool AimingWithHand();

    // Hand interaction is on this frame (weapon put away, in first person, no menu): the game's
    // interaction pick is made around the right hand (PointInteract).
    bool HandInteract();
    // A controller's pointing ray in game world space (starts just in front of the controller); right by default.
    bool HandRayWorld(glm::vec3& origin, glm::vec3& direction, bool left = false);
    // The gravity gloves' pointing line (stage space, or game world): `line` 0 the hand's (the controller's pointing ray,
    // which the drawn hand's fingers follow: as HIGGS points), 1 out of the palm (as ROCK and Half-Life: Alyx), 2 sighting
    // along the hand (from the eyes through the controller, on from it).
    constexpr int kGloveLines = 3;
    bool GloveRayStage(int line, bool left, glm::vec3& origin, glm::vec3& direction);
    bool GloveRayWorld(int line, bool left, glm::vec3& origin, glm::vec3& direction);
    // A stage-space point and direction in game world space, with the same mapping as the shown body's
    // hands. world_scale (optional) receives game units per tracked metre.
    bool StageToWorld(const glm::vec3& stage_point, const glm::vec3& stage_direction, glm::vec3& world_point, glm::vec3& world_direction,
        float* world_scale = nullptr);

    // Aim rotation (stage space) shared by the view, the aim and the meshes.
    glm::mat4 AimRotation();
    // The aim rotation currently baked into the game camera's parent transform.
    glm::mat4 AppliedAimRotation();
    void      RecordAppliedAim(const glm::mat4& rotation, int frame);
    // The rotation the camera's parent will carry this frame. If the game stopped updating its camera
    // (paused by a menu or popup), it still carries the last applied one.
    glm::mat4 CameraParentAimRotation(int frame);

    glm::mat4 RightHandPose();
    // Grip (palm) and aim poses of a controller, stage space.
    glm::mat4 GripPose(bool left);
    glm::mat4 AimPose(bool left);
    // The headset's pose, stage space, sampled with the hands.
    glm::mat4 HeadPose();
    // A game world direction in stage space (the inverse of StageToWorld's direction mapping).
    bool WorldToStageDirection(const glm::vec3& world_direction, glm::vec3& stage_direction);
    // A game world point in stage space (the inverse of StageToWorld's point mapping).
    bool WorldToStage(const glm::vec3& world_point, glm::vec3& stage_point);
    // (fix147, fix148) Where the camera's root (the world camera's parent) is this frame, game world: in first person its place as the
    // game last worked it out, moved on by your character's move since (the root itself is put only later in the frame, after the body),
    // else the root's own. Its parent's turn and scale are the root's own.
    glm::vec3 CameraRootPlace(const RE::NiAVObject* root);
    // (fix157) For what is placed at the frame's start, before your character moves (the left hand's weapon's copy): how much further the
    // camera root will be put this frame than CameraRootPlace says now (game world; zero when not known). Main thread.
    glm::vec3 FrameStartLead();
    // (fix169) The camera root's place as what is placed at the frame's start takes it (CameraRootPlace + FrameStartLead). Main thread.
    bool FrameStartRoot(glm::vec3& out);
    // (fix148, the game's thread: every node's world update, after the game's) The camera root has worked out its world place: noted, with
    // your character's place then (CameraRootPlace moves it on by your move since).
    void AfterNodeUpdate(const RE::NiAVObject* obj);
    // (fix185) The camera root watched this frame (the world camera's parent, noted at the frame's start; null if none).
    const RE::NiAVObject* WatchedRoot();
    // (fix185) The camera root's place as the game last worked it out and your character's place at that moment (game world); false
    // before the first. For where the game has your eyes from your feet (climbing's view tied to you).
    bool LastRootWrite(glm::vec3& root, glm::vec3& you);
    // (fix195, the game's thread: every node's world update, before the game's; after Climbing's) "Tie the view to the body": in first
    // person on foot the camera root is put exactly where CameraRootPlace told the body (and the gun in its hand) this frame, so the
    // view and the gun come from one place. One pointer compared for every other node.
    void BeforeNodeUpdate(RE::NiAVObject* obj);
    // (fix195) The room's turn as it will be this frame. The camera hook sets the room later in the frame than the body places its
    // hands, so while you turn with the stick the hands were placed with the frame before's turn. This is the frame before's turned on
    // by your character's heading change since, once that has measured closer to the room the hook then sets than the frame before's
    // alone (checked every frame; the frame before's until then). Only from the frame just before's room and for a stick turn's frame (a
    // snap turn shows from the next frame, as before). The room as it is once the hook has set it this frame.
    std::optional<glm::mat3> RoomRotationNow();
    // (fix195, the camera hook, right after SetRoomRotation) The heading the room was just worked out from: your character's (+0x88)
    // as the hook leaves it, less a snap turn the hook added after working out the camera (that one shows from the next frame on).
    // `carry` false: the next frame's room is not carried on (a snap turn this frame, a conversation; the second review).
    void NoteRoomHeading(float heading, bool carry);

    // Room orientation (game world, column-vector) maintained by the camera rotation hook. It stays
    // fixed while only the head turns and changes with snap or game turning.
    void                     SetRoomRotation(const RE::NiQuaternion& q);
    std::optional<glm::mat3> RoomRotation();
    // (fix145) The same, as the camera hook gave it (the game's quaternion): the ladder's view keeps it.
    std::optional<RE::NiQuaternion> RoomQuaternion();

    // Stage-space position recentering was done at.
    glm::vec3 StandingOriginPosition();

    // Scale applied to tracked head and hand movement so the player's eye height maps onto the character's.
    float TrackingScale();
    void  UpdateTrackingScale(float character_eye_height);
    // (fix146) The character's eyes above its feet as last measured (metres; 1.65 until then): the view at the body's eyes on a ladder.
    float CharacterEyeHeight();
    // (fix185) It has been measured at least once (else CharacterEyeHeight is its 1.65 m guess).
    bool CharacterEyeHeightKnown();

    // Stage-space transforms and vectors in the game's havok axes.
    glm::mat4 ToHavokSpace(const glm::mat4& mat);
    glm::vec3 ToHavokVector(const glm::vec3& v);
}
