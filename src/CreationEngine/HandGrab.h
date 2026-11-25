#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

// Hand grab: an object you hold sits in your hand, locked to it, turns with it, and is thrown with your hand's
// speed when you let go.
// - With the weapon put away, squeeze a grip with your hand near a loose object (the use prompt shows it): the
//   game's own grab starts on that object, held in that hand. Let go of the grip to drop it; let go while swinging
//   to throw it. Holding use (A) on an object still grabs it (right hand); squeezing a grip then takes it into that
//   hand, and the other grip passes it across.
// - Fists out count as empty hands (as HIGGS has it in Skyrim VR): grabbing, the gravity gloves and the hand's
//   interaction pick work with only your fists out (read from your equipped weapons: a hand-to-hand weapon; grenades and
//   mines stay equipped in their own slot, thrown with their own button, and do not count), and a hand
//   busy with the hand grab does not also punch when you swing it. Any other weapon out: the hands do not grab (a grip
//   squeezed then says so in the log).
//
// How the game holds an object (found with the grab test builds, Starfield 1.16.236):
// - Grabbing (rva 1a50600, the player) takes the interaction pick's target and hangs it on a Havok mouse
//   spring (hknpBSMouseSpringAction, made by rva 1a51a70) by the middle of its collision shape's own bounding
//   box (the spring's grab point, +0xb0, in the physics body's space), wherever the pick touched it; the spot
//   the pick touched is only the spring's first target. The player keeps its springs at PlayerCharacter+0xd80
//   (count) / +0xd88 (pointers). Dropping (rva 1a53a80, the player) removes them.
// - Every frame the player's grab update (rva 1a52da0) works out a target in front of your view and writes
//   it into the spring (spring+0xa0, at 1a535ca), then builds the target orientation from the camera's
//   rotation (spring+0x70..0x9f, at 1a537d0).
// - Every physics step (rva 23d3e00) the spring first slows the object (object damping, spring+0xdc, 0.75),
//   then gives the held spot the speed (1 - damping) x its speed - elasticity x (distance to target) / dt
//   (damping spring+0xd8 0.5, elasticity +0xd4 0.2), changing it by at most max force (+0xd0, 250) x dt.
//   With the game's values the object trails a moving target and drifts in after it stops.
// What this changes:
// - The target follows the holding hand (see the grip below; without a box, the middle of the closed hand): where the shown
//   body draws that hand this frame. The body draws it from the same controller sample the grab update reads, but where the
//   play area is when it draws it, which moves in between as you walk or turn. As HIGGS moves held objects with the player's
//   own movement each frame (not a speed guessed from the frames before), that move is measured each frame (the body tells
//   when it places the hands, OnHandsPlaced): known when the body drew the hands before the physics steps; otherwise the
//   frame before's move is used, as it was or per physics step (Starfield steps physics at 60 a second, one or two steps a
//   frame), whichever has foretold it better. Where the object is drawn tells (at the next frame's start) whether drawn objects
//   show their frame's physics or the frame before's; then it is led one more frame of the hand's motion (and its orientation
//   one more frame of the hand's turn). How much of that frame leads it is learned, separately for the part of the motion that
//   comes from walking or turning and the hand's own, from where drawn objects end up (a lead of k frames puts it (k - k')
//   x the frame's motion ahead of the hand, k' being the lead it needs), only from frames the lead alone decided (held at
//   full stiffness, nothing blocking it or touching it): a whole frame of both put it 3.8 cm ahead while walking 7.7 cm a
//   frame (half a frame was needed) and 0.1 cm ahead while moving the hand 1.3 cm a frame (a whole one).
//   A log line every few seconds of holding, and a summary when any hold ends, gives how far the drawn object is from where
//   the hand holds it (still, moving the hand, walking or turning), how far its orientation is from the hand's (still,
//   turning) and the leads learned.
// - Elasticity and damping go to 1 over the first quarter second ("hold stiffness"): the held spot then lands on
//   the target every step. It is still a physics object: when something blocks it, it pushes gently. Each physics step also
//   checks where the held spot is against where the step before sent it: pushed off by something it touches (a table, a
//   wall, you) in 2 of 4 steps, it is held more softly (elasticity 0.35, as HIGGS softens its hold while an object collides)
//   so it rests against the obstacle instead of shaking, back to full strength once nothing has pushed it for 10 steps. The
//   hold summary in the log counts those pushes and which way they went (up, down, away from you, toward you, sideways).
// - Where the held spot goes is worked out with the rotation the object is being turned to this frame (the rows given to
//   the game), not the one it was last drawn with, which is a frame or two behind while the hand turns.
// - Drawn in the hand ("draw held objects exactly in your hand"): physics moves the object in fixed steps (60 a second) and
//   the game draws it at its body's pose pushed forward in time, while the hand is drawn where the controller is this frame,
//   so frame to frame it landed a little ahead of or behind the hand (2-3 cm on average walking, up to a step of its motion).
//   As ROCK does in Fallout 4 VR, the held object is drawn where the hand holds it (the place worked out by the grab update,
//   kept relative to the controller), and physics keeps moving the real body as before (the follow numbers in the log are the
//   physics body's): a node with a collision object has its world update handed to the collision object (bhkNPCollisionObject
//   type table slot 56, rva 23781c0), which for a moving body calls the game's scene writer (rva 237cb20) with the body's
//   pose; for the held object's own collision object the writer is given the place in the hand instead (ROCK hooks Fallout 4
//   VR's writer, 0x1E06B00, the same way). How the node sits on its body is measured at the first writes of a hold, and every
//   write is checked (the node must end up where it was drawn; most of the first 30 off, it is drawn where physics has it).
//   While the object touches something (held softly, blocked) or is more than 12 cm or 30 degrees from the hand's place (a
//   wall or table stops it), it is drawn where physics has it, eased in and out; after the hold what is left of the
//   difference fades out in a few frames. Only objects whose 3D root's own collision object moves the held body are drawn
//   this way. The fingers close around it where it is drawn. The hold summary says in how many frames it was drawn in the
//   hand, in how many writes the node landed where it was drawn, and whether that stayed until the next frame.
// - It does not bump into you ("held objects don't bump into your own body"): at the frame after a grab starts, the held
//   body is put in your collision group with the game's own function (bhkNPCollisionObject's SetCollisionFilterGroup, type
//   table slot 65): the game's collision filter lets bodies of the same group collide only when both are body parts, so it
//   no longer touches your character controller or your body, and touches everything else as before (HIGGS does this in
//   Skyrim VR). The body is found in your physics world by its id, and the collision object it belongs to from its own record
//   (+0x98), so a body of a part of an object counts too. Its group is put back once the hold has ended and the object is
//   clear of you (its bounding sphere outside a 0.4 m capsule from your feet up 1.9 m, three frames running; after 3 s
//   anyway), if you are still in the physics world it was in and it is still there as it was.
// - The orientation turns with the hand from the moment of the grab (led a frame, as above).
// - The grip (like HIGGS in Skyrim VR, with the object's bounding box from its base record instead of its mesh):
//   the game's grab start (rva 1a50600, +0x6c6, just before it makes the springs) tells which object is taken
//   ([rsp+0x40]). The object turns (over the first quarter second) to sit in the hand the way a hand holds things: a
//   long object (glass, bottle, handle) with its longest side across the palm, a box's side most facing the palm flat
//   on it; a flat round one flat on the palm; a chunky one square on the palm. With the body shown it rests against the
//   drawn hand's palm at the base of its fingers (HIGGS's palm point), square to the drawn hand's knuckle line (measured
//   once from the body's own hand bones: body::GetHandGripFrame); without it, on the controller's grip point, a little
//   toward the fingers. Each finger of the shown body then wraps it as a hand does: it bends at the knuckle until the
//   finger meets the box or the cylinder, then its outer joints curl in until the fingertip does. (HIGGS keeps the tilt and
//   fits the fingers to the real mesh; it only turns weapons and objects with grab points made for them.) With turning
//   off: the point of the box facing the palm rests on the palm, raised until none of it is inside the hand.
// - Placing it: the target is where the point the springs hold must be for that grip. That point is measured at the
//   springs' first physics step (rva 23d3e00, +0xc9: the grab point in the game world), turned into the object's own
//   space with its pose read just before the springs were made (grab start +0x6c6; the new spring comes from +0x6d6).
//   It is used when the object was at rest then (its 3D where it was the frame before); otherwise, and until then,
//   the middle of the object's box stands in for it.
// - Along a long object the hand holds it across from the palm when the palm is at the object, else (reaching for it, or
//   pulling it) at the middle of its length.
// - Letting go of the grip drops the object inside the grab update and gives it your hand's velocity (where the physics
//   world's velocity function cannot be used, as in 1.16.236, the spring takes one more physics step first, its target the
//   held point moved on by that velocity x the step, so the object leaves with it).
// - Gravity gloves (as HIGGS / ROCK, after Half-Life: Alyx): a grip squeezed with nothing next to the hand aims. The
//   hand's interaction cast then reaches far (5 m by default), starting beyond the hand reach, every other frame as a 30 cm
//   sphere (HIGGS's far cast) and in between as the game's own 10 cm one, whose find goes first (the game's pick takes the
//   first thing its sphere touches, HIGGS the one nearest the pointing line); something found for a few frames running with
//   the hand steady is locked on (a buzz, and the cast stays on it); moving the hand (or flicking the fingertips) back toward you, along the line from the object or toward your head,
//   faster than 1.2 m/s pulls it: the game's grab starts on it, and the grab update flies it to its place in the hand on an
//   arc (HIGGS's flight time, 0.3 to 0.7 s; the arc of a thrown object), held at full strength all the way, so it arrives
//   in the hand if the grip is still held. Letting go of the grip as you flick or while it flies (people open the hand after
//   the flick) still brings it: a squeeze of either grip catches it on the way or for a second at the open hand (HIGGS);
//   otherwise it drops there. Something the game will not take (too heavy, a person, a counter) gives a long soft buzz, and
//   the aim skips it from then on. The grab start (rva 1a50600, +0x1ee) otherwise takes
//   what a ray from the camera hits first; for grabs a grip starts (near or far) that head ray is skipped, so the hand's
//   own target is taken. Hand speeds come from the runtime, or from the controller poses when it gives none.
// - The orientation: the grab update builds the target orientation from a rotation slot it fills with the camera's
//   stored rotation rows (each a column of the column-vector rotation, as for every node); the hand's turn since the grab
//   and the turn into the hand are applied to it (a log line checks the result once each grab has turned in).
namespace HandGrab
{
    // The held object's bounding box where it is now: a point x of the box (min..max, object space) is at
    // r * (s * x) + t in the game world.
    struct HeldShape
    {
        glm::mat3 r{ 1.0f };
        glm::vec3 t{ 0.0f };
        float     s{ 1.0f };
        glm::vec3 min{ 0.0f };
        glm::vec3 max{ 0.0f };
        int       grab{ 0 }; // changes with every grab
        // Round objects (two sides of the box about equal): a cylinder along `axis` (0..2, object space) through the
        // box's middle, `radius` in object units. Otherwise the box itself.
        bool      cylinder{ false };
        int       axis{ -1 };
        float     radius{ 0.0f };
    };

