#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

// (fix172) Physical interaction with people, as PLANCK and HIGGS do it in Skyrim VR (npcs grabbed
// with the hands, the physical interactions as the PLANCK Skyrim VR mod's; chosen: stages 0-2 of the
// research's plan, no active ragdoll). Starfield 1.16.236; everything is done with the game's own functions, each checked against the
// game's code before it is used (byte checks; a part whose code is not as expected stays off with a log line).
//
// Stage 0, a probe (logging only): every 10 s the bodies of your physics world near you are counted by layer (living people's ragdolls
// 8, corpses 32, people without a controller 33, character controllers 30) and kind (static, moving, moved by animation), the people
// near you are listed with their life and knock states and where their ragdoll's bodies are, and the time the game spends in its physics
// update each frame (the world update, rva 2354360, timed around the game's own call).
//
// Stage 1, bodies: the hand grab (the game's own grab) holds a corpse, or a person knocked down, by the bone the hand touched:
// - The game's grab takes actors already (dead ones; the living only with its own setting bAllowHavokGrabTheLiving): the hand grab now
//   holds them as bodies, not as boxes (no turning to sit in the hand, no box to rest on the palm), with softer springs than for
//   objects and a force cap (so a ragdoll does not stretch or fly), as HIGGS holds a ragdoll with force-limited motors.
// - A person knocked down (alive): the game's grab-the-living setting is turned on only around the hand grab's own start of that grab.
// - Let go by itself when the bone you hold ends up 0.7 m from your hand (HIGGS's auto-drop), when the person gets up, and at once
//   when a menu opens, the game saves or a loading screen comes. While held, a person knocked down stays down (up to 30 s).
// - The gravity gloves do not pull bodies (a ragdoll flown by one limb stretches; not in this build).
// Stage 2, using only the game's own reactions:
// - Weapons put away, an open hand pushed into someone (faster than 1.2 m/s): a light push is the game's "bumped into" reaction, a
//   firm one (2.5 m/s) a stagger, harder (4 m/s) a bigger stagger: the game's own stagger function (rva 19038a0, as its hits call it).
// - Fists out, a punch past the knockdown speed that hits someone (impact melee's hit, the game's damage): they are knocked down
//   (PushActorAway's code, rva 18c19a0, force from the punch's speed); the game gets them up.
// - Gripping a standing person (weapons put away): knocked down in place (force 0), then held as above once they are down. (fix173:
//   held standing instead, setting HoldPeople: their touched arm bent toward your hand (or their body leaned), dragged where you pull;
//   the knockdown only with HoldPeople off and GrabKnocksDown on. Shoves from 1.0 m/s, staggers from 1.5 and 2.5, a refused bump a
//   stagger.)
// - Blocking: an NPC's melee hit at you whose path your blade is in at the hit moment (the right hand's melee weapon, the left hand's
//   weapon) misses (the game's melee hit, which impact melee already hooks, is made to sweep nothing), the attacker recoils (the game's
//   "recoil" action) and the blocking hand buzzes.
// - Anger (PLANCK's): shoves and knockdowns are not hits, so the game does not count them as an assault; someone shoved or knocked
//   down again and again (6 points within a minute: a shove 1, a grip's knockdown 2) raises the game's assault alarm (as script
//   SendAssaultAlarm). Punches are the game's own hits: the game's own crime and combat rules apply to them.
// - Followers (your teammates), commanded people, children, ghosts, people in a scene, sitting, talking, restrained or unconscious
//   are never staggered, knocked down or made angry (a light bump only); your hand hits never hurt a follower (the setting).
namespace Physical
{
    // Finds and checks the game's functions (at start).
    void Install();
    // Once per engine frame (main thread, tracking::Snapshot, after GrenadeHand::Update): the probe, the held body's checks, the
    // shoves, the queued reactions.
    void Update();

    // ---- For the hand grab ----
    // The game's grab start is about to make its springs for `ref` (HandGrab's hook in it; main thread). True: it is a person (alive
    // or dead), held as a body. `by_hand`: the hand grab started it.
    bool OnGrabTaken(uintptr_t ref, bool by_hand);
    // The grab now is of a body (from OnGrabTaken until another grab starts).
    bool HoldingBody();
    // The springs' strength for a body: from the game's own values (force, elasticity, damping) to the ones to bring it to.
    void BodySpring(float game_force, float game_elastic, float game_damping, float& force, float& elastic, float& damping);
    // Around the hand grab's own start of a grab by a grip (main thread): `handle` is the pick's target. A standing person is knocked down
    // instead (Handled(): no grab now; GrabAgain comes once they are down); a person down is grabbed with the game's grab-the-living
    // setting on for this one call (put back when this ends).
    class GripGrab
    {
    public:
        GripGrab(uint32_t handle, int hand);
        ~GripGrab();
        GripGrab(const GripGrab&)            = delete;
        GripGrab& operator=(const GripGrab&) = delete;
        bool Handled() const { return m_handled; }

