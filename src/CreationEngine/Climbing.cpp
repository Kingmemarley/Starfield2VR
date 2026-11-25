#include "Climbing.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <glm/glm.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include <RE/N/NiCamera.h>
#include <mods/VR.hpp>
#include "CreationEngineSingletonManager.h"
#include "NiMath.h"
#include "FavoritesWheel.h"
#include "HandActions.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "LadderClimb.h"
#include "LeftWeapon.h"
#include "Physical.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace Climbing
{
    namespace
    {
        // ---- The game's ray pick (Starfield 1.16.236) ----
        // The pick data's constructor (rva 4951d0: pick, filter info, from[3], to[3]) zeroes the results, puts the filter info at +0x14 and
        // +0xFC, the ray's start at +0x30, its way (to - from) at +0x40 and its inverse at +0x50, and +0x100/+0x101 to 1 (+0x101: the pick may
        // be put off when the frame's picks took too long; set to 0 here, so it always runs). Five game functions make picks with it, one of
        // them as a character controller (rva 1b05da0: filter (group << 16) | 30).
        constexpr uintptr_t kPickCtorRva      = 0x4951d0;
        constexpr uint8_t   kPickCtorStart[]  = { 0x48, 0x83, 0xEC, 0x38, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x20, 0xC5, 0xF8, 0x29, 0x7C, 0x24, 0x10, 0xB8,
              0xFF, 0xFF, 0x00, 0x00, 0x66, 0x89, 0x41, 0x10 };
        constexpr uintptr_t kPickCtorFilterAt = 0x4952a6; // mov [rcx+0FCh],edx; mov word [rcx+100h],101h; mov [rcx+102h],r10b; mov [rcx+14h],edx
        constexpr uint8_t   kPickCtorFilter[] = { 0x89, 0x91, 0xFC, 0x00, 0x00, 0x00, 0x66, 0xC7, 0x81, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x44, 0x88, 0x91,
              0x02, 0x01, 0x00, 0x00, 0x89, 0x51, 0x14 };
        // The cell's pick (rva b05b80: cell, pick data -> the object hit or none): the cell's physics world (an interior's own, else the
        // world's), then the bhk pick (rva 2358360, at b05c0a), which reads the hit's layer back from +0xCC (at b05c17).
        constexpr uintptr_t kCellPickRva      = 0xb05b80;
        constexpr uint8_t   kCellPickStart[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
              0x57, 0x48, 0x83, 0xEC, 0x40 };
        constexpr uintptr_t kCellPickCallAt   = 0xb05c0a;
        constexpr uintptr_t kCellPickLayerAt  = 0xb05c17;
        constexpr uint8_t   kCellPickLayer[]  = { 0x8B, 0x87, 0xCC, 0x00, 0x00, 0x00, 0x83, 0xE0, 0x7F };
        // The bhk pick (rva 2358360) copies the first hit into the pick data: its place (+0x70), normal (+0x80) and fraction of the way
        // (+0x90; 3.4e38 when nothing was hit), the body hit (+0xB8) and that body's filter info (+0xCC: its layer in bits 0-6).
        constexpr uintptr_t kBhkPickRva       = 0x2358360;
        constexpr uint8_t   kBhkPickStart[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x41,
              0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        constexpr uintptr_t kBhkPickCopyAt    = 0x235866b;
        constexpr uint8_t   kBhkPickCopy[]    = { 0xC5, 0xF8, 0x10, 0x47, 0x30, 0xC5, 0xF8, 0x11, 0x43, 0x70, 0xC5, 0xF8, 0x10, 0x4F, 0x40, 0xC5, 0xF8,
              0x11, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x47, 0x50, 0x89, 0x83, 0x90, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kBhkPickBodyAt    = 0x23586c6;
        constexpr uint8_t   kBhkPickBody[]    = { 0x8B, 0x47, 0x78, 0x89, 0x83, 0xB8, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kBhkPickFilterAt  = 0x23586f3;
        constexpr uint8_t   kBhkPickFilter[]  = { 0x8B, 0x87, 0x8C, 0x00, 0x00, 0x00, 0x89, 0x83, 0xCC, 0x00, 0x00, 0x00 };
        // (Review) The bhk pick holds the physics world it casts in: it takes a reference on it (0x142E93470: +1 at [world+0x10], at
        // 2358456) and keeps it at +0xE0 (at 235845b, letting go of one already there); the game's callers let go of it after (0x142E934A0 on
        // [pick+0xE0]: 0x14057F912, 0x141ADED9D). So does each pick here.
        constexpr uintptr_t kBhkPickWorldAt   = 0x235845b; // mov rcx,[rbx+0E0h]; mov [rbx+0E0h],rsi; test rcx,rcx; je; call release
        constexpr uint8_t   kBhkPickWorld[]   = { 0x48, 0x8B, 0x8B, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB3, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9,
              0x74, 0x05, 0xE8 };
        constexpr uintptr_t kBhkPickHoldAt    = 0x2358456; // call the reference's take
        constexpr uintptr_t kWorldHoldRva     = 0x2e93470;
        constexpr uintptr_t kWorldReleaseRva  = 0x2e934a0;
        constexpr uint8_t   kWorldRelease[]   = { 0x48, 0x83, 0x79, 0x08, 0x00, 0x74, 0x1A, 0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x48, 0x0F,
              0xC1, 0x41, 0x10, 0x48, 0x83, 0xF8, 0x01, 0x75, 0x07, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x10, 0xC3 };
        constexpr size_t    kPickSize         = 0x140;
        constexpr size_t    kPickWorld        = 0xe0;
        constexpr size_t    kPickPlace        = 0x70;
        constexpr size_t    kPickNormal       = 0x80;
        constexpr size_t    kPickFraction     = 0x90;
        constexpr size_t    kPickBody         = 0xb8;
        constexpr size_t    kPickHitFilter    = 0xcc;
        constexpr size_t    kPickMaySkip      = 0x101;
        constexpr size_t    kPickSkipped      = 0x102;

        // ---- The game's queued actor move (as Physical drags people with it) ----
        // 0x1418C5440(unused, actor, cell, place[3], heading, bool, bool) fills a pooled command {type table 0x144CB9958, +8 place, +0x18
        // actor, +0x20 cell, +0x28 heading, +0x2C/+0x2D the bools} and posts it (0x1406634C0): run at once inside the game's own update,
        // else queued for it. Its run (the table's thunk 0x1418CF6C0: add rcx,8; jmp 0x1418CFD40), only while the actor is still in that
        // cell, calls the actor's SetPosition (slot 0x9B8: Actor::SetPosition for the player too) with true, then its heading (0x1419169C0).
        constexpr uintptr_t kQueueMoveRva     = 0x18c5440;
        constexpr uint8_t   kQueueMoveStart[] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x0A, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xDC };
        constexpr uintptr_t kQueueMoveTableAt = 0x18c55a8; // lea rcx,[the command's type table]
        constexpr uintptr_t kQueueMoveTable   = 0x4cb9958;
        constexpr uintptr_t kQueueMovePostAt  = 0x18c5601; // call the post
        constexpr uintptr_t kQueueMovePost    = 0x6634c0;
        constexpr uintptr_t kMoveThunkRva     = 0x18cf6c0;
        constexpr uint8_t   kMoveThunk[]      = { 0x48, 0x83, 0xC1, 0x08, 0xE9 };
        constexpr uintptr_t kMoveRunRva       = 0x18cfd40;
        constexpr uint8_t   kMoveRun[]        = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0x81, 0xB0,
              0x00, 0x00, 0x00, 0x48, 0x39, 0x43, 0x18, 0x75, 0x47, 0x48, 0x8B, 0x01, 0x41, 0xB0, 0x01, 0x48, 0x8B, 0xD3, 0xFF, 0x90, 0xB8, 0x09, 0x00,
              0x00, 0xC5, 0xFA, 0x10, 0x4B, 0x20, 0x48, 0x8B, 0x4B, 0x10, 0xE8 };
        constexpr uintptr_t kMoveRunHeading   = 0x29; // vmovss xmm1,[rbx+20h]: the heading about to be set (rbx: the command + 8)
        constexpr uintptr_t kCmdPlace         = 0x00;
        constexpr uintptr_t kCmdActor         = 0x10;
        constexpr uintptr_t kCmdHeading       = 0x20;
        // Actor::SetPosition (0x14193DCB0): with its bool, your controller ([[actor+0x228]+8]+0x458) is put there, its speed zeroed, and its
        // fall height (+0x348) zeroed (at 0x14193DE22).
        constexpr uintptr_t kSetPositionRva   = 0x193dcb0;
        constexpr uint8_t   kSetPositionStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
              0x83, 0xEC, 0x60 };
        constexpr uintptr_t kSetPositionControllerAt = 0x193dd45;
        constexpr uint8_t   kSetPositionController[] = { 0x49, 0x8B, 0x86, 0x28, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xE6, 0x00, 0x00, 0x00, 0x48,
              0x8B, 0x78, 0x08, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0xD9, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xBF, 0x58, 0x04, 0x00, 0x00 };
        constexpr uintptr_t kSetPositionFallAt = 0x193de22;
        constexpr uint8_t   kSetPositionFall[]  = { 0x89, 0xAF, 0x48, 0x03, 0x00, 0x00 };
        constexpr uintptr_t kPlayerTableRva     = 0x4cc2868; // PlayerCharacter's type table (its RTTI: offset 0)
        constexpr uintptr_t kSetPositionSlot    = 0x9b8;

        // ---- Your character controller ----
        // PlayerCharacter+0x228 its AI process, +0x8 the middle-high data, +0x458 the controller (as HandGrab and Physical read it), +0x4D0
        // its proxy, +0x3C the proxy's filter info (layer 30 and your group: 0x9001E). The controller's gravity factor (+0x350) is written by
        // the game as the cell's gravity times yours (1 in zero gravity, at 0x141947FB9), used in the in-air update (0x1423F4307), and 0
        // there is the game's own case (0x1423F45F6: the air time zeroed, the fall height kept at where you are: 0x1423F465C). The fall
        // height (+0x348: where a fall started, 0 none) is read by the fall damage (0x141A058DC..0x141A058FD).
        constexpr uintptr_t kActorProcess      = 0x228;
        constexpr uintptr_t kProcessMiddle     = 0x08;
        constexpr uintptr_t kMiddleController  = 0x458;
        constexpr uintptr_t kControllerProxy   = 0x4d0;
        constexpr uintptr_t kProxyFilter       = 0x3c;
        constexpr uintptr_t kProxyVelocity     = 0xb0;
        constexpr uintptr_t kControllerFall    = 0x348;
        constexpr uintptr_t kControllerGravity = 0x350;
        constexpr uintptr_t kGravityWriteAt    = 0x1947fb9;
        constexpr uint8_t   kGravityWrite[]    = { 0xC5, 0xFA, 0x11, 0x8D, 0x50, 0x03, 0x00, 0x00 };
        constexpr uintptr_t kGravityUseAt      = 0x23f4307;
        constexpr uint8_t   kGravityUse[]      = { 0xC5, 0xFA, 0x59, 0x87, 0x50, 0x03, 0x00, 0x00 };
        constexpr uintptr_t kGravityZeroAt     = 0x23f45f6;
        constexpr uint8_t   kGravityZero[]     = { 0xC5, 0xF8, 0x2E, 0xBB, 0x50, 0x03, 0x00, 0x00 };
        constexpr uintptr_t kGravityZeroFallAt = 0x23f465c;
        constexpr uint8_t   kGravityZeroFall[] = { 0x33, 0xC0, 0x89, 0x83, 0x4C, 0x03, 0x00, 0x00, 0xC5, 0xFA, 0x11, 0xA3, 0x48, 0x03, 0x00, 0x00 };
        // (Review) The controller's state now ([ctrl+0x2E8], as its state machine at +0x2C0 reads it at +0x28: 0x1402B62B0), written as the
        // state changes (0x1423AFE00): 0 on the ground, 2 in the air (the in-air state's type, its type table 0x144D6D7D8 slot 3 = 0x14047D9E0).
        constexpr uintptr_t kControllerNow     = 0x2e8;
        constexpr uint32_t  kStateOnGround     = 0;
        constexpr uint32_t  kStateInAir        = 2;
        constexpr uintptr_t kStateWriteAt      = 0x23afe00;
        constexpr uint8_t   kStateWrite[]      = { 0x89, 0x93, 0xE8, 0x02, 0x00, 0x00 };
        constexpr uintptr_t kStateReadRva      = 0x2b62b0;
        constexpr uint8_t   kStateRead[]       = { 0x8B, 0x41, 0x28, 0xC3 };
        constexpr uintptr_t kInAirTableRva     = 0x4d6d7d8;
        constexpr uintptr_t kInAirTypeRva      = 0x47d9e0;
        constexpr uint8_t   kInAirType[]       = { 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3 };
        constexpr uintptr_t kFallDamageReadAt  = 0x1a058dc;
        constexpr uint8_t   kFallDamageRead[]  = { 0x48, 0x8B, 0x81, 0x28, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x6A, 0x48, 0x8B, 0x40, 0x08, 0x48, 0x85,
              0xC0, 0x74, 0x61, 0x48, 0x8B, 0x80, 0x58, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x55, 0xC5, 0xFA, 0x10, 0x80, 0x48, 0x03, 0x00, 0x00 };

        // ---- A reference (TESObjectREFR) and an actor ----
        constexpr uintptr_t kRefHeading  = 0x88; // data.angle.z (the snap turn writes it)
        constexpr uintptr_t kRefLocation = 0x8c; // data.location
        constexpr uintptr_t kRefCell     = 0xb0;
        constexpr uintptr_t kActorState  = 0xf8; // bits 17-20: its life state (0 alive and well; Physical's research)
        constexpr uintptr_t kActorState2 = 0xfc; // & 0x1800: in furniture (sitting, lying, getting in or out)

        // ---- Collision layers (Starfield.esm's layer records) ----
        // The rays are cast as the line-of-sight layer (41: it meets statics, moving statics, terrain, ground, trees, props, large clutter,
        // character controllers; not triggers, small clutter or water) and, for walls on the way, also as the camera's layer (39: statics,
        // terrain, ground, trees, transparent ones, and the invisible walls at the world's edges; water hits are passed over).
        constexpr uint32_t kLayerLineOfSight = 41;
        constexpr uint32_t kLayerCamera      = 39;
        constexpr uint32_t kLayerController  = 30;
        constexpr uint32_t kLayerWater       = 11;
        bool Climbable(uint32_t layer)
        {
            switch (layer) {
            case 1:  // static
            case 2:  // moving static (gates, platforms)
            case 9:  // trees
            case 10: // props
            case 13: // terrain
            case 17: // ground
                return true;
            default:
                return false;
            }
        }
        const char* LayerName(uint32_t layer)
        {
            switch (layer) {
            case 0: return "unidentified";
            case 1: return "static";
            case 2: return "moving static";
            case 3: return "transparent";
            case 4: return "clutter";
            case 5: return "weapon";
            case 8: return "body";
            case 9: return "trees";
            case 10: return "props";
            case 11: return "water";
            case 12: return "trigger";
            case 13: return "terrain";
            case 17: return "ground";
            case 27: return "invisible wall";
            case 29: return "large clutter";
            case 30: return "character controller";
            case 32: return "dead body";
            case 33: return "body without a controller";
            case 55: return "clutter (no cut)";
            case 60: return "vehicle";
            default: return "other";
            }
        }

        // ---- How it feels ----
        // (fix180) 0.20 m, was 0.10 (VR Climbing's 6.75 units): after fix179 climbing worked but grip was hard to find; the
        // log, 87 of ~180 squeezes found nothing: 46 with the wall 0.11-0.53 m off the palm, 37 with nothing on any ray.
        constexpr float   kReach          = 0.20f;    // a climbable surface within this of the palm's middle (m)
        constexpr float   kFromOutside    = 0.30f;    // (fix180) rays from this far out back to the palm: a hand pushed into a wall
        // (fix181) Your hand through a wall: its near side from your eyes, this far from the palm (the review: within the drawn wall hand's
        // reach, BodyIK's kWallNone 0.30, so the hand is drawn on it).
        constexpr float   kThrough        = 0.25f;
        constexpr float   kHeadBeyond     = 0.06f;    // the ray from your eyes to the palm goes on this far past it (a hand inside a collider)
        constexpr int64_t kDecideMaxUs    = 600'000;  // (review) the hand grab decides a squeeze first; this long at most (a fallback)
        constexpr float   kEase           = 13.0f;    // the body eased toward where the hands pull it (a second's fraction: VR Climbing's 13)
        constexpr float   kFeetUp         = 0.35f;    // the casts for walls on the way start this high above your feet...
        constexpr float   kWaistUp        = 0.90f;    // ...and this high (and from your eyes)
        constexpr float   kBodyGap        = 0.25f;    // ...and stop the body this far before what they hit (about your controller's radius)
        constexpr float   kHeadGap        = 0.20f;    // ...the eyes this far (VR Climbing's 14-16 units from the headset)
        constexpr float   kTargetMax      = 0.75f;    // where the hands pull you is kept within this of where you are (a cut-short move)
        constexpr float   kHandJump       = 0.5f;     // a hand moving more than this in a frame is a tracking jump, not a pull
        constexpr float   kTeleport       = 2.0f;     // you moved more than this in a frame: a load or a teleport (you let go)
        constexpr float   kFloorProbe     = 0.50f;    // a move down is cast from this high above your feet to the ground below
        constexpr int64_t kFallWatchUs    = 3'000'000; // after letting go: the fall height watched this long
        constexpr int     kRing           = 8;        // moves asked and not yet run, kept for the run's hook

        using PickCtorFn = void* (*)(void* pick, uint32_t filter, const float* from, const float* to);
        using CellPickFn = void* (*)(void* cell, void* pick);
        using ReleaseFn  = void (*)(void* world);
        using MoveFn     = void (*)(void* unused, void* actor, void* cell, const float* place, float heading, bool a, bool b);

        uintptr_t           g_base = 0;
        std::atomic<bool>   g_ready{ false };
        std::string         g_installed{ "not installed" };
        std::atomic<const char*> g_broken{ nullptr }; // (a fault since: off for the session; read by the settings panel's thread)
        uintptr_t           g_pick_ctor = 0;
        uintptr_t           g_cell_pick = 0;
        uintptr_t           g_world_release = 0;
        bool                g_state_ok = false; // the controller's state as found: the fall height is only ever written in the air
        uintptr_t           g_queue_move = 0;
        bool                g_gravity_ok = false; // the gravity factor's code as found: it is set to 0 while you hold on
        safetyhook::MidHook g_run_hook{};

        std::atomic<int64_t>   g_frame{ 0 };
        std::atomic<uintptr_t> g_player{ 0 };
        std::atomic<bool>      g_holds[2]{};
        std::atomic<bool>      g_active{ false };

        // Moves asked (their place exactly as given, the frame they were asked on), for the run's hook (any thread: a lock).
        struct Asked {
            float   place[3]{};
            int64_t frame{ 0 };
            bool    used{ false };
        };
        std::mutex           g_ring_mutex;
        Asked                g_ring[kRing]{};
        int                  g_ring_next{ 0 };
        std::atomic<int64_t> g_ran{ 0 };
        std::atomic<int64_t> g_ran_lag[3]{}; // run in the frame asked, the next, later

        // ---- (fix185) The view tied to you while you climb ----
        // The game puts its camera root at the first-person skeleton's camera node, not at your character: while climbing it went 4-7 cm
        // off on average and moved against your climb (fix184's test). Tied, it is put at your character's place as asked this frame plus
        // where the game had your eyes from your feet (ahead, to the side and up, kept turned with your heading), measured standing still.
        constexpr int64_t kTieInUs   = 150'000;    // eased in over this as a hand takes hold...
        constexpr int64_t kTieOutUs  = 300'000;    // ...and back to the game's own camera over this after letting go
        constexpr float   kTieStill  = 0.005f;     // your eyes measured only while you and the camera root each move less than this a frame
        constexpr float   kTieFar    = 0.5f;       // the move asked this far from where you are (the game dropped it): tied to where you are
        constexpr int64_t kEyesStillKeepUs  = 10'000'000; // a measure standing still used up to this long after (the review: was 2 minutes)...
        constexpr int64_t kEyesMovingKeepUs = 2'000'000;  // ...else the height of the last one on the ground at all, this fresh
        struct Eyes {
            float   ahead{ 0.0f };
            float   side{ 0.0f };
            float   up{ 0.0f };
            bool    ok{ false };
            int64_t us{ 0 };
        };
        Eyes      g_eyes_still{};  // (main thread) measured standing still, averaged
        Eyes      g_eyes_moving{}; // (main thread) the last measured on the ground (a fallback)
        glm::vec3 g_eyes_prev_you{ 0.0f }, g_eyes_prev_root{ 0.0f };
        float     g_eyes_prev_heading{ 0.0f };
        bool      g_eyes_prev_ok{ false };
        struct Tie {
            bool      holding{ false };
            int64_t   start_us{ 0 };
            int64_t   end_us{ 0 };
            float     end_linear{ 0.0f }; // (the weight before its easing curve, as you let go)
            Eyes      eyes{};             // this climb's
            glm::vec3 place{ 0.0f };      // your place as asked this frame, plus your eyes (game world)
            bool      have_place{ false };
            int64_t   place_frame{ -1 };  // (the engine frame it was asked on)
            glm::vec3 prev_place{ 0.0f }; // (the review) the frame before's, for what is placed at a frame's start (ViewTieLead)
            bool      have_prev{ false };
        };
        std::mutex                   g_tie_mutex;
        Tie                          g_tie{};
        std::atomic<bool>            g_tie_ok{ false };     // (main thread, each frame) first person, no ladder, the headset
        std::atomic<bool>            g_tie_live{ false };   // tied or easing back now (so the many callers of ViewTie skip its lock otherwise)
        std::atomic<RE::NiAVObject*> g_tie_root{ nullptr }; // (main thread, each frame) the camera root while tied or easing out
        std::atomic<int64_t>         g_asked_frame{ -1 };   // the frame the last move was asked on...
        std::atomic<int64_t>         g_run_frame{ -1 };     // ...and the frame the last one ran on (OnMoveRun)
        // The log's numbers (the game's threads, at the root's update; taken each second while climbing).
        struct TieStats {
            int    passes{ 0 }, frames{ 0 };
            double off_sum{ 0.0 };
            float  off_max{ 0.0f };
            int    moving{ 0 }, against{ 0 };
            double step_sum{ 0.0 };
            float  step_max{ 0.0f };
            double change_sum{ 0.0 }, game_change_sum{ 0.0 };
            float  change_max{ 0.0f }, game_change_max{ 0.0f };
            int    steps{ 0 }, changes{ 0 };
            int    before{ 0 }, after{ 0 };
            int    refused{ 0 };
            int    repeats{ 0 };   // (the review) a second update of the root in a frame: the first's place written again
            int    untouched{ 0 }; // (the review) updates finding the root's place bit for bit as the mod last wrote it (the game had not put it)
        };
        struct TieTrack {
            int64_t               frame{ -1 };
            glm::vec3             placed{ 0.0f }, game{ 0.0f }, step{ 0.0f }, game_step{ 0.0f };
            bool                  have{ false }, have_step{ false };
            RE::NiPoint3          written{};          // the local place last written...
            int64_t               written_frame{ -1 }; // ...in this frame
            const RE::NiAVObject* written_obj{ nullptr };
            bool                  have_written{ false };
        };
        // (The review) Easing back after letting go, for one line as it ends: it needs the game to put the root again each frame.
        struct TieEase {
            int   passes{ 0 }, untouched{ 0 };
            float last_off{ 0.0f }; // the game's root from the tied place at the last update
            bool  any{ false }, at_once{ false };
        };
        std::mutex g_tie_stats_mutex;
        TieStats   g_tie_stats{};
        TieTrack   g_tie_track{};
        TieEase    g_tie_ease{};

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (fix180) Where each hand holds the wall, for the shown body (GripSurface; any thread: a lock).
        constexpr int64_t kGripEaseUs = 150'000;
        struct HeldSurface {
            glm::vec3 point{ 0.0f };
            glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
            bool      holding{ false };
            int64_t   start_us{ 0 };
            int64_t   end_us{ 0 };
            float     end_weight{ 0.0f };
        };
        std::mutex  g_surface_mutex;
        HeldSurface g_surface[2]{};

        float SurfaceWeight(const HeldSurface& s, int64_t now)
        {
            if (s.holding) {
                return std::clamp(static_cast<float>(now - s.start_us) / static_cast<float>(kGripEaseUs), 0.0f, 1.0f);
            }
            if (s.end_us == 0) {
                return 0.0f;
            }
            return std::clamp(s.end_weight * (1.0f - static_cast<float>(now - s.end_us) / static_cast<float>(kGripEaseUs)), 0.0f, 1.0f);
        }

        void HoldSurface(int h, const glm::vec3& point, const glm::vec3& normal, int64_t now)
        {
            std::scoped_lock _{ g_surface_mutex };
            auto& s    = g_surface[h];
            // (The review) Taken hold again while still easing off: eased in from where it was, not from 0.
            const float was = SurfaceWeight(s, now);
            s.point    = point;
            s.normal   = normal;
            s.holding  = true;
            s.start_us = now - static_cast<int64_t>(was * static_cast<float>(kGripEaseUs));
            s.end_us   = 0;
        }

        void LeaveSurface(int h, int64_t now)
        {
            std::scoped_lock _{ g_surface_mutex };
            auto& s = g_surface[h];
            if (!s.holding) {
                return;
            }
            s.end_weight = SurfaceWeight(s, now);
            s.holding    = false;
            s.end_us     = now;
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // Every read and write of the game's memory under a guard: a bad address fails instead of crashing the game.
        bool ReadRaw(void* dst, uintptr_t src, size_t n)
        {
            if (src < 0x10000) {
                return false;
            }
            __try {
                std::memcpy(dst, reinterpret_cast<const void*>(src), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        template <class T>
        bool Read(uintptr_t at, T& out)
        {
            return ReadRaw(&out, at, sizeof(T));
        }

        bool WriteFloat(uintptr_t at, float value)
        {
            if (at < 0x10000) {
                return false;
            }
            __try {
                *reinterpret_cast<volatile float*>(at) = value;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Where a rip-relative operand (its 4 bytes `disp_at` into the instruction, `length` long) points; 0 if unreadable.
        uintptr_t RipTarget(uintptr_t at, size_t disp_at, size_t length)
        {
            int32_t disp = 0;
            return Read(at + disp_at, disp) ? at + length + static_cast<intptr_t>(disp) : 0;
        }

        uintptr_t CallTarget(uintptr_t at)
        {
            uint8_t op = 0;
            return Read(at, op) && op == 0xE8 ? RipTarget(at, 1, 5) : 0;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there (in place
        // of the check of the bytes at the game's base plus the rva), and the address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // ---- You, as the game has you now ----
        struct You {
            uintptr_t player{ 0 };
            uintptr_t cell{ 0 };
            uintptr_t controller{ 0 };
            uintptr_t proxy{ 0 };
            glm::vec3 place{ 0.0f };
            float     heading{ 0.0f };
            uint32_t  filter{ 0 }; // your controller's filter info (0: not read)
        };

        bool ReadYou(You& y)
        {
            y.player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (y.player == 0) {
                return false;
            }
            float at[3]{};
            if (!ReadRaw(at, y.player + kRefLocation, sizeof(at))) {
                return false;
            }
            y.place = glm::vec3{ at[0], at[1], at[2] };
            if (!Finite(y.place) || !Read(y.player + kRefHeading, y.heading) || !std::isfinite(y.heading) || !Read(y.player + kRefCell, y.cell)) {
                return false;
            }
            uintptr_t process = 0, middle = 0;
            if (Read(y.player + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0 &&
                Read(middle + kMiddleController, y.controller) && y.controller != 0 && Read(y.controller + kControllerProxy, y.proxy) && y.proxy != 0) {
                Read(y.proxy + kProxyFilter, y.filter);
            }
            return true;
        }

        // ---- The game's ray pick ----
        struct Hit {
            bool      hit{ false };
            float     fraction{ 1.0f };
            float     distance{ 0.0f };
            glm::vec3 point{ 0.0f };
            glm::vec3 normal{ 0.0f };
            glm::vec3 way{ 0.0f }; // (fix180) the ray's own way (unit)
            uint32_t  layer{ 0 };
            uint32_t  filter{ 0 };
            uint32_t  body{ 0xFFFFFF };
            uintptr_t object{ 0 };
        };
        std::atomic<int64_t> g_picks{ 0 };

        // The pick made, then the world it held let go of (as the game's own callers do). A fault inside the game's pick would leave its
        // world's read lock held (a freeze rather than a crash) and its reference taken: an accepted risk; climbing is off after one.
        bool PickRaw(uintptr_t ctor, uintptr_t pick, uintptr_t release, uintptr_t cell, uint8_t* data, uint32_t filter, const float* from, const float* to,
            uintptr_t* object)
        {
            __try {
                reinterpret_cast<PickCtorFn>(ctor)(data, filter, from, to);
                data[kPickMaySkip] = 0; // (never put off for the frame's budget)
                *object            = reinterpret_cast<uintptr_t>(reinterpret_cast<CellPickFn>(pick)(reinterpret_cast<void*>(cell), data));
                void* world        = *reinterpret_cast<void**>(data + kPickWorld);
                if (world != nullptr) {
                    *reinterpret_cast<void**>(data + kPickWorld) = nullptr;
                    reinterpret_cast<ReleaseFn>(release)(world);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A ray from `from` to `to` (game world), cast as `layer` in your collision group. False: no pick made (not ready, or it faulted).
        bool Pick(const You& you, const glm::vec3& from, const glm::vec3& to, uint32_t layer, Hit& out)
        {
            out = Hit{};
            if (!g_ready.load() || you.cell == 0 || you.filter == 0 || !Finite(from) || !Finite(to)) {
                return false;
            }
            alignas(16) uint8_t data[kPickSize]{};
            const float         a[3]{ from.x, from.y, from.z };
            const float         b[3]{ to.x, to.y, to.z };
            const uint32_t      filter = (you.filter & 0xFFFF0000u) | layer;
            uintptr_t           object = 0;
            g_picks.fetch_add(1, std::memory_order_relaxed);
            if (!PickRaw(g_pick_ctor, g_cell_pick, g_world_release, you.cell, data, filter, a, b, &object)) {
                g_broken.store("the game's ray pick faulted");
                g_ready = false;
                spdlog::error("[Climb] (fix179) the game's ray pick faulted (cell {:x}): climbing is off for this session", you.cell);
                return false;
            }
            float fraction = 0.0f;
            std::memcpy(&fraction, data + kPickFraction, sizeof(fraction));
            if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0001f || data[kPickSkipped] != 0) {
                return true; // nothing on the way
            }
            float place[4]{}, normal[4]{};
            std::memcpy(place, data + kPickPlace, sizeof(place));
            std::memcpy(normal, data + kPickNormal, sizeof(normal));
            std::memcpy(&out.body, data + kPickBody, sizeof(out.body));
            std::memcpy(&out.filter, data + kPickHitFilter, sizeof(out.filter));
            out.hit      = true;
            out.fraction = fraction;
            out.distance = glm::length(to - from) * fraction;
            out.point    = glm::vec3{ place[0], place[1], place[2] };
            out.normal   = glm::vec3{ normal[0], normal[1], normal[2] };
            out.way      = glm::length(to - from) > 1e-6f ? (to - from) / glm::length(to - from) : glm::vec3{ 0.0f };
            out.layer    = out.filter & 0x7Fu;
            out.object   = object;
            // (Once: the place the pick gives against the way's fraction, for the log: they must agree.)
            static int told = 0;
            if (told < 2 && Finite(out.point)) {
                ++told;
                spdlog::info("[Climb] (fix179) a pick's hit: fraction {:.3f} of {:.2f} m, its place {:.3f} m from where the fraction puts it, layer {} ({}), "
                             "body {:x}, object {:x}",
                    fraction, glm::length(to - from), glm::length(out.point - (from + (to - from) * fraction)), out.layer, LayerName(out.layer), out.body,
                    out.object);
            }
            return true;
        }

        // ---- The queued move ----
        bool QueueMoveRaw(uintptr_t fn, uintptr_t actor, uintptr_t cell, const float* place, float heading)
        {
            __try {
                reinterpret_cast<MoveFn>(fn)(nullptr, reinterpret_cast<void*>(actor), reinterpret_cast<void*>(cell), place, heading, false, false);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool QueueMove(const You& you, const glm::vec3& to)
        {
            if (you.cell == 0 || !Finite(to)) {
                return false;
            }
            const float place[3]{ to.x, to.y, to.z };
            {
                std::scoped_lock _{ g_ring_mutex };
                Asked& a = g_ring[g_ring_next];
                g_ring_next = (g_ring_next + 1) % kRing;
                std::memcpy(a.place, place, sizeof(place));
                a.frame = g_frame.load(std::memory_order_relaxed);
                a.used  = true;
            }
            if (!QueueMoveRaw(g_queue_move, you.player, you.cell, place, you.heading)) {
                g_broken.store("the game's queued move faulted");
                g_ready = false;
                spdlog::error("[Climb] (fix179) the game's queued move faulted: climbing is off for this session");
                return false;
            }
            return true;
        }

        // (The game's thread, as a queued move runs, just before it sets the actor's heading.) One of the moves asked here: your heading as
        // it is now, so the move turns you nowhere (a snap turn or your head's turn made since it was asked is kept).
        void OnMoveRun(safetyhook::Context& ctx)
        {
            const uintptr_t data   = ctx.rbx;
            const uintptr_t player = g_player.load(std::memory_order_relaxed);
            uintptr_t       actor  = 0;
            if (player == 0 || !Read(data + kCmdActor, actor) || actor != player) {
                return;
            }
            float place[3]{};
            if (!ReadRaw(place, data + kCmdPlace, sizeof(place))) {
                return;
            }
            int64_t asked = -1;
            {
                std::scoped_lock _{ g_ring_mutex };
                for (auto& a : g_ring) {
                    if (a.used && std::memcmp(a.place, place, sizeof(place)) == 0) {
                        asked  = a.frame;
                        a.used = false;
                        break;
                    }
                }
            }
            if (asked < 0) {
                return; // the game's own move of you (a teleport, MoveTo): its heading as it asked
            }
            float heading = 0.0f;
            if (Read(player + kRefHeading, heading) && std::isfinite(heading)) {
                WriteFloat(data + kCmdHeading, heading);
            }
            g_ran.fetch_add(1, std::memory_order_relaxed);
            g_run_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed); // (fix185: before or after the root's update)
            const int64_t lag = std::clamp<int64_t>(g_frame.load(std::memory_order_relaxed) - asked, 0, 2);
            g_ran_lag[lag].fetch_add(1, std::memory_order_relaxed);
        }

        // ---- The state of each hand and of the climb ----
        enum Phase : int { kIdle = 0, kDeciding, kHolding, kRefused };
        struct HandState {
            Phase     phase{ kIdle };
            int64_t   pressed_us{ 0 };
            glm::vec3 prev_stage{ 0.0f };
        };
        HandState g_hands[2]{};
        bool      g_grip_was[2]{ false, false };

        bool      g_climbing{ false };
        int64_t   g_start_us{ 0 };
        int64_t   g_last_move_us{ 0 };
        glm::vec3 g_start_place{ 0.0f };
        glm::vec3 g_target{ 0.0f };
        glm::vec3 g_last_request{ 0.0f };
        bool      g_have_request{ false };
        glm::vec3 g_last_actual{ 0.0f };
        bool      g_have_actual{ false };
        // The gravity factor while you hold on.
        bool      g_gravity_zeroed{ false };
        float     g_gravity_saved{ 1.0f };
        uintptr_t g_gravity_controller{ 0 };
        int       g_gravity_rewrites{ 0 };
        // The last second's numbers, for the log.
        struct Second {
            int64_t start_us{ 0 };
            int     moves{ 0 };
            int64_t ran_at_start{ 0 };
            int64_t lag_at_start[3]{};
            float   err_sum{ 0.0f };
            float   err_max{ 0.0f };
            int     err_n{ 0 };
            int     cut{ 0 };
            int     floor{ 0 };
            int64_t picks_at_start{ 0 };
            float   asked{ 0.0f };
            int     on_ground{ 0 }, in_air{ 0 }, other_state{ 0 }, state_changes{ 0 }; // (fix185) your controller's state, frames
        };
        Second    g_second{};
        int64_t   g_last_state{ -1 }; // (fix185) your controller's state the frame before (-1: not read)
        int       g_climb_logs{ 0 };
        int       g_climbs{ 0 };
        float     g_asked_total{ 0.0f };
        // After letting go: where, and whether the game counts the fall from there.
        struct FallWatch {
            bool      on{ false };
            int64_t   us{ 0 };
            float     z{ 0.0f };
            uintptr_t controller{ 0 };
            bool      told{ false };
            bool      set{ false };
        };
        FallWatch g_fall{};

        // Refusals told at most this often each (a squeeze that is no climb is common).
        bool TellRefusal(const std::string& why)
        {
            static std::string last_why;
            static int64_t     last_us = 0;
            static int         told    = 0;
            const int64_t      now     = NowUs();
            if (told >= 300 || (why == last_why && now - last_us < 5'000'000)) {
                return false;
            }
            last_why = why;
            last_us  = now;
            ++told;
            return true;
        }

        // Why no hand may hold you on anything now (empty: it may).
        std::string Posture(const You& you);

        std::string Blocked(const You& you)
        {
            static auto vr       = VR::get();
            const auto& settings = GameFlow::gStore.internalSettings;
            if (!settings.climbing) {
                return "the setting Climbing is off";
            }
            if (vr == nullptr || !vr->is_hmd_active() || !vr->is_using_controllers()) {
                return "no headset or controllers";
            }
            if (std::string why = Posture(you); !why.empty()) {
                return why;
            }
            if (you.controller == 0 || you.proxy == 0 || (you.filter & 0x7Fu) != kLayerController || (you.filter >> 16) == 0) {
                return std::format("your character controller not as expected (filter info {:x})", you.filter);
            }
            return {};
        }

        // (fix185: also where the game's eyes are measured for the view tied to you) Why where you are and what you do now is no climb's
        // (empty: it may be).
        std::string Posture(const You& you)
        {
            if (ModSettings::showFlatScreenDisplay() || ModSettings::cameraFlat()) {
                return "the flat screen";
            }
            if (GameFlow::isLoadingScreenShown()) {
                return "a loading screen";
            }
            if (GameFlow::isShowingMenu()) {
                return "a menu";
            }
            if (GameFlow::isInDialogue()) {
                return "a conversation";
            }
            if (GameFlow::isWorkshopShown()) {
                return "build mode";
            }
            if (GameFlow::isPilotingShip()) {
                return "the pilot's seat";
            }
            if (GameFlow::isFavoritesShown() || FavoritesWheel::IsOpen()) {
                return "the favorites";
            }
            if (GameFlow::isScannerOpen() || HandActions::GripsAreShoulders()) {
                return "the scanner or the star map (the grips are LB and RB there)";
            }
            if (!GameFlow::isInFirstPerson()) {
                return "third person";
            }
            if (GameFlow::isOnLadder() || LadderClimb::ActivatedRecently() || LadderClimb::ViewAtHead()) {
                return "a ladder";
            }
            if (GameFlow::isControlledByAI()) {
                return "the game moves you now";
            }
            uint32_t state = 0, state2 = 0;
            if (Read(you.player + kActorState2, state2) && (state2 & 0x1800u) != 0) {
                return "a seat (sitting, lying, getting in or out)";
            }
            if (Read(you.player + kActorState, state) && ((state >> 17) & 0xFu) != 0) {
                return "you are down (dead, unconscious or bleeding out)";
            }
            if (!HandGrab::HandsFree()) {
                return "a weapon in your hands";
            }
            if (HandGrab::HoldingWith(true) || HandGrab::HoldingWith(false) || Physical::HoldingBody() || Physical::HoldingPerson(true) ||
                Physical::HoldingPerson(false) || Physical::PersonPending(true) || Physical::PersonPending(false)) {
                return "something or someone is held (or on the way to a hand)";
            }
            return {};
        }

        // Why this hand's squeeze is not the climb's (empty: it may be).
        std::string HandBlocked(bool left)
        {
            if (HandActions::GripIsShoulder(left)) {
                return "the grip is LB or RB here";
            }
            if (!left && HandActions::RightGripTaken()) {
                return "a grenade or a power took the grip";
            }
            if (left && LeftWeapon::Held()) {
                return "the left hand holds its weapon";
            }
            return {};
        }

        // A hand's grip place (the palm's middle) and its sideways way, game world, from its stage pose.
        bool HandWorld(bool left, glm::vec3& palm, glm::vec3& side, glm::vec3* stage = nullptr)
        {
            const glm::mat4 pose = tracking::GripPose(left);
            const glm::vec3 at{ pose[3] };
            if (!Finite(at)) {
                return false;
            }
            if (stage != nullptr) {
                *stage = at;
            }
            return tracking::StageToWorld(at, glm::vec3{ pose[0] }, palm, side);
        }

        bool HeadWorld(glm::vec3& head)
        {
            glm::vec3       way{ 0.0f };
            const glm::mat4 pose = tracking::HeadPose();
            return tracking::StageToWorld(glm::vec3{ pose[3] }, glm::vec3{ 0.0f, 0.0f, -1.0f }, head, way);
        }

        // ---- Holding on ----
        // A climbable surface at the hand: rays from the palm's middle along the world's six ways and across the palm both ways (kReach),
        // and from your eyes through the palm (a hand already inside a collider, which is bigger than what is drawn). The nearest climbable
        // hit wins; the nearest other one is told when there is none.
        struct Surface {
            bool        found{ false };
            Hit         hit{};
            int         ray{ -1 };
            Hit         other{};
            int         other_ray{ -1 };
            int         rays{ 0 };
            bool        back_seen{ false }; // (fix181) a climbable side facing away from you was met (your hand through it)
            uint32_t    back_body{ 0xFFFFFF };
            uintptr_t   back_object{ 0 };   // ...which body and object it was (its near side must be the same one's)
        };

        Surface FindSurface(const You& you, const glm::vec3& palm, const glm::vec3& side, const glm::vec3& head)
        {
            Surface   s{};
            glm::vec3 ways[8]{ { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
                { 0.0f, 0.0f, -1.0f }, side, -side };
            const auto take = [&](const Hit& h, int ray) {
                if (!h.hit) {
                    return;
                }
                // (fix180, the review) Not the floor or a table top under your waist when you are not climbing yet: reaching down for loot
                // with the wider reach would take hold of the ground (gravity off, and reaching on down lifts you).
                glm::vec3 out = h.normal;
                if (Finite(out) && glm::length(out) > 0.5f && glm::dot(out, h.way) > 0.0f) {
                    out = -out;
                }
                const bool floor_like = Finite(out) && glm::length(out) > 0.5f && glm::normalize(out).z > 0.7f && h.point.z < you.place.z + kWaistUp &&
                                        !g_climbing;
                // (fix181) Not a side facing away from you (after fix180 it sometimes grabbed the wall from the side facing away
                // from the player, so the hand went backwards; the log, 13 of 60 holds with the drawn hand turned 147-172 degrees: your hand
                // had gone through a thin wall or an edge, and a ray from the palm met its far side). The near side, from your eyes, below.
                // (The review) Only a steep side (a wall's face, not a ledge's top reached from below your eyes, nor an underside), and
                // clearly turned away (more than ~105 degrees from your eyes: the bad holds were turned 147-172), so a side seen edge-on
                // or round a corner still counts.
                bool faces_away = false;
                if (Finite(out) && glm::length(out) > 0.5f && glm::length(head - h.point) > 1e-3f) {
                    const glm::vec3 n = glm::normalize(out);
                    faces_away        = std::fabs(n.z) < 0.7f && glm::dot(n, glm::normalize(head - h.point)) < -0.25f;
                }
                if (Climbable(h.layer) && faces_away && !s.back_seen) {
                    s.back_seen   = true;
                    s.back_body   = h.body;
                    s.back_object = h.object;
                }
                if (Climbable(h.layer) && !floor_like && !faces_away) {
                    if (!s.found || h.distance < s.hit.distance) {
                        s.found = true;
                        s.hit   = h;
                        s.ray   = ray;
                    }
                } else if (s.other_ray < 0 || h.distance < s.other.distance) {
                    s.other     = h;
                    s.other_ray = ray;
                }
            };
            for (int i = 0; i < 8; ++i) {
                const float len = glm::length(ways[i]);
                if (!(len > 0.5f)) {
                    continue;
                }
                Hit h{};
                if (!Pick(you, palm, palm + ways[i] / len * kReach, kLayerLineOfSight, h)) {
                    return s;
                }
                ++s.rays;
                take(h, i);
            }
            // (fix180) The level diagonals too (rays 9-12: a wall at an angle to the world's ways), and rays from outside back to the palm
            // (13-17: from 0.3 m out level each way and from above; nothing drawn stops your hand, so it is often already inside the wall,
            // where a ray from the palm sees nothing): their hit is the wall's outside where the hand went in.
            const float     d = 0.70710678f;
            const glm::vec3 diagonals[4]{ { d, d, 0.0f }, { d, -d, 0.0f }, { -d, d, 0.0f }, { -d, -d, 0.0f } };
            for (int i = 0; i < 4; ++i) {
                Hit h{};
                if (!Pick(you, palm, palm + diagonals[i] * kReach, kLayerLineOfSight, h)) {
                    return s;
                }
                ++s.rays;
                take(h, 9 + i);
            }
            const glm::vec3 outside[5]{ { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
            for (int i = 0; i < 5; ++i) {
                // (The review) The level ones only from your side of the hand (a rail or thin panel past the hand: its far face is not
                // where the hand went in).
                if (i < 4 && glm::dot(outside[i], head - palm) <= 0.0f) {
                    continue;
                }
                Hit h{};
                if (!Pick(you, palm + outside[i] * kFromOutside, palm, kLayerLineOfSight, h)) {
                    return s;
                }
                ++s.rays;
                if (h.hit && Finite(h.point)) {
                    h.distance = glm::length(h.point - palm); // (from the palm, as the others)
                }
                // (The review) Only within the reach of the palm, as the others (else told as the nearest other thing).
                if (h.hit && (!Finite(h.point) || h.distance > kReach)) {
                    if (s.other_ray < 0 || h.distance < s.other.distance) {
                        s.other     = h;
                        s.other_ray = 13 + i;
                    }
                    continue;
                }
                take(h, 13 + i);
            }
            const glm::vec3 to_palm = palm - head;
            const float     reach   = glm::length(to_palm);
            if (reach > 0.05f && reach < 2.0f) {
                Hit h{};
                if (Pick(you, head, palm + to_palm / reach * kHeadBeyond, kLayerLineOfSight, h)) {
                    ++s.rays;
                    if (h.hit && Finite(h.point)) {
                        h.distance = glm::length(h.point - palm); // (told and compared as the other rays: from the palm)
                    }
                    // (Review) Only a surface at the hand: a counter, table or door frame between your eyes and the hand is not held.
                    // (fix181) Unless your hand went through a wall (its far side met): then its near side, the one facing you, up to
                    // kThrough from the palm.
                    // (The review: the same thing's near side, not a crate or frame in front of it.)
                    const bool same    = (s.back_object != 0 && h.object == s.back_object) || (s.back_body != 0xFFFFFF && h.body == s.back_body);
                    const bool through = s.back_seen && !s.found && h.hit && Finite(h.point) && h.distance <= kThrough && same;
                    if (h.hit && (!Finite(h.point) || h.distance > kReach) && !through) {
                        if (s.other_ray < 0 || h.distance < s.other.distance) {
                            s.other     = h;
                            s.other_ray = 8;
                        }
                    } else {
                        take(h, 8);
                    }
                }
            }
            return s;
        }

        // ---- The gravity factor ----
        void HoldGravity(const You& you)
        {
            if (!g_gravity_ok || you.controller == 0) {
                return;
            }
            if (g_gravity_zeroed && g_gravity_controller != you.controller) {
                g_gravity_zeroed = false; // (another controller: the old one is not yours any more)
            }
            float now = 0.0f;
            if (!Read(you.controller + kControllerGravity, now) || !std::isfinite(now)) {
                return;
            }
            if (now == 0.0f) {
                return; // (held at 0)
            }
            if (now < 0.0f || now > 10.0f) {
                return; // (not a gravity factor as the game writes them: left alone)
            }
            // The game wrote it (first, or again: a cell of another gravity): kept to put back, and 0 while you hold on.
            if (g_gravity_zeroed && ++g_gravity_rewrites <= 10) {
                spdlog::info("[Climb] (fix179) the game set your gravity factor again ({:.2f}, was {:.2f}): kept for when you let go", now, g_gravity_saved);
            }
            g_gravity_saved      = now;
            g_gravity_controller = you.controller;
            g_gravity_zeroed     = WriteFloat(you.controller + kControllerGravity, 0.0f);
        }

        // The gravity factor put back (-1: nothing to put back, or not yet). Only into the controller it was taken from, still yours (after a
        // load the old one may be gone, and a new one has the game's own); (review) one not readable now is tried again each frame
        // (PutGravityBackLater) while yours is that one.
        float PutGravityBack(const You& you)
        {
            if (!g_gravity_zeroed) {
                return -1.0f;
            }
            if (you.controller == 0) {
                return -1.0f; // (not read this frame: kept to try again)
            }
            if (you.controller != g_gravity_controller) {
                g_gravity_zeroed = false; // (another controller now: the game's own value is in it)
                return -1.0f;
            }
            float now = 0.0f;
            if (!Read(g_gravity_controller + kControllerGravity, now)) {
                return -1.0f; // (kept to try again)
            }
            if (now == 0.0f && !WriteFloat(g_gravity_controller + kControllerGravity, g_gravity_saved)) {
                return -1.0f;
            }
            g_gravity_zeroed = false; // (put back, or the game wrote its own meanwhile)
            return now == 0.0f ? g_gravity_saved : now;
        }

        void PutGravityBackLater(const You& you)
        {
            if (g_climbing || !g_gravity_zeroed) {
                return;
            }
            static int told = 0;
            if (const float put = PutGravityBack(you); put >= 0.0f && told < 20) {
                ++told;
                spdlog::info("[Climb] (fix179) your gravity factor put back to {:.2f} (it could not be as you let go)", put);
            }
        }

        // ---- (fix185) The view tied to you ----
        // Your heading's ways in the game world (the game's: ahead is (sin, cos) of data.angle.z), for your eyes kept turned with you.
        glm::vec3 EyesAt(const Eyes& e, float heading)
        {
            const glm::vec3 ahead{ std::sin(heading), std::cos(heading), 0.0f };
            const glm::vec3 side{ std::cos(heading), -std::sin(heading), 0.0f };
            return ahead * e.ahead + side * e.side + glm::vec3{ 0.0f, 0.0f, e.up };
        }

        Eyes EyesFrom(const glm::vec3& offset, float heading, int64_t now)
        {
            const glm::vec3 ahead{ std::sin(heading), std::cos(heading), 0.0f };
            const glm::vec3 side{ std::cos(heading), -std::sin(heading), 0.0f };
            return Eyes{ glm::dot(offset, ahead), glm::dot(offset, side), offset.z, true, now };
        }

        // How far tied, before the easing curve (0..1): rising over kTieInUs while a hand holds on, falling over kTieOutUs after.
        float TieLinear(const Tie& t, int64_t now)
        {
            if (t.holding) {
                return std::clamp(static_cast<float>(now - t.start_us) / static_cast<float>(kTieInUs), 0.0f, 1.0f);
            }
            if (t.end_us == 0) {
                return 0.0f;
            }
            return std::clamp(t.end_linear * (1.0f - static_cast<float>(now - t.end_us) / static_cast<float>(kTieOutUs)), 0.0f, 1.0f);
        }

        float TieWeight(const Tie& t, int64_t now)
        {
            const float x = TieLinear(t, now);
            return x * x * (3.0f - 2.0f * x); // (eased: no sudden start or stop)
        }

        // (Each frame, main thread, not climbing nor easing out) Where the game has your eyes from your feet: the camera root as it last
        // worked it out, less your place at that moment. Only as you could climb now (Posture: no menu, seat, conversation, pilot's seat,
        // scanner, build mode, the game moving you, a weapon in your hands (it moves the game's camera)...), on the ground, standing (the
        // review: not crouched or sneaking: your eyes no more than 0.15 m under your character's standing eye height, fix146's). Averaged
        // while you and the root stand still (and your heading does not turn); else only its height kept, as a fallback (the review:
        // while you walk the root is a frame's move behind you, 8-9 cm, so its ahead and side parts are not your eyes').
        void NoteEyes(const You& you, int64_t now)
        {
            glm::vec3 root{ 0.0f }, at{ 0.0f };
            uint32_t  state = 0xFFFFFFFFu;
            if (!tracking::LastRootWrite(root, at) || you.controller == 0 || !Read(you.controller + kControllerNow, state) ||
                state != kStateOnGround || !Posture(you).empty()) {
                g_eyes_prev_ok = false;
                return;
            }
            const glm::vec3 offset = root - at;
            // (A crouch lowers the game's camera about 0.6 m: under 1.3 m is never standing, as fix146 takes it.)
            const bool crouched = offset.z < 1.3f || (tracking::CharacterEyeHeightKnown() && offset.z < tracking::CharacterEyeHeight() - 0.15f);
            if (!Finite(offset) || offset.z > 2.5f || crouched || glm::length(glm::vec2{ offset.x, offset.y }) > 0.6f || glm::length(at - you.place) > 0.3f) {
                g_eyes_prev_ok = false;
                return;
            }
            const Eyes e  = EyesFrom(offset, you.heading, now);
            g_eyes_moving = e;
            const bool still = g_eyes_prev_ok && glm::length(you.place - g_eyes_prev_you) < kTieStill && glm::length(root - g_eyes_prev_root) < kTieStill &&
                               glm::length(at - you.place) < kTieStill && std::abs(you.heading - g_eyes_prev_heading) < 0.02f;
            if (still) {
                if (!g_eyes_still.ok || now - g_eyes_still.us > 2'000'000) {
                    g_eyes_still = e;
                } else {
                    g_eyes_still.ahead += (e.ahead - g_eyes_still.ahead) * 0.25f;
                    g_eyes_still.side += (e.side - g_eyes_still.side) * 0.25f;
                    g_eyes_still.up += (e.up - g_eyes_still.up) * 0.25f;
                    g_eyes_still.us = now;
                }
            }
            g_eyes_prev_you     = you.place;
            g_eyes_prev_root    = root;
            g_eyes_prev_heading = you.heading;
            g_eyes_prev_ok      = true;
        }

        // A hand takes hold (the climb starts): tied, eased in (from where it was if still easing out from the last climb, with that
        // climb's eyes).
        void TieStart(int64_t now)
        {
            std::string told;
            {
                std::scoped_lock _{ g_tie_mutex };
                const float was = TieLinear(g_tie, now);
                if (!(was > 0.0f) || !g_tie.eyes.ok) {
                    // (The review) A reading standing still wins only while fresh (kEyesStillKeepUs) and as high as a fresh one on the move
                    // (within 0.1 m): else the fresh one's height, with the still one's ahead and side (any age; none: straight above you).
                    const bool fresh_moving = g_eyes_moving.ok && now - g_eyes_moving.us < kEyesMovingKeepUs;
                    if (g_eyes_still.ok && now - g_eyes_still.us < kEyesStillKeepUs &&
                        (!fresh_moving || std::abs(g_eyes_moving.up - g_eyes_still.up) <= 0.1f)) {
                        g_tie.eyes = g_eyes_still;
                        told       = std::format("measured standing still {:.1f} s before", (now - g_eyes_still.us) * 1e-6);
                    } else if (fresh_moving) {
                        g_tie.eyes       = g_eyes_moving;
                        g_tie.eyes.ahead = g_eyes_still.ok ? g_eyes_still.ahead : 0.0f;
                        g_tie.eyes.side  = g_eyes_still.ok ? g_eyes_still.side : 0.0f;
                        told             = std::format("its height measured on the ground while moving, {:.1f} s before; ahead and side {}",
                            (now - g_eyes_moving.us) * 1e-6,
                            g_eyes_still.ok ? std::format("from standing still {:.0f} s before", (now - g_eyes_still.us) * 1e-6) : std::string{ "none (never measured standing still)" });
                    } else {
                        g_tie.eyes = Eyes{};
                    }
                } else {
                    told = "kept from the climb before (still easing back)";
                }
                g_tie.holding    = true;
                g_tie.start_us   = now - static_cast<int64_t>(was * static_cast<float>(kTieInUs));
                g_tie.end_us     = 0;
                g_tie.end_linear = 0.0f;
                g_tie.have_place = false;
                g_tie.have_prev  = false;
                g_tie_live.store(g_tie.eyes.ok, std::memory_order_relaxed);
                if (g_tie.eyes.ok) {
                    const Eyes& e = g_tie.eyes;
                    told = std::format("your eyes {:.2f} m above your feet, {:+.2f} m ahead, {:+.2f} m to the right ({})", e.up, e.ahead, e.side, told);
                }
            }
            {
                std::scoped_lock _{ g_tie_stats_mutex };
                g_tie_stats = TieStats{};
                g_tie_track = TieTrack{};
            }
            g_last_state = -1;
            if (g_climb_logs < 400) {
                ++g_climb_logs;
                if (!told.empty()) {
                    spdlog::info("[Climb] (fix185) the view tied to your character while you hold on (eased in over {:.2f} s, back to the game's camera "
                                 "over {:.2f} s after letting go): {}",
                        kTieInUs * 1e-6, kTieOutUs * 1e-6, told);
                } else {
                    spdlog::info("[Climb] (fix185) the view not tied to you this climb: where the game has your eyes was never measured on the ground "
                                 "with your hands free (the game's camera as before)");
                }
            }
        }

        // Every hand let go: eased back to the game's camera (`at_once`: a load or a teleport, at once).
        void TieEnd(int64_t now, bool at_once)
        {
            bool was_holding = false;
            {
                std::scoped_lock _{ g_tie_mutex };
                was_holding = g_tie.holding;
                if (g_tie.holding) {
                    g_tie.end_linear = TieLinear(g_tie, now);
                    g_tie.holding    = false;
                    g_tie.end_us     = now;
                }
                if (at_once) {
                    g_tie.end_linear = 0.0f;
                }
                g_tie.have_place = false;
                g_tie.have_prev  = false;
            }
            std::scoped_lock _{ g_tie_stats_mutex };
            if (was_holding) {
                g_tie_ease = TieEase{};
            }
            g_tie_ease.at_once |= at_once;
        }

        // (Each frame, the end of Update) Whether the view may be tied now, and the camera root to tie (none when not tied).
        void TieFrame(int64_t now)
        {
            static auto vr = VR::get();
            const bool  ok = g_ready.load() && vr != nullptr && vr->is_hmd_active() && !ModSettings::cameraFlat() && GameFlow::isInFirstPerson() &&
                            !GameFlow::isOnLadder() && !LadderClimb::ViewAtHead() && !LadderClimb::ActivatedRecently() &&
                            !GameFlow::isLoadingScreenShown();
            bool live = false;
            {
                std::scoped_lock _{ g_tie_mutex };
                live = g_tie.eyes.ok && (g_tie.holding || TieLinear(g_tie, now) > 0.0f);
            }
            g_tie_ok.store(ok, std::memory_order_relaxed);
            // (The review) The view back on the game's camera: one line (the easing back needs the game to put the root each frame).
            static bool was_live = false;
            if (was_live && !live && g_climb_logs < 400) {
                TieEase e{};
                {
                    std::scoped_lock _{ g_tie_stats_mutex };
                    e = g_tie_ease;
                }
                ++g_climb_logs;
                spdlog::info("[Climb] (fix185) the view back on the game's camera ({}): {}", e.at_once ? "at once: a load or a teleport" : "eased back",
                    e.any ? std::format("the game's root {:.3f} m from the tied place at the last of {} updates while easing back; {} of them found the "
                                        "root's place as the mod left it (the game had not put it again)",
                                e.last_off, e.passes, e.untouched)
                          : std::string{ "no update of the root while easing back" });
            }
            was_live = live;
            g_tie_live.store(live, std::memory_order_relaxed);
            g_tie_root.store(ok && live ? const_cast<RE::NiAVObject*>(tracking::WatchedRoot()) : nullptr, std::memory_order_release);
        }

        // The engine's frame number (set before tracking::Snapshot, so the same all through a frame, before Climbing runs too).
        int64_t EngineFrame()
        {
            static auto vr = VR::get();
            return vr != nullptr ? static_cast<int64_t>(vr->m_engine_frame_count.load()) : 0;
        }

        // This frame's move asked: the place the view is tied to (with your heading as the move keeps it).
        void TiePlace(const glm::vec3& asked, float heading)
        {
            const int64_t    frame = EngineFrame();
            std::scoped_lock _{ g_tie_mutex };
            if (g_tie.holding && g_tie.eyes.ok) {
                g_tie.have_prev   = g_tie.have_place && frame - g_tie.place_frame == 1;
                g_tie.prev_place  = g_tie.place;
                g_tie.place       = asked + EyesAt(g_tie.eyes, heading);
                g_tie.have_place  = Finite(g_tie.place);
                g_tie.place_frame = frame;
            }
        }

        TieStats TakeTieStats()
        {
            std::scoped_lock _{ g_tie_stats_mutex };
            const TieStats s = g_tie_stats;
            g_tie_stats      = TieStats{};
            return s;
        }

        // The camera root's own place in its parent's frame and that parent's world transform, read under a guard.
        bool ReadRootFrame(const RE::NiAVObject* obj, RE::NiPoint3& local, RE::NiTransform& parent, bool& has_parent)
        {
            __try {
                local      = obj->local.translate;
                has_parent = obj->parent != nullptr;
                if (has_parent) {
                    std::memcpy(&parent, &obj->parent->world, sizeof(parent));
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool WriteRootPlace(RE::NiAVObject* obj, const RE::NiPoint3& local)
        {
            __try {
                obj->local.translate = local;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void StartClimb(const You& you, int64_t now)
        {
            g_climbing        = true;
            g_active          = true;
            g_start_us        = now;
            g_last_move_us    = now;
            g_start_place     = you.place;
            g_target          = you.place;
            g_have_request    = false;
            g_last_actual     = you.place;
            g_have_actual     = true;
            g_asked_total     = 0.0f;
            g_second          = Second{};
            g_second.start_us = now;
            g_second.ran_at_start = g_ran.load();
            for (int i = 0; i < 3; ++i) {
                g_second.lag_at_start[i] = g_ran_lag[i].load();
            }
            g_second.picks_at_start = g_picks.load();
            g_gravity_rewrites      = 0;
            g_fall                  = FallWatch{};
            ++g_climbs;
            HoldGravity(you);
            TieStart(now); // (fix185)
            static bool told = false;
            if (!told) {
                told = true;
                float fall = 0.0f;
                Read(you.controller + kControllerFall, fall);
                spdlog::info("[Climb] (fix179) your character controller {:x} (proxy {:x}, filter info {:08x}: the rays are cast in group {}), fall height "
                             "{:.2f}, cell {:x}",
                    you.controller, you.proxy, you.filter, you.filter >> 16, fall, you.cell);
            }
        }

        void LetGo(const You& you, const std::string& why)
        {
            if (!g_climbing) {
                return;
            }
            g_climbing          = false;
            g_active            = false;
            const int64_t now   = NowUs();
            TieEnd(now, false); // (fix185: the view eased back to the game's camera)
            for (int h = 0; h < 2; ++h) {
                g_holds[h] = false;
                LeaveSurface(h, now); // (fix180: the drawn hand eased off the wall)
                // (A grip still squeezed takes hold of nothing again until it is let go.)
                g_hands[h].phase = g_grip_was[h] ? kRefused : kIdle;
            }
            const float gravity = PutGravityBack(you);
            // The fall from here: the game's own (its fall height set from where you are by its in-air update; watched for a moment). Where
            // you let go: the last move asked (it may run after this), else where you are.
            g_fall            = FallWatch{};
            g_fall.on         = you.controller != 0;
            g_fall.us         = now;
            g_fall.z          = g_have_request && glm::length(g_last_request - you.place) < 0.5f ? g_last_request.z : you.place.z;
            g_fall.controller = you.controller;
            if (g_climb_logs < 400) {
                ++g_climb_logs;
                spdlog::info("[Climb] (fix179) let go ({}) after {:.1f} s: at {:.2f},{:.2f},{:.2f}, {:+.2f} m up from where you took hold ({:.2f} m pulled "
                             "in all); gravity factor {}; a fall from here is the game's own (its gravity and fall damage, counted from where you let go)",
                    why, (now - g_start_us) * 1e-6, you.place.x, you.place.y, you.place.z, you.place.z - g_start_place.z, g_asked_total,
                    gravity >= 0.0f    ? std::format("put back to {:.2f}", gravity)
                    : g_gravity_zeroed ? std::string{ "not readable now: put back as soon as it is" }
                                       : std::string{ "not changed" });
            }
        }

        // After letting go: the game's fall height should be where you let go. Its in-air update sets it from where you are (with the
        // gravity factor 0 meanwhile, and as you rise); below the world's zero height its own test (a new height only above the one it has)
        // leaves it at 0, which would count no fall at all: then it is set here, once you are falling. (Review) Only ever in the controller's
        // in-air state, and the watch ends as you are on the ground again (let go standing: nothing to count, and a grounded controller is
        // never given a height).
        void WatchFall(const You& you, int64_t now)
        {
            if (!g_fall.on) {
                return;
            }
            if (now - g_fall.us > kFallWatchUs || you.controller != g_fall.controller || !g_state_ok) {
                g_fall.on = false;
                return;
            }
            float    height = 0.0f;
            uint32_t state  = 0xFFFFFFFFu;
            if (!Read(g_fall.controller + kControllerFall, height) || !std::isfinite(height) || !Read(g_fall.controller + kControllerNow, state)) {
                g_fall.on = false;
                return;
            }
            if (state == kStateOnGround) {
                g_fall.on = false; // (landed, or never left the ground)
                return;
            }
            if (state != kStateInAir) {
                return; // (jumping, swimming, flying...: the game's own; watched on)
            }
            if (height != 0.0f) {
                if (!g_fall.told) {
                    g_fall.told = true;
                    if (g_climb_logs < 400) {
                        ++g_climb_logs;
                        spdlog::info("[Climb] (fix179) the game counts the fall from {:.2f} ({:+.2f} m from where you let go{})", height, height - g_fall.z,
                            g_fall.set ? ", set by the mod" : "");
                    }
                }
                return;
            }
            bool          pending = false;
            const int64_t frame   = g_frame.load(std::memory_order_relaxed);
            {
                std::scoped_lock _{ g_ring_mutex };
                for (const auto& a : g_ring) {
                    pending |= a.used && frame - a.frame < 10; // (a move the game dropped, its cell changed, is not waited for)
                }
            }
            // (Only once the last move asked has run, and you are falling: standing where you let go, there is no fall to count.)
            if (!g_fall.set && !pending && you.place.z < g_fall.z - 0.05f && WriteFloat(g_fall.controller + kControllerFall, g_fall.z)) {
                g_fall.set  = true;
                g_fall.told = false;
            }
        }

        // How far part of your body may go from `from` along `way` (unit), up to `length` m: cast as the line-of-sight and the camera's
        // layers (water passed over), stopping `gap` before what either hits. -1: a pick could not be made (climbing turned off).
        float Clear(const You& you, const glm::vec3& from, const glm::vec3& way, float length, float gap)
        {
            float allowed = length;
            if (!Finite(from)) {
                return allowed;
            }
            for (const uint32_t layer : { kLayerLineOfSight, kLayerCamera }) {
                Hit h{};
                if (!Pick(you, from, from + way * (length + gap), layer, h)) {
                    return -1.0f;
                }
                if (h.hit && h.layer != kLayerWater) {
                    allowed = std::min(allowed, std::max(0.0f, h.distance - gap));
                    // (The first few, to see what stops a move: never you, nor triggers, if the game's filter takes the rays' layers as
                    // expected.)
                    static int told = 0;
                    if (told < 12) {
                        ++told;
                        spdlog::info("[Climb] (fix179) a move cut short: cast as layer {} it met {} (layer {}, filter info {:08x}, body {:x}) {:.2f} m "
                                     "away",
                            layer, LayerName(h.layer), h.layer, h.filter, h.body, h.distance);
                    }
                }
            }
            return allowed;
        }

        // A move from `base` to `target` with nothing solid on the way, its level part and its rise each cut short on their own (so you slide
        // up a wall you pull yourself against): the level part cast from your feet, waist and eyes, the rise from your eyes (a move down is
        // stopped at the ground afterwards). False: a pick could not be made.
        bool ClearMove(const You& you, const glm::vec3& base, const glm::vec3& eyes, glm::vec3& target, bool& cut)
        {
            const glm::vec3 off = target - base;
            const glm::vec3 level{ off.x, off.y, 0.0f };
            const float     level_len = glm::length(level);
            glm::vec3       allowed_level{ 0.0f };
            if (level_len > 1e-4f) {
                const glm::vec3 way = level / level_len;
                float           go  = level_len;
                const glm::vec3 from[3]{ base + glm::vec3{ 0.0f, 0.0f, kFeetUp }, base + glm::vec3{ 0.0f, 0.0f, kWaistUp }, eyes };
                const float     gap[3]{ kBodyGap, kBodyGap, kHeadGap };
                for (int i = 0; i < 3; ++i) {
                    const float c = Clear(you, from[i], way, go, gap[i]);
                    if (c < 0.0f) {
                        return false;
                    }
                    go = std::min(go, c);
                }
                cut |= go < level_len - 1e-4f;
                allowed_level = way * go;
            }
            float rise = off.z;
            if (rise > 1e-4f) {
                const float c = Clear(you, eyes + allowed_level, glm::vec3{ 0.0f, 0.0f, 1.0f }, rise, kHeadGap);
                if (c < 0.0f) {
                    return false;
                }
                cut |= c < rise - 1e-4f;
                rise = c;
            }
            target = base + allowed_level + glm::vec3{ 0.0f, 0.0f, rise };
            return true;
        }

        void Move(const You& you, int64_t now)
        {
            const auto& settings = GameFlow::gStore.internalSettings;
            const float dt       = std::clamp(static_cast<float>(now - g_last_move_us) * 1e-6f, 0.0f, 0.1f);
            g_last_move_us       = now;
            // The holding hands' motion in the room (each frame's, mapped into the game world as the body draws the hands), averaged.
            glm::vec3 sum{ 0.0f };
            int       n = 0;
            for (int h = 0; h < 2; ++h) {
                if (g_hands[h].phase != kHolding) {
                    continue;
                }
                const glm::vec3 stage{ tracking::GripPose(h == 0)[3] };
                glm::vec3       a{}, b{}, way{};
                if (Finite(stage) && tracking::StageToWorld(stage, glm::vec3{ 0.0f, 0.0f, -1.0f }, a, way) &&
                    tracking::StageToWorld(g_hands[h].prev_stage, glm::vec3{ 0.0f, 0.0f, -1.0f }, b, way)) {
                    const glm::vec3 d = a - b;
                    if (Finite(d) && glm::length(d) < kHandJump) {
                        sum += d;
                        ++n;
                    }
                }
                if (Finite(stage)) {
                    g_hands[h].prev_stage = stage;
                }
            }
            const float pull = std::clamp(settings.climbPull, 1.0f, 3.0f);
            if (n > 0) {
                const glm::vec3 step = -(sum / static_cast<float>(n)) * pull;
                g_target += step;
                g_asked_total += glm::length(step);
                g_second.asked += glm::length(step);
            }
            // Where you are (a load or a teleport: let go).
            const glm::vec3 actual = you.place;
            if (g_have_actual && glm::length(actual - g_last_actual) > kTeleport) {
                LetGo(you, "you were moved over 2 m at once (a load or a teleport)");
                TieEnd(now, true); // (fix185: the game's camera at once)
                return;
            }
            g_last_actual = actual;
            g_have_actual = true;
            // The move asked the frame before against where you are now (for the log); from there on if it has not run yet (or just has).
            glm::vec3 base = actual;
            if (g_have_request) {
                const float err = glm::length(actual - g_last_request);
                g_second.err_sum += err;
                g_second.err_max = std::max(g_second.err_max, err);
                ++g_second.err_n;
                if (err < 0.03f) {
                    base = g_last_request;
                }
            }
            // Where the hands pull you, kept within reach of where you are.
            glm::vec3 off = g_target - base;
            if (const float d = glm::length(off); d > kTargetMax) {
                g_target = base + off * (kTargetMax / d);
            }
            // Nothing solid on the way: the move cut short a gap before it (its level part and its rise each on their own).
            glm::vec3 eyes{ 0.0f };
            if (glm::length(g_target - base) > 1e-4f && HeadWorld(eyes)) {
                // (The eyes as the camera has them now, with you where the move starts. (fix185, the review) Tied, the camera already sits
                // on the move asked: nothing to add.)
                if (glm::vec3 tied{}; !(ViewTie(tied) > 0.0f)) {
                    eyes += base - actual;
                }
                bool cut = false;
                if (!ClearMove(you, base, eyes, g_target, cut)) {
                    return;
                }
                if (cut) {
                    ++g_second.cut;
                }
            }
            // Eased toward it (VR Climbing's 13 a second): the hand keeps close to where it took hold, without jolts.
            const float factor = 1.0f - std::exp(-kEase * dt);
            glm::vec3   next   = base + (g_target - base) * std::clamp(factor, 0.0f, 1.0f);
            // A move down stops at the ground under you.
            if (next.z < base.z - 1e-4f) {
                Hit             h{};
                const glm::vec3 from = base + glm::vec3{ 0.0f, 0.0f, kFloorProbe };
                const glm::vec3 to{ next.x, next.y, next.z - 0.03f };
                if (!Pick(you, from, to, kLayerLineOfSight, h)) {
                    return;
                }
                if (h.hit && next.z < h.point.z) {
                    next.z     = h.point.z;
                    g_target.z = std::max(g_target.z, h.point.z);
                    ++g_second.floor;
                }
            }
            // Asked every frame (each run also zeroes the controller's speed: you hang where you are).
            if (QueueMove(you, next)) {
                g_last_request = next;
                g_have_request = true;
                ++g_second.moves;
                // (fix185) The view tied to this place (the move runs later this frame or at once: the same either way).
                TiePlace(next, you.heading);
                g_asked_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            HoldGravity(you);
            // (fix185) Your controller's state each frame (0 on the ground, 2 in the air), and how often it changed.
            {
                uint32_t state = 0xFFFFFFFFu;
                if (you.controller != 0 && Read(you.controller + kControllerNow, state)) {
                    if (state == kStateOnGround) {
                        ++g_second.on_ground;
                    } else if (state == kStateInAir) {
                        ++g_second.in_air;
                    } else {
                        ++g_second.other_state;
                    }
                    if (g_last_state >= 0 && static_cast<uint32_t>(g_last_state) != state) {
                        ++g_second.state_changes;
                    }
                    g_last_state = state;
                }
            }
            // Each second: what was asked and what the game did with it.
            if (now - g_second.start_us >= 1'000'000) {
                if (g_climb_logs < 400) {
                    ++g_climb_logs;
                    float     gravity = -1.0f, fall = 0.0f;
                    int32_t   state   = -1;
                    float     v[4]{};
                    Read(you.controller + kControllerGravity, gravity);
                    Read(you.controller + kControllerFall, fall);
                    Read(you.controller + kControllerNow, state);
                    ReadRaw(v, you.proxy + kProxyVelocity, sizeof(v));
                    const int64_t ran = g_ran.load() - g_second.ran_at_start;
                    spdlog::info("[Climb] (fix179) holding on ({}{}{}) {:.1f} s: the last second {} moves asked ({:.2f} m pulled, x{:.1f}), {} run by the "
                                 "game ({} in the frame asked, {} a frame later, {} later); you {:.3f} m from the move asked the frame before on average "
                                 "({:.3f} at most); {} cut short by something on the way, {} stopped at the ground; {} picks; gravity factor {:.2f} (kept "
                                 "{:.2f}), speed {:.2f} m/s, fall height {:.2f}, controller state {}; {:+.2f} m up from where you took hold",
                        g_hands[0].phase == kHolding ? "left" : "", g_hands[0].phase == kHolding && g_hands[1].phase == kHolding ? " and " : "",
                        g_hands[1].phase == kHolding ? "right" : "", (now - g_start_us) * 1e-6, g_second.moves, g_second.asked, pull, ran,
                        g_ran_lag[0].load() - g_second.lag_at_start[0], g_ran_lag[1].load() - g_second.lag_at_start[1],
                        g_ran_lag[2].load() - g_second.lag_at_start[2], g_second.err_n > 0 ? g_second.err_sum / g_second.err_n : 0.0f,
                        g_second.err_max, g_second.cut, g_second.floor, g_picks.load() - g_second.picks_at_start, gravity, g_gravity_saved,
                        glm::length(glm::vec3{ v[0], v[1], v[2] }), fall, state, actual.z - g_start_place.z);
                    // (fix185) The view tied to you: the game's own camera root against the tied place, and how steady each moved.
                    const TieStats t = TakeTieStats();
                    float          w = 0.0f;
                    {
                        std::scoped_lock _{ g_tie_mutex };
                        w = TieWeight(g_tie, now);
                    }
                    spdlog::info("[Climb] (fix185) the view tied to you, the last second (weight now {:.2f}{}): {} camera root updates in {} frames ({} "
                                 "before this frame's move ran, {} after; {} refused as too far; {} a second in the same frame, given the first's "
                                 "place; {} found the root as the mod left it, not put again by the game); the game's own root {:.3f} m from the tied place on "
                                 "average ({:.3f} at most), moving against your move in {} of the {} frames it moved over 5 mm; the tied view's step "
                                 "{:.3f} m on average ({:.3f} at most), its change frame to frame {:.4f} ({:.4f} at most) (the game's own root's: {:.4f} "
                                 "({:.4f})); your controller {} frames on the ground, {} in the air, {} other, {} changes",
                        w, g_tie_ok.load() ? "" : ", not tied now: not in first person, a ladder, a loading screen or the flat screen", t.passes,
                        t.frames, t.before, t.after, t.refused, t.repeats, t.untouched, t.frames > 0 ? t.off_sum / t.frames : 0.0, t.off_max, t.against,
                        t.moving,
                        t.steps > 0 ? t.step_sum / t.steps : 0.0, t.step_max, t.changes > 0 ? t.change_sum / t.changes : 0.0, t.change_max,
                        t.changes > 0 ? t.game_change_sum / t.changes : 0.0, t.game_change_max, g_second.on_ground, g_second.in_air,
                        g_second.other_state, g_second.state_changes);
                }
                g_second          = Second{};
                g_second.start_us = now;
                g_second.ran_at_start = g_ran.load();
                for (int i = 0; i < 3; ++i) {
                    g_second.lag_at_start[i] = g_ran_lag[i].load();
                }
                g_second.picks_at_start = g_picks.load();
            }
        }

        // A hand's squeeze decided: a climbable surface at it holds you on.
        void TryHold(const You& you, int h, int64_t now)
        {
            const bool left = h == 0;
            auto&      st   = g_hands[h];
            glm::vec3  palm{}, side{}, stage{}, eyes{};
            if (!HandWorld(left, palm, side, &stage) || !HeadWorld(eyes)) {
                st.phase = kRefused;
                return;
            }
            const Surface s = FindSurface(you, palm, side, eyes);
            if (!g_ready.load()) {
                return;
            }
            if (!s.found) {
                st.phase = kRefused;
                const std::string what = s.other_ray >= 0 ? std::format("the nearest thing there is {} (layer {}, {:.2f} m, ray {})", LayerName(s.other.layer),
                                                                s.other.layer, s.other.distance, s.other_ray)
                                                          : std::string{ "nothing there" };
                if (TellRefusal(std::string{ "surface" } + (left ? "L" : "R"))) {
                    spdlog::info("[Climb] (fix179) {} grip: nothing climbable within {:.2f} m of the palm ({} rays; {}){}", left ? "left" : "right", kReach,
                        s.rays, what, s.back_seen ? "; (fix181) only a side facing away from you met (your hand through it), its near side not found" : "");
                }
                return;
            }
            st.phase      = kHolding;
            st.prev_stage = stage;
            g_holds[h]    = true;
            // (fix180) The surface where it took hold, for the shown hand: the pick's own normal, turned to face back along the ray (out of
            // the wall, toward where the ray came from); none usable: back along the ray.
            {
                glm::vec3 normal = s.hit.normal;
                if (!Finite(normal) || glm::length(normal) < 0.5f) {
                    normal = -s.hit.way;
                }
                if (glm::length(normal) > 1e-4f) {
                    normal = glm::normalize(normal);
                    if (glm::dot(normal, s.hit.way) > 0.0f) {
                        normal = -normal;
                    }
                } else {
                    normal = glm::vec3{ 0.0f, 0.0f, 1.0f };
                }
                HoldSurface(h, s.hit.point, normal, now);
            }
            HandGrab::LeaveGrip(left); // (its gravity gloves' aim ends; no grab with this squeeze)
            haptics::MeleePulse(left, 0.35f, 0.05f);
            const bool first = !g_climbing;
            if (first) {
                StartClimb(you, now);
            }
            if (g_climb_logs < 400) {
                ++g_climb_logs;
                // (fix180) Which kind of ray found it: from the palm (0-7), from your eyes (8), the palm's level diagonals (9-12), from outside
                // back to the palm (13-17: the hand was in the wall).
                const char* kind = s.ray <= 7 ? "from the palm" : s.ray == 8 ? "from your eyes" : s.ray <= 12 ? "from the palm, at an angle" : "from outside: your hand was in it";
                spdlog::info("[Climb] (fix180) your {} hand holds on{}: {} (layer {}) {:.2f} m from the palm (ray {}, {}; {} cast), at {:.2f},{:.2f},{:.2f}, "
                             "{:.2f} s after the squeeze; you at {:.2f},{:.2f},{:.2f}{}",
                    left ? "left" : "right", first ? "" : " too", LayerName(s.hit.layer), s.hit.layer, s.hit.distance, s.ray, kind, s.rays, s.hit.point.x,
                    s.hit.point.y, s.hit.point.z, (now - st.pressed_us) * 1e-6, you.place.x, you.place.y, you.place.z,
                    first ? std::format("; gravity factor {:.2f} {}", g_gravity_saved, g_gravity_zeroed ? "set to 0 while you hold on" : "left as it is")
                          : std::string{});
            }
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        const uintptr_t b = g_base;
        if (b == 0) {
            g_installed = "the game's module was not found";
            spdlog::warn("[Climb] (fix179) climbing is off: {}", g_installed);
            return;
        }
        // Each piece as in Starfield 1.16.236's file, else climbing stays off (logged with which).
        std::string differs;
        const auto  need = [&](bool ok, const char* what) {
            if (!ok) {
                differs += differs.empty() ? what : std::string{ ", " } + what;
            }
        };
        need(SameCode(kPickCtorRva, kPickCtorStart, sizeof(kPickCtorStart)) &&
                 SameCode(kPickCtorFilterAt, kPickCtorFilter, sizeof(kPickCtorFilter)),
            "the pick data's constructor");
        need(SameCode(kCellPickRva, kCellPickStart, sizeof(kCellPickStart)) && CallTarget(At(kCellPickCallAt)) == At(kBhkPickRva) &&
                 SameCode(kCellPickLayerAt, kCellPickLayer, sizeof(kCellPickLayer)),
            "the cell's pick");
        need(SameCode(kBhkPickRva, kBhkPickStart, sizeof(kBhkPickStart)) && SameCode(kBhkPickCopyAt, kBhkPickCopy, sizeof(kBhkPickCopy)) &&
                 SameCode(kBhkPickBodyAt, kBhkPickBody, sizeof(kBhkPickBody)) &&
                 SameCode(kBhkPickFilterAt, kBhkPickFilter, sizeof(kBhkPickFilter)),
            "the bhk pick's hit");
        need(SameCode(kBhkPickWorldAt, kBhkPickWorld, sizeof(kBhkPickWorld)) && CallTarget(At(kBhkPickWorldAt + sizeof(kBhkPickWorld) - 1)) ==
                                                                                   At(kWorldReleaseRva) &&
                 At(kWorldHoldRva) != 0 && CallTarget(At(kBhkPickHoldAt)) == At(kWorldHoldRva) && // (fix190: 0 never a match)
                 SameCode(kWorldReleaseRva, kWorldRelease, sizeof(kWorldRelease)),
            "the bhk pick's hold on its world (and its let-go)");
        uintptr_t run_slot_10 = 0, run_slot_18 = 0;
        Read(At(kQueueMoveTable) + 0x10, run_slot_10);
        Read(At(kQueueMoveTable) + 0x18, run_slot_18);
        need(SameCode(kQueueMoveRva, kQueueMoveStart, sizeof(kQueueMoveStart)) && At(kQueueMoveTable) != 0 &&
                 RipTarget(At(kQueueMoveTableAt), 3, 7) == At(kQueueMoveTable) && At(kQueueMovePost) != 0 &&
                 CallTarget(At(kQueueMovePostAt)) == At(kQueueMovePost), // (fix190: 0 never a match)
            "the queued move");
        // (fix190) Also its hooked place below known in this game version (GameCode: 0 when not), else nothing is hooked.
        need((run_slot_10 == At(kMoveThunkRva) || run_slot_18 == At(kMoveThunkRva)) && SameCode(kMoveThunkRva, kMoveThunk, sizeof(kMoveThunk)) &&
                 RipTarget(At(kMoveThunkRva + 4), 1, 5) == At(kMoveRunRva) && SameCode(kMoveRunRva, kMoveRun, sizeof(kMoveRun)) &&
                 At(kMoveRunRva + kMoveRunHeading) != 0,
            "the queued move's run");
        uintptr_t player_slot = 0;
        Read(At(kPlayerTableRva) + kSetPositionSlot, player_slot);
        need(player_slot == At(kSetPositionRva) && SameCode(kSetPositionRva, kSetPositionStart, sizeof(kSetPositionStart)) &&
                 SameCode(kSetPositionControllerAt, kSetPositionController, sizeof(kSetPositionController)) &&
                 SameCode(kSetPositionFallAt, kSetPositionFall, sizeof(kSetPositionFall)),
            "your SetPosition (the controller and its fall height)");
        need(SameCode(kFallDamageReadAt, kFallDamageRead, sizeof(kFallDamageRead)), "the fall damage's read of the fall height");
        if (!differs.empty()) {
            g_installed = "not as in Starfield 1.16.236: " + differs;
            spdlog::warn("[Climb] (fix179) climbing is off: {}", g_installed);
            return;
        }
        g_gravity_ok = SameCode(kGravityWriteAt, kGravityWrite, sizeof(kGravityWrite)) && SameCode(kGravityUseAt, kGravityUse, sizeof(kGravityUse)) &&
                       SameCode(kGravityZeroAt, kGravityZero, sizeof(kGravityZero)) &&
                       SameCode(kGravityZeroFallAt, kGravityZeroFall, sizeof(kGravityZeroFall));
        uintptr_t in_air_type = 0;
        Read(At(kInAirTableRva) + 3 * 8, in_air_type);
        g_state_ok = SameCode(kStateWriteAt, kStateWrite, sizeof(kStateWrite)) && SameCode(kStateReadRva, kStateRead, sizeof(kStateRead)) &&
                     in_air_type == At(kInAirTypeRva) && SameCode(kInAirTypeRva, kInAirType, sizeof(kInAirType));
        g_run_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kMoveRunRva + kMoveRunHeading)), &OnMoveRun);
        if (!g_run_hook) {
            g_installed = "the queued move's heading could not be hooked (a move would undo a turn made meanwhile)";
            spdlog::warn("[Climb] (fix179) climbing is off: {}", g_installed);
            return;
        }
        g_pick_ctor  = At(kPickCtorRva);
        g_cell_pick  = At(kCellPickRva);
        g_world_release = At(kWorldReleaseRva);
        g_queue_move = At(kQueueMoveRva);
        g_ready      = true;
        g_installed  = "ready";
        spdlog::info("[Climb] (fix179) climbing: ready (the game's ray pick rva {:x} -> {:x}, its pick data rva {:x}; the queued move rva {:x}, your "
                     "heading kept at its run rva {:x}; the gravity factor while you hold on: {}; the fall height after letting go: {}; setting Climbing, "
                     "pull x ClimbPull)",
            kCellPickRva, kBhkPickRva, kPickCtorRva, kQueueMoveRva, kMoveRunRva + kMoveRunHeading,
            g_gravity_ok ? "set to 0 (the game's code as expected)" : "left as it is (the game's code differs; each move holds you instead)",
            g_state_ok ? "set to where you let go if the game leaves it at 0 in the air" : "left to the game (its state's code differs)");
    }

    void Update()
    {
        g_frame.fetch_add(1, std::memory_order_relaxed);
        const int64_t now = NowUs();
        You           you{};
        const bool    have_you = ReadYou(you);
        g_player.store(have_you ? you.player : 0, std::memory_order_relaxed);
        const bool grip[2]{ HandGrab::GripDown(true), HandGrab::GripDown(false) };
        if (!g_ready.load()) {
            if (g_climbing) {
                const char* broken = g_broken.load();
                LetGo(you, broken != nullptr ? std::string{ broken } : std::string{ "climbing is off" });
            }
            g_grip_was[0] = grip[0];
            g_grip_was[1] = grip[1];
            TieFrame(now); // (fix185: not tied)
            return;
        }
        if (have_you) {
            WatchFall(you, now);
            PutGravityBackLater(you);
        }
        // (fix185) Where the game has your eyes from your feet, measured while not climbing nor easing back (for the next climb's view).
        if (have_you && !g_climbing && GameFlow::isInFirstPerson()) {
            bool easing = false;
            {
                std::scoped_lock _{ g_tie_mutex };
                easing = TieLinear(g_tie, now) > 0.0f;
            }
            if (!easing) {
                NoteEyes(you, now);
            } else {
                g_eyes_prev_ok = false;
            }
        } else {
            g_eyes_prev_ok = false;
        }
        // (Asked only while holding on or deciding a squeeze.)
        const bool        asked = g_climbing || (grip[0] && (!g_grip_was[0] || g_hands[0].phase == kDeciding)) ||
                           (grip[1] && (!g_grip_was[1] || g_hands[1].phase == kDeciding));
        const std::string why   = !asked ? std::string{} : have_you ? Blocked(you) : std::string{ "no player" };
        // Holding on: let go as the grips open, or when climbing is not for now.
        if (g_climbing) {
            if (!why.empty()) {
                LetGo(you, why);
                if (GameFlow::isLoadingScreenShown()) {
                    TieEnd(now, true); // (fix185, the review: a load moves you far: the game's camera at once, not eased)
                }
            } else {
                for (int h = 0; h < 2; ++h) {
                    if (g_hands[h].phase == kHolding && !grip[h]) {
                        g_hands[h].phase = kIdle;
                        g_holds[h]       = false;
                        LeaveSurface(h, now); // (fix180)
                    }
                }
                if (g_hands[0].phase != kHolding && g_hands[1].phase != kHolding) {
                    LetGo(you, "you let go");
                }
            }
        }
        // Squeezes: each decided once the hand grab has had its turn.
        for (int h = 0; h < 2; ++h) {
            auto&      st   = g_hands[h];
            const bool left = h == 0;
            if (!grip[h]) {
                if (st.phase != kHolding) {
                    st.phase = kIdle;
                }
                continue;
            }
            if (!g_grip_was[h] && st.phase != kHolding) {
                st.phase      = kDeciding;
                st.pressed_us = now;
            }
            if (st.phase != kDeciding) {
                continue;
            }
            std::string no = why;
            if (no.empty()) {
                no = HandBlocked(left);
            }
            if (no.empty() && (HandGrab::HoldingWith(left) || Physical::HoldingPerson(left) || Physical::PersonPending(left))) {
                no = "the hand grab took it (something or someone next to the hand)";
            }
            if (!no.empty()) {
                st.phase = kRefused;
                if (have_you && TellRefusal(no)) {
                    spdlog::info("[Climb] (fix179) {} grip: no climbing ({})", left ? "left" : "right", no);
                }
                continue;
            }
            // (Review) The hand grab decides the squeeze first (its pick following the hand a few frames, the left grip's 0.2 s wait for the
            // wheel's gesture; then a grab, the gloves' aim or nothing): waited for, at most kDecideMaxUs.
            if (HandGrab::Deciding(left) && now - st.pressed_us < kDecideMaxUs) {
                continue;
            }
            TryHold(you, h, now);
        }
        g_grip_was[0] = grip[0];
        g_grip_was[1] = grip[1];
        if (g_climbing && have_you) {
            Move(you, now);
        }
        TieFrame(now); // (fix185: whether the view is tied for the rest of this frame, and the root to tie)
    }

    bool HoldsGrip(bool left) { return g_holds[left ? 0 : 1].load(std::memory_order_relaxed); }

    bool Active() { return g_active.load(std::memory_order_relaxed); }

    bool GripSurface(bool left, WallGrip& out)
    {
        const int64_t      now = NowUs();
        std::scoped_lock   _{ g_surface_mutex };
        const HeldSurface& s = g_surface[left ? 0 : 1];
        const float        w = SurfaceWeight(s, now);
        if (!(w > 0.0f)) {
            return false;
        }
        out.point  = s.point;
        out.normal = s.normal;
        out.weight = w;
        return true;
    }

    float ViewTie(glm::vec3& place)
    {
        if (!g_tie_live.load(std::memory_order_relaxed) || !g_tie_ok.load(std::memory_order_relaxed)) {
            return 0.0f;
        }
        const int64_t now = NowUs();
        Tie           t{};
        {
            std::scoped_lock _{ g_tie_mutex };
            t = g_tie;
        }
        const float w = t.eyes.ok ? TieWeight(t, now) : 0.0f;
        if (!(w > 0.0f)) {
            return 0.0f;
        }
        // Where you are now with your eyes on you (turned with your heading now); while holding on, the place asked this frame instead
        // (the same once the move has run; the move's own place before it has), unless the game dropped that move.
        const uintptr_t player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
        float           at[3]{};
        float           heading = 0.0f;
        if (player == 0 || !ReadRaw(at, player + kRefLocation, sizeof(at)) || !Read(player + kRefHeading, heading) || !std::isfinite(heading)) {
            return 0.0f;
        }
        const glm::vec3 you{ at[0], at[1], at[2] };
        if (!Finite(you)) {
            return 0.0f;
        }
        glm::vec3 out = you + EyesAt(t.eyes, heading);
        if (t.holding && t.have_place && glm::length(t.place - out) < kTieFar) {
            out = t.place;
        }
        if (!Finite(out)) {
            return 0.0f;
        }
        place = out;
        return std::min(w, 1.0f);
    }

    glm::vec3 ViewTieLead()
    {
        // (The review) Asked at a frame's start before this frame's move (tracking::Snapshot runs the left hand's copies before Climbing):
        // this frame's step taken as the frame before's. Once this frame's move is asked, CameraRootPlace already gives it: nothing.
        const int64_t    frame = EngineFrame();
        std::scoped_lock _{ g_tie_mutex };
        if (!g_tie.holding || !g_tie.have_place || g_tie.place_frame == frame || !g_tie.have_prev || frame - g_tie.place_frame != 1) {
            return glm::vec3{ 0.0f };
        }
        const glm::vec3 step = g_tie.place - g_tie.prev_place;
        return Finite(step) && glm::length(step) < 0.5f ? step : glm::vec3{ 0.0f };
    }

    void BeforeNodeUpdate(RE::NiAVObject* obj)
    {
        if (obj == nullptr || obj != g_tie_root.load(std::memory_order_acquire)) {
            return;
        }
        glm::vec3   tied{ 0.0f };
        const float w = ViewTie(tied);
        if (!(w > 0.0f)) {
            return;
        }
        // The root's place as the game has just put it (its local place, in its parent's frame), in the game world.
        RE::NiPoint3    local_now{};
        RE::NiTransform parent{};
        bool            has_parent = false;
        if (!ReadRootFrame(obj, local_now, parent, has_parent)) {
            return;
        }
        const int64_t frame   = g_frame.load(std::memory_order_relaxed);
        const bool    holding = g_active.load(std::memory_order_relaxed);
        // (The review) The camera tree updated twice in a frame (fix148 saw 63 updates in 59 frames): the place written the first time
        // is written again, not eased toward the tie a second time. And whether the game had put the root again since the mod's last write
        // (easing back after letting go needs it to): counted.
        RE::NiPoint3 again{};
        bool         repeat = false, untouched = false;
        {
            std::scoped_lock _{ g_tie_stats_mutex };
            const auto&      tr = g_tie_track;
            if (tr.have_written && tr.written_obj == obj) {
                repeat    = tr.written_frame == frame;
                untouched = !repeat && std::memcmp(&local_now, &tr.written, sizeof(local_now)) == 0;
                again     = tr.written;
            }
            if (repeat) {
                ++g_tie_stats.passes;
                ++g_tie_stats.repeats;
            }
        }
        if (repeat) {
            WriteRootPlace(obj, again);
            return;
        }
        const glm::mat3 turn  = has_parent ? NiMath::to_glm(parent.rotate) : glm::mat3{ 1.0f };
        const glm::vec3 base  = has_parent ? NiMath::to_glm(parent.translate) : glm::vec3{ 0.0f };
        const float     scale = has_parent && parent.scale > 1e-4f ? parent.scale : 1.0f;
        const glm::vec3 game  = base + turn * (NiMath::to_glm(local_now) * scale);
        const glm::vec3 placed = game + (tied - game) * w;
        const glm::vec3 local  = glm::transpose(turn) * (placed - base) / scale;
        // (The review: the tied place itself against the game's, not the blend, which gets near the game's as the weight falls.)
        if (!Finite(game) || !Finite(local) || glm::length(tied - game) > 3.0f) {
            // (Far from the game's own place: not trusted; the view stays where the game puts it. Told the first few times.)
            static std::atomic<int> told{ 0 };
            if (told.fetch_add(1) < 5) {
                spdlog::warn("[Climb] (fix185) the tied place {:.2f},{:.2f},{:.2f} is {:.2f} m from the game's camera root {:.2f},{:.2f},{:.2f}: the game's "
                             "kept",
                    tied.x, tied.y, tied.z, glm::length(tied - game), game.x, game.y, game.z);
            }
            std::scoped_lock _{ g_tie_stats_mutex };
            ++g_tie_stats.refused;
            return;
        }
        const RE::NiPoint3 written = NiMath::to_ni(local);
        if (!WriteRootPlace(obj, written)) {
            return;
        }
        // The log's numbers, once a frame (the first of the root's updates in it).
        std::scoped_lock _{ g_tie_stats_mutex };
        auto&            s  = g_tie_stats;
        auto&            tr = g_tie_track;
        tr.written       = written;
        tr.written_frame = frame;
        tr.written_obj   = obj;
        tr.have_written  = true;
        ++s.passes;
        if (untouched) {
            ++s.untouched;
        }
        if (!holding) {
            auto& e = g_tie_ease;
            ++e.passes;
            if (untouched) {
                ++e.untouched;
            }
            e.last_off = glm::length(tied - game);
            e.any      = true;
        }
        if (frame == tr.frame) {
            return;
        }
        ++s.frames;
        const float off = glm::length(game - placed);
        s.off_sum += off;
        s.off_max = std::max(s.off_max, off);
        if (tr.have && frame - tr.frame == 1) {
            const glm::vec3 step      = placed - tr.placed;
            const glm::vec3 game_step = game - tr.game;
            const float     len       = glm::length(step);
            s.step_sum += len;
            s.step_max = std::max(s.step_max, len);
            ++s.steps;
            if (glm::length(game_step) > 0.005f && len > 0.002f) {
                ++s.moving;
                if (glm::dot(game_step, step) < 0.0f) {
                    ++s.against;
                }
            }
            if (tr.have_step) {
                const float change      = glm::length(step - tr.step);
                const float game_change = glm::length(game_step - tr.game_step);
                s.change_sum += change;
                s.change_max = std::max(s.change_max, change);
                s.game_change_sum += game_change;
                s.game_change_max = std::max(s.game_change_max, game_change);
                ++s.changes;
            }
            tr.step      = step;
            tr.game_step = game_step;
            tr.have_step = true;
        } else {
            tr.have_step = false;
        }
        tr.frame  = frame;
        tr.placed = placed;
        tr.game   = game;
        tr.have   = true;
        // This frame's move asked: had it run yet as the root was put?
        if (g_asked_frame.load(std::memory_order_relaxed) == frame) {
            if (g_run_frame.load(std::memory_order_relaxed) == frame) {
                ++s.after;
            } else {
                ++s.before;
            }
        }
    }

    std::string Status()
    {
        if (!g_ready.load()) {
            const char* broken = g_broken.load();
            return "Climbing: off (" + (broken != nullptr ? std::string{ broken } : g_installed) + ")";
        }
        if (g_active.load()) {
            return std::format("Climbing: holding on now ({} climbs so far)", g_climbs);
        }
        return std::format("Climbing: ready ({} climbs so far; squeeze a grip with your hand against a wall, rock or ledge and pull)", g_climbs);
    }
}