    // Finds and hooks the grab code (by signature; stays off with a log line if not found).
    void Install();
    // Once per engine frame (main thread, from tracking::Snapshot, at the frame's start): the grip buttons.
    void Update();
    // The shown body is about to place the hands (BodyIK, once a frame, any thread): where the play area is now.
    void OnHandsPlaced();
    // Your hands can grab and pull: no weapon out, or only your fists (as HIGGS treats fists as empty hands). Main thread.
    bool HandsFree();
    // That hand is busy with the hand grab (holding, aiming the gravity gloves, a grip deciding what to grab) or was until
    // 0.6 s ago: its swings are not melee then (ImpactMelee), so with your fists out a throw does not also punch.
    bool HandBusy(bool left);
    // Keep that hand busy (as above) for another 0.6 s: another hand action has it (a power pushed out, a grenade thrown).
    void MarkBusy(bool left);
    // Another hand action took that grip (main thread, before Update): the hand grab ends its gravity-glove aim or reach on
    // that hand and does not grab with this squeeze, until the grip is let go.
    void LeaveGrip(bool left);
    // (fix154) The left grip belongs to the left hand's weapon now (dual wielding): its squeezes start no grab or gloves' aim (its d-pad
    // stays). Set each frame by LeftWeapon.
    void SetLeftGripWeapon(bool on);
    // That hand holds something with the hand grab (or catches what it pulled).
    bool HoldingWith(bool left);
    // (fix179, for Climbing) That grip's squeeze is still being decided by the hand grab (grab, gloves' aim or nothing). Any thread.
    bool Deciding(bool left);
    // (fix172, for Physical; main thread) Let go of what is held in the next grab update, keeping its own speed (a body stretched too far, a
    // person getting up); or at once, with the game's drop (a menu opened, the game saving); and a grip's grab started again at the next
    // frame's start if that grip is still squeezed (a person it knocked down is down).
    void LetGo(const char* why);
    bool DropNow(const char* why);
    void GrabAgain(bool left);
    // The weapon out shoots (a gun, or a weapon whose type could not be read; not fists, melee weapons, grenades). Any thread (its
    // reads are guarded, its record kept under a lock; the body update calls the next one from the game's worker threads).
    bool GunOut();
    // The weapon out is a gun by its type, read (not a weapon whose type could not be read). Any thread.
    bool GunKnownOut();
    // A melee weapon is out in your hands (not fists): its form id (0: none, or another kind of weapon, or put away). Main thread.
    uint32_t MeleeWeaponOut();
    // That hand's grip is squeezed, as the hand grab read it this frame (after Update; hands follow "Swap Controller Inputs").
    // (fix158) The weapon out is a one-handed melee weapon by its read type (1-4: sword, dagger, axe, mace; not two-handed, not a gun):
    // the body holds it by its own model (BodyIK). Any thread.
    bool OneHandedMeleeOut();
    bool GripDown(bool left);
    // That hand's trigger pull (0..1; hands follow "Swap Controller Inputs").
    float TriggerPull(bool left);
    // A grenade or mine is equipped in its slot (thrown with the grenade button). Main thread.
    bool ThrowableEquipped();
    // ...and its form (0: none). Main thread.
    uintptr_t ThrowableForm();
    // Where that hand holds things now (game world): the palm's surface where an object rests against it, the way the palm faces
    // (out of it), the fingers' way and the line across the palm (with the body shown, the drawn hand's own, as a held object is
    // placed). Main thread.
    bool HoldFrame(bool left, glm::vec3& surface, glm::vec3& facing, glm::vec3& forward, glm::vec3& side);
    // A 3D's own collision object in your collision group, as a held object is put there (it then no longer touches you or your
    // character controller): 1 it is, 0 not yet (its body not in your physics world yet, or the change not done yet: check again
    // next frame; `ask` false only checks), -1 never (not that kind of collision object). `what` says which. Main thread.
    int PutInYourGroup(uintptr_t root, bool ask, std::string& what);
    // (fix155) The same for a weapon's copy (its collision on its parts, through a proxy on its root; several bodies): every body of its
    // 3D put in your collision group and on the layer that touches nothing (it is drawn only). 1 all are, 0 not yet (check again), -1
    // never (the game's functions not as expected). Main thread.
    int PutApart(uintptr_t root, bool ask, std::string& what);
    // (fix187) How a copy's bodies move now (the same bodies PutApart finds): how many are keyframed and how many are moved by
    // physics (gravity pulls those). 1 read, 0 not found (yet). Main thread.
    int PartsMotion(uintptr_t root, int& moving, int& keyframed, std::string& what);
    // The form id of the weapon equipped for your hands (not fists, grenades or mines; 0: none). Any thread (read at most every 100 ms,
    // under the armed state's lock, as GunKnownOut).
    uint32_t WeaponInHandsId();
    // Its form, as last read (any thread; 0: none).
    uintptr_t WeaponInHandsForm();
    // For the controller mapping: while true, the right grip does not press the left trigger (aim), and
    // the left grip does not turn the left stick into a d-pad.
    bool SuppressRightGripTrigger();
    bool SuppressLeftGripDpad();
    // ...the right trigger does not fire (or draw your weapon, or punch): a weapon is in the right grip (its trigger equips it),
    // or that trigger just did, until it is let go.
    bool SuppressRightTrigger();
    // ...hold the game's holster button (reload held, X): a picked-up weapon put in the bag when you had no weapon before (if it could not be unequipped).
    bool PressHolster();
    // ...tap reload (X): a picked-up weapon just equipped while your weapon was put away is drawn (the game's "press reload to draw").
    bool PressReload();
    // ...the game's fire (RT): -1 as your trigger says; 1 pressed (the trigger that equipped a picked-up weapon, still held, fires it
    // once it is out: a fresh press for the game).
    int FireOverride();
    // A weapon you picked up and equipped (by its trigger) is in your hands, drawn: behind a shoulder or at a hip the
    // gun hand's grip puts it in the bag (so the grenade from behind the shoulder waits until it is gone). Any thread.
    bool PickedWeaponInHands();
    // For the shown body's fingers: how far a hand closes because it holds something (0 = not holding).
    float HeldCurl(bool left);
    // The box of what that hand holds, for the fingers to close around (false: not holding, or no box known).
    bool HeldBox(bool left, HeldShape& out);
    // (fix170) This hand is the second on the held object (both hands hold it): where its grip on the object is put this frame (game
    // world), for the body to draw the hand there. False: not the second hand (or not this frame).
    bool SupportGrip(bool left, glm::vec3& grip);
    // One line for the settings panel.
    std::string Status();

    // While the gravity gloves aim (main thread): what to show for it (GunSight draws it, game world). kind 1: what the
    // gloves are aimed at (the game will let you take it), ringed: its middle and a radius around it; 2: the same, locked
    // on; 3: nothing found yet: a point along the gloves' line, for a dot that shows where it goes.
    struct GloveMark
    {
        int       hand{ -1 };
        int       kind{ 0 };
        glm::vec3 point{ 0.0f };
        float     radius{ 0.0f };
    };
    bool AimMark(GloveMark& out);

    // Test builds (the physics probe): the object held now, as the grab update had it within the last half second.
    struct HeldDebug
    {
        uintptr_t world{ 0 };   // the held body's physics world (hknpBSWorld; 0: not found from its 3D)
        uintptr_t spring{ 0 };  // the mouse spring holding it
        uint32_t  body{ 0 };    // its physics body
        uintptr_t root{ 0 };    // its 3D (0: not known)
        glm::vec3 head{ 0.0f }; // your head (the camera), game world
        glm::vec3 object{ 0.0f };
        bool      have_head{ false };
        bool      have_object{ false };
    };
    bool DebugHeld(HeldDebug& out);
}