    private:
        bool m_handled{ false };
        bool m_living{ false };
    };
    // The gravity gloves' target (its handle): true when it must not be pulled (a person). Main thread.
    bool RefusePull(uint32_t handle);
    // (fix173) That hand holds a standing person now (Physical's hold: no knockdown since fix173, setting HoldPeople; their held part
    // bent toward your hand, dragged where you pull). Any thread.
    bool HoldingPerson(bool left);
    // (fix179, for Climbing; main thread) That hand's grip has a person on the way (a seated one being got up, a knocked-down one to be
    // grabbed once down): its squeeze is not a climb.
    bool PersonPending(bool left);
    // (fix174) Where that hand's held person is held (on their surface, as their bones were last drawn; game world), for the drawn hand to
    // be put on it (BodyIK). False: none (or not seen in the last 0.2 s). Any thread. (fix177) With the held part's line under that point
    // (`centre` on it, `axis` along it), its thickness (`radius`, m), the hold's ease-in (`weight`) and a number for the hold (`grab`): the
    // hand's palm turned to it and its fingers closed round it, as round a held object.
    // (fix179) The held point is now on their outer surface: the game's own body capsule for that part (its ragdoll's, read from the
    // game or, failing that, from skeleton.nif's numbers), made bigger by the clothes they wear there. `normal` the way out of them at
    // it; `radius` that surface's distance from the part's line; `flat` a part of the trunk (the fingers lie flat on it); `caps` the
    // held part's surface and up to three near it (game world: from `cap_a` to `cap_b`, `cap_r` thick), for the drawn hand to be kept
    // outside all of them.
    struct HeldPersonGrip
    {
        glm::vec3 at{ 0.0f };
        glm::vec3 centre{ 0.0f };
        glm::vec3 axis{ 0.0f, 0.0f, 1.0f };
        float     radius{ 0.05f };
        float     weight{ 0.0f };
        int       grab{ 0 };
        glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
        bool      flat{ false };
        int       caps{ 0 };
        glm::vec3 cap_a[4]{};
        glm::vec3 cap_b[4]{};
        float     cap_r[4]{};
        // (fix180) The held bone as last drawn (game world: a point x of its frame is at bone_t + bone_r * (x * bone_s); moved on as the
        // held point was for the frame it is late), for the drawn hand to be kept in its frame (locked to the grabbed spot).
        glm::vec3 bone_t{ 0.0f };
        glm::mat3 bone_r{ 1.0f };
        float     bone_s{ 1.0f };
        // (fix181) Their model (the 3D root's model node, as the bend is sent for it) and the held part's bare body: its capsule's
        // thickness without the clothes (m; its line is cap_a[0] to cap_b[0]).
        uintptr_t model{ 0 };
        float     bare_r{ 0.05f };
        char      bone[24]{}; // (fix181, the review) the held bone's name (as their bend is sent for it)
    };
    bool HeldPersonContact(bool left, HeldPersonGrip& out);

    // ---- For impact melee ----
    // An instant hit struck a person (main thread): which hand, whom, the swing's peak speed (m/s) and its way (stage space).
    void OnHandHit(int hand, uintptr_t target, float speed, const glm::vec3& stage_direction);
    // The hit must not be applied to this person (a follower; the setting). Main thread.
    bool SpareTarget(uintptr_t target);
    // An NPC's melee hit is about to sweep (the hook's thread): the attacker, the sweep's start and way, its length and the reach in which a
    // person struck counts (game world). True: your blade is in its path: the hit must miss.
    bool BlockAttack(uintptr_t attacker, const glm::vec3& origin, const glm::vec3& direction, float length, float reach);

    // (fix173) The physics update's time since the last call (the game's world update each frame, timed around its own call): how many ran,
    // their average and the longest (ms); for the frame-rate watch's line (CreationEngineRendererModule). Any thread. False: not timed.
    bool TakePhysicsTimes(int& calls, float& average_ms, float& longest_ms);

    // One line for the settings panel.
    std::string Status();
}
