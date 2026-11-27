#pragma once
#include "glm/glm.hpp"
#include <CreationEngine/memory/ScanHelper.h>
#include <CreationEngine/memory/offsets.h>
#include <glm/detail/type_quat.hpp>
#include <glm/gtx/matrix_major_storage.hpp>
#include <string>
#include <vector>

namespace GameFlow
{
    struct DebugData {
        std::vector<glm::vec3> points{};
        std::vector<std::string> ui_parts{}; // (fix189) copies of the names, used only under GameFlow's lock (GameFlow::uiPartsDrawn)
    };

    struct Settings
    {
        struct HudSettings {
            float hudScale{0.4f};
            int perspective{150};
        } hudSettings{};
        struct InternalSettings {
            int headAimingAbsolute{1};
            float flatScreenDistance{1.5f};
            bool nvidiaAndTAAfix{true};
            bool preventZoom{false};
            bool alternativeJoyLayout{false};
            // (fix194) The face buttons send the game the button printed on them (Touch A, B, X, Y: the game's A, B, X, Y), so its
            // prompts name the button to press. Off: the old layout (right A = A, left X = B, right B = Y, left Y = X).
            bool faceButtonsAsPrinted{true};
            bool decoupledPitch{false};
            bool pawnControl{true};
            // Shown body (ported from SFVR): the third-person body is drawn in first person, its arms
            // follow the controllers and its hand holds the gun.
            bool showBody{true};
            bool matchBodyHeight{true}; // scale head/hand movement so your eye height matches the character's
            bool bodyLean{true};        // the spine bends toward the head before the body follows
            bool bodyFacing{true};      // the torso turns partly toward the hands
            bool supportHand{true};     // the left hand takes the gun's support grip when brought near it
            bool fingerPoses{true};     // fingers follow trigger/grip/thumb touch
            bool walkingLegs{true};     // procedural steps while moving
            bool bodyTracking{true};    // Quest body tracking (torso, elbows, legs) when the runtime provides it
            bool handTracking{true};    // finger joints from hand tracking when the runtime provides it
            bool realCrouch{false};     // bend the body when your real head goes below where you recentred
            bool crouchView{true};      // lower the view to the crouched body's eyes when crouching with the button
            // (fix195) Weapon steadiness with the body shown:
            // - tieViewToBody: in first person on foot the camera root is put where the body (and the gun in its hand) was placed this
            //   frame, so the gun does not move against the view while you move (tracking::BeforeNodeUpdate).
            // - lockGunGrip: how the hand holds each gun (and where the left hand's support grip is on it) is taken once it has held
            //   still a moment and kept for that gun, instead of copying the hidden first-person rig's animation every frame; live
            //   during a reload or a draw.
            // - virtualStock: holding a gun with both hands, the gun hand near its shoulder, its aim line starts partly (up to 25%) from the
            //   shoulder, which is steadier than the hand.
            bool tieViewToBody{true};
            bool lockGunGrip{true};
            bool virtualStock{true};
            // (fix199) Sitting in a seat the game shows with your own body: the view at that body's eyes, your head's lean from where you
            // sat held softly past 15 cm, never past 30 cm (SeatView).
            bool seatViewAtEyes{true};
            // Turning with the right stick (on foot): 0 = snap in fixed steps, 1 = smooth.
            int   turnMode{0};
            float snapTurnDegrees{45.0f};
            float smoothTurnSpeed{1.0f};  // scales the stick for smooth turning (1 = the game's own speed)
            // On foot, the view level with your head: the game's look kept level (the right stick's up and down tilts nothing).
            bool  levelView{true};
            // Controller vibration.
            bool  haptics{true};
            float hapticStrength{0.8f};
            bool  hapticGameRumble{true}; // pass on the game's own rumble (hits, explosions, landing)
            // Hand interaction: with the weapon put away, the prompt is for whatever is near the right hand.
            bool  handInteract{true};
            float handReach{0.35f};       // metres in front of the controller that count as "near the hand"
            // Impact melee: a hard swing with the weapon out attacks, and hits what the hand or weapon swept through.
            bool  impactMelee{true};
            float meleeSwingSpeed{2.2f};  // m/s of the fist, blade or gun front that counts as a swing
            bool  meleeInstantHits{true}; // hit the moment a swing connects (every swing counts) instead of pressing melee
            bool  meleeThrow{true};       // melee weapon out: squeeze the grip, swing and let go to throw it; squeeze again to call it back
            // Hand grab: an object held with the game's grab is held in your hand and turns with it.
            bool  handGrab{true};
            bool  gripGrab{true};         // squeeze a grip next to an object to grab it; let go to drop or throw it
            float grabStiffness{1.0f};    // 0 = the game's soft hold, 1 = locked to the hand
            bool  grabInHand{true};       // a held object is drawn exactly where your hand holds it (physics shown while it touches something)
            bool  grabIgnoresYou{true};   // a held object does not bump into your own body
            bool  grabAlign{true};        // held objects turn to sit square in the hand (round ones across the palm)
            bool  twoHandHold{true};      // (fix170) the other hand grips the held object: both hands hold and turn it (ROCK's shared pose)
            bool  oneInEachHand{true};    // (fix170) the other hand grabs something else: each hand holds its own object
            bool  gravityGloves{true};    // grip with nothing near the hand: point at something, flick back, it flies to the hand
            float pullRange{5.0f};        // metres the gravity gloves reach
            bool  gloveMarker{true};      // while the gloves aim: a ring on what they will pull (a dot along the line before)
            bool  gloveHighlight{true};   // ...drawn on the object itself, by the game's own highlight (blue, white once locked on)
            float gloveHighlightFill{0.4f}; // ...its glow over the object (0: an outline only; 1: the strongest, still see-through)
            bool  gloveFrozen{true};      // (fix151) ...as the game's frozen look instead (Sunless Space's frost on the object), the outline only without it
            int   gloveAimLine{0};        // the gloves' pointing line: 0 the hand's (HIGGS), 1 out of the palm (ROCK), 2 sighting along the hand
            bool  mouthEat{true};         // food or a drink held at your mouth: a steady buzz; let go there to eat or drink it (Consume.h)
            bool  bagStash{true};         // let go of something you can carry behind a shoulder or at a hip: it goes in your bag
            bool  autoEquipWeapons{true}; // a weapon in your grip: the trigger equips it; your weapon before comes back
            // Aiming, powers and grenades with the hands.
            bool  gunSight{true};          // a red dot where the gun's shots go while aiming with the hand
            float gunSightDistance{15.0f}; // metres out along the shot the dot sits
            bool  gunLaser{true};          // ...as a laser (Resident Evil 4 VR's): a red beam from the gun and its dot where it hits
            float gunLaserBeamLength{0.30f}; // (fix148) its beam seen this far from the gun (m), fading to nothing at its end (or at the dot, nearer)
            bool  templeGestures{true};    // a trigger at a temple: the left, the flashlight; the right, the scanner (TempleGestures)
            bool  scannerPlain{true};      // (fix143) the hand scanner without the blue tint, visor frame and dark edges (MenuClean)
            bool  levelViewMenus{true};    // (fix143) the view kept level in the container, barter and workbench menus (GameFlow::menusKeepLevelView)
            bool  workbenchFirstPerson{true}; // (fix143) the game's third-person camera at workbenches skipped (WorkbenchCamera)
            bool  ladderFirstPerson{true};  // (fix143) ladders in first person, climbed with the hands (LadderClimb)
            bool  climbing{true};           // (fix179) a grip squeezed with the hand against a wall, rock or ledge holds you on; pull to climb (Climbing)
            float climbPull{1.0f};          // (fix179) ...your body moves this many times your hand's pull (more: further for each pull, seated)
            bool  dialoguePointer{true};   // (fix142) a conversation's responses chosen by pointing a hand and pulling its trigger
            bool  dialogueTouch{true};     // (fix143) a conversation's answers bigger, at arm's length, pressed by touching them with a finger
            bool  dialoguePanel{true};     // (fix145) the conversation on a panel of its own, fixed in the room in front of the person speaking (ConversationPanel)
            bool  dialogueTouchPress{false}; // (fix173) ...its answers also pressed by pushing a finger into them (off: pointed at, the trigger pulled; the panel stays)
            bool  dialogueTurn{true};      // (fix173) in a conversation the right stick turns you smoothly, whatever the turn mode (the panel kept in front of the person)
            bool  scannerGrips{true};      // (fix173) in the hand scanner and the star map (fix175: and build mode; fix203: and character creation) a grip's squeeze taps LB (left) or RB (right); no grab or throw there
            bool  messagesBlock{true};     // (fix178) the game's messages (notifications, mission updates...) in one place below your view's middle, bigger (MessagesBlock)
            bool  buildConsole{true};      // (fix177) build mode as a console: its card, list, hints and footer low in front of your seat, its cards, warnings and notifications near your view's middle (BuildConsole)
            bool  menuStickNavigate{true}; // (fix146) the left stick navigates menus (the game's own), the pointer stepping aside meanwhile
            float conversationDistance{0.60f}; // (fix147) the conversation's panel this far from your eyes (m), as large in view at any distance
            bool  tradeStickTabs{true};   // (fix147) the trading menu: the left stick's left and right are LB and RB (the trader, you, your ship)
            bool  dualWield{true};        // (fix154) a melee weapon in the left hand too, chosen with the left trigger in the inventory (LeftWeapon)
            bool  favoritesWheel{true};   // (fix171) the favorites as a Spell Wheel VR-style wheel at the left hand (left grip, then left trigger), not the game's menu (FavoritesWheel)
            bool  pushPowers{true};        // a grip held and that hand pushed out uses your power (LB + RB), aimed along the push
            float pushPowerSpeed{1.5f};    // m/s the push must reach
            bool  shoulderGrenade{true};   // right grip behind the right shoulder takes a grenade; let go as you throw to throw it
            bool  grenadeInHand{true};     // ...and while you hold it, a copy of the grenade sits in your hand (GrenadeHand.h)
            // (fix172) Physical interaction with people, as PLANCK in Skyrim VR (Physical.h).
            bool  physicalProbe{false};    // every 10 s in the log: the bodies and people near you, the physics update's time (fix189: off by default, PhysicalProbeV2)
            bool  grabBodies{true};        // the hand grab holds corpses and people knocked down by the bone your hand touches
            float bodyGrabStrength{0.5f};  // ...0 the game's own floaty hold, 1 firm (the force capped either way)
            bool  keepHeldDown{true};      // ...a person you hold stays down while you hold them (at most 30 s)
            bool  grabKnocksDown{true};    // weapons put away: gripping a standing person knocks them down, then you hold them (holdPeople off)
            bool  holdPeople{true};        // (fix173) weapons put away: gripping a standing person holds them standing (arm bent to your hand, dragged)
            bool  shovePeople{true};       // weapons put away: an open hand pushed into someone bumps them (light) or staggers them (firm)
            bool  punchKnockdown{true};    // fists out: a punch faster than knockdownSpeed knocks them down (the game gets them up)
            float knockdownSpeed{6.0f};    // ...m/s of the fist
            bool  saberBlock{true};        // an NPC's melee hit at you that meets your blade misses; the attacker recoils
            bool  shoveAnger{true};        // people shoved or knocked down again and again raise the game's assault alarm (PLANCK)
            bool  spareFollowers{true};    // your hand's hits and thrown weapons never hurt your followers
        } internalSettings{};
        DebugData debugData{};
    };

    extern Settings gStore;

//    inline int gameLoopFrameCount() {
//        // 1.14.74
//        static REL::Relocation<int*> gameFc{(uintptr_t)MemoryScan::mod + 0x6101e3c};
//        return *gameFc;
//    }

    inline int renderLoopFrameCount() {
        static REL::Relocation<int*> globalFrameCountAddr{ GameStore::MemoryOffsets::CreationRenderer::GlobalFrameCount() };
//        static REL::Relocation<int*> renderFc{(uintptr_t)MemoryScan::mod + 0x6a50260};
        return *globalFrameCountAddr;
    }
}

