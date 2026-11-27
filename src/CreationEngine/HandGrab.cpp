#include "HandGrab.h"
#include "GloveHighlight.h"
#include "GrenadeHand.h"
#include "HandActions.h"

#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define HANDGRAB_RETURN_ADDRESS() _ReturnAddress()
#else
#define HANDGRAB_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <mods/VR.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include <CreationEngine/memory/ScanHelper.h>
#include <ModSettings.h>

#include "Consume.h"
#include "CreationEngineSingletonManager.h"
#include "Haptics.h"
#include "Physical.h"
#include "FavoritesWheel.h"
#include "RE/N/NiCamera.h"
#include "PointInteract.h"
#include "body/BodyIK.h"
#include "body/SafeScan.h"
#include "body/SceneGraph.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace HandGrab
{
    namespace
    {
        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        uintptr_t Base() { return reinterpret_cast<uintptr_t>(MemoryScan::mod); }
        bool      InImage(uintptr_t a) { return a >= Base() && a < Base() + MemoryScan::mod_size; }

        bool Same(uintptr_t at, const uint8_t* bytes, size_t n)
        {
            if (at < Base() || at + n > Base() + MemoryScan::mod_size) {
                return false;
            }
            return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
        }

        uintptr_t RelTarget(uintptr_t rel_at, uintptr_t next_instruction)
        {
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(rel_at), sizeof(rel));
            return next_instruction + rel;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there (inside the
        // game's image, as Same checks), and the address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // ---- The player's grab update (Starfield 1.16.236, rva 1a52da0; player in r14, frame pointer rbp) ----
        // 1a53592  vmovss [rbp-0x50],xmm4 ; [rbp-0x4c],xmm5 ; [rbp-0x48],xmm6 ; [rbp-0x44],xmm7   target x y z 0
        // 1a535a6  mov edx,esi ; mov r15,[rbp-0x80] ; cmp dword [r15],0 ; jbe ; vmovups xmm0,[rbp-0x50]
        //          ... then every spring gets vmovups [spring+0xa0],xmm0
        constexpr const char* kTargetPattern =
            "C5 FA 11 65 B0 C5 FA 11 6D B4 C5 FA 11 75 B8 C5 FA 11 7D BC 8B D6 4C 8B 7D 80 41 83 3F 00 76 ? C5 F8 10 45 B0";
        constexpr uintptr_t kTargetRva  = 0x1a53592;
        constexpr uintptr_t kTargetHook = 0x14;
        constexpr ptrdiff_t kTargetSlot = -0x50; // rbp-relative
        // 1a53739  vmovss xmm3,[r14+0xd9c] ; xmm2,[r14+0xd98] ; xmm1,[r14+0xda0] ; lea rcx,[rbp+0x70] ; call
        //          then the target orientation = camera rows at [rbp+0x110], [rbp+0x120], [rbp+0x130]
        //          (stored at 1a536d1, 1a536dd, 1a536e9) x the spring's rotation x the grab's own rotation.
        constexpr uintptr_t kRotationFromTarget = 0x1a7;
        constexpr uint8_t   kRotationSite[]     = { 0xC4, 0xC1, 0x7A, 0x10, 0x9E, 0x9C, 0x0D, 0x00, 0x00, 0xC4, 0xC1, 0x7A, 0x10, 0x96, 0x98, 0x0D, 0x00,
                  0x00, 0xC4, 0xC1, 0x7A, 0x10, 0x8E, 0xA0, 0x0D, 0x00, 0x00, 0x48, 0x8D, 0x4D, 0x70, 0xE8 };
        constexpr uintptr_t kRowStoreFromTarget = 0x13f;
        constexpr uint8_t   kRowStore[]         = { 0xC5, 0xF8, 0x11, 0x85, 0x10, 0x01, 0x00, 0x00 }; // vmovups [rbp+0x110],xmm0
        constexpr ptrdiff_t kRowsSlot           = 0x110;
        // 1a52f46  mov [rbp+0x830],rbx: taken for the physics world, kept for the whole update; it is not (the physics test
        // build read 0x9001e there at the target hook: your character controller's collision filter info), so the throw
        // below never finds a velocity function there and the spring's own step throws instead.
        constexpr uintptr_t kWorldStoreBack = 0x64c;
        constexpr uint8_t   kWorldStore[]   = { 0x48, 0x89, 0x9D, 0x30, 0x08, 0x00, 0x00 };
        constexpr ptrdiff_t kWorldSlot      = 0x830;
        // 1a53507  mov rcx,r14 ; call drop (rva 1a53a80): the update's own "object stuck too far" drop.
        constexpr uintptr_t kDropCallBack = 0x8b;
        constexpr uint8_t   kDropCall[]   = { 0x49, 0x8B, 0xCE, 0xE8 };
        // Its condition (read for the log): site - 0xae vcomiss xmm1,[rip+X]: the held point over X from the spring's target
        // (grab mode other than 2); site - 0xa4 vcomiss xmm1,[rip+Y] and site - 0x9a vmovss xmm0,[rip+Z]: over Y, and Z.
        constexpr uintptr_t kDropFarBack  = 0xae;
        constexpr uintptr_t kDropSlowBack = 0xa4;
        constexpr uintptr_t kDropTimeBack = 0x9a;
        constexpr uint8_t   kComissXmm1Rip[] = { 0xC5, 0xF8, 0x2F, 0x0D };
        constexpr uint8_t   kMovssXmm0Rip[]  = { 0xC5, 0xFA, 0x10, 0x05 };
        constexpr uint8_t   kDropPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24,
              0xE1, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };

        // ---- Starting a grab (rva 1a50600, the player): what the use button does once held ----
        constexpr const char* kStartPattern =
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 E8 FE FF FF 48 81 EC D8 01 00 00 C5 F8 29 70 A8 C5 F8 29 78 98 C5 78 29 40 88 4C 8B F9 "
            "83 B9 54 0F 00 00 00";
        constexpr uintptr_t kStartRva = 0x1a50600;
        // +0x69 mov rcx,[interaction pick (ViewCaster) singleton] ; +0x70 call (its target)
        constexpr uintptr_t kCasterLoad  = 0x69;
        constexpr uint8_t   kCasterBytes[] = { 0x48, 0x8B, 0x0D };
        // +0x228 mov edx,[null handle] ; test rax,rax ; je
        constexpr uintptr_t kNullLoad    = 0x228;
        constexpr uint8_t   kNullBytes[] = { 0x8B, 0x15 };
        constexpr uintptr_t kCasterTarget = 0x10; // the pick's target handle
        constexpr uintptr_t kCasterPoint  = 0x40; // the point the pick hit (x y z)
        // +0x6a9 lea rax,[rsp+0x48] ; mov [rsp+0x28],rax ; mov [rsp+0x20],r12 ; vmovss xmm3,[..] ; mov r8d,1
        // +0x6c6 mov rdx,rbx ; mov rcx,r15 ; call (makes the springs, holding the object by the point at [rsp+0x48])
        //        [rsp+0x40] = the object taken (TESObjectREFR), [rsp+0x48] = the spot the pick touched (x y z)
        constexpr uintptr_t kSpringsArgs      = 0x6a9;
        constexpr uint8_t   kSpringsArgsBytes[] = { 0x48, 0x8D, 0x44, 0x24, 0x48, 0x48, 0x89, 0x44, 0x24, 0x28, 0x4C, 0x89, 0x64, 0x24, 0x20 };
        constexpr uintptr_t kSpringsCall      = 0x6c6;
        constexpr uint8_t   kSpringsCallBytes[] = { 0x48, 0x8B, 0xD3, 0x49, 0x8B, 0xCF, 0xE8 };
        // +0x6d6 mov rax,[rbx] ; mov edx,6 ; mov rcx,rbx ; call [rax+0xb8]   (only when springs were made; rsi = player+0xd80,
        //        the player's spring list: [rsi] count, [rsi+8] the springs)
        constexpr uintptr_t kSpringsMade      = 0x6d6;
        constexpr uint8_t   kSpringsMadeBytes[] = { 0x48, 0x8B, 0x03, 0xBA, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0xB8, 0x00, 0x00, 0x00 };
        // +0x1ee vcomiss xmm0,xmm6 ; jae +0x2fd: the grab start casts its own ray from the camera (your head in VR) out to
        //   the pick's point and takes what that ray hits first (and where) instead of the pick's target, unless it reaches
        //   past it. For a grab a grip started, the pick was made at the hand, so that head ray is skipped: the jump is
        //   taken, as when the ray hits nothing before the pick's target. (+0x620 / +0x676: the heaviest object it grabs.)
        constexpr uintptr_t kStartRepick       = 0x1ee;
        constexpr uint8_t   kStartRepickBytes[] = { 0xC5, 0xF8, 0x2F, 0xC6, 0x0F, 0x83, 0x05, 0x01, 0x00, 0x00 };
        constexpr uintptr_t kStartRepickSkip   = 0x2fd;
        constexpr uintptr_t kStartMaxMass      = 0x620; // vmovss xmm0,[rip+X] ; vcomiss xmm0,[rbp+0x128] (the object's mass)
        constexpr ptrdiff_t kTakenRefSlot     = 0x40;
        constexpr ptrdiff_t kTouchedSlot      = 0x48; // the pick's point: where its swept sphere met the object (its centre, off the surface)

        // ---- Making the springs (rva 1a51a70, called at the grab start's +0x6cc) ----
        // +0xc0e..+0xc42: the collision shape's own bounding box (its function from the shape-type table, for the identity
        //   transform), (min + max) x 0.5 -> [rsp+0x150], which +0x10cc..+0x10dc copies into the spring's grab point (+0xb0):
        //   the springs always hold an object by the middle of its collision shape's box, wherever the pick touched it.
        // +0x10fb vmovups xmm0,[rip+X] ; vmovups [rsi+0xc0],xmm0: the error a new spring keeps "from its last step" starts as
        //   a constant from the game's data (read when this installs, and logged); the end of every full step (step +0x488)
        //   replaces it with that step's error, so while it still holds the constant the spring has not stepped yet.
        constexpr uintptr_t kMakerCallRel     = kSpringsCall + 7; // the call's rel32 (after mov rdx,rbx ; mov rcx,r15 ; E8)
        constexpr uint8_t   kMakerPrologue[]  = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x20, 0x44, 0x89, 0x40, 0x18 };
        constexpr uintptr_t kMakerShapeBox    = 0xc0e;
        constexpr uint8_t   kMakerShapeBoxBytes[] = { 0x4C, 0x8D, 0x84, 0x24, 0x20, 0x03, 0x00, 0x00 }; // lea r8,[rsp+0x320]
        constexpr uintptr_t kMakerBoxMiddle   = 0xc1d;
        constexpr uint8_t   kMakerBoxMiddleBytes[] = { 0x41, 0xFF, 0xD1,                              // call r9
            0xC5, 0xF8, 0x10, 0x84, 0x24, 0x20, 0x03, 0x00, 0x00,                                   // vmovups xmm0,[rsp+0x320]
            0xC5, 0xF8, 0x58, 0x8C, 0x24, 0x30, 0x03, 0x00, 0x00,                                   // vaddps xmm1,xmm0,[rsp+0x330]
            0xC5, 0xF0, 0x59, 0x15 };                                                               // vmulps xmm2,xmm1,[rip+half]
        constexpr uintptr_t kMakerMiddleStore = 0xc3a;
        constexpr uint8_t   kMakerMiddleStoreBytes[] = { 0xC5, 0xF8, 0x11, 0x94, 0x24, 0x50, 0x01, 0x00, 0x00 }; // vmovups [rsp+0x150],xmm2
        constexpr uintptr_t kMakerGrabPoint   = 0x10cc;
        constexpr uint8_t   kMakerGrabPointBytes[] = { 0xC5, 0xF8, 0x10, 0x8C, 0x24, 0x50, 0x01, 0x00, 0x00,  // vmovups xmm1,[rsp+0x150]
            0xC5, 0xF8, 0x11, 0x8E, 0xB0, 0x00, 0x00, 0x00 };                                       // vmovups [rsi+0xb0],xmm1
        constexpr uintptr_t kMakerFreshError  = 0x10fb;
        constexpr uint8_t   kMakerFreshLoad[]  = { 0xC5, 0xF8, 0x10, 0x05 };                            // vmovups xmm0,[rip+X]
        constexpr uint8_t   kMakerFreshStore[] = { 0xC5, 0xF8, 0x11, 0x86, 0xC0, 0x00, 0x00, 0x00 };    // vmovups [rsi+0xc0],xmm0

        // ---- The object (CommonLibSF layouts; the grab start itself reads ref+0x98, +0xb0 and +0xc8 this way) ----
        constexpr uintptr_t kRefBase       = 0x98;  // TESObjectREFR::data.objectReference (TESBoundObject*)
        constexpr uintptr_t kRefLoaded     = 0xb8;  // TESObjectREFR::loadedData (LOADED_REF_DATA*)
        constexpr uintptr_t kLoaded3D      = 0x08;  // LOADED_REF_DATA::data3D (NiAVObject*)
        constexpr uintptr_t kBoundMin      = 0x50;  // TESBoundObject::boundMin (NiPoint3), boundMax follows (TESObject is 0x50 bytes)
        constexpr uintptr_t kBoundMinOld   = 0x48;  // where CommonLibSF's comment puts it
        constexpr uintptr_t kRecordFrom    = 0x40;  // read from here (covers both)
        constexpr uintptr_t kNodeWorld     = 0x80;  // NiAVObject::world (rotate rows, translate, scale)
        constexpr uintptr_t kNodeWorldBefore = 0xc0; // NiAVObject::previousWorld (the frame before; CommonLibSF)
        constexpr uintptr_t kNodeBound     = 0x100; // NiAVObject::worldBound (centre, radius)

        // ---- The spring's physics step (rva 23d3e00: the spring, the world, the step's dt in xmm2) ----
        // Each step: the object's speed x object damping (+0xdc); then an impulse at the held point so its speed
        // becomes (1 - damping (+0xd8)) x that speed - elasticity (+0xd4) x (held point - target (+0xa0)) / dt,
        // changing the speed by at most max force (+0xd0) x dt. The error it worked from is kept at +0xc0.
        // With elasticity and damping at 1 the held point lands exactly on the target every step.
        constexpr const char* kStepPattern =
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 81 EC 80 01 00 00 C5 F8 29 70 E8 C5 F8 29 78 D8 C5 78 29 40 C8 C5 78 29 48 "
            "B8 C5 78 29 50 A8 C5 78 29 58 98 48 8D 6C 24 60 48 83 E5 E0 8B 41 20 4C 8B F2 25 FF FF FF 00 48 8B F9 C5 F8 28 F2";
        constexpr uintptr_t kStepRva = 0x23d3e00;
        struct StepCheck {
            uintptr_t offset;
            uint8_t   bytes[9];
        };
        // vbroadcastss from the spring's object damping, elasticity, damping and max force.
        // +0xc4 vmovups xmm0,[rbp+0x60] ; +0xc9 vsubps xmm10,xmm0,[rdi+0xa0]: just after the step put the held point in the
        // game world (the spring's local grab point through the body's transform, rva 2e944c0) at [rbp+0x60]. Hooked at
        // +0xc9, not +0xc4: +0xc4 is the call's return address, and a hook's jump there would look to a stack walk (a crash
        // log, an exception passing through) like this function's exit, unwinding its frame wrongly.
        constexpr uintptr_t kStepPointCheck   = 0xc4;
        constexpr uint8_t   kStepPointBytes[] = { 0xC5, 0xF8, 0x10, 0x45, 0x60, 0xC5, 0x78, 0x5C, 0x97, 0xA0, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kStepPoint        = 0xc9;
        constexpr ptrdiff_t kStepPointSlot    = 0x60; // rbp-relative
        constexpr StepCheck kStepChecks[] = {
            { 0x21c, { 0xC4, 0xE2, 0x79, 0x18, 0xB7, 0xDC, 0x00, 0x00, 0x00 } },
            { 0x27f, { 0xC4, 0xE2, 0x79, 0x18, 0x8F, 0xD4, 0x00, 0x00, 0x00 } },
            { 0x288, { 0xC4, 0xE2, 0x79, 0x18, 0x97, 0xD8, 0x00, 0x00, 0x00 } },
            { 0x3d6, { 0xC4, 0xE2, 0x79, 0x18, 0x87, 0xD0, 0x00, 0x00, 0x00 } },
        };

        // ---- The player and the spring ----
        constexpr uintptr_t kSpringCount   = 0xd80;
        constexpr uintptr_t kSpringArray   = 0xd88;
        constexpr uintptr_t kGrabMode      = 0xf54; // 0 when not grabbing
        constexpr uintptr_t kSpringBody    = 0x20;
        constexpr uintptr_t kSpringTarget  = 0xa0;
        constexpr uintptr_t kSpringError   = 0xc0;
        constexpr uintptr_t kSpringForce   = 0xd0;  // the game uses 250
        constexpr uintptr_t kSpringElastic = 0xd4;  // 0.2
        constexpr uintptr_t kSpringDamping = 0xd8;  // 0.5
        constexpr uintptr_t kWorldBodies   = 0x18;  // the world's body interface
        constexpr uintptr_t kSetVelocity   = 0xa0;  // its function: (body id, &linear, &angular, 0)

        // ---- A held object and your own collision (Starfield 1.16.236, found with the physics test build) ----
        // The world's collision filter (bhkCollisionFilter, its pair test at rva 239b500) lets two bodies of the same nonzero
        // group (filter info bits 16-31) collide only when both are body parts (layers 8, 32, 33; adjacent bones of a ragdoll
        // never): a held object put in your group no longer touches your character controller (layer 30) or your body, and
        // still touches everything else by its layer (bits 0-6), as HIGGS does in Skyrim VR. bhkNPCollisionObject's
        // SetCollisionFilterGroup (type table slot 65, rva 2379a60) changes a body's group the way the game does (with the
        // world locked, or as a command while the world steps).
        constexpr uintptr_t kNodeCollision     = 0x110; // NiAVObject::collisionObject
        constexpr uintptr_t kCollisionSystem   = 0x20;  // bhkNPCollisionObject: its bhkPhysicsSystem...
        constexpr uintptr_t kCollisionIndex    = 0x28;  // ...and its body's index there (int)
        constexpr uintptr_t kSystemInstance    = 0x20;  // bhkPhysicsSystem: its Havok physics system in the world...
        constexpr uintptr_t kInstanceWorld     = 0x28;  // ...that one's world (0x18 into the hknpBSWorld)...
        constexpr uintptr_t kInstanceIds       = 0x30;  // ...its bodies' ids (u32)...
        constexpr uintptr_t kInstanceCount     = 0x38;  // ...and how many
        constexpr uintptr_t kWorldFromInstance = 0x18;
        constexpr uintptr_t kWorldBodyArray    = 0x38;  // hknpBSWorld: its bodies (kBodySize bytes each)...
        constexpr uintptr_t kWorldBodyCount    = 0x40;  // ...and how many
        constexpr uintptr_t kBodySize          = 0xb0;
        constexpr uintptr_t kBodyId            = 0x50;  // a body's own id (its index in bits 0-23, a serial above)
        constexpr uintptr_t kBodyFlags         = 0x54;  // bits 0-1: in use
        constexpr uintptr_t kBodyFilter        = 0x8c;  // its collision filter info
        constexpr uintptr_t kBodyOwner         = 0x98;  // the bhkNPCollisionObject it belongs to (every body's record, the test build)
        constexpr uint32_t  kBodyIndexMask     = 0xffffff;
        constexpr size_t    kSetGroupSlot      = 65;
        constexpr uintptr_t kSetGroupRva       = 0x2379a60;
        constexpr uint8_t   kSetGroupBytes[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50,
              0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x41, 0x20 };
        // (fix155) A weapon's copy is built from its parts' models: its collision object is a part's, and the root has a proxy for it
        // (bhkNPCollisionProxyObject, type table 0x144D67AC0: +0x10 the node it moves, +0x20 the part's bhkNPCollisionObject; made by
        // the collision setup at 0x142359C23, 0x14237CFA0 its constructor; a helper's research, checked). Its group is set on the part's
        // object, and each body of the system on its own (slot 65 changes one body); also its layer (slot 64, SetCollisionLayer, rva
        // 23798a0: the filter's low 7 bits) to L_NONCOLLIDABLE (15: it touches only the invisible walls, cone projectiles and the item
        // pickers, never you, people, clutter or projectiles; Starfield.esm's layer records).
        constexpr uintptr_t kCollProxyTableRva = 0x4d67ac0;
        constexpr uintptr_t kCollProxyNode     = 0x10;
        constexpr uintptr_t kCollProxyTarget   = 0x20;
        constexpr size_t    kSetLayerSlot      = 64;
        constexpr uintptr_t kSetLayerRva       = 0x23798a0;
        constexpr uint8_t   kSetLayerBytes[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50,
              0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x41, 0x20 };
        constexpr uintptr_t kSetLayerMaskAt    = 0x14f; // and r8d,0FFFFFF80h; and r14d,7Fh; or r8d,r14d
        constexpr uint8_t   kSetLayerMask[]    = { 0x41, 0x83, 0xE0, 0x80, 0x41, 0x83, 0xE6, 0x7F, 0x45, 0x0B, 0xC6 };
        constexpr uint32_t  kLayerNonCollidable = 15;
        constexpr int       kApartNodes        = 256; // (nodes of a copy's 3D looked through)
        constexpr uint32_t  kApartBodies       = 64;  // (bodies of one physics system)
        // Your collision filter info: PlayerCharacter+0x228 its AI process, +0x8 that one's middle-high data, +0x458 your
        // character controller (bhkCharProxyController), +0x4d0 its hknpBSCharacterProxy, +0x3c the proxy's filter info
        // (0x9001e: layer 30, group 9).
        constexpr uintptr_t kActorProcess      = 0x228;
        constexpr uintptr_t kProcessMiddle     = 0x08;
        constexpr uintptr_t kMiddleController  = 0x458;
        constexpr uintptr_t kControllerProxy   = 0x4d0;
        constexpr uintptr_t kProxyFilter       = 0x3c;
        constexpr uint32_t  kLayerCharController = 30;

        constexpr float   kPalmForward  = 0.0f;    // without a box: the held spot sits at the centre of the closed hand (the grip pose)
        constexpr float   kPalmSkin     = 0.025f;  // the palm's surface is this far from the grip pose's centre, out of the palm
        constexpr float   kHalfPalm     = 0.045f;  // half the palm's width: a long object is held at least this far from its ends
        constexpr float   kGripAhead    = 0.01f;   // a held object sits this much toward the fingers from the palm's centre
        constexpr float   kNearPalm     = 0.10f;   // the palm this near the object: it decides where along a long object it is held
        constexpr float   kStillMove    = 0.003f;  // an object that moved less than this (m) since the frame before is at rest...
        constexpr float   kStillTurn    = 0.01f;   // ...and turned less than this (radians, about)
        constexpr float   kJumpMove     = 0.5f;    // more than this in one frame is a jump, not motion (m)...
        constexpr float   kJumpTurn     = 0.5f;    // ...or this (radians, about)
        // Gravity gloves (as HIGGS and ROCK do it): a grip squeezed with nothing next to the hand aims; the hand's cast
        // then reaches far, and the first thing it finds is locked (a tick); moving the hand back toward you along the
        // line from the object faster than kFlickSpeed pulls it: the game's grab starts on it, and it flies to the hand on
        // an arc (HIGGS's flight time), steered all the way (ROCK's default), into the hand if the grip is still held.
        constexpr float   kPullRadius   = 0.30f;   // the far cast's radius (m; HIGGS's FarCastRadius, three times the game's pick)...
        // ...taken every other frame with the game's own (the game's pick takes the first thing its sphere touches, where HIGGS
        // takes the one nearest the pointing line): what the narrow cast finds (pointed straight at) goes before the wide one's
        constexpr float   kPullNarrowRadius = 0.10f;
        constexpr float   kFlickSpeed   = 1.2f;    // m/s of the hand toward you along the object-to-hand line (HIGGS, ROCK)
        constexpr float   kFlickStart   = 0.6f;    // below this the hand is steady (locking on, aiming elsewhere)
        constexpr float   kFingerReach  = 0.10f;   // from the controller to the fingertips (m), for a wrist flick's speed
        constexpr int     kLockFrames   = 4;       // the cast finds the same thing this many frames running: locked on
        constexpr float   kUnlockCos    = 0.94f;   // the hand pointing over about 20 degrees away from it, steady: aim again
        constexpr float   kPullGravity  = 9.81f;   // the arc's shape (m/s^2): it rises g T^2 / 8 above the straight line midway
        constexpr float   kPullMaxSpeed = 10.0f;   // a flight lasts long enough to stay about this fast (m/s, ROCK)
        constexpr int64_t kPullAskUs    = 500'000; // a pull that has not become a grab by then is forgotten
        constexpr int64_t kReleaseGraceUs = 150'000; // the grip let go while locked on: a flick this soon after still pulls...
        // ...and a pulled object whose grip was let go (as the hand flicked, or while it flew) waits this long at the hand for a
        // squeeze to catch it (HIGGS: up to a second after it should have arrived)
        constexpr int64_t kCatchWindowUs  = 1'000'000;
        constexpr int     kRefusedKept    = 8;       // things the game would not let you pull, skipped by the aim from then on
        // Following the hand as you move (HIGGS moves held objects with the player's own movement each frame, not a speed
        // guessed from the frames before): the body draws the hand where the play area is then, which moves between the grab
        // update reading the hand and the hand being drawn.
        constexpr float   kFollowJump    = 0.5f;   // where the hand holds it moving more than this in one frame is a jump...
        constexpr float   kFollowJumpCos = 0.996f; // ...or the play area turning more than about 5 degrees in it (a snap turn)
        constexpr float   kFollowMaxLead = 0.25f;  // the hand moving more than this (m) in the play area in one frame is no motion
        constexpr float   kMoving        = 0.01f;  // moving: the play area (or the hand in it) moved over this in a frame (m)
        constexpr float   kLeadTurnCos   = 0.866f; // the hand turning over 30 degrees in one frame is not led (a glitch, a hand change)
        constexpr float   kTurning       = 1.0f;   // turning: the hand turned over this in a frame (degrees)
        constexpr int     kMaxStepsAhead = 3;      // per-step movement is counted for at most this many steps of a frame
        constexpr int     kVotesNeeded   = 30;     // frames of motion before it is known whether drawn objects lag a frame...
        constexpr float   kVoteShare     = 0.85f;  // ...and the share of them that must agree
        constexpr int64_t kRampUs       = 250'000; // the spring goes from the game's strength to the locked one over this long
        constexpr float   kLockForce    = 1500.0f; // locked: the speed may change by up to this x dt per step
        constexpr float   kLockElastic  = 1.0f;
        constexpr float   kLockDamping  = 1.0f;
        constexpr float   kBlockedError = 0.30f;   // the object is this far off the target at a step: something blocks it...
        constexpr float   kBlockedElastic = 0.3f;  // ...so it is pushed toward the hand more gently (no jitter, no pushing through)
        // Touching something (HIGGS holds things more softly while they collide): each physics step, where the held point is
        // vs where the step before sent it. Pushed off by more than kPushed (a fall of up to kFallAllowance in a step is
        // gravity) in 2 of the last 4 steps: held at kContactElastic, back to full strength once nothing has pushed it for
        // kContactHoldSteps steps.
        constexpr float   kPushed          = 0.006f;
        constexpr float   kFallAllowance   = 0.01f;
        constexpr float   kContactElastic  = 0.35f;
        constexpr int     kContactHoldSteps = 10;
        constexpr float   kMaxLead      = 0.05f;   // the target is moved ahead along the hand's motion by at most this long
        constexpr float   kHeldCurl     = 0.8f;    // fingers of a holding hand close at least this far
        constexpr float   kGripOn       = 0.75f;   // grip pressed past this...
        constexpr float   kGripOff      = 0.40f;   // ...and released below this
        constexpr float   kThrowBoost   = 1.25f;   // thrown objects get a little more than the hand's speed
        constexpr int     kHistory      = 4;       // hand velocity samples kept for throwing
        // (fix189) No hand moves faster than this: a faster sample (a tracking glitch) counts as this fast, so a throw is never flung off.
        constexpr float   kMaxHandSpeed = 15.0f;   // m/s
        // (fix189) A jump of more than this from one pose to the next, faster than any hand: the tracking lost and found again, no speed.
        constexpr float   kPoseJump     = 0.30f;   // m

        using PlayerFn   = void (*)(void* player);
        using VelocityFn = void (*)(void* bodies, uint32_t body, const float* linear, const float* angular, uint32_t flags);

        safetyhook::MidHook g_target_hook{};
        safetyhook::MidHook g_rotation_hook{};
        safetyhook::MidHook g_step_hook{};
        safetyhook::MidHook g_springs_hook{};
        safetyhook::MidHook g_made_hook{};
        safetyhook::MidHook g_point_hook{};
        safetyhook::MidHook g_repick_hook{};
        std::atomic<bool>   g_ready{ false };
        std::atomic<bool>   g_grip_ready{ false };
        std::atomic<bool>   g_pull_ready{ false }; // gravity gloves (needs the grab start to keep the hand's pick)
        std::atomic<bool>   g_step_ready{ false };
        const char*         g_off_reason{ "not installed yet" };
        uintptr_t           g_start_fn{ 0 };
        uintptr_t           g_drop_fn{ 0 };
        uintptr_t           g_caster_global{ 0 };
        uintptr_t           g_null_global{ 0 };
        // Set by Install before the grab start's hooks exist; what reads them only runs for a grab that went through those.
        bool                g_holds_middle{ false }; // the spring maker is the code that holds objects by their shape's middle
        bool                g_know_fresh{ false };   // what a new spring's kept error starts as is known...
        float               g_fresh_error[4]{};      // ...this

        // The grab in progress (the hooks run in the player's grab update, one frame at a time; Update runs on the
        // main thread). The physics step's hook takes only g_track_mutex, which is never held while calling the game
        // and is only ever taken after g_mutex, never before it.
        std::mutex g_mutex;
        uintptr_t  g_spring{ 0 };
        bool       g_have_base{ false };
        bool       g_rebase{ false };      // the object moved to the other hand: keep its orientation, follow the new hand
        glm::mat3  g_camera0{ 1.0f };      // camera rotation the game used when the grab started (column-vector)
        glm::mat3  g_hand0{ 1.0f };        // the holding hand's world rotation then
        glm::mat3  g_last_turned{ 1.0f };  // the orientation given last frame
        glm::mat3  g_object0{ 1.0f };      // the held object's own rotation when g_hand0 was taken (for the turn check)
        glm::mat3  g_rows_to_object{ 1.0f }; // the object's rotation relative to the rotation rows the game holds it by (fixed per grab)
        bool       g_have_object{ false };
        bool       g_turn_told{ false };
        std::atomic<int> g_turn_checks{ 0 };
        glm::quat  g_turn{ 1.0f, 0.0f, 0.0f, 0.0f }; // the turn (world, from the grab's start) that sits the object square in the hand
        int64_t    g_turn_us{ 0 };         // when it started turning
        int64_t    g_start_us{ 0 };
        int64_t    g_last_us{ 0 };
        bool       g_release{ false };     // let go of the grip: drop and throw in the next grab update
        glm::vec3  g_throw_linear{ 0.0f };
        glm::vec3  g_throw_angular{ 0.0f };
        bool       g_plan_ok{ false };
        float      g_force0{ 0.0f }, g_elastic0{ 0.0f }, g_damping0{ 0.0f };
        float      g_force1{ 0.0f }, g_elastic1{ 0.0f }, g_damping1{ 0.0f };
        float      g_written_force{ -1.0f };
        bool       g_have_anchor{ false };
        glm::vec3  g_anchor_prev{ 0.0f };
        int64_t    g_anchor_us{ 0 };
        glm::vec3  g_walk{ 0.0f };          // how fast the player moves (world, m/s), smoothed
        int64_t    g_stats_us{ 0 };
        int        g_stats_logs{ 0 };
        int        g_updates{ 0 };
        glm::vec3  g_placed{ 0.0f };       // where this grab's held point was last sent (g_mutex)
        bool       g_have_placed{ false };
        bool       g_stats_pending{ false }; // this hold has not had its summary line yet
        int        g_holds_told{ 0 };
        // The holding hand's world rotation and the play area's as each frame's grab update read them (the latest frame's and
        // the one before), for leading the held object's orientation by the hand's turn over a frame.
        struct TurnSeen {
            bool      valid{ false };
            uint64_t  frame{ 0 };
            int       hand{ -1 };
            uintptr_t spring{ 0 };
            glm::mat3 hand_rot{ 1.0f };
            glm::mat3 room{ 1.0f };
        };
        TurnSeen   g_turn_seen{}, g_turn_seen_before{};

        // A held object's collision shape, when it is one convex shape (hknpConvexShape, most loose objects), in its 3D's own
        // space: its corners, its faces' planes (n.x + w <= 0 inside, the shape's convex radius already out) and its box (with
        // that radius). The hand is placed on this surface (as HIGGS and ROCK hold the real surface, not a box).
        struct Hull {
            bool                   valid{ false };
            std::vector<glm::vec3> corners;
            std::vector<glm::vec4> planes; // empty: the corners only
            glm::vec3              lo{ 0.0f }, hi{ 0.0f };
            float                  radius{ 0.0f }; // the convex radius, object units
        };

        // The object taken by the latest grab start and how the hand holds it (g_mutex).
        struct Taken {
            bool      valid{ false };
            int64_t   us{ 0 };            // when the grab started
            int       serial{ 0 };
            uintptr_t ref{ 0 };
            uintptr_t object{ 0 };        // the object taken, also when it has no box (valid false)
            uintptr_t root{ 0 };          // its 3D (the box is in this node's space)
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };
            glm::vec3 touched{ 0.0f };    // the spot the pick touched (the springs' first target), object space
            glm::vec3 contact{ 0.0f };    // the point of the box against the palm, object space
            bool      have_contact{ false };
            float     lift{ 0.0f };       // raised off the palm by this much (m) so none of it is inside the hand
            // Its shape: round (two sides of the box about equal) = a cylinder along `axis`; flat round = a disc.
            bool      cylinder{ false };
            bool      disc{ false };
            int       axis{ -1 };
            float     radius{ 0.0f };     // object units
            float     fit{ 1.0f };        // the box's half diagonal / the bounding sphere's radius (round things: above 1)
            bool      measure_told{ false }; // the measured held point (or its absence) has been logged
            bool      seated{ false };       // it has been seated in a hand once (later: taken into the other hand)
            bool      seated_drawn{ false }; // ...against the drawn hand (else the controller's grip point)
            Hull      hull{};                // its collision shape (valid: the box above is the shape's part of the game's size box)
        };
        Taken g_taken{};
        int   g_taken_serial{ 0 };
        // The object of the grab the grab update follows (from g_taken when it adopts a new spring, if that grab start was within
        // 2 s; 0 once nothing is held), for eating and drinking (g_mutex).
        uintptr_t g_held_object{ 0 };
        bool  g_use_taken{ false };       // the grab in progress has its box
        bool  g_need_contact{ false };    // find where the palm meets the box (new grab, other hand)
        std::atomic<int> g_boxes{ 0 };

        // What the physics step needs: the held point's place at the hand, read once per frame, and how it moves on.
        struct Track {
            glm::vec3 palm{ 0.0f };
            glm::vec3 velocity{ 0.0f }; // flying to the hand: the path's speed + the hand's + walking, world m/s
            glm::vec3 walk{ 0.0f };
            float     elastic{ 0.0f };  // the elasticity planned for this moment of the grab
            int64_t   us{ 0 };
            bool      pulling{ false }; // flying to the hand (gravity gloves): held at full strength all the way
            // Held: where the body draws the hand this frame is the place read (palm) moved by how the play area moves in
            // between (move; or step_move each physics step), plus one more frame of the hand's motion (lead) when drawn
            // objects lag a frame behind their physics.
            glm::vec3 move{ 0.0f };
            glm::vec3 step_move{ 0.0f };
            bool      per_step{ false };
            glm::vec3 lead{ 0.0f };
            uint64_t  frame{ 0 };       // the frame (HandGrab's count) the grab update read it in
            int       steps{ 0 };       // physics steps since
            // Where your head is (game world), for telling which way contacts push the object.
            glm::vec3 body{ 0.0f };
            bool      have_body{ false };
            // The point the springs hold, in the object's own space (for measuring where it is drawn).
            glm::vec3 held{ 0.0f };
            bool      have_held{ false };
            // The held object's box, for the fingers.
            bool      box{ false };
            bool      cylinder{ false };
            int       axis{ -1 };
            float     radius{ 0.0f };
            uintptr_t root{ 0 };
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };
            int       serial{ 0 };
        };
        std::mutex             g_track_mutex;
        Track                  g_track{};
        // Contact, per physics step of the held spring (g_track_mutex).
        struct Contact {
            uintptr_t spring{ 0 };
            int       serial{ 0 };
            bool      have_prev{ false };
            glm::vec3 prev_held{ 0.0f };     // the held point at the step before's start...
            glm::vec3 prev_target{ 0.0f };   // ...where that step sent it...
            float     prev_elastic{ 0.0f };  // ...and how (lands prev_held + elastic x (target - held) with damping 1)
            float     prev_damping{ 0.0f };
            float     prev_force{ 0.0f };
            float     prev_dt{ 0.0f };
            unsigned  recent{ 0 };           // pushed or not, the last 4 measured steps (bit 0 the latest)
            int       steps{ 0 };
            int       contact_until{ -1 };   // held softly until this step
            float     soft{ 1.0f };          // the elasticity cap now (eased toward kContactElastic or 1)
        };
        Contact                g_contact{};
        // For the physics probe (test builds): the held spring and body as the grab update read them.
        struct DebugHeldInfo {
            uintptr_t spring{ 0 };
            uint32_t  body{ 0 };
            int64_t   us{ 0 };
        };
        std::mutex    g_debug_mutex; // taken last (nothing else is locked while it is held)
        DebugHeldInfo g_debug{};
        std::atomic<uintptr_t> g_track_spring{ 0 }; // the held spring while the hand holds it
        std::atomic<int>       g_steps{ 0 };
        std::atomic<int>       g_blocked_steps{ 0 };
        std::atomic<int64_t>   g_step_dt_us{ 0 };
        std::atomic<int64_t>   g_step_age_us{ 0 };

        // Where the game's springs really hold the object. The spring keeps its grab point in the physics body's own
        // space (the middle of the collision shape's box, set when the springs are made); the spring's first physics
        // step, with the object still at rest, gives that point in the game world, and the object's 3D as it was then
        // turns it into the object's own space (g_track_mutex).
        struct Capture {
            bool      armed{ false };   // waiting for the spring's first step
            bool      have{ false };
            int       serial{ 0 };      // the grab (g_taken.serial)
            uintptr_t spring{ 0 };
            glm::mat3 r{ 1.0f };        // the object's 3D in the game world when the springs were made
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
            bool      still{ true };    // ...not moving then (its 3D was where it was the frame before)
            float     moved{ -1.0f };   // how far it had moved since the frame before (m; below 0: not known)
            float     turned{ -1.0f };  // ...and turned (radians, about)
            float     fresh[3]{};       // the spring's kept error before its first step...
            bool      know_fresh{ false };
            bool      stepped{ false }; // ...which it no longer had when the springs were made (it had stepped already)
            glm::vec3 held{ 0.0f };     // the held point, object space
            glm::vec3 world{ 0.0f };    // ... and where it was in the game world
            bool      first{ false };   // seen at the spring's very first step (nothing moved yet)
        };
        Capture                g_capture{};
        std::atomic<uintptr_t> g_capture_spring{ 0 }; // the spring whose first step is awaited (set/cleared under g_track_mutex)
        std::atomic<int>       g_captures{ 0 };

        // Following the hand while you move (g_track_mutex). The grab update reads the hand (the frame's controller sample) and
        // the play area; the shown body draws the hand from the same sample, but where the play area is by then. Both are
        // noted each frame; the difference is how far where the hand holds the object moved in between.
        std::atomic<uint64_t> g_frame{ 0 }; // frames (counted at each frame's start, main thread)
        struct FrameRead {
            uint64_t  frame{ 0 };
            bool      valid{ false };
            glm::vec3 anchor{ 0.0f };      // the play area's place (its origin in the game world)...
            glm::mat3 room{ 1.0f };        // ...and turn
            glm::vec3 point{ 0.0f };       // the held point's place in the hand, as read (game world)
            int       key{ 0 };            // the grab, hand and way of holding it (a change is no motion)
            int64_t   us{ 0 };
        };
        FrameRead g_read{};        // by the grab update
        FrameRead g_read_before{}; // ...the one before
        struct DrawRead {
            uint64_t  frame{ 0 };
            bool      valid{ false };
            glm::vec3 anchor{ 0.0f };
            glm::mat3 room{ 1.0f };
            int       steps{ 0 };          // physics steps between the grab update and the drawing (-1: drawn before the update)
            int64_t   us{ 0 };
        };
        DrawRead g_draw{};         // by the body, as it places the hands
        struct Follow {
            bool      have_move{ false };
            glm::vec3 move{ 0.0f };        // the latest measured move of where the hand holds it (drawn - read)...
            int       move_steps{ 0 };     // ...the physics steps before the hand was drawn then...
            uint64_t  move_frame{ 0 };     // ...and its frame
            // How well that frame's move would have foretold the next one as it was (per frame) or per physics step.
            float     err_frame{ 0.0f }, err_step{ 0.0f };
            int       err_samples{ 0 };
            bool      per_step{ false };
            bool      have_drawn{ false };
            glm::vec3 drawn{ 0.0f };       // where the hand held it as drawn...
            uint64_t  drawn_frame{ 0 };
            glm::vec3 drawn_anchor{ 0.0f };  // ...the play area then...
            glm::mat3 drawn_room{ 1.0f };
            int       drawn_key{ 0 };
            glm::vec3 drawn_lead{ 0.0f };  // ...and how far that moved from the frame before (the same grab and hand)
            bool      have_drawn_lead{ false };
            // How much of the frame before's motion leads it, learned from where drawn objects end up (the play area's part, as
            // you walk or turn, needed about half a frame where the hand's own needed a whole one: 3.8 cm ahead at 7.7 cm a
            // frame of walking, 0.1 cm at 1.3 cm a frame of the hand's own, with a whole frame of both).
            float     lead_area{ 0.5f }; // (starting from what was measured)
            float     lead_hand{ 1.0f };
            // Learned only from frames the lead alone decided: the last few frames whose target was led that way and how
            // stiffly it was held then (not yet in full during a grab's first quarter second), by frame & 3 (the frame a
            // drawing shows is the one before the frame it is measured in, whose grab update has already run by then), and
            // the last frame a physics step was held back (blocked by something, or held softly while touching something).
            uint64_t  led_frames[4]{};
            float     led_elastic[4]{};
            uint64_t  disturbed_frame{ 0 };
            // That frame's motion split into the play area's part (you walk or turn) and the hand's own (for the log).
            bool      have_parts{ false };
            glm::vec3 area_move{ 0.0f }, hand_move{ 0.0f };
            uint64_t  parts_frame{ 0 };
            // The orientation your hand holds the object in, as read by the grab update that frame, and how far the hand
            // turned since the frame before (degrees; below 0: not known) (for the log).
            bool      have_want{ false };
            glm::mat3 want{ 1.0f };
            uint64_t  want_frame{ 0 };
            float     hand_turn{ -1.0f };
            // The targets the physics steps were given: the last of the latest frame with steps, and of the one before.
            glm::vec3 target{ 0.0f }, target_before{ 0.0f };
            uint64_t  target_frame{ 0 }, target_before_frame{ 0 };
            // Does a drawn object show the physics of its own frame, or of the frame before?
            int       fresh_votes{ 0 }, stale_votes{ 0 };
            int       shows{ 0 };          // 0 not known yet (taken as its own frame's), 1 its own frame's, 2 the frame before's
        };
        Follow g_follow{};
        struct FollowStats {
            int    drawn{ 0 };             // frames the hands were drawn after the grab update...
            int    drawn_first{ 0 };       // ...or before it
            int    drawn_before_steps{ 0 }; // ...of those after it, drawn before any physics step of the frame
            double delay_us{ 0.0 };
            double steps_before{ 0.0 };
            int    exact{ 0 }, predicted{ 0 }, fallback{ 0 };
            // The held object as drawn vs where the hand held it: standing still, moving the hand (the play area still), and
            // walking or turning (the play area moving); "along" is the part of the gap along that motion (behind: below 0).
            int    still_n{ 0 };
            double still_sum{ 0.0 };
            float  still_max{ 0.0f };
            int    hand_n{ 0 };
            double hand_sum{ 0.0 }, hand_along_sum{ 0.0 }, hand_move_sum{ 0.0 };
            float  hand_max{ 0.0f };
            int    moving_n{ 0 };
            double moving_sum{ 0.0 }, along_sum{ 0.0 }, move_sum{ 0.0 };
            float  moving_max{ 0.0f };
            // Its orientation as drawn vs the one the hand holds it in (degrees): the hand still, and the hand turning.
            int    rot_still_n{ 0 }, rot_turn_n{ 0 };
            double rot_still_sum{ 0.0 }, rot_turn_sum{ 0.0 }, rot_rate_sum{ 0.0 };
            float  rot_still_max{ 0.0f }, rot_turn_max{ 0.0f };
            int    rot_led{ 0 };           // frames its orientation was led one more frame of the hand's turn
            // Physics steps: measured for contact, pushed off where the step before sent it (how far, which way), held softly.
            int    push_steps{ 0 }, pushed{ 0 };
            double push_sum{ 0.0 };
            float  push_max{ 0.0f };
            int    push_up{ 0 }, push_down{ 0 }, push_away{ 0 }, push_toward{ 0 }, push_side{ 0 };
            int    soft_steps{ 0 };
            // Drawn in the hand (the scene writer): frames, of those exactly in the hand, how far from physics (sum, m), whether
            // the node was where it was drawn after the writer (checked, off), and still there at the next frame's start.
            int    present_frames{ 0 }, present_in_hand{ 0 }, present_kept{ 0 }, present_lost{ 0 }, present_wrote{ 0 }, present_wrote_off{ 0 };
            double present_offset{ 0.0 };
        };
        FollowStats g_follow_stats{};

        // Thrown by the spring itself (g_track_mutex): when the physics world's velocity function cannot be used, the held
        // spring keeps stepping after you let go until the next grab update drops it (a frame), each step's target the held
        // point moved on by the throw's velocity x the step (elasticity and damping 1: the held point leaves with exactly that
        // velocity). With the hold's target fixed for a frame, a second step in a frame would otherwise leave it nearly still.
        struct Throw {
            uintptr_t spring{ 0 };
            glm::vec3 velocity{ 0.0f };
            bool      done{ false };     // at least one step gave it the velocity
            int       frames{ 0 };       // grab updates waited for that step
            int64_t   us{ 0 };           // when it was armed (a throw not dropped soon after is forgotten)
        };
        Throw                  g_throw{};
        std::atomic<uintptr_t> g_throw_spring{ 0 };
        thread_local uintptr_t t_step_spring{ 0 }; // the spring step running on this thread, and its dt
        thread_local float     t_step_dt{ 0.0f };

        // Gravity gloves. The pull the grips ask for (g_mutex), and the flight under way in the grab update (g_mutex).
        struct PullAsk {
            bool    pending{ false };
            int64_t us{ 0 };
        };
        PullAsk g_pull_ask{};
        struct Pull {
            bool      active{ false };
            bool      have_from{ false };
            int64_t   start_us{ 0 };
            float     duration{ 0.5f }; // s
            float     distance{ 0.0f }; // m
            glm::vec3 from{ 0.0f };     // where the held point started (game world)
        };
        Pull              g_pull{};
        std::atomic<int>  g_caught_hand{ -1 }; // a pull reached the hand: the grips tick it (main thread)
        std::atomic<bool> g_in_flight{ false }; // a pulled object is on its way to the hand (the fingers wait for it)
        std::atomic<bool> g_needs_catch{ false };  // pulled as the grip was let go: a squeeze catches it, else it drops...
        std::atomic<int64_t> g_catch_deadline_us{ 0 }; // ...once this time has passed (set when it is at the hand)
        bool              g_release_keep{ false }; // the next drop keeps the object's own speed (g_mutex)
        std::atomic<int>  g_pulls{ 0 };
        std::atomic<int>  g_repicks{ 0 };      // grab starts whose head ray was skipped
        thread_local bool t_hand_start{ false }; // the grab start running now was started by a grip (its pick is the hand's)

        // From the grab start's "object taken" hook to its "springs made" hook, in the same call on the same thread.
        struct Made {
            bool      valid{ false };
            int       serial{ 0 };
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
            bool      still{ true };
            float     moved{ -1.0f };
            float     turned{ -1.0f };
        };
        thread_local Made t_made{};

        std::atomic<int>  g_hand{ 1 };              // the hand holding (0 left, 1 right)
        std::atomic<bool> g_grip_grab{ false };     // the grip holds the current grab (started or taken with it)
        std::atomic<int>  g_start_hand{ -1 };       // a grip waiting for the pick to follow its hand
        std::atomic<bool> g_right_latched{ false }; // the right grip was squeezed to grab: no aiming until it is let go
        std::atomic<bool> g_left_free{ false };     // the left grip is down and grabbing nothing: it may work the d-pad
        std::atomic<bool> g_left_weapon_grip{ false }; // (fix154) the left grip is the left hand's weapon's (dual wielding)
        std::atomic<int>  g_grabs{ 0 };
        std::atomic<int>  g_grip_grabs{ 0 };
        std::atomic<int>  g_takeovers{ 0 };
        std::atomic<int>  g_throws{ 0 };

        // Main thread only.
        bool      g_grip_down[2]{ false, false };
        // A grip another hand action took (a grenade from behind the shoulder, a power pushed out): the hand grab leaves it
        // alone until it is let go. 0: not taken; 1: taken, the grab has not seen it squeezed yet (the two read the grip a
        // moment apart); 2: seen squeezed. The frame it was taken (a grip the grab never sees squeezed is let go of after a few).
        std::atomic<int>      g_left_alone[2]{ 0, 0 };
        std::atomic<uint64_t> g_left_alone_frame[2]{ 0, 0 };
        int       g_start_wait{ 0 };
        glm::vec3 g_linear_history[2][kHistory]{};
        glm::vec3 g_angular_history[2][kHistory]{};
        int       g_history_next[2]{ 0, 0 };
        int64_t   g_last_nothing_log{ 0 };
        struct PoseTrack {
            bool      have{ false };
            glm::vec3 at{ 0.0f };
            glm::mat3 turn{ 1.0f };
            int64_t   us{ 0 };
            glm::vec3 linear{ 0.0f }, angular{ 0.0f }; // from the last two poses (stage space)
        };
        PoseTrack g_pose_track[2];
        bool      g_speed_source_told{ false };
        std::atomic<int> g_aim_hand{ -1 };  // gravity gloves: the hand aiming (its grip held, nothing was next to it)
        uint32_t  g_aim_target{ 0 };        // what it has locked on (the pick's handle; 0: nothing yet)
        glm::vec3 g_lock_ray{ 0.0f };       // the hand's pointing direction when it locked on
        uint32_t  g_seen_target{ 0 };       // not locked yet: what the cast has found...
        int       g_seen_frames{ 0 };       // ...for this many frames running
        int       g_far_frames_left{ 0 };   // an aim just ended: the pick may still hold a far cast's result this many frames
        float     g_aim_peak{ 0.0f };       // locked: the fastest the hand has moved back toward you since (m/s), for the log
        int64_t   g_aim_release_us{ 0 };    // locked: when the grip was let go (a flick just then still pulls)
        int       g_aims{ 0 }, g_locks{ 0 }; // for the log
        uint32_t  g_pull_handle{ 0 };       // what the latest pull went for (the pick's handle)
        uint32_t  g_refused[kRefusedKept]{}; // what the game would not take when pulled: the aim skips it for a minute
        int64_t   g_refused_us[kRefusedKept]{};
        int       g_refused_next{ 0 };
        struct CastSeen {
            uint32_t  handle{ 0 };
            glm::vec3 point{ 0.0f };
            int       frame{ -100 };        // the aim's frame it was read in
        };
        CastSeen  g_seen_narrow{}, g_seen_wide{}; // aiming, not locked: what each cast found last
        bool      g_cast_narrow{ false };   // the far cast asked for next is the narrow one
        int       g_aim_frames{ 0 };        // frames aimed (for how fresh a find is)
        // This aim, for the log when it ends without locking on: when it started, its far looks and how many found something.
        int64_t   g_aim_start_us{ 0 };
        int       g_aim_looks{ 0 };
        int       g_aim_finds{ 0 };
        int       g_aim_ends_told{ 0 };
        int       g_cast_count_seen{ 0 };   // the hand casts counted when last looked at
        float     g_lock_radius{ kPullRadius }; // locked: the cast that found it

        bool Refused(uint32_t handle)
        {
            const int64_t now = NowUs();
            for (int i = 0; i < kRefusedKept; ++i) {
                if (handle != 0 && g_refused[i] == handle && now - g_refused_us[i] < 60'000'000) {
                    return true;
                }
            }
            return false;
        }

        bool Active()
        {
            static auto vr = VR::get();
            return GameFlow::gStore.internalSettings.handGrab && vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isInFirstPerson() &&
                   !ModSettings::showFlatScreenDisplay() && !GameFlow::isOnLadder(); // (fix143: the grips climb a ladder)
        }

        // ---- Your hands with a weapon out: free if it is only your fists ----
        // As HIGGS lets a hand with fists (or nothing) grab in Skyrim VR with weapons drawn, fists out count as empty hands:
        // in Starfield melee, attacking or the mod menu's stick clicks bring them out. Starfield 1.16.236, CommonLibSF's
        // layouts (they match it where this mod has checked them: the character controller at +0x458 of the same process
        // data): PlayerCharacter+0x228 its AI process, +0x8 that one's middle-high data, +0x270 its equipped items (count,
        // capacity, items of 0x28 bytes; guarded by the lock at +0x268); an item's +0x0 is its form. A weapon (form type 0x30
        // at +0x2e; its id at +0x28) has its instance data at +0x248, that one's general data at +0x50 and the weapon's type at
        // +0x38 there: 0 is hand-to-hand, the fists (the game's own melee reads the same fields: ImpactMelee). Only fists found
        // makes the hands free; anything else, or nothing readable, keeps them off as before (and the log says what was found).
        constexpr uintptr_t kMiddleEquipped    = 0x270;
        constexpr uintptr_t kEquippedItemSize  = 0x28;
        constexpr uint32_t  kEquippedMax       = 16;
        constexpr uintptr_t kFormId            = 0x28;
        constexpr uintptr_t kFormType          = 0x2e;
        constexpr uint8_t   kFormTypeWeapon    = 0x30;
        constexpr uintptr_t kWeaponInstance    = 0x248;
        constexpr uintptr_t kInstanceGeneral   = 0x50;
        constexpr uintptr_t kGeneralType       = 0x38;
        constexpr uint32_t  kWeaponHandToHand  = 0;
        // Grenades and mines (types 10 and 11, as Fallout 4's weapon types; the Frag Grenade, 000115EF, read as 10) stay equipped
        // in their own slot whatever is in your hands: thrown with their own button (RB), never held. They do not count.
        constexpr uint32_t  kWeaponGrenade     = 10;
        constexpr uint32_t  kWeaponMine        = 11;
        constexpr int64_t   kArmedRecheckUs    = 100'000; // read again at most this often

        // Melee weapons (Fallout 4's types 1 to 6: swords, daggers, axes, maces, two-handed ones): no gun sight for those.
        constexpr uint32_t  kWeaponMeleeFirst  = 1;
        constexpr uint32_t  kWeaponMeleeLast   = 6;

        enum class Armed { Unknown, Fists, Weapon };
        struct ArmedSeen {
            Armed       armed{ Armed::Unknown };
            std::string what;  // for the log
            int64_t     us{ 0 };
            bool        ranged{ false };    // the weapon in your hands shoots (or its type could not be read)
            bool        throwable{ false }; // a grenade or mine is equipped in its own slot
            uint32_t    weapon_id{ 0 };     // the weapon in your hands (its form id; 0: none, or fists)
            uintptr_t   weapon_form{ 0 };   // and its form
            uintptr_t   throwable_form{ 0 }; // the grenade or mine in its slot (its form; 0: none)
            bool        gun_known{ false }; // the weapon in your hands is a gun by its type, read (fix135: not a weapon whose type is unknown)
        };
        std::atomic<uintptr_t> g_weapon_form{ 0 }; // the same, for any thread
        std::mutex g_armed_mutex;
        ArmedSeen  g_armed{};
        int        g_armed_tells{ 0 };

        // What you have equipped as weapons now: fists only, another weapon (or one whose type could not be read), or no
        // weapon found (or the list could not be read). Also whether that weapon shoots, and whether a grenade or mine is in
        // its slot.
        Armed ReadArmed(std::string& what, bool& ranged, bool& throwable, uint32_t& weapon_id, uintptr_t& weapon_form, uintptr_t& throwable_form,
            bool& gun_known)
        {
            ranged         = false;
            throwable      = false;
            weapon_id      = 0;
            weapon_form    = 0;
            throwable_form = 0;
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  process = 0, middle = 0;
            struct List {
                uint32_t  size;
                uint32_t  capacity;
                uintptr_t items;
            } list{};
            if (player == 0 || !SafeRead(&process, player + kActorProcess, sizeof(process)) || !SafeRead(&middle, process + kProcessMiddle, sizeof(middle)) ||
                !SafeRead(&list, middle + kMiddleEquipped, sizeof(list)) || list.size > list.capacity || list.capacity > 256 ||
                (list.size > 0 && list.items == 0)) {
                what = "your equipped items could not be read";
                return Armed::Unknown;
            }
            int fists = 0, others = 0;
            for (uint32_t i = 0; i < std::min(list.size, kEquippedMax); ++i) {
                uintptr_t form = 0;
                uint8_t   type = 0;
                if (!SafeRead(&form, list.items + i * kEquippedItemSize, sizeof(form)) || form == 0 || !SafeRead(&type, form + kFormType, sizeof(type)) ||
                    type != kFormTypeWeapon) {
                    continue;
                }
                uint32_t  id = 0, weapon_type = 0xffffffffu;
                uintptr_t instance = 0, general = 0;
                SafeRead(&id, form + kFormId, sizeof(id));
                const bool typed = SafeRead(&instance, form + kWeaponInstance, sizeof(instance)) && instance != 0 &&
                                   SafeRead(&general, instance + kInstanceGeneral, sizeof(general)) && general != 0 &&
                                   SafeRead(&weapon_type, general + kGeneralType, sizeof(weapon_type));
                const bool thrown = typed && (weapon_type == kWeaponGrenade || weapon_type == kWeaponMine);
                if (typed && weapon_type == kWeaponHandToHand) {
                    ++fists;
                } else if (!thrown) {
                    ++others;
                    if (typed && (weapon_type < kWeaponMeleeFirst || weapon_type > kWeaponMeleeLast)) {
                        gun_known = true;
                    }
                    if (!typed || weapon_type < kWeaponMeleeFirst || weapon_type > kWeaponMeleeLast) {
                        ranged = true;
                    }
                    if (weapon_form == 0) {
                        weapon_id   = id;
                        weapon_form = form;
                    }
                }
                if (thrown) {
                    throwable = true;
                    if (throwable_form == 0) {
                        throwable_form = form;
                    }
                }
                what += fmt::format("{}weapon {:08x} ({})", what.empty() ? "" : ", ", id,
                    !typed                              ? std::string{ "its type not readable" }
                    : weapon_type == kWeaponHandToHand ? std::string{ "hand-to-hand: fists" }
                    : thrown ? fmt::format("type {}: a {} in its own slot, not in your hands", weapon_type, weapon_type == kWeaponGrenade ? "grenade" : "mine")
                             : fmt::format("type {}", weapon_type));
            }
            if (others > 0) {
                return Armed::Weapon;
            }
            if (fists > 0) {
                return Armed::Fists;
            }
            what = fmt::format("no weapon in your hands among your {} equipped items{}", list.size, what.empty() ? "" : fmt::format(" ({})", what));
            return Armed::Unknown;
        }

        // What is equipped, read again every 0.1 s at most (with g_armed_mutex held). Said in the log when it changes while
        // the weapon is out.
        const ArmedSeen& ArmedNowLocked()
        {
            const int64_t now = NowUs();
            if (g_armed.us == 0 || now - g_armed.us >= kArmedRecheckUs) {
                std::string what;
                bool        ranged = false, throwable = false, gun_known = false;
                uint32_t    weapon_id      = 0;
                uintptr_t   weapon_form    = 0;
                uintptr_t   throwable_form = 0;
                const Armed armed  = ReadArmed(what, ranged, throwable, weapon_id, weapon_form, throwable_form, gun_known);
                if ((armed != g_armed.armed || what != g_armed.what) && g_armed_tells < 30 && GameFlow::isWeaponDrawn()) {
                    ++g_armed_tells;
                    spdlog::info("[Grab] with your weapon out: {}; {}", what,
                        armed == Armed::Fists ? "your hands still grab and pull (fists count as empty hands)"
                                              : "your hands do not grab or pull until it is put away");
                }
                g_armed       = ArmedSeen{ armed, std::move(what), now, ranged, throwable, weapon_id, weapon_form, throwable_form, gun_known };
                g_weapon_form = weapon_form;
            }
            return g_armed;
        }

        // Your hands can grab and pull: no weapon out, or only your fists (main thread).
        bool HandsFreeNow()
        {
            if (!GameFlow::isWeaponDrawn()) {
                return true;
            }
            std::scoped_lock _{ g_armed_mutex };
            return ArmedNowLocked().armed == Armed::Fists;
        }

        // Each hand busy with the hand grab (holding, aiming the gloves, a grip deciding what to grab) and for a moment after:
        // its swings are then not melee (impact melee), so with your fists out a throw does not also punch.
        std::atomic<int64_t> g_busy_until[2]{ 0, 0 };
        constexpr int64_t    kBusyAfterUs = 600'000;

        // A grip squeezed now would grab (hand interaction is on: weapon put away or only fists out, first person, no menu).
        bool GripGrabsHere()
        {
            const auto& settings = GameFlow::gStore.internalSettings;
            return g_ready.load(std::memory_order_relaxed) && g_grip_ready.load(std::memory_order_relaxed) && settings.handGrab && settings.gripGrab &&
                   tracking::HandInteract();
        }

        uint32_t SpringCount(uintptr_t player)
        {
            uint32_t count = 0;
            return SafeRead(&count, player + kSpringCount, sizeof(count)) && count <= 8 ? count : 0;
        }

        // The first grab spring's address (identifies the grab).
        uintptr_t CurrentSpring(uintptr_t player)
        {
            uintptr_t data = 0, spring = 0;
            if (SpringCount(player) == 0 || !SafeRead(&data, player + kSpringArray, sizeof(data)) || !SafeRead(&spring, data, sizeof(spring))) {
                return 0;
            }
            return spring;
        }

        bool IsPlayer(uintptr_t actor)
        {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            return player != nullptr && actor == reinterpret_cast<uintptr_t>(player);
        }

        int ControllerIndex(int hand)
        {
            static auto vr = VR::get();
            return hand == 0 ? vr->get_left_controller_index() : vr->get_right_controller_index();
        }

        // A hand's rotation in the game world (column-vector). Its local axes do not matter: only how it turns
        // from one moment to another is used.
        bool HandWorldRotation(bool left, glm::mat3& out)
        {
            const auto room = tracking::RoomRotation();
            if (!room) {
                return false;
            }
            const glm::mat4 aim = tracking::AimPose(left);
            glm::mat3       hand;
            for (int c = 0; c < 3; ++c) {
                hand[c] = *room * tracking::ToHavokVector(glm::normalize(glm::vec3{ aim[c] }));
            }
            out = hand;
            return std::isfinite(hand[0][0]) && std::isfinite(hand[1][1]) && std::isfinite(hand[2][2]);
        }

        // The centre of the closed hand in the game world; scale receives game units per tracked metre.
        bool PalmWorld(bool left, glm::vec3& palm, float& scale)
        {
            const glm::mat4 grip    = tracking::GripPose(left);
            const glm::mat4 aim     = tracking::AimPose(left);
            const glm::vec3 forward = glm::normalize(-glm::vec3{ aim[2] });
            glm::vec3       unused{};
            return tracking::StageToWorld(glm::vec3{ grip[3] } + forward * kPalmForward, forward, palm, unused, &scale);
        }

        // The palm's surface and the way it faces (out of the palm), in the game world. OpenXR's grip pose: its +X
        // leaves the back of a right hand and the palm of a left hand; the centre is the middle of the closed fist.
        bool PalmSurfaceWorld(int hand, glm::vec3& surface, glm::vec3& facing)
        {
            const glm::mat4 grip      = tracking::GripPose(hand == 0);
            const bool      left_pose = ControllerIndex(hand) == 1;
            const glm::vec3 x{ grip[0] };
            if (glm::length(x) < 1e-4f) {
                return false;
            }
            const glm::vec3 out = glm::normalize(x) * (left_pose ? 1.0f : -1.0f);
            return tracking::StageToWorld(glm::vec3{ grip[3] } + out * kPalmSkin, out, surface, facing);
        }

        // Where the hand holds things (game world): the palm's surface where an object rests, the way the palm faces, the
        // fingers' way and the line across the palm (orthonormal). With the body shown, the drawn hand's own: its palm at the
        // base of the fingers and its knuckle line (body::GetHandGripFrame, placed with the controller as the body places the
        // hand). Otherwise the controller's: its grip point's palm side and its aim.
        struct GripFrame {
            glm::vec3 surface{ 0.0f };
            glm::vec3 facing{ 0.0f };
            glm::vec3 forward{ 0.0f };
            glm::vec3 side{ 0.0f };
            float     ahead{ 0.0f }; // how far the surface point is toward the fingertips from the palm's centre (m)
            bool      drawn{ false };
        };

        bool HandGripWorld(int hand, GripFrame& out)
        {
            glm::mat3 aim{ 1.0f };
            if (!HandWorldRotation(hand == 0, aim)) {
                return false;
            }
            out = GripFrame{};
            body::HandGripFrame frame;
            if (body::IsActive() && body::GetHandGripFrame(hand == 0, frame)) {
                const glm::mat4 grip = tracking::GripPose(hand == 0);
                glm::vec3       at{}, unused{};
                if (tracking::StageToWorld(glm::vec3{ grip[3] }, glm::vec3{ 0.0f, 0.0f, -1.0f }, at, unused)) {
                    auto world        = [&](const float* v) { return aim * glm::vec3{ v[0], v[1], v[2] }; };
                    out.surface       = at + world(frame.contact);
                    out.facing        = glm::normalize(world(frame.facing));
                    glm::vec3 across  = world(frame.knuckles);
                    across           -= out.facing * glm::dot(across, out.facing);
                    const glm::vec3 fingers = world(frame.fingers);
                    if (glm::length(across) > 1e-3f && Finite(out.surface) && Finite(out.facing)) {
                        out.side    = glm::normalize(across);
                        out.forward = glm::normalize(glm::cross(out.side, out.facing));
                        if (glm::dot(out.forward, fingers) < 0.0f) {
                            out.forward = -out.forward;
                        }
                        out.ahead = glm::dot(world(frame.contact), out.forward); // (from the grip point: the palm's middle)
                        out.drawn = Finite(out.forward) && Finite(out.side) && std::isfinite(out.ahead);
                        if (out.drawn) {
                            return true;
                        }
                    }
                }
            }
            out = GripFrame{};
            if (!PalmSurfaceWorld(hand, out.surface, out.facing)) {
                return false;
            }
            glm::vec3 forward = -aim[2];
            forward -= out.facing * glm::dot(forward, out.facing);
            if (glm::length(forward) < 1e-4f) {
                return false;
            }
            out.forward = glm::normalize(forward);
            out.side    = glm::cross(out.facing, out.forward);
            return true;
        }

        // A node's world transform: a point x of its space is at r * (s * x) + t (its stored rows are glm's columns).
        struct NodeXf {
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
        };

        bool NodeTransform(uintptr_t node, uintptr_t offset, NodeXf& out)
        {
            float raw[16]{};
            if (node == 0 || !SafeRead(raw, node + offset, sizeof(raw))) {
                return false;
            }
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    out.r[a][b] = raw[a * 4 + b];
                }
            }
            out.t = { raw[12], raw[13], raw[14] };
            out.s = raw[15];
            return Finite(out.t) && Finite(out.r[0]) && Finite(out.r[1]) && Finite(out.r[2]) && std::isfinite(out.s) && out.s > 0.01f && out.s < 100.0f &&
                   std::abs(glm::length(out.r[0]) - 1.0f) < 0.1f;
        }

        bool NodeWorld(uintptr_t node, NodeXf& out) { return NodeTransform(node, kNodeWorld, out); }

        glm::vec3 ToNode(const NodeXf& x, const glm::vec3& p) { return glm::transpose(x.r) * (p - x.t) / x.s; }

        // A node transform as stored (NiTransform: three rows of 4 floats, translate, scale), and composed.
        NodeXf FromRaw(const float* raw)
        {
            NodeXf out;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    out.r[a][b] = raw[a * 4 + b];
                }
            }
            out.t = { raw[12], raw[13], raw[14] };
            out.s = raw[15];
            return out;
        }

        NodeXf Compose(const NodeXf& outer, const NodeXf& inner) { return NodeXf{ outer.r * inner.r, outer.r * (outer.s * inner.t) + outer.t, outer.s * inner.s }; }

        NodeXf Inverse(const NodeXf& x)
        {
            const glm::mat3 r = glm::transpose(x.r);
            return NodeXf{ r, -(r * x.t) / x.s, 1.0f / x.s };
        }

        bool FiniteXf(const NodeXf& x) { return Finite(x.r[0]) && Finite(x.r[1]) && Finite(x.r[2]) && Finite(x.t) && std::isfinite(x.s) && x.s > 1e-4f; }

        float AngleBetween(const glm::mat3& a, const glm::mat3& b)
        {
            return std::acos(std::clamp(0.5f * (glm::dot(a[0], b[0]) + glm::dot(a[1], b[1]) + glm::dot(a[2], b[2]) - 1.0f), -1.0f, 1.0f));
        }

        // ---- Drawing the held object in the hand ----
        // Physics moves a held object in fixed steps (60 a second) and the game draws it at its body's pose pushed forward in
        // time, while your hand is drawn where the controller is this frame: frame to frame the object lands a little ahead of
        // or behind the hand, by up to a step of its motion (about 8 cm walking). As ROCK does in Fallout 4 VR (its held scene
        // presentation), the object is drawn where the hand holds it instead, while physics keeps moving the real body as
        // before, through the game's own scene writer.
        // Starfield 1.16.236 (read from the physics test build's dump): a node with a collision object has its world update
        // (NiAVObject::UpdateWorldData, rva 2bd9330, where BSFadeNode's type table slot 79 points) handed to its collision
        // object's own (bhkNPCollisionObject type table slot 56, rva 23781c0). For a moving (dynamic) body that one takes the
        // body's transform from the physics world (or that transform predicted ahead by a time the physics world keeps), puts
        // its place in game units and calls the scene writer (rva 237cb20, from 2378647) with the collision object and that
        // transform (rotation columns of 4 floats at 0, 16 and 32, the place at 48; 32-byte aligned), which writes the node.
        // So for the held object's collision object the writer is given the place in the hand instead. (fix80 gave the root's
        // world update another local transform instead: the writer overwrote it, and putting the old one back afterwards froze
        // the node's local transform.) Fallout 4 VR's writer is 0x1E06B00, which ROCK hooks the same way.
        // While the object touches something (held softly, blocked) or is far from the hand (a wall stops it), it is drawn
        // where physics has it, eased between the two; after the hold, what is left of the difference fades out over a few
        // frames. How the node's pose relates to its body's (node = body x offset) is measured at the hold's first writer
        // calls (three alike in a row), and each write is checked: the node must end up where it was drawn.
        constexpr uintptr_t kWriterRva         = 0x237cb20;
        constexpr uint8_t   kWriterBytes[]     = { 0x4C, 0x8B, 0xDC, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0x80, 0x48, 0x81, 0xEC, 0x80, 0x01, 0x00, 0x00,
                  0x4C, 0x8B, 0x41, 0x10 };
        constexpr uintptr_t kWriterCallRva     = 0x237863a; // lea rdx,[rbp+0xc0] ; mov rcx,r14 ; vzeroupper ; call writer
        constexpr uint8_t   kWriterCallBytes[] = { 0x48, 0x8D, 0x95, 0xC0, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xCE, 0xC5, 0xF8, 0x77, 0xE8 };
        constexpr uintptr_t kWriterCallLength  = 0x12;      // to the call's return address
        constexpr uintptr_t kCollisionOwner    = 0x10;      // NiCollisionObject: the node it moves
        constexpr float     kInHandMaxGap      = 0.12f;  // farther than this from the hand's place: drawn where physics has it (m)...
        constexpr float     kInHandMaxAngle    = 0.52f;  // ...or turned more than this from it (radians, 30 degrees)
        constexpr float     kToPhysicsRate     = 0.35f;  // easing per frame toward physics...
        constexpr float     kToHandRate        = 0.2f;   // ...and back into the hand
        constexpr float     kFadeRate          = 0.6f;   // after the hold: what is left of the difference, kept per frame
        constexpr int       kOffsetAlike       = 3;      // writer calls in a row giving the same node-from-body offset before it is used...
        constexpr float     kOffsetSameM       = 0.001f; // ...the same: within 1 mm...
        constexpr float     kOffsetSameRad     = 0.0035f; // ...and 0.2 degrees
        constexpr int       kOffsetTries       = 90;     // calls measured before giving up (the node does not follow the writer's input)
        constexpr float     kWroteSameM        = 0.001f; // the node after the writer within this of where it was drawn (and 0.2 degrees)...
        constexpr int       kWroteCheck        = 30;     // ...checked over the first writes of a hold: most of them off, the hold is drawn
        constexpr int       kWroteOffMax       = 20;     //    where physics has it

        struct Present {
            uintptr_t root{ 0 };
            uintptr_t collision{ 0 };        // the root's own collision object, moving the held body (0: none, not drawn in the hand)
            int       hand{ 0 };
            bool      holding{ false };      // held in the hand (not flying) and its place in the hand known
            uint64_t  frame{ 0 };            // the grab update that set it
            int       serial{ 0 };           // the grab
            glm::mat3 in_hand_r{ 1.0f };     // the object's rotation and origin in the hand's frame (HandFrame)
            glm::vec3 in_hand_t{ 0.0f };
            // The node's pose from its body's (node = body x offset), measured at the first writer calls.
            NodeXf    offset{};
            NodeXf    offset_last{};
            int       offset_alike{ 0 };
            int       offset_tries{ 0 };
            bool      offset_known{ false };
            bool      off{ false };           // not drawn in the hand this hold (the writer did not do as expected)
            int       wrote{ 0 }, wrote_off{ 0 }; // writes checked, and of those the node not where it was drawn
            // The writer's, once a frame:
            uint64_t  drawn_frame{ 0 };
            float     physics_weight{ 1.0f }; // 0: drawn in the hand, 1: where physics has it
            glm::vec3 diff_t{ 0.0f };         // drawn - physics (world), faded out after the hold
            glm::quat diff_q{ 1.0f, 0.0f, 0.0f, 0.0f };
            NodeXf    physics{};              // where physics had it that frame (the node's world, as the game would draw it)...
            NodeXf    drawn{};                // ...and where it was drawn
            bool      told_missing{ false };
            bool      told_wrote{ false };
            uint64_t  release_frame{ 0 };     // the hold ended (what is left fades out, then this is forgotten)
        };
        std::mutex             g_present_mutex; // taken last (nothing else is locked while it is held)
        Present                g_present{};
        std::atomic<uintptr_t> g_present_collision{ 0 }; // the collision object whose writer calls are changed, for a quick look first
        std::atomic<uintptr_t> g_present_root{ 0 };      // its node
        constexpr uint64_t     kForgetFrames = 20;     // frames after a hold's end that its object is forgotten (faded by then)
        std::atomic<int>       g_present_frames{ 0 }, g_present_in_hand{ 0 }, g_present_kept{ 0 }, g_present_lost{ 0 };
        std::atomic<int>       g_present_wrote{ 0 }, g_present_wrote_off{ 0 };
        std::atomic<int64_t>   g_present_offset_um{ 0 };
        std::atomic<int>       g_present_tells{ 0 };      // log lines about how holds are drawn...
        std::atomic<int>       g_present_fail_tells{ 0 }; // ...and about drawing in the hand failing
        safetyhook::InlineHook g_writer_hook{};
        uintptr_t              g_writer_return{ 0 };   // the collision object's world update's call (its return address)
        std::atomic<bool>      g_writer_ready{ false };
        std::atomic<int>       g_writer_others{ 0 };   // calls for the held object from anywhere else (left as they are)
        void                   OnWriter(void* collision, float* transform);

        // The hand's frame for what it holds: the controller's aim rotation and grip point in the game world (the body
        // places the hand from the same controller pose).
        bool HandFrame(int hand, glm::mat3& r, glm::vec3& t)
        {
            if (!HandWorldRotation(hand == 0, r)) {
                return false;
            }
            const glm::mat4 grip = tracking::GripPose(hand == 0);
            glm::vec3       unused{};
            return tracking::StageToWorld(glm::vec3{ grip[3] }, glm::vec3{ 0.0f, 0.0f, -1.0f }, t, unused) && Finite(t);
        }

        // A writer's transform as a pose (scale 1), and a pose put into a copy of one (its fourth floats kept).
        NodeXf FromWriter(const float* in)
        {
            NodeXf x = FromRaw(in);
            x.s      = 1.0f;
            return x;
        }

        void ToWriter(const NodeXf& x, const float* in, float* out)
        {
            std::memcpy(out, in, 16 * sizeof(float));
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    out[a * 4 + b] = x.r[a][b];
                }
            }
            out[12] = x.t.x;
            out[13] = x.t.y;
            out[14] = x.t.z;
        }

        bool Rigid(const glm::mat3& r)
        {
            for (int i = 0; i < 3; ++i) {
                if (std::abs(glm::length(r[i]) - 1.0f) > 0.01f) {
                    return false;
                }
            }
            return std::abs(glm::determinant(r) - 1.0f) < 0.03f;
        }

        // Where physics has a held object's 3D: while it is drawn in the hand its world is the drawn place, so the place the
        // writer was given for it (the latest, within two frames); otherwise its world.
        bool HeldWorld(uintptr_t root, NodeXf& out)
        {
            if (root != 0 && g_present_root.load(std::memory_order_acquire) == root) {
                const uint64_t   frame = g_frame.load(std::memory_order_relaxed);
                std::scoped_lock _{ g_present_mutex };
                if (g_present.root == root && g_present.drawn_frame != 0 && g_present.drawn_frame + 2 >= frame) {
                    out = g_present.physics;
                    return true;
                }
            }
            return NodeWorld(root, out);
        }

        // The held object's hold has ended (or stopped being updated): what is left of the difference fades out.
        void EndPresent()
        {
            std::scoped_lock _{ g_present_mutex };
            if (g_present.holding) {
                g_present.holding       = false;
                g_present.release_frame = g_frame.load(std::memory_order_relaxed);
            }
        }

        // Where a held object is drawn now (for the fingers): in the hand, eased with physics as the writer does it.
        bool DrawnWorld(uintptr_t root, NodeXf& out)
        {
            Present p;
            bool    drawn_here = false;
            {
                std::scoped_lock _{ g_present_mutex };
                if (root != 0 && g_present.root == root && g_present.drawn_frame != 0 && !g_present.off &&
                    g_present_root.load(std::memory_order_relaxed) == root) {
                    p          = g_present;
                    drawn_here = true;
                }
            }
            NodeXf physics;
            if (!drawn_here || !HeldWorld(root, physics)) {
                return NodeWorld(root, out);
            }
            glm::mat3 hand_r{ 1.0f };
            glm::vec3 hand_t{ 0.0f };
            if (p.holding && HandFrame(p.hand, hand_r, hand_t)) {
                const glm::mat3 r = hand_r * p.in_hand_r;
                const float     w = p.physics_weight;
                out.t             = glm::mix(hand_r * p.in_hand_t + hand_t, physics.t, w);
                out.r             = glm::mat3_cast(glm::slerp(glm::quat_cast(r), glm::quat_cast(physics.r), w));
                out.s             = physics.s;
            } else {
                out.t = physics.t + p.diff_t;
                out.r = glm::mat3_cast(p.diff_q * glm::quat_cast(physics.r));
                out.s = physics.s;
            }
            return FiniteXf(out) || NodeWorld(root, out);
        }

        // At each frame's start: a hold the grab update stopped updating ends; an ended one is forgotten once faded.
        void SweepPresent()
        {
            const uint64_t frame = g_frame.load(std::memory_order_relaxed);
            std::scoped_lock _{ g_present_mutex };
            auto& p = g_present;
            if (p.root == 0) {
                return;
            }
            if (p.holding && p.frame + 30 < frame) {
                p.holding       = false;
                p.release_frame = frame;
            }
            if (!p.holding && p.release_frame + kForgetFrames < frame) {
                p = Present{};
                g_present_collision.store(0, std::memory_order_release);
                g_present_root.store(0, std::memory_order_release);
            }
        }

        // Where the palm meets the box (object space; the palm at p faces n). Outside the box: the first point of the box
        // straight out of the palm, else the box's point nearest the palm. Inside it: the box's side behind the palm, so
        // the object comes out in front of the palm.
        glm::vec3 ContactPoint(const glm::vec3& p, const glm::vec3& n, const glm::vec3& lo, const glm::vec3& hi)
        {
            auto ray = [&](const glm::vec3& from, const glm::vec3& dir, float& t_in, float& t_out) {
                t_in  = -1e30f;
                t_out = 1e30f;
                for (int a = 0; a < 3; ++a) {
                    if (std::abs(dir[a]) < 1e-8f) {
                        if (from[a] < lo[a] || from[a] > hi[a]) {
                            return false;
                        }
                        continue;
                    }
                    float t0 = (lo[a] - from[a]) / dir[a];
                    float t1 = (hi[a] - from[a]) / dir[a];
                    if (t0 > t1) {
                        std::swap(t0, t1);
                    }
                    t_in  = std::max(t_in, t0);
                    t_out = std::min(t_out, t1);
                }
                return t_in <= t_out;
            };
            const bool inside = p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
            float      t_in = 0.0f, t_out = 0.0f;
            if (inside) {
                return ray(p, -n, t_in, t_out) ? p - n * t_out : p;
            }
            if (ray(p, n, t_in, t_out) && t_in >= 0.0f) {
                return p + n * t_in;
            }
            return glm::clamp(p, lo, hi);
        }

        // How far (m) to raise a box resting on the palm by the point `contact` so none of it is under the surface of the
        // hand: the palm and the open fingers, a patch kHandHalfLength x kHandHalfWidth around a point kHandAhead in front
        // of the palm's centre (toward the fingertips). n: the way the palm faces; forward: toward the fingertips; ahead: how
        // far the palm's point the contact rests on is toward the fingertips from the palm's centre (m).
        constexpr float kHandAhead      = 0.03f;
        constexpr float kHandHalfLength = 0.10f;
        constexpr float kHandHalfWidth  = 0.05f;

        float RestLift(const NodeXf& xf, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& contact, const glm::vec3& n, const glm::vec3& forward_in,
            float ahead)
        {
            glm::vec3 forward = forward_in - n * glm::dot(forward_in, n);
            if (glm::length(forward) < 1e-4f) {
                return 0.0f;
            }
            forward                = glm::normalize(forward);
            const glm::vec3 side   = glm::cross(n, forward);
            const glm::vec3 centre = forward * (kHandAhead - ahead);
            float           lowest = 0.0f;
            constexpr int   kSteps = 6; // points across each face of the box
            for (int axis = 0; axis < 3; ++axis) {
                const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
                for (int face = 0; face < 2; ++face) {
                    for (int i = 0; i <= kSteps; ++i) {
                        for (int j = 0; j <= kSteps; ++j) {
                            glm::vec3 q;
                            q[axis] = face ? hi[axis] : lo[axis];
                            q[a1]   = lo[a1] + (hi[a1] - lo[a1]) * static_cast<float>(i) / kSteps;
                            q[a2]   = lo[a2] + (hi[a2] - lo[a2]) * static_cast<float>(j) / kSteps;
                            const glm::vec3 d   = xf.r * (xf.s * (q - contact)); // from the palm's point, with the contact on it
                            const glm::vec3 rel = d - centre;
                            if (std::abs(glm::dot(rel, forward)) > kHandHalfLength || std::abs(glm::dot(rel, side)) > kHandHalfWidth) {
                                continue; // not over the hand
                            }
                            lowest = std::min(lowest, glm::dot(d, n));
                        }
                    }
                }
            }
            return std::isfinite(lowest) ? std::min(-lowest, 0.5f) : 0.0f;
        }

        // ---- The held object's real collision shape (Hull) ----

        // Where a line from outside the shape (start, going along d) first meets its surface.
        bool HullEnter(const Hull& h, const glm::vec3& start, const glm::vec3& d, glm::vec3& out)
        {
            if (!h.valid || h.planes.empty()) {
                return false;
            }
            float enter = 0.0f, leave = std::numeric_limits<float>::max();
            for (const glm::vec4& p : h.planes) {
                const glm::vec3 n{ p };
                const float     s0 = glm::dot(n, start) + p.w;
                const float     sd = glm::dot(n, d);
                if (std::abs(sd) < 1e-7f) {
                    if (s0 > 0.0f) {
                        return false; // beside that face, going along it: never inside
                    }
                    continue;
                }
                const float t = -s0 / sd;
                if (sd < 0.0f) {
                    enter = std::max(enter, t);
                } else {
                    leave = std::min(leave, t);
                }
                if (enter > leave) {
                    return false;
                }
            }
            if (!(enter > 0.0f)) {
                return false; // it started inside
            }
            out = start + d * enter;
            return Finite(out);
        }

        // The shape's surface facing the palm (object space): met by the line along the palm's facing d through `through`
        // (its depth along d does not matter), else through the shape's middle. Without its faces, its corner nearest the
        // palm sets how deep the surface is.
        bool HullContact(const Hull& h, const glm::vec3& through, const glm::vec3& d_in, glm::vec3& out)
        {
            if (!h.valid || !(glm::length(d_in) > 1e-6f)) {
                return false;
            }
            const glm::vec3 d      = glm::normalize(d_in);
            const glm::vec3 mid    = 0.5f * (h.lo + h.hi);
            const float     beyond = glm::length(h.hi - h.lo) + 1.0f;
            if (!h.planes.empty()) {
                for (const glm::vec3& p : { through, mid }) {
                    const glm::vec3 on = p - d * glm::dot(p - mid, d);
                    if (HullEnter(h, on - d * beyond, d, out)) {
                        return true;
                    }
                }
                return false;
            }
            float nearest = std::numeric_limits<float>::max();
            for (const glm::vec3& v : h.corners) {
                nearest = std::min(nearest, glm::dot(v, d));
            }
            if (!std::isfinite(nearest)) {
                return false;
            }
            // Beside the shape (a palm still far from it): its middle instead.
            glm::vec3 on = through - d * glm::dot(through - mid, d);
            if (glm::any(glm::lessThan(on, h.lo)) || glm::any(glm::greaterThan(on, h.hi))) {
                on = mid;
            }
            out = on + d * (nearest - h.radius - glm::dot(on, d));
            return Finite(out);
        }

        // As RestLift, for the shape: how far to raise it so none of it is under the palm and open fingers. Points across that
        // patch are followed along the palm's facing into the shape's faces (a face crossing the hand between corners counts),
        // and its corners over the patch (with its convex radius).
        float RestLiftHull(const NodeXf& xf, const Hull& h, const glm::vec3& contact, const glm::vec3& n, const glm::vec3& forward_in, float ahead)
        {
            glm::vec3 forward = forward_in - n * glm::dot(forward_in, n);
            if (glm::length(forward) < 1e-4f) {
                return 0.0f;
            }
            forward                = glm::normalize(forward);
            const glm::vec3 side   = glm::cross(n, forward);
            const glm::vec3 centre = forward * (kHandAhead - ahead);
            float           lowest = 0.0f;
            if (!h.planes.empty()) {
                const glm::mat3 to_object = glm::transpose(xf.r);
                const glm::vec3 n_o       = glm::normalize(to_object * n);
                const float     beyond    = glm::length(h.hi - h.lo) + 1.0f;
                constexpr int   kSteps    = 6;
                for (int i = 0; i <= kSteps; ++i) {
                    for (int j = 0; j <= kSteps; ++j) {
                        const float     a = kHandHalfLength * (2.0f * static_cast<float>(i) / kSteps - 1.0f);
                        const float     b = kHandHalfWidth * (2.0f * static_cast<float>(j) / kSteps - 1.0f);
                        const glm::vec3 p = contact + to_object * (centre + forward * a + side * b) / xf.s; // on the palm, object space
                        glm::vec3       entry{};
                        if (HullEnter(h, p - n_o * beyond, n_o, entry)) {
                            lowest = std::min(lowest, glm::dot(xf.r * (xf.s * (entry - contact)), n));
                        }
                    }
                }
            }
            for (const glm::vec3& v : h.corners) {
                const glm::vec3 d   = xf.r * (xf.s * (v - contact));
                const glm::vec3 rel = d - centre;
                if (std::abs(glm::dot(rel, forward)) > kHandHalfLength || std::abs(glm::dot(rel, side)) > kHandHalfWidth) {
                    continue;
                }
                lowest = std::min(lowest, glm::dot(d, n) - h.radius * xf.s);
            }
            return std::isfinite(lowest) ? std::min(-lowest, 0.5f) : 0.0f;
        }

        // Round objects: two sides of the box within 15% of each other and the third clearly different, and the bounding
        // sphere smaller than the box the way a cylinder's is (a box's corners are filled, so its sphere reaches them: half
        // diagonal / radius about 1; a cylinder's are empty: sqrt(2r^2 + h^2/4) / sqrt(r^2 + h^2/4)). Round when the measured
        // ratio is nearer the cylinder's than the box's: a cylinder along the odd side (a disc when that one is short).
        void ClassifyShape(Taken& taken)
        {
            if (!(taken.fit > 1.0f && taken.fit < 4.0f)) {
                return;
            }
            const glm::vec3 e = taken.hi - taken.lo;
            for (int k = 0; k < 3; ++k) {
                const int   i = (k + 1) % 3, j = (k + 2) % 3;
                const float m = 0.5f * (e[i] + e[j]);
                if (m <= 1e-4f) {
                    continue;
                }
                if (std::abs(e[i] - e[j]) <= 0.15f * std::max(e[i], e[j]) && std::abs(e[k] - m) > 0.15f * m) {
                    const float r        = 0.5f * m;
                    const float h        = 0.5f * e[k];
                    const float expected = std::sqrt(2.0f * r * r + h * h) / std::sqrt(r * r + h * h);
                    if (taken.fit < 1.0f + 0.5f * (expected - 1.0f)) {
                        return; // as full as a box
                    }
                    taken.axis     = k;
                    taken.radius   = r;
                    taken.disc     = e[k] < 0.6f * m;
                    taken.cylinder = !taken.disc;
                    return;
                }
            }
        }

        // How the object sits once turned: which of its axes lies on the palm's facing (palm_axis, its sign), which across
        // the palm (across, -1 when none is chosen).
        struct Seat {
            int   palm_axis{ -1 };
            float palm_sign{ 1.0f };
            int   across{ -1 };
        };

        // The turn (world) that sits the object (rotation r0) in a hand whose palm faces n, fingers toward f, `side` across
        // the palm, the way a hand holds things:
        // - long objects (the longest side over 1.3 x the next): the longest side across the palm (a glass, a bottle, a
        //   handle) and, for a box, the side most facing the palm turned flat onto it;
        // - a disc: flat on the palm;
        // - chunky objects: the side most facing the palm flat on it, then the smallest twist that lines the edges up with
        //   the fingers or across the palm.
        glm::quat SquareTurn(const glm::mat3& r0, const Taken& taken, const glm::vec3& n, const glm::vec3& f, const glm::vec3& side, Seat& seat)
        {
            const glm::vec3 axes[3] = { glm::normalize(r0[0]), glm::normalize(r0[1]), glm::normalize(r0[2]) };
            const glm::vec3 e       = taken.hi - taken.lo;
            seat                    = Seat{};
            if (taken.disc) {
                const int k    = taken.axis;
                seat.palm_axis = k;
                seat.palm_sign = glm::dot(axes[k], n) >= 0.0f ? 1.0f : -1.0f;
                return glm::rotation(axes[k], n * seat.palm_sign);
            }
            int longest = 0;
            for (int i = 1; i < 3; ++i) {
                if (e[i] > e[longest]) {
                    longest = i;
                }
            }
            float next = 0.0f;
            for (int i = 0; i < 3; ++i) {
                if (i != longest) {
                    next = std::max(next, e[i]);
                }
            }
            const bool long_object = taken.cylinder || e[longest] > 1.3f * next;
            if (long_object) {
                const int       k     = taken.cylinder ? taken.axis : longest;
                const glm::quat cross = glm::rotation(axes[k], side * (glm::dot(axes[k], side) >= 0.0f ? 1.0f : -1.0f));
                seat.across           = k;
                if (taken.cylinder) {
                    return cross; // round about that axis: no roll to choose
                }
                // A box: of the other two sides, the one most facing the palm rolls flat onto it (about the across axis).
                int   flat  = -1;
                float along = -1.0f;
                for (int j = 0; j < 3; ++j) {
                    if (j != k && std::abs(glm::dot(cross * axes[j], n)) > along) {
                        along = std::abs(glm::dot(cross * axes[j], n));
                        flat  = j;
                    }
                }
                glm::vec3 b = cross * axes[flat];
                b -= side * glm::dot(b, side);
                if (glm::length(b) < 1e-4f) {
                    return cross;
                }
                b               = glm::normalize(b);
                seat.palm_axis  = flat;
                seat.palm_sign  = glm::dot(b, n) >= 0.0f ? 1.0f : -1.0f;
                const glm::vec3 to   = n * seat.palm_sign;
                const float     roll = std::atan2(glm::dot(glm::cross(b, to), side), glm::dot(b, to));
                return glm::angleAxis(roll, side) * cross;
            }
            // Chunky: the side most facing the palm flat on it, then the smallest twist about the palm's facing.
            int k = 0;
            for (int i = 1; i < 3; ++i) {
                if (std::abs(glm::dot(axes[i], n)) > std::abs(glm::dot(axes[k], n))) {
                    k = i;
                }
            }
            seat.palm_axis      = k;
            seat.palm_sign      = glm::dot(axes[k], n) >= 0.0f ? 1.0f : -1.0f;
            const glm::quat lay = glm::rotation(axes[k], n * seat.palm_sign);
            float           best = 10.0f;
            for (int j = 0; j < 3; ++j) {
                if (j == k) {
                    continue;
                }
                glm::vec3 b = lay * axes[j];
                b -= n * glm::dot(b, n);
                if (glm::length(b) < 1e-4f) {
                    continue;
                }
                b = glm::normalize(b);
                for (const glm::vec3& target : { f, -f, side, -side }) {
                    const float angle = std::atan2(glm::dot(glm::cross(b, target), n), glm::dot(b, target)); // about n, b to target
                    if (std::abs(angle) < std::abs(best)) {
                        best = angle;
                    }
                }
            }
            return best < 9.0f ? glm::angleAxis(best, n) * lay : lay;
        }

        // Where the tracked space's origin is in the game world: it moves as the player walks.
        bool AnchorWorld(glm::vec3& anchor)
        {
            glm::vec3 unused{};
            return tracking::StageToWorld(tracking::StandingOriginPosition(), glm::vec3{ 0.0f, 0.0f, -1.0f }, anchor, unused);
        }

        float TurnCos(const glm::mat3& turn) { return std::clamp(0.5f * (turn[0][0] + turn[1][1] + turn[2][2] - 1.0f), -1.0f, 1.0f); }

        // One more frame of motion for drawn objects that show the frame before's physics: the frame before's motion of where
        // the hand holds it, its play-area part and the hand's own each scaled by what was learned (g_track_mutex held).
        glm::vec3 FrameLead()
        {
            if (g_follow.shows != 2 || !g_follow.have_drawn_lead) {
                return glm::vec3{ 0.0f };
            }
            if (!g_follow.have_parts) {
                return g_follow.drawn_lead;
            }
            return g_follow.area_move * g_follow.lead_area + g_follow.hand_move * g_follow.lead_hand;
        }

        // The turn into the hand as far as it has eased in now (g_mutex held).
        glm::mat3 EasedTurn(int64_t now)
        {
            const float t = std::clamp(static_cast<float>(now - g_turn_us) / static_cast<float>(kRampUs), 0.0f, 1.0f);
            return glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, g_turn, t * t * (3.0f - 2.0f * t)));
        }

        // The grab update's reading and the body's drawing of the same frame are both in (g_track_mutex held): where the hand
        // held the object as drawn (drawn), and how far that is from where the update read it. Physics steps still to come in
        // this frame go exactly there; the next frame foretells its own move from this one (unless the play area jumped).
        bool MeasureMove(glm::vec3& drawn)
        {
            const glm::mat3 turn = g_draw.room * glm::transpose(g_read.room);
            drawn                = g_draw.anchor + turn * (g_read.point - g_read.anchor);
            const glm::vec3 move = drawn - g_read.point;
            if (!Finite(drawn)) {
                return false;
            }
            const bool jump = glm::length(move) > kFollowJump || TurnCos(turn) < kFollowJumpCos; // a snap turn, a teleport, a load
            if (!jump && g_follow.have_move && g_follow.move_frame + 1 == g_read.frame && g_follow.move_steps >= 0 && g_draw.steps >= 0 &&
                glm::length(g_follow.move) + glm::length(move) > kMoving) {
                // How well the frame before's move would have foretold this one: as it was, or scaled by the physics steps run.
                const glm::vec3 by_frame = g_follow.move;
                const glm::vec3 by_step  = g_follow.move_steps > 0 && g_draw.steps > 0
                                               ? g_follow.move * (static_cast<float>(std::min(g_draw.steps, kMaxStepsAhead)) /
                                                                     static_cast<float>(std::min(g_follow.move_steps, kMaxStepsAhead)))
                                               : by_frame;
                const float e_frame = glm::length(by_frame - move);
                const float e_step  = glm::length(by_step - move);
                if (g_follow.err_samples == 0) {
                    g_follow.err_frame = e_frame;
                    g_follow.err_step  = e_step;
                } else {
                    g_follow.err_frame += (e_frame - g_follow.err_frame) * 0.05f;
                    g_follow.err_step += (e_step - g_follow.err_step) * 0.05f;
                }
                ++g_follow.err_samples;
                if (g_follow.err_samples >= kVotesNeeded) {
                    const bool per_step = g_follow.per_step ? g_follow.err_step < 1.1f * g_follow.err_frame : g_follow.err_step < 0.8f * g_follow.err_frame;
                    if (per_step != g_follow.per_step) {
                        g_follow.per_step = per_step;
                        static int told   = 0;
                        if (++told <= 10) {
                            spdlog::info("[Grab] your movement is foretold {} (each frame's move from the one before is off by {:.1f} cm taken per physics "
                                         "step, {:.1f} cm taken as it was)",
                                per_step ? "per physics step" : "per frame", g_follow.err_step * 100.0f, g_follow.err_frame * 100.0f);
                        }
                    }
                }
            }
            // The frame's motion of where the hand holds it (for the lead): the play area's part (carrying where it held it a frame
            // ago) and the hand's own; none across a snap turn, a jump, a change of grab, hand or held point.
            g_follow.have_drawn_lead = false;
            g_follow.have_parts      = false;
            if (!jump && g_follow.have_drawn && g_follow.drawn_frame + 1 == g_read.frame && g_follow.drawn_key == g_read.key) {
                const glm::mat3 area_turn = g_draw.room * glm::transpose(g_follow.drawn_room);
                const glm::vec3 carried   = g_draw.anchor + area_turn * (g_follow.drawn - g_follow.drawn_anchor);
                if (TurnCos(area_turn) >= kFollowJumpCos && Finite(carried) && glm::length(carried - g_follow.drawn) <= kFollowJump &&
                    glm::length(drawn - carried) <= kFollowMaxLead) {
                    g_follow.drawn_lead      = drawn - g_follow.drawn;
                    g_follow.have_drawn_lead = true;
                    g_follow.area_move       = carried - g_follow.drawn;
                    g_follow.hand_move       = drawn - carried;
                    g_follow.parts_frame     = g_read.frame;
                    g_follow.have_parts      = true;
                }
            }
            g_follow.have_move   = !jump;
            g_follow.move        = move;
            g_follow.move_steps  = g_draw.steps;
            g_follow.move_frame  = g_read.frame;
            g_follow.have_drawn   = true;
            g_follow.drawn        = drawn;
            g_follow.drawn_frame  = g_read.frame;
            g_follow.drawn_anchor = g_draw.anchor;
            g_follow.drawn_room   = g_draw.room;
            g_follow.drawn_key    = g_read.key;
            if (g_track.frame == g_read.frame && !g_track.pulling) {
                g_track.move     = move;
                g_track.per_step = false;
                g_track.lead     = FrameLead();
            }
            return true;
        }

        bool CallPlayer(uintptr_t function, uintptr_t player)
        {
            __try {
                reinterpret_cast<PlayerFn>(function)(reinterpret_cast<void*>(player));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallVelocity(uintptr_t function, uintptr_t bodies, uint32_t body, const float* linear, const float* angular)
        {
            __try {
                reinterpret_cast<VelocityFn>(function)(reinterpret_cast<void*>(bodies), body, linear, angular, 0);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ---- A held object and your collision ----

        // A body found from the 3D that owns it: its collision object, its world and its record (every read checked).
        struct BodyRef {
            uintptr_t collision{ 0 };
            uintptr_t world{ 0 };  // hknpBSWorld
            uintptr_t record{ 0 };
            uint32_t  filter{ 0 }; // its collision filter info now
            uint32_t  bodies{ 0 }; // how many bodies its physics system has (one for most objects; the parts of one share a group)
        };

        // A body of a physics world (hknpBSWorld): its record, the collision object it belongs to (the record's +0x98, checked:
        // a bhkNPCollisionObject whose physics system has this body at its index, in this world) and its filter info now.
        bool ResolveBodyIn(uintptr_t world, uint32_t body, BodyRef& out)
        {
            uintptr_t bodies = 0, collision = 0, table = 0, function = 0, system = 0, instance = 0, ids = 0, world18 = 0;
            uint32_t  body_count = 0, record_id = 0, flags = 0, filter = 0, count = 0, id = 0;
            int32_t   index = -1;
            if (world == 0 || !SafeRead(&bodies, world + kWorldBodyArray, sizeof(bodies)) || !SafeRead(&body_count, world + kWorldBodyCount, sizeof(body_count)) ||
                (body & kBodyIndexMask) >= body_count) {
                return false;
            }
            const uintptr_t record = bodies + static_cast<uintptr_t>(body & kBodyIndexMask) * kBodySize;
            if (!SafeRead(&record_id, record + kBodyId, sizeof(record_id)) || record_id != body || !SafeRead(&flags, record + kBodyFlags, sizeof(flags)) ||
                (flags & 3u) == 0 || !SafeRead(&filter, record + kBodyFilter, sizeof(filter)) || !SafeRead(&collision, record + kBodyOwner, sizeof(collision)) ||
                collision == 0 || !SafeRead(&table, collision, sizeof(table)) ||
                !SafeRead(&function, table + kSetGroupSlot * sizeof(uintptr_t), sizeof(function)) || function != At(kSetGroupRva) ||
                !SafeRead(&system, collision + kCollisionSystem, sizeof(system)) || !SafeRead(&index, collision + kCollisionIndex, sizeof(index)) ||
                !SafeRead(&instance, system + kSystemInstance, sizeof(instance)) || !SafeRead(&count, instance + kInstanceCount, sizeof(count)) ||
                !SafeRead(&ids, instance + kInstanceIds, sizeof(ids)) || index < 0 || static_cast<uint32_t>(index) >= count ||
                !SafeRead(&id, ids + static_cast<uintptr_t>(index) * sizeof(uint32_t), sizeof(id)) || id != body ||
                !SafeRead(&world18, instance + kInstanceWorld, sizeof(world18)) || world18 != world + kWorldFromInstance) {
                return false;
            }
            out = BodyRef{ collision, world, record, filter, count };
            return true;
        }

        // The same from the 3D that owns the body: its root's own collision object, which must be the body's.
        bool ResolveBody(uintptr_t root, uint32_t body, BodyRef& out)
        {
            uintptr_t collision = 0, system = 0, instance = 0, world18 = 0;
            if (root == 0 || !SafeRead(&collision, root + kNodeCollision, sizeof(collision)) || collision == 0 ||
                !SafeRead(&system, collision + kCollisionSystem, sizeof(system)) || !SafeRead(&instance, system + kSystemInstance, sizeof(instance)) ||
                !SafeRead(&world18, instance + kInstanceWorld, sizeof(world18)) || world18 < kWorldFromInstance) {
                return false;
            }
            BodyRef b;
            if (!ResolveBodyIn(world18 - kWorldFromInstance, body, b) || b.collision != collision) {
                return false;
            }
            out = b;
            return true;
        }

        // The held 3D's root's own collision object, when it moves the held body and the root and is its physics system's only
        // body (the scene writer is called with it for that node; parts with bodies of their own would stay where physics has
        // them); 0 otherwise.
        uintptr_t HeldCollision(uintptr_t root, uint32_t body)
        {
            BodyRef   b;
            uintptr_t owner = 0;
            return ResolveBody(root, body, b) && b.bodies == 1 && SafeRead(&owner, b.collision + kCollisionOwner, sizeof(owner)) && owner == root ? b.collision
                                                                                                                                                 : 0;
        }

        // Your character proxy, its collision group and its world (hknpBSWorld, +0x160 in the proxy).
        constexpr uintptr_t kProxyWorld = 0x160;
        struct PlayerCollision {
            uintptr_t proxy{ 0 };
            uintptr_t world{ 0 };
            uint32_t  group{ 0 };
        };

        bool ReadPlayerCollision(PlayerCollision& out)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  process = 0, middle = 0, controller = 0, proxy = 0, world = 0;
            uint32_t   info = 0;
            if (player == 0 || !SafeRead(&process, player + kActorProcess, sizeof(process)) || !SafeRead(&middle, process + kProcessMiddle, sizeof(middle)) ||
                !SafeRead(&controller, middle + kMiddleController, sizeof(controller)) || !SafeRead(&proxy, controller + kControllerProxy, sizeof(proxy)) ||
                !SafeRead(&info, proxy + kProxyFilter, sizeof(info)) || (info & 0x7fu) != kLayerCharController || (info >> 16) == 0 ||
                !SafeRead(&world, proxy + kProxyWorld, sizeof(world)) || world == 0) {
                return false;
            }
            out = PlayerCollision{ proxy, world, info >> 16 };
            return true;
        }

        // ---- Gravity gloves: the object nearest the pointing line ----
        // The game's pick takes the first thing its sphere touches, so the floor (or the shelf) under a small object gets in
        // first. HIGGS (Skyrim VR) sweeps a sphere, keeps every hit and takes the object nearest the pointing line. Here the
        // world's movable bodies are looked through (a few thousand records a frame) for the one nearest the hand's pointing
        // line, and every other far cast is then aimed from the hand straight at it with a thin sphere (PointInteract's
        // steer): the game itself still decides whether it is something you can take (its prompt shows it), and a wall in the
        // way still stops it. An object it does not find a few times running is skipped for a moment; one the game would not
        // let you pull, for a minute. When nothing movable is near the line, the pointing casts work as before.
        // A body's record (hknpBody, Starfield 1.16.236, checked against held objects): its place at +0x30 (world units), its
        // box's corners at +0x60 (x, y, z) and +0x6c (z), +0x70 (x, y) (the place inside it, the box about the object's
        // size), its flags at +0x54: static 1, dynamic 2, keyframed 4 (moved by animation), active 8.
        constexpr uintptr_t kBodyPlace       = 0x30;
        constexpr uintptr_t kBodyBox         = 0x60;
        constexpr size_t    kBodyLookBytes   = kBodyFilter + 4 - kBodyPlace; // +0x30 to +0x90
        constexpr uint32_t  kBodyKindMask    = 7u;
        constexpr uint32_t  kBodyMovable     = 2u;      // dynamic, neither static nor keyframed
        constexpr float     kSteerRadius     = 0.02f;   // the thin cast aimed at the object (m)
        constexpr float     kSteerCone       = 10.0f;   // degrees off the pointing line an object may be...
        constexpr float     kSteerNearLine   = 0.25f;   // ...or this near the line (m), for objects close by
        constexpr float     kSteerPerMetre   = 0.3f;    // a metre further away counts as this many degrees further off the line
        constexpr float     kSteerOtherLayer = 4.0f;    // degrees added for an object not on an item's layer
        constexpr float     kSteerKeep       = 3.0f;    // the one aimed at stays unless another is this many degrees nearer
        constexpr float     kSteerOnLine     = 2.0f;    // the line passes this near its box (degrees): pointed straight at
        constexpr float     kSteerMinPast    = 0.15f;   // it must be this far past where the far cast starts (m)
        constexpr float     kSteerMaxSize    = 4.0f;    // bigger (its box, m) is not something to pull
        constexpr float     kSteerHitNear    = 0.15f;   // a hit this near its box (m, plus the cast's radius) is the object
        constexpr int       kSteerMisses     = 4;       // aimed casts that do not find it before it is skipped...
        constexpr int64_t   kSteerSkipUs     = 3'000'000;  // ...for this long
        constexpr int64_t   kSteerRefusedUs  = 60'000'000; // one the game would not take or let you pull
        constexpr uint32_t  kSteerScanChunk  = 12288;   // records looked through per frame while aiming
        constexpr int64_t   kSteerScanStale  = 1'500'000; // a list older than this is made again in full when an aim starts
        constexpr size_t    kSteerMaxBodies  = 4096;    // movable bodies kept
        constexpr int       kSteerSkipKept   = 64;
        constexpr int       kLockMissesMax   = 3;       // locked on by aiming at it: casts in a row that miss it before it is lost...
        constexpr int       kLockOthersMax   = 8;       // ...or that were not aimed at it (it never came back to it)
        constexpr int       kFreshFrames     = 5;       // a find stays usable this many frames (the next cast of its kind replaces it)

        // Collision layers (as Fallout 4's; clutter 4 and the character controller 30 checked in Starfield's logs).
        bool ItemLayer(uint32_t layer) { return layer == 4 || layer == 5 || layer == 29; } // clutter, weapons, large clutter
        bool NeverLayer(uint32_t layer)
        {
            switch (layer) {
            case 6:  // projectiles
            case 7:  // spells
            case 8:  // bodies (biped)
            case 12: // triggers
            case 15: // not collidable
            case 19: // small debris
            case 20: // large debris
            case 25: // shell casings
            case 30: // character controllers
            case 32: // dead bodies
            case 33: // bodies without a controller
                return true;
            default:
                return false;
            }
        }

        // The movable bodies of your world (ids), from whole passes through its records, a chunk at a time.
        struct SteerScan {
            uintptr_t             world{ 0 };
            uint32_t              next{ 0 };          // the next record to look at
            int64_t               done_us{ 0 };       // when the latest whole pass ended (0: none yet)
            uint32_t              records{ 0 };       // how many records the latest pass looked through
            bool                  faulted{ false };   // a record could not be read in the latest pass
            std::vector<uint32_t> bodies;             // movable bodies of the latest whole pass
        };
        SteerScan g_scan;
        uint32_t  g_scan_building[kSteerMaxBodies]{}; // ...and of the pass going on
        uint32_t  g_scan_built{ 0 };
        int64_t   g_scan_told_us{ 0 };

        // Reads of many physics records a frame: straight from memory, an exception (a record that is not there any more)
        // caught here (no system call per read, as SafeRead makes; they are thousands a frame).
        bool GuardedCopy(void* dst, uintptr_t src, size_t n)
        {
            if (src == 0) {
                return false;
            }
            __try {
                std::memcpy(dst, reinterpret_cast<const void*>(src), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Records [from, to) of a world's body array: the movable ones' ids added to out (up to cap). False: a read failed.
        bool ScanRecords(uintptr_t bodies, uint32_t from, uint32_t to, uint32_t* out, uint32_t cap, uint32_t& written)
        {
            __try {
                for (uint32_t i = from; i < to; ++i) {
                    const auto*    word  = reinterpret_cast<const volatile uint32_t*>(bodies + static_cast<uintptr_t>(i) * kBodySize + kBodyId);
                    const uint32_t id    = word[0];
                    const uint32_t flags = word[1];
                    if ((id & kBodyIndexMask) == i && (flags & kBodyKindMask) == kBodyMovable && written < cap) {
                        out[written++] = id;
                    }
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        struct SteerSkip {
            uint32_t body{ 0 };
            int64_t  until_us{ 0 };
        };
        SteerSkip g_steer_skip[kSteerSkipKept]{};
        int       g_steer_skip_next{ 0 };

        // The object the far casts are aimed at (not locked on yet), and how the latest aimed casts went.
        struct SteerTarget {
            uint32_t  body{ 0 };
            glm::vec3 at{ 0.0f };                  // its box's middle (world)
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };      // its box
            float     degrees{ 0.0f };             // off the pointing line
            float     distance{ 0.0f };            // from the hand (m)
            uint32_t  layer{ 0 };
            int       misses{ 0 };                 // aimed casts in a row that did not find it
        };
        SteerTarget g_steer{};
        CastSeen    g_seen_steer{};                // what the latest aimed cast found (the object itself)
        uint32_t    g_seen_steer_body{ 0 };        // ...and which body it was aimed at
        bool        g_steer_next{ false };         // the far cast asked for is the aimed one
        bool        g_lock_steered{ false };       // locked on through the aimed cast: it keeps following the object...
        uint32_t    g_lock_body{ 0 };              // ...this body
        int         g_lock_misses{ 0 };
        int         g_lock_others{ 0 };            // ...and casts since that were not aimed at it
        uint32_t    g_pull_body{ 0 };              // the body the latest pull went for (0: not an aimed one)
        int         g_aim_steer_looks{ 0 };        // this aim: aimed casts, and how many found the object
        int         g_aim_steer_finds{ 0 };
        // This aim, for the log: how near the nearest movable object came to each of the gloves' lines (degrees; which one
        // matches how you point).
        float       g_aim_near[tracking::kGloveLines]{};
        int         g_lock_lines_told{ 0 };
        glm::vec3   g_lock_point{ 0.0f };          // locked on (not by aiming at a body): where the cast found it
        // What to show while aiming (AimMark).
        GloveMark   g_mark{};
        bool        g_have_mark{ false };
        constexpr float kMarkFoundRadius = 0.12f;  // ringing a find that is not a body's box (m)
        constexpr float kMarkPointer     = 2.0f;   // the pointing dot this far along the line (m)
        int         g_steer_found_told{ 0 }, g_steer_skip_told{ 0 };

        bool SteerSkipped(uint32_t body, int64_t now)
        {
            for (const auto& s : g_steer_skip) {
                if (s.body == body && now < s.until_us) {
                    return true;
                }
            }
            return false;
        }

        void SkipSteer(uint32_t body, int64_t for_us)
        {
            if (body == 0) {
                return;
            }
            const int64_t now = NowUs();
            for (auto& s : g_steer_skip) {
                if (s.body == body) {
                    s.until_us = std::max(s.until_us, now + for_us);
                    return;
                }
            }
            g_steer_skip[g_steer_skip_next] = SteerSkip{ body, now + for_us };
            g_steer_skip_next               = (g_steer_skip_next + 1) % kSteerSkipKept;
        }

        // Looks through up to `budget` more records of your world for movable bodies; a whole pass replaces the list.
        void ScanMovable(uint32_t budget)
        {
            PlayerCollision you;
            uintptr_t       bodies = 0;
            uint32_t        count  = 0;
            if (!ReadPlayerCollision(you) || !SafeRead(&bodies, you.world + kWorldBodyArray, sizeof(bodies)) ||
                !SafeRead(&count, you.world + kWorldBodyCount, sizeof(count)) || bodies == 0 || count == 0 || count > kBodyIndexMask) {
                return;
            }
            if (you.world != g_scan.world) {
                g_scan       = SteerScan{};
                g_scan.world = you.world;
                g_scan_built = 0;
            }
            if (g_scan.next >= count) {
                g_scan.next  = 0;
                g_scan_built = 0;
            }
            if (g_scan.next == 0) {
                g_scan.faulted = false;
            }
            const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(count, static_cast<uint64_t>(g_scan.next) + budget));
            // In blocks, so one record that cannot be read costs only its block.
            constexpr uint32_t kBlock = 1024;
            for (uint32_t from = g_scan.next; from < end; from += kBlock) {
                if (!ScanRecords(bodies, from, std::min(end, from + kBlock), g_scan_building, static_cast<uint32_t>(kSteerMaxBodies), g_scan_built)) {
                    g_scan.faulted = true;
                }
            }
            g_scan.next = end;
            if (end >= count) {
                g_scan.bodies.assign(g_scan_building, g_scan_building + g_scan_built);
                g_scan_built   = 0;
                g_scan.next    = 0;
                g_scan.done_us = NowUs();
                g_scan.records = count;
            }
        }

        // A movable body now (its record read once, checked): its box's middle, its box and its filter info. False: gone, no
        // longer movable, or too big to pull.
        struct BodyLook {
            glm::vec3 at{ 0.0f };
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };
            uint32_t  filter{ 0 };
        };

        // Your world's body array now (the one the list was made from).
        bool BodyArray(uintptr_t& bodies, uint32_t& count)
        {
            return g_scan.world != 0 && SafeRead(&bodies, g_scan.world + kWorldBodyArray, sizeof(bodies)) &&
                   SafeRead(&count, g_scan.world + kWorldBodyCount, sizeof(count)) && bodies != 0 && count != 0 && count <= kBodyIndexMask;
        }

        bool LookAtBodyIn(uintptr_t bodies, uint32_t count, uint32_t body, float scale, BodyLook& out)
        {
            uint8_t raw[kBodyLookBytes]{};
            if ((body & kBodyIndexMask) >= count ||
                !GuardedCopy(raw, bodies + static_cast<uintptr_t>(body & kBodyIndexMask) * kBodySize + kBodyPlace, sizeof(raw))) {
                return false;
            }
            uint32_t  id = 0, flags = 0;
            glm::vec3 place{}, a{};
            float     bz = 0.0f, bx = 0.0f, by = 0.0f;
            std::memcpy(&id, raw + (kBodyId - kBodyPlace), sizeof(id));
            std::memcpy(&flags, raw + (kBodyFlags - kBodyPlace), sizeof(flags));
            std::memcpy(&out.filter, raw + (kBodyFilter - kBodyPlace), sizeof(out.filter));
            std::memcpy(&place, raw, sizeof(place));
            std::memcpy(&a, raw + (kBodyBox - kBodyPlace), sizeof(a));
            std::memcpy(&bz, raw + (kBodyBox + 0xc - kBodyPlace), sizeof(bz));
            std::memcpy(&bx, raw + (kBodyBox + 0x10 - kBodyPlace), sizeof(bx));
            std::memcpy(&by, raw + (kBodyBox + 0x14 - kBodyPlace), sizeof(by));
            if (id != body || (flags & kBodyKindMask) != kBodyMovable || !Finite(place)) {
                return false;
            }
            const glm::vec3 b{ bx, by, bz };
            const glm::vec3 lo = glm::min(a, b), hi = glm::max(a, b);
            const glm::vec3 size = hi - lo;
            if (Finite(lo) && Finite(hi) && glm::length((lo + hi) * 0.5f - place) < 2.0f * scale) {
                if (std::max({ size.x, size.y, size.z }) > kSteerMaxSize * scale) {
                    return false;
                }
                out.lo = lo;
                out.hi = hi;
                out.at = (lo + hi) * 0.5f;
            } else {
                // No box that fits (not what was measured): its place, with room around it.
                out.at = place;
                out.lo = place - glm::vec3{ 0.3f * scale };
                out.hi = place + glm::vec3{ 0.3f * scale };
            }
            return true;
        }

        bool LookAtBody(uint32_t body, float scale, BodyLook& out)
        {
            uintptr_t bodies = 0;
            uint32_t  count  = 0;
            return BodyArray(bodies, count) && LookAtBodyIn(bodies, count, body, scale, out);
        }

        float DistanceToBox(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) { return glm::length(p - glm::clamp(p, lo, hi)); }

        // How many degrees a box (or a point: lo = hi) is off one of the gloves' lines, seen from the line's start (-1: no line).
        float LineDegrees(int line, int hand, const glm::vec3& lo, const glm::vec3& hi)
        {
            glm::vec3 origin{}, dir{};
            if (!tracking::GloveRayWorld(line, hand == 0, origin, dir)) {
                return -1.0f;
            }
            const glm::vec3 middle = 0.5f * (lo + hi);
            const float     along  = glm::dot(middle - origin, dir);
            if (!(along > 0.01f)) {
                return 180.0f;
            }
            return glm::degrees(std::atan2(DistanceToBox(origin + dir * along, lo, hi), along));
        }

        const char* LineName(int line) { return line == 1 ? "palm" : (line == 2 ? "sight" : "hand"); }

        // The marker's ring around a box (its corners about on it).
        float BoxRingRadius(const glm::vec3& lo, const glm::vec3& hi, float scale) { return std::clamp(0.45f * glm::length(hi - lo), 0.06f * scale, 0.6f * scale); }

        std::string NearText(float degrees) { return degrees < 1000.0f ? fmt::format("{:.0f}", degrees) : std::string{ "-" }; }

        // The movable body nearest the hand's pointing line (by how many degrees off it, a little for distance, as HIGGS takes
        // the hit nearest the line), in front of where the far cast starts and within reach; not your own or anything you hold
        // (your collision group). `keep` stays the choice unless another is clearly nearer the line.
        bool ChooseSteer(const glm::vec3& origin, const glm::vec3& dir, float range, float scale, uint32_t keep, SteerTarget& out,
            float* nearest_degrees = nullptr)
        {
            PlayerCollision you;
            uintptr_t       bodies = 0;
            uint32_t        count  = 0;
            if (g_scan.bodies.empty() || !ReadPlayerCollision(you) || you.world != g_scan.world || !BodyArray(bodies, count)) {
                return false;
            }
            const float   hand_reach = std::clamp(GameFlow::gStore.internalSettings.handReach, 0.05f, 3.0f);
            const float   nearest    = (std::min(hand_reach, 1.0f) + kSteerMinPast) * scale;
            const float   furthest   = (range + 0.3f) * scale;
            const int64_t now        = NowUs();
            SteerTarget   best{}, kept{};
            float         best_score = std::numeric_limits<float>::max(), kept_score = best_score;
            for (const uint32_t body : g_scan.bodies) {
                BodyLook look;
                if (SteerSkipped(body, now) || !LookAtBodyIn(bodies, count, body, scale, look)) {
                    continue;
                }
                const uint32_t group = look.filter >> 16, layer = look.filter & 0x7fu;
                if ((group != 0 && group == you.group) || NeverLayer(layer)) {
                    continue;
                }
                const glm::vec3 v     = look.at - origin;
                const float     along = glm::dot(v, dir);
                if (!(along >= nearest && along <= furthest)) {
                    continue;
                }
                // How far the line passes from its box (0: through it), as HIGGS measures from where the sweep touched it, not from
                // its middle: a big object pointed straight at is on the line.
                const float off     = DistanceToBox(origin + dir * along, look.lo, look.hi);
                const float degrees = glm::degrees(std::atan2(off, along));
                if (nearest_degrees && std::isfinite(degrees)) {
                    *nearest_degrees = std::min(*nearest_degrees, degrees);
                }
                if (!std::isfinite(degrees) || (degrees > kSteerCone && off > kSteerNearLine * scale)) {
                    continue;
                }
                const float score = degrees + kSteerPerMetre * along / scale + (ItemLayer(layer) ? 0.0f : kSteerOtherLayer);
                const SteerTarget t{ body, look.at, look.lo, look.hi, degrees, along / scale, layer, 0 };
                if (score < best_score) {
                    best_score = score;
                    best       = t;
                }
                if (body == keep) {
                    kept_score = score;
                    kept       = t;
                }
            }
            if (best.body == 0) {
                return false;
            }
            out = kept.body != 0 && kept_score <= best_score + kSteerKeep ? kept : best;
            return true;
        }

        // The game's own function (no exception handler around it: one inside, after it took the world's lock, would leave
        // the world locked and the game frozen; a crash says more). True when it changed the group.
        using SetGroupFn = bool (*)(void*, uint32_t);
        bool CallSetGroup(uintptr_t collision, uint32_t group) { return reinterpret_cast<SetGroupFn>(At(kSetGroupRva))(reinterpret_cast<void*>(collision), group); }

        // A held object put in your collision group, to be put back once the hold has ended and it is clear of you (the grab
        // update asks for it; the frame's start, outside the grab update, does it and puts it back) (g_group_mutex).
        struct GroupChange {
            bool      pending{ false };   // asked for, not done yet
            bool      active{ false };    // done, to be put back
            uintptr_t spring{ 0 };
            uintptr_t ref{ 0 };
            uintptr_t root{ 0 };
            uintptr_t collision{ 0 };
            uintptr_t world{ 0 };         // the world it and you were in
            uint32_t  body{ 0 };
            uint32_t  original{ 0 };      // its filter info before
            uint32_t  group{ 0 };         // the group it was given (yours)
            bool      releasing{ false }; // the hold has ended: put back once clear of you
            int64_t   release_us{ 0 };
            int       clear_frames{ 0 };
        };
        std::mutex        g_group_mutex;
        GroupChange       g_group{};       // the object held now
        GroupChange       g_letgo{};       // the one let go last, until it is clear of you
        GroupChange       g_group_asked{};
        // (fix170) An object held on its own (adopted): its group change moves with it, all moves made here at the frame's start (UpdateGroup)
        // so the player's update thread never writes these slots under UpdateGroup's copy (the review): AdoptHeld asks for its spring's
        // change to be kept aside (asked before the game's grab is reset, so the hold is never taken for ended), EndAdopted for it to go
        // to the let-go slot.
        uintptr_t         g_group_adopt_ask{ 0 };
        bool              g_group_adopt_end{ false };
        GroupChange       g_adopted_group{};
        std::atomic<bool> g_group_ready{ false };
        std::atomic<bool> g_layer_ready{ false }; // (fix155: SetCollisionLayer checked)
        std::atomic<int>  g_group_changes{ 0 };
        constexpr float   kYouRadius     = 0.4f;   // your body as a capsule this wide (m)...
        constexpr float   kYouHeight     = 1.9f;   // ...from your feet (your reference's place) up this far
        constexpr uintptr_t kRefLocation = 0x8c;   // TESObjectREFR::data.location (OBJ_REFR at +0x80; its base at +0x98, as above)
        constexpr int     kClearFrames   = 3;      // a let-go object clear of you this many frames running gets its group back...
        constexpr int64_t kClearTimeout  = 3'000'000; // ...or after this long anyway (us)

        // A new grab (the grab update, g_mutex held): put the object in your group at the next frame's start.
        void AskGroup(uintptr_t spring, uintptr_t ref, uintptr_t root)
        {
            uint32_t body = 0;
            if (!g_group_ready.load() || root == 0 || !SafeRead(&body, spring + kSpringBody, sizeof(body))) {
                return;
            }
            std::scoped_lock _{ g_group_mutex };
            g_group_asked = GroupChange{ true, false, spring, ref, root, 0, 0, body, 0, 0 };
        }

        // Is a let-go object clear of you (its bounding sphere outside your body's capsule, with a margin)? Your body is where
        // your character is (your reference's place, which your character controller moves), not your head.
        bool ClearOfYou(uintptr_t root)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            float      at[3]{}, sphere[4]{};
            if (player == 0 || !SafeRead(at, player + kRefLocation, sizeof(at)) || !SafeRead(sphere, root + kNodeBound, sizeof(sphere))) {
                return true;
            }
            const glm::vec3 feet{ at[0], at[1], at[2] };
            const glm::vec3 centre{ sphere[0], sphere[1], sphere[2] };
            if (!Finite(feet) || !Finite(centre) || !std::isfinite(sphere[3])) {
                return true;
            }
            const float     z = std::clamp(centre.z, feet.z, feet.z + kYouHeight);
            const glm::vec3 nearest{ feet.x, feet.y, z };
            return glm::length(centre - nearest) > sphere[3] + kYouRadius + 0.05f;
        }

        // Puts a held object back in its own group: only while you are still in the world it was in, and it is still there as
        // it was (its reference's 3D, that 3D's collision object with the same body, the body in use) and in the group it was
        // given.
        void RestoreGroup(const GroupChange& g, const char* why)
        {
            const int       n = g_group_changes.load();
            PlayerCollision you;
            if (!ReadPlayerCollision(you) || you.world != g.world) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] {}: you are in another physics world now (loaded, travelled), so the object's collision group is left", why);
                }
                return;
            }
            uintptr_t  loaded = 0, root = 0;
            BodyRef    b;
            const bool there = g.ref != 0 && SafeRead(&loaded, g.ref + kRefLoaded, sizeof(loaded)) && loaded != 0 &&
                               SafeRead(&root, loaded + kLoaded3D, sizeof(root)) && root == g.root && ResolveBodyIn(g.world, g.body, b) &&
                               b.collision == g.collision;
            if (!there) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] {}: the object is no longer in the world as it was (picked up, unloaded), so its collision group is left", why);
                }
                return;
            }
            if ((b.filter >> 16) != g.group) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] {}: the object's collision group was changed by the game meanwhile ({}), so it is left", why, b.filter >> 16);
                }
                return;
            }
            const bool ok = CallSetGroup(g.collision, g.original >> 16);
            if (n <= 20 || n % 10 == 0) {
                spdlog::info("[Grab] {}: the object collides with you again (collision group {} -> {}{})", why, g.group, g.original >> 16,
                    ok ? "" : "; the game did not change it");
            }
        }

        // At each frame's start (main thread): a let-go object gets its group back once clear of you; a new hold's object goes
        // in yours.
        void UpdateGroup(uintptr_t spring_now)
        {
            GroupChange held, letgo, asked, adopted_end;
            {
                std::scoped_lock _{ g_group_mutex };
                // (fix170) The held object now held on its own: its change kept aside (not ended); one let go: to the let-go slot.
                if (g_group_adopt_ask != 0) {
                    if (g_group.active && g_group.spring == g_group_adopt_ask) {
                        g_adopted_group = g_group;
                        g_group         = GroupChange{};
                    }
                    g_group_adopt_ask = 0;
                }
                if (g_group_adopt_end) {
                    adopted_end       = g_adopted_group;
                    g_adopted_group   = GroupChange{};
                    g_group_adopt_end = false;
                }
                held                  = g_group;
                letgo                 = g_letgo;
                asked                 = g_group_asked;
                g_group_asked.pending = false;
            }
            const int64_t now = NowUs();
            // The hold ended (no spring, another one, or the same spring now holding another body): it waits in the let-go slot
            // until clear of you; an earlier one still there gets its group back now.
            uint32_t   body_now = 0;
            const bool same     = held.active && held.spring == spring_now && spring_now != 0 &&
                              SafeRead(&body_now, spring_now + kSpringBody, sizeof(body_now)) && body_now == held.body;
            if (held.active && !same) {
                if (letgo.active) {
                    RestoreGroup(letgo, "another object let go");
                }
                letgo              = held;
                letgo.releasing    = true;
                letgo.release_us   = now;
                letgo.clear_frames = 0;
                held               = GroupChange{};
            }
            // (fix170) An object held on its own let go: it waits in the let-go slot too (before the next test: taken by your other
            // hand's grab just then, it stays in yours).
            if (adopted_end.active) {
                if (letgo.active) {
                    RestoreGroup(letgo, "another object let go");
                }
                letgo              = adopted_end;
                letgo.releasing    = true;
                letgo.release_us   = now;
                letgo.clear_frames = 0;
            }
            // Taken again before it got its group back: it stays in yours.
            if (asked.pending && asked.spring == spring_now && letgo.active && asked.root == letgo.root && asked.body == letgo.body) {
                held           = letgo;
                held.spring    = spring_now;
                held.releasing = false;
                letgo          = GroupChange{};
                asked.pending  = false;
            }
            if (letgo.active) {
                letgo.clear_frames = ClearOfYou(letgo.root) ? letgo.clear_frames + 1 : 0;
                if (letgo.clear_frames >= kClearFrames || now - letgo.release_us > kClearTimeout) {
                    RestoreGroup(letgo, letgo.clear_frames >= kClearFrames ? "let go and clear of you" : "let go (still close to you after 3 s)");
                    letgo = GroupChange{};
                }
            }
            {
                std::scoped_lock _{ g_group_mutex };
                g_group = held;
                g_letgo = letgo;
            }
            if (!asked.pending || asked.spring != spring_now || !GameFlow::gStore.internalSettings.grabIgnoresYou) {
                return;
            }
            PlayerCollision you;
            BodyRef         b;
            const int       n = g_group_changes.fetch_add(1) + 1;
            if (!ReadPlayerCollision(you)) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::warn("[Grab] your collision group could not be read (your character controller is not laid out as in 1.16.236); the held object "
                                 "can bump into you");
                }
                return;
            }
            if (!ResolveBodyIn(you.world, asked.body, b)) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] the held object's body {:x} was not found in your physics world as expected (its record and its collision object); it "
                                 "can bump into you",
                        asked.body);
                }
                return;
            }
            if (b.bodies != 1) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] the held object has {} bodies sharing a collision group (its parts do not bump into each other that way), so it "
                                 "keeps its group and can bump into you",
                        b.bodies);
                }
                return;
            }
            if ((b.filter >> 16) == you.group) {
                return; // already in yours
            }
            if (!CallSetGroup(b.collision, you.group)) {
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] the game did not change the held object's collision group; it can bump into you");
                }
                return;
            }
            asked.collision = b.collision;
            asked.world     = b.world;
            asked.original  = b.filter;
            asked.group     = you.group;
            asked.pending   = false;
            asked.active    = true;
            {
                std::scoped_lock _{ g_group_mutex };
                g_group = asked;
            }
            if (n <= 20 || n % 10 == 0) {
                spdlog::info("[Grab] the held object no longer collides with you: collision group {} -> yours ({}), layer {} kept", b.filter >> 16, you.group,
                    b.filter & 0x7fu);
            }
        }

        // ---- (fix161) The held object put in the game's moving objects ----
        // Everything grabbed with the gravity gloves stopped a few inches off the ground (pulled ones: the body flew to
        // the hand, the 3D stayed). Each frame the game moves the 3D only of the objects under each loaded cell's moving-objects node
        // (its 3D root's child 4: the per-frame job 0x1417CE9A0 -> 0x1417C26A0, each cell's 0x1417C2E10). A body that wakes up joins it
        // through the game's wake-up handler (hknpWorld's bodies-activated signal -> 0x140D4E6E0(ref): its 3D moved under that node,
        // queued at 0x140B1C5D0, and bit 28 of the root's flags set at 0x140D4E8E5); it leaves it again as the body comes to rest
        // (0x140D4E920). The handler refuses while the reference's loaded flags (LOADED_REF_DATA +0x20) have 0x20, 0x2000 or 0x10000
        // (0x140D4E7B4, 7C9, 7DE): 0x10000 is set while an effect sequence plays on it (0x1403E6444), cleared as the last one ends
        // (0x1403E6A56). The gloves' glow is one, still on the object (it ends at its next update) as the pull's grab wakes the body:
        // the object stayed under its cell's still objects and was never moved (helpers' research; fix160's log: every update switch
        // on, the root's world update run once in 20 frames, bit 28 clear). Objects already moving had joined before the glow came.
        // Now, while the gloves hold or pull an object, each frame at the frame's start (main thread): not joined (its 3D root not under
        // its cell's moving-objects node) and nothing holding it back any more, the game's handler is called for it (as the signal does, from the physics step; it takes
        // the reference's lock and queues the move itself). Logged at the start and 20 frames in. (fix160's turning on of the update
        // switches is gone: they were never off.)
        constexpr uintptr_t kNodeFlags      = 0x118; // NiAVObject: 64-bit flags
        constexpr uintptr_t kNodeStamp      = 0x128; // NiAVObject: the frame its world update last ran (u32; the game's frame count)
        constexpr uintptr_t kNodeParentAt   = 0x38;
        constexpr uint64_t  kNodeMoving     = 0x10000000; // (bit 28: under its cell's moving objects, set by the handler)
        constexpr uintptr_t kLoadedFlags    = 0x20;       // LOADED_REF_DATA: flags (u32)
        constexpr uint32_t  kLoadedHeld     = 0x20 | 0x2000 | 0x10000; // (the handler refuses with any; 0x10000: an effect sequence plays)
        constexpr uint32_t  kLoadedEffect   = 0x10000;
        constexpr uintptr_t kJoinRva        = 0xD4E6E0;
        constexpr int       kJoinTries      = 3;  // calls at most a hold (the handler queues the move: it shows a frame or two later)
        constexpr int       kJoinGapFrames  = 10; // frames between tries
        // (Joined: the root's parent is its cell's moving-objects node, as the game's own rest handler tests (0x140D4EB50): the cell (ref
        // +0xB0) -> +0x120 -> +0x120 its 3D root, children at +0x138 (u16 count at +0x142), child 4 only (fix162: child 3, 0x140610F70's, is the
        // still objects' node: fix161 counted two holds there as joined, their world update not run each frame; a helper's research; the
        // handler itself puts a reference passing its keyword test 0x1404804C0 under child 3: such an object never counts as joined here,
        // and the handler called 3 times only sets its bit 28 again: harmless, the review). Bit 28
        // alone also marks objects updated each frame for other reasons (a running controller: 0x140B3A0D4); the review.)
        constexpr uintptr_t kRefCellAt      = 0xB0;
        constexpr uintptr_t kCellLoadedAt   = 0x120;
        constexpr uintptr_t kCellRootAt     = 0x120;
        constexpr uintptr_t kNodeChildrenAt = 0x138;
        constexpr uintptr_t kNodeChildCount = 0x142;
        constexpr uint8_t   kFormTypeActor  = 0x4B; // (never for an actor: the handler's actor paths were not studied; the review)
        // The handler's code checked at Install: its start, its three tests of the loaded flags (0x20, 0x2000, 0x10000) and where it sets
        // bit 28. (fix190: each run its own constants, as the port tool reads them; the same bytes as fix161's table.)
        constexpr uint8_t   kJoinStart[]        = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
                   0x48, 0x8B, 0x81, 0x98, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x48, 0x2E, 0x83, 0xE9, 0x29 };
        constexpr uintptr_t kJoinTest20Rva      = 0xD4E7B4;
        constexpr uint8_t   kJoinTest20[]       = { 0xBA, 0x20, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8, 0xFF, 0x4F, 0x60, 0xFF, 0x84, 0xC0, 0x0F, 0x85, 0x37,
                   0x01, 0x00, 0x00 };
        constexpr uintptr_t kJoinTest2000Rva    = 0xD4E7C9;
        constexpr uint8_t   kJoinTest2000[]     = { 0xBA, 0x00, 0x20, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8, 0xEA, 0x4F, 0x60, 0xFF, 0x84, 0xC0, 0x0F, 0x85, 0x22,
                   0x01, 0x00, 0x00 };
        constexpr uintptr_t kJoinTestEffectRva  = 0xD4E7DE;
        constexpr uint8_t   kJoinTestEffect[]   = { 0xBA, 0x00, 0x00, 0x01, 0x00, 0x48, 0x8B, 0xCB, 0xE8, 0xD5, 0x4F, 0x60, 0xFF, 0x84, 0xC0, 0x0F, 0x85, 0x0D,
                   0x01, 0x00, 0x00 };
        constexpr uintptr_t kJoinMovingRva      = 0xD4E8E5;
        constexpr uint8_t   kJoinMoving[]       = { 0x48, 0x81, 0x88, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10 };
        std::atomic<bool> g_join_ready{ false };
        struct KeepDrawn {
            uintptr_t spring{ 0 };
            uintptr_t ref{ 0 };
            uintptr_t root{ 0 };
            uint32_t  body{ 0 };
            int       frames{ 0 };
            uint32_t  stamp0{ 0 };
            int       joins{ 0 };      // calls of the game's handler this hold
            int       join_frame{ 0 }; // the frame of the last
            int       held_back{ 0 };  // frames it could not join yet (the glow's effect still ending)
            int       clear_frame{ -1 }; // (fix162) the first frame nothing held it back (the game itself joins it then, most likely)
            bool      told_joined{ false };
        };
        std::mutex g_keep_mutex;
        KeepDrawn  g_keep_asked{}; // (set at the grab's start, physics side; taken by the frame's start)
        KeepDrawn  g_keep{};       // (main thread only)
        std::atomic<int> g_keep_tells{ 0 };

        struct Joined {
            uint64_t  flags{ 0 };
            uint32_t  stamp{ 0 };
            uintptr_t parent{ 0 };
            uint32_t  loaded_flags{ 0 };
            uintptr_t moving{ 0 };    // its cell's moving-objects node (child 4), 0 if not found
            uint8_t   form_type{ 0 }; // the reference's
        };

        bool ReadJoinedGuarded(uintptr_t ref, uintptr_t root, uintptr_t loaded, Joined& out)
        {
            __try {
                out.form_type    = *reinterpret_cast<const uint8_t*>(ref + kFormType);
                const uintptr_t cell        = *reinterpret_cast<const uintptr_t*>(ref + kRefCellAt);
                const uintptr_t cell_loaded = cell != 0 ? *reinterpret_cast<const uintptr_t*>(cell + kCellLoadedAt) : 0;
                const uintptr_t cell_root   = cell_loaded != 0 ? *reinterpret_cast<const uintptr_t*>(cell_loaded + kCellRootAt) : 0;
                if (cell_root != 0 && *reinterpret_cast<const uint16_t*>(cell_root + kNodeChildCount) > 4) {
                    const uintptr_t children = *reinterpret_cast<const uintptr_t*>(cell_root + kNodeChildrenAt);
                    if (children != 0) {
                        out.moving = *reinterpret_cast<const uintptr_t*>(children + 4 * sizeof(uintptr_t));
                    }
                }
                out.flags        = *reinterpret_cast<const uint64_t*>(root + kNodeFlags);
                out.stamp        = *reinterpret_cast<const uint32_t*>(root + kNodeStamp);
                out.parent       = *reinterpret_cast<const uintptr_t*>(root + kNodeParentAt);
                out.loaded_flags = *reinterpret_cast<const uint32_t*>(loaded + kLoadedFlags); // (without the reference's lock: the handler checks again)
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        using JoinFn = void (*)(void*);
        bool CallJoinGuarded(uintptr_t ref)
        {
            __try {
                reinterpret_cast<JoinFn>(At(kJoinRva))(reinterpret_cast<void*>(ref));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void AskKeepDrawn(uintptr_t spring, uintptr_t ref, uintptr_t root)
        {
            uint32_t body = 0;
            if (!g_join_ready.load() || root == 0 || ref == 0 || !SafeRead(&body, spring + kSpringBody, sizeof(body))) {
                return;
            }
            std::scoped_lock _{ g_keep_mutex };
            g_keep_asked = KeepDrawn{ spring, ref, root, body };
        }

        std::string DescribeJoined(const Joined& j)
        {
            const bool in_moving = j.parent != 0 && j.parent == j.moving;
            return fmt::format("{} (its cell's moving-objects node {}; root flags {:x}, bit 28 {}), its loaded flags {:x}{}, stamp {}",
                in_moving ? "in the moving objects" : "not in the moving objects", j.moving != 0 ? "found" : "not found", j.flags,
                (j.flags & kNodeMoving) != 0, j.loaded_flags,
                (j.loaded_flags & kLoadedEffect) != 0 ? " (an effect sequence still plays on it)" : "", j.stamp);
        }

        // At each frame's start (main thread), while the gloves hold or pull the object of this grab.
        void UpdateKeepDrawn(uintptr_t spring_now)
        {
            {
                std::scoped_lock _{ g_keep_mutex };
                if (g_keep_asked.spring != 0) {
                    g_keep       = g_keep_asked;
                    g_keep_asked = KeepDrawn{};
                }
            }
            auto& k = g_keep;
            if (k.spring == 0) {
                return;
            }
            uint32_t  body_now = 0;
            uintptr_t loaded = 0, root = 0;
            if (spring_now != k.spring || !SafeRead(&body_now, spring_now + kSpringBody, sizeof(body_now)) || body_now != k.body ||
                !SafeRead(&loaded, k.ref + kRefLoaded, sizeof(loaded)) || loaded == 0 || !SafeRead(&root, loaded + kLoaded3D, sizeof(root)) || root != k.root) {
                k = KeepDrawn{};
                return;
            }
            Joined j;
            if (!ReadJoinedGuarded(k.ref, root, loaded, j)) {
                k = KeepDrawn{};
                return;
            }
            ++k.frames;
            const bool joined = j.parent != 0 && j.parent == j.moving;
            if (k.frames == 1) {
                k.stamp0 = j.stamp;
                if (g_keep_tells.fetch_add(1) < 40) {
                    spdlog::info("[Grab] (fix161) the held object at the start: {}", DescribeJoined(j));
                }
            } else if (k.frames == 20) {
                if (g_keep_tells.fetch_add(1) < 40) {
                    spdlog::info("[Grab] (fix161) the held object 20 frames in: {}; its world update {} (stamp {} -> {}); the game's handler called {} "
                                 "time(s), {} frame(s) waited for the glow's effect to end",
                        DescribeJoined(j), j.stamp - k.stamp0 >= 10 ? "runs each frame" : "does not run each frame", k.stamp0, j.stamp, k.joins,
                        k.held_back);
                }
            }
            if (joined) {
                if (k.joins > 0 && !k.told_joined) {
                    k.told_joined = true;
                    if (g_keep_tells.fetch_add(1) < 40) {
                        spdlog::info("[Grab] (fix161) the held object is in the moving objects now ({} frame(s) in, after {} call(s))", k.frames, k.joins);
                    }
                }
                return;
            }
            if (j.form_type == kFormTypeActor || j.moving == 0) {
                return; // (an actor, or no moving-objects node found: left to the game)
            }
            if ((j.loaded_flags & kLoadedHeld) != 0) {
                ++k.held_back; // (the glow's effect ends at its next update: a frame or two)
                return;
            }
            // (fix162) The game's frame update calls its wake-up handler for every awake body (0x1418481B1, worlds with deactivation on):
            // given two frames to join it itself, the call here only if it has not.
            if (k.clear_frame < 0) {
                k.clear_frame = k.frames;
            }
            if (k.frames - k.clear_frame < 2) {
                return;
            }
            if (k.joins >= kJoinTries || (k.joins > 0 && k.frames - k.join_frame < kJoinGapFrames)) {
                return;
            }
            ++k.joins;
            k.join_frame  = k.frames;
            const bool ok = CallJoinGuarded(k.ref);
            if (g_keep_tells.fetch_add(1) < 40) {
                spdlog::info("[Grab] (fix161) the held object put in the game's moving objects ({} frame(s) in, call {}{})", k.frames, k.joins,
                    ok ? "" : ": it failed with an exception");
            }
            if (!ok) {
                g_join_ready = false;
            }
        }

        // A new grab's spring as the game set it, and the locked one it is brought to while the object comes to the
        // hand (the game's values are kept if they look unexpected). Called with g_mutex held.
        void PlanStiffness(uintptr_t spring, int grab)
        {
            g_plan_ok   = false;
            float force = 0.0f, elastic = 0.0f, damping = 0.0f;
            if (!SafeRead(&force, spring + kSpringForce, sizeof(force)) || !SafeRead(&elastic, spring + kSpringElastic, sizeof(elastic)) ||
                !SafeRead(&damping, spring + kSpringDamping, sizeof(damping))) {
                return;
            }
            const float stiff = std::clamp(GameFlow::gStore.internalSettings.grabStiffness, 0.0f, 1.0f);
            if (!(force > 1.0f && force < 100000.0f) || !(elastic > 0.0f && elastic <= 1.0f) || !(damping >= 0.0f && damping <= 1.0f)) {
                if (grab <= 5) {
                    spdlog::warn("[Grab] grab {}: the spring looks unexpected (force {:.1f}, elasticity {:.3f}, damping {:.3f}); left as the game set it", grab,
                        force, elastic, damping);
                }
                return;
            }
            g_force0   = force;
            g_elastic0 = elastic;
            g_damping0 = damping;
            g_force1   = std::max(force, force + (kLockForce - force) * stiff);
            g_elastic1 = elastic + (kLockElastic - elastic) * stiff;
            g_damping1 = damping + (kLockDamping - damping) * stiff;
            if (Physical::HoldingBody()) {
                // (fix172) A person's body (a ragdoll held by one bone): softer, its force capped, so it does not stretch or fly (as HIGGS
                // holds ragdolls with force-limited motors).
                Physical::BodySpring(force, elastic, damping, g_force1, g_elastic1, g_damping1);
            }
            g_plan_ok  = true;
            if (grab <= 5) {
                spdlog::info("[Grab] grab {}: spring goes from force {:.0f}, elasticity {:.2f}, damping {:.2f} to force {:.0f}, elasticity {:.2f}, damping {:.2f}",
                    grab, force, elastic, damping, g_force1, g_elastic1, g_damping1);
            }
        }

        // share: 0 = the game's spring, 1 = the planned one; lock: full strength whatever the setting (an object flying
        // to the hand must keep to its path). Returns the elasticity for now. g_mutex held, grab update.
        float ApplyStiffness(uintptr_t spring, float share, bool lock = false)
        {
            if (!g_plan_ok) {
                float elastic = 0.0f;
                SafeRead(&elastic, spring + kSpringElastic, sizeof(elastic));
                return elastic;
            }
            float force   = g_force0 + (g_force1 - g_force0) * share;
            float elastic = g_elastic0 + (g_elastic1 - g_elastic0) * share;
            float damping = g_damping0 + (g_damping1 - g_damping0) * share;
            if (lock && !Physical::HoldingBody()) { // (fix172: a body is never held at full strength)
                force   = std::max(force, kLockForce);
                elastic = kLockElastic;
                damping = kLockDamping;
            }
            *reinterpret_cast<float*>(spring + kSpringForce)   = force;
            *reinterpret_cast<float*>(spring + kSpringElastic) = elastic;
            *reinterpret_cast<float*>(spring + kSpringDamping) = damping;
            g_written_force                                    = force;
            return elastic;
        }

        // HIGGS's flight time for a pull from d metres: 0.30 s close by, 0.50 s at 1 m, 0.60 s at 2 m, 0.70 s at 5 m.
        float PullTime(float d) { return 0.715619f - 0.415619f * std::exp(-0.656256f * std::max(d, 0.0f)); }

        // A float a rip-relative instruction (4 bytes, then a disp32) reads, or NaN if the bytes are not the expected ones.
        float RipFloat(uintptr_t at, const uint8_t* prefix)
        {
            float value = std::numeric_limits<float>::quiet_NaN();
            if (Same(at, prefix, 4)) {
                const uintptr_t from = RelTarget(at + 4, at + 8);
                if (!InImage(from) || !SafeRead(&value, from, sizeof(value))) {
                    value = std::numeric_limits<float>::quiet_NaN();
                }
            }
            return value;
        }

        // How the held object followed the hand (for tuning): a line every few seconds of holding (since the line before), and
        // one when a hold ends, however short, for the whole hold (g_mutex held).
        struct HoldStats {
            int         steps{ 0 }, blocked{ 0 }, updates{ 0 };
            int64_t     age_us{ 0 };
            FollowStats f{};
        };
        HoldStats g_hold_total{}; // this hold so far (g_mutex)

        void AddStats(HoldStats& to, const HoldStats& from)
        {
            to.steps += from.steps;
            to.blocked += from.blocked;
            to.updates += from.updates;
            to.age_us += from.age_us;
            FollowStats&       a = to.f;
            const FollowStats& b = from.f;
            a.drawn += b.drawn;
            a.drawn_first += b.drawn_first;
            a.drawn_before_steps += b.drawn_before_steps;
            a.delay_us += b.delay_us;
            a.steps_before += b.steps_before;
            a.exact += b.exact;
            a.predicted += b.predicted;
            a.fallback += b.fallback;
            a.still_n += b.still_n;
            a.still_sum += b.still_sum;
            a.still_max = std::max(a.still_max, b.still_max);
            a.hand_n += b.hand_n;
            a.hand_sum += b.hand_sum;
            a.hand_along_sum += b.hand_along_sum;
            a.hand_move_sum += b.hand_move_sum;
            a.hand_max = std::max(a.hand_max, b.hand_max);
            a.moving_n += b.moving_n;
            a.moving_sum += b.moving_sum;
            a.along_sum += b.along_sum;
            a.move_sum += b.move_sum;
            a.moving_max = std::max(a.moving_max, b.moving_max);
            a.rot_still_n += b.rot_still_n;
            a.rot_turn_n += b.rot_turn_n;
            a.rot_still_sum += b.rot_still_sum;
            a.rot_turn_sum += b.rot_turn_sum;
            a.rot_rate_sum += b.rot_rate_sum;
            a.rot_still_max = std::max(a.rot_still_max, b.rot_still_max);
            a.rot_turn_max  = std::max(a.rot_turn_max, b.rot_turn_max);
            a.rot_led += b.rot_led;
            a.push_steps += b.push_steps;
            a.pushed += b.pushed;
            a.push_sum += b.push_sum;
            a.push_max = std::max(a.push_max, b.push_max);
            a.push_up += b.push_up;
            a.push_down += b.push_down;
            a.push_away += b.push_away;
            a.push_toward += b.push_toward;
            a.push_side += b.push_side;
            a.soft_steps += b.soft_steps;
            a.present_frames += b.present_frames;
            a.present_in_hand += b.present_in_hand;
            a.present_kept += b.present_kept;
            a.present_lost += b.present_lost;
            a.present_wrote += b.present_wrote;
            a.present_wrote_off += b.present_wrote_off;
            a.present_offset += b.present_offset;
        }

        void LogHolding(int64_t now, bool ended)
        {
            HoldStats part;
            part.steps   = g_steps.exchange(0);
            part.blocked = g_blocked_steps.exchange(0);
            part.age_us  = g_step_age_us.exchange(0);
            g_step_dt_us.exchange(0);
            part.updates = std::exchange(g_updates, 0);
            int   shows = 0, fresh = 0, stale = 0;
            bool  per_step_now = false;
            float lead_area = 0.0f, lead_hand = 0.0f;
            {
                std::scoped_lock track{ g_track_mutex };
                part.f       = std::exchange(g_follow_stats, FollowStats{});
                part.f.present_frames  = g_present_frames.exchange(0);
                part.f.present_in_hand = g_present_in_hand.exchange(0);
                part.f.present_kept    = g_present_kept.exchange(0);
                part.f.present_lost    = g_present_lost.exchange(0);
                part.f.present_wrote     = g_present_wrote.exchange(0);
                part.f.present_wrote_off = g_present_wrote_off.exchange(0);
                part.f.present_offset  = static_cast<double>(g_present_offset_um.exchange(0)) * 1e-6;
                shows        = g_follow.shows;
                fresh        = g_follow.fresh_votes;
                stale        = g_follow.stale_votes;
                per_step_now = g_follow.per_step;
                lead_area    = g_follow.lead_area;
                lead_hand    = g_follow.lead_hand;
            }
            AddStats(g_hold_total, part);
            const int64_t since = now - g_stats_us;
            g_stats_us          = now;
            HoldStats shown     = part;
            if (ended) {
                shown           = std::exchange(g_hold_total, HoldStats{});
                g_stats_pending = false;
                const int n     = ++g_holds_told;
                if (shown.updates == 0 || (n > 50 && n % 10 != 0)) {
                    return;
                }
            } else {
                ++g_stats_logs;
            }
            const FollowStats& f    = shown.f;
            auto               cm   = [](double sum, int count) { return count > 0 ? sum / count * 100.0 : 0.0; };
            auto               mean = [](double sum, int count) { return count > 0 ? sum / count : 0.0; };
            std::string        drawing;
            if (f.drawn > 0 || f.drawn_first > 0) {
                drawing = fmt::format("your hand was drawn {:.1f} ms after the grab update read it, after {:.1f} physics steps ({} frames with none, {} "
                                      "frames drawn before the update); ",
                    f.drawn > 0 ? f.delay_us / f.drawn * 1e-3 : 0.0, f.drawn > 0 ? f.steps_before / f.drawn : 0.0, f.drawn_before_steps, f.drawn_first);
            } else {
                drawing = "the body did not draw your hand (not shown), so the play area's move stood in; ";
            }
            const std::string what = ended ? fmt::format("hold ended after {:.1f} s (all of it)", static_cast<double>(g_last_us - g_start_us) * 1e-6)
                                           : fmt::format("holding (the last {:.1f} s)", static_cast<double>(since) * 1e-6);
            spdlog::info("[Grab] {}: {} physics steps for {} frames, each {:.1f} ms after the hand was read; {}your move known in {} frames, foretold {} in "
                         "{} ({} from the play area alone); the held object as drawn vs where your hand holds it: still {:.1f} cm (max {:.1f}, {} frames), "
                         "moving your hand {:.1f} cm (max {:.1f}, {:+.1f} cm along its motion, {} frames at {:.1f} cm a frame), walking or turning {:.1f} cm "
                         "(max {:.1f}, {:+.1f} cm along your move, {} frames at {:.1f} cm a frame); its orientation vs your hand's: still {:.1f} degrees (max "
                         "{:.1f}, {} frames), turning {:.1f} degrees (max {:.1f}, {} frames at {:.1f} degrees a frame; led a frame in {}); drawn objects show "
                         "{} (votes {}:{}); {} steps blocked; walking {:.1f} m/s; something pushed it off in {} of {} physics steps (by {:.1f} cm, max {:.1f}: "
                         "up {}, down {}, away from you {}, toward you {}, sideways {}), held softly in {} steps; lead learned: {:.2f} of a frame of walking or "
                         "turning, {:.2f} of the hand's own; drawn {} (exactly in your hand in {}, {:.1f} cm from where physics has it on average; the "
                         "game's writer put it where it was drawn in {} of {} writes checked; still there at the next frame's start in {} frames, "
                         "replaced in {})",
                what, shown.steps, shown.updates, shown.steps > 0 ? static_cast<double>(shown.age_us) / shown.steps * 1e-3 : 0.0, drawing, f.exact,
                per_step_now ? "per physics step" : "per frame", f.predicted, f.fallback, cm(f.still_sum, f.still_n), f.still_max * 100.0f, f.still_n,
                cm(f.hand_sum, f.hand_n), f.hand_max * 100.0f, cm(f.hand_along_sum, f.hand_n), f.hand_n, cm(f.hand_move_sum, f.hand_n),
                cm(f.moving_sum, f.moving_n), f.moving_max * 100.0f, cm(f.along_sum, f.moving_n), f.moving_n, cm(f.move_sum, f.moving_n),
                mean(f.rot_still_sum, f.rot_still_n), f.rot_still_max, f.rot_still_n, mean(f.rot_turn_sum, f.rot_turn_n), f.rot_turn_max, f.rot_turn_n,
                mean(f.rot_rate_sum, f.rot_turn_n), f.rot_led,
                shows == 2 ? "the frame before's reading (led one more frame)" : (shows == 1 ? "their own frame's reading" : "not known yet"), fresh, stale,
                shown.blocked, glm::length(g_walk), f.pushed, f.push_steps, cm(f.push_sum, f.pushed), f.push_max * 100.0f, f.push_up, f.push_down, f.push_away,
                f.push_toward, f.push_side, f.soft_steps, lead_area, lead_hand,
                f.present_frames > 0 ? fmt::format("by the hand in {} frames", f.present_frames) : std::string{ "where physics has it" }, f.present_in_hand,
                cm(f.present_offset, f.present_frames), f.present_wrote - f.present_wrote_off, f.present_wrote, f.present_kept, f.present_lost);
        }

        // Once a frame, in the grab update, before the target goes into the springs: the target becomes the holding
        // hand (the physics step moves it on with the hand between frames). Letting go of the grip drops the object
        // here and throws it.
        // ---- (fix170) Two hands ----
        // After fix169: holding items in both hands at the same time, both kinds: one
        // object in both hands, and one object in each hand (two helpers' research: the exe, HIGGS's and ROCK's source).
        // (1) Both hands on one object: the other grip squeezed with its palm at the held object joins it (the second hand). The one
        //     game grab and its one spring are kept; only the place and turn the mod gives it change: ROCK's shared pose (Fallout 4
        //     VR, brunocatani/ROCK, coordinateLooseObjectProxy; FRIK's pivot): the first hand's grip stays exactly where its hand
        //     holds it, and the object is turned by the shortest turn that brings the line from the first grip to the second grip
        //     onto the line toward the second palm (the grip distance fixed as it was taken: it never stretches); the roll stays the
        //     first hand's. Eased in over 0.15 s. The second hand is drawn on its grip and its fingers close round the object.
        //     Letting go with the second hand: held by the first as it lies; with the first: taken into the second as it lies (hand to
        //     hand passing: bring the other hand to it); the second hand stretched 25 cm off its grip lets go.
        // (2) One object in each hand: the game's grab holds one reference (player+0xD80 its springs, +0xDA4 one handle, +0xF54 its
        //     mode; its start refuses while the mode is set, 0x141A50600), but its spring steps on its own (the step, 0x1423D3E00,
        //     reads nothing of the player). So the other grip squeezed away from the held object "adopts" its spring: in the player's
        //     update, where the game holds that list's lock just before its grab update (0x141A305E6), the spring is taken out of
        //     the list and the game's own drop is called on the empty list (it resets the game's grab, removing no spring); the
        //     spring keeps stepping in the world, steered here each frame (its target +0xA0 and its turn +0x70..+0x9F, written as
        //     the game's grab update writes them); then the other hand starts its own grab with the game's. Let go: thrown by its
        //     own step (as the grab's throw) and taken out of the world as the game's drop does it (0x141A53A80's own calls).
        constexpr float   kSupportReach   = 0.10f;   // the other palm this near the held object's box (m) joins it
        constexpr float   kSupportAimMin  = 0.10f;   // grips nearer than this: the second hand rides along, aiming nothing
        constexpr float   kSupportStretch = 0.25f;   // the second palm this far off its grip: it lets go
        constexpr int64_t kSupportBlendUs = 150'000; // the shared pose eased in over this
        constexpr float   kAdoptFar       = 1.0f;    // an adopted object this far from where its hand holds it...
        constexpr int     kAdoptFarFrames = 45;      // ...for this many frames running is let go (stuck behind something)

        struct Support {
            bool      active{ false };
            int       hand{ -1 };
            int       serial{ 0 };                       // the grab it joined (g_taken.serial)
            int64_t   join_us{ 0 };
            glm::vec3 grip{ 0.0f };                      // where the second palm holds it, object space
            bool      aims{ false };                     // the grips far enough apart to turn it
            glm::quat turn{ 1.0f, 0.0f, 0.0f, 0.0f };    // this frame's world turn toward the second hand (OnTarget -> OnRotation)
            uint64_t  turn_frame{ 0 };
            glm::vec3 grip_world{ 0.0f };                // the second grip where the object is put this frame (the body draws that hand there)
            bool      have_grip_world{ false };
            float     stretch{ 0.0f };                   // the second palm's distance from its grip (m)
        };
        Support          g_support{};                    // (g_mutex)
        std::atomic<int> g_support_hand{ -1 };
        bool             g_seat_keep_tilt{ false };      // (g_mutex) the next seating keeps the object's tilt (from a two-handed hold)
        glm::mat3        g_last_given{ 1.0f };           // (g_mutex) the rows OnRotation gave the game last...
        bool             g_have_given{ false };          // ...(for an adopted spring's own turn: see AdoptTurn)

        struct Adopted {
            bool        active{ false };
            int         hand{ -1 };
            uintptr_t   spring{ 0 };                     // the game's spring, out of the player's list (the list's reference is ours)
            uint32_t    body{ 0 };
            uintptr_t   world{ 0 };                      // the physics world it steps in (as the drop finds it)
            uintptr_t   ref{ 0 };
            uintptr_t   root{ 0 };
            float       scale{ 1.0f };
            glm::vec3   held{ 0.0f };                    // the point the spring holds, object space
            glm::mat3   in_hand_r{ 1.0f };               // the object in the hand's frame (HandFrame)...
            glm::vec3   in_hand_t{ 0.0f };               // ...and its origin there
            glm::mat3   k{ 1.0f };                       // the spring's own turn = the rows given x k (AdoptTurn)
            glm::mat3   rows_to_object{ 1.0f };
            bool        have_rows{ false };
            bool        box{ false };                    // its box, for the fingers (as g_track had it)
            bool        cylinder{ false };
            int         axis{ -1 };
            float       radius{ 0.0f };
            glm::vec3   lo{ 0.0f }, hi{ 0.0f };
            int         serial{ 0 };
            int64_t     us{ 0 };
            int         far_frames{ 0 };
            int         steers{ 0 };
            int64_t     loading{ 0 };                    // (GameFlow::lastLoadingScreen when taken)
            bool        handover{ false };               // (taken by the game's grab of your other hand: its body still held)
            bool        release{ false };                // let go: thrown by its own step, then taken out of the world
            bool        thrown_armed{ false };
            int         release_frames{ 0 };
            glm::vec3   velocity{ 0.0f };
            const char* why{ "" };
        };
        Adopted          g_adopted{};                    // (g_mutex)
        std::atomic<int> g_adopted_hand{ -1 };
        // For the body (its own threads; leaf locks, never held while taking another): the adopted object's box for the fingers, and
        // where the second hand's grip is put this frame.
        struct AdoptBox {
            bool      valid{ false };
            uintptr_t root{ 0 };
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };
            bool      cylinder{ false };
            int       axis{ -1 };
            float     radius{ 0.0f };
            int       serial{ 0 };
        };
        std::mutex g_adopt_box_mutex;
        AdoptBox   g_adopt_box{};
        std::mutex g_support_draw_mutex;
        glm::vec3  g_support_draw{ 0.0f };     // (the second grip in the first hand's frame: HandFrame)
        int        g_support_draw_first{ -1 };
        int        g_support_draw_hand{ -1 };
        int64_t    g_support_draw_us{ 0 };
        struct AdoptAsk {
            bool    pending{ false };
            int     holder{ -1 };
            int     other{ -1 };
            int64_t us{ 0 };
        };
        AdoptAsk          g_adopt_ask{};                 // (g_mutex)
        std::atomic<bool> g_adopt_asked{ false };        // (an ask waiting: OnGrabSlot looks; else it returns at once)
        std::atomic<int>  g_adopt_result{ 0 };           // 1 adopted (the other hand's grab starts), -1 not (passed as before)
        std::atomic<int>  g_adopt_other{ -1 };
        std::atomic<int>  g_adoptions{ 0 };
        // The adopted spring's throw at its own physics step (OnHeldPoint; g_track_mutex).
        std::atomic<uintptr_t> g_adopt_throw_spring{ 0 };
        glm::vec3              g_adopt_throw_velocity{ 0.0f };
        bool                   g_adopt_throw_done{ false };
        int64_t                g_adopt_throw_us{ 0 };

        // The game's drop's own calls (0x141A53A80, checked at their offsets in it when this installs), for taking an adopted spring
        // out of its world as the drop takes its listed ones.
        constexpr uintptr_t kDropCellWorldCall = 0x9a;   // call: a cell's physics data (an interior's), rva 510d40
        constexpr uintptr_t kDropExtWorldLoad  = 0x9f;   // mov rcx,[rip+x]: the exterior's (rva 61e7488)
        constexpr uintptr_t kDropNotifyTable   = 0xdd;   // lea r12,[rip+x]: the functor's table (rva 4cc19a0)
        constexpr uintptr_t kDropBodyObject    = 0x12d;  // call: the body's collision object (rva 238c170)
        constexpr uintptr_t kDropNotify        = 0x177;  // call: told it is let go (rva 241a2e0)
        constexpr uintptr_t kDropBodyFlag      = 0x18e;  // call: the body's "grabbed" flag 0x40000000 cleared (rva 2361d40)
        constexpr uintptr_t kDropWorldLock     = 0x19a;  // call: the world's write lock (world+0xb10, rva 22cea30)
        constexpr uintptr_t kDropHkWorld       = 0x1a2;  // call: its hknpWorld (rva 3060ec0)
        constexpr uintptr_t kDropRemove        = 0x1b1;  // call: the action removed (rva 308b8c0)
        constexpr uintptr_t kDropWorldUnlock   = 0x1b9;  // call: unlocked (rva 2361f50)
        constexpr uintptr_t kDropRelease       = 0x1c7;  // call: the spring's reference released (rva 2e934a0)
        // (fix190) Where each of those goes (the places named above), checked against them at Install.
        constexpr uintptr_t kCellWorldRva      = 0x510d40;
        constexpr uintptr_t kExtWorldRva       = 0x61e7488;
        constexpr uintptr_t kNotifyTableRva    = 0x4cc19a0;
        constexpr uintptr_t kBodyObjectRva     = 0x238c170;
        constexpr uintptr_t kNotifyRva         = 0x241a2e0;
        constexpr uintptr_t kBodyFlagRva       = 0x2361d40;
        constexpr uintptr_t kWorldLockRva      = 0x22cea30;
        constexpr uintptr_t kHkWorldRva        = 0x3060ec0;
        constexpr uintptr_t kRemoveRva         = 0x308b8c0;
        constexpr uintptr_t kWorldUnlockRva    = 0x2361f50;
        constexpr uintptr_t kReleaseRva        = 0x2e934a0;
        constexpr uintptr_t kRefCellSlot       = 0xb0;   // the player's cell (the drop reads it so)
        constexpr uintptr_t kWorldOfData       = 0x50;   // ...its physics data's bhkWorld
        constexpr uintptr_t kSpringTurn        = 0x70;   // the spring's turn: three rows of four floats
        uintptr_t g_fn_cell_world{ 0 }, g_ext_world_global{ 0 }, g_notify_table{ 0 }, g_fn_body_object{ 0 }, g_fn_notify{ 0 }, g_fn_body_flag{ 0 },
            g_fn_world_lock{ 0 }, g_fn_hk_world{ 0 }, g_fn_remove{ 0 }, g_fn_world_unlock{ 0 }, g_fn_release{ 0 };
        std::atomic<bool>   g_adopt_ready{ false };
        safetyhook::MidHook g_slot_hook{};
        // 1a305c5: lea rdi,[rsi+0xd80] ; lea rbx,[rdi+0x10] ; mov rcx,rbx ; call (the list's lock) ; mov [rbp+0x128],rbx ; mov [rbp+0x130],rdi
        // 1a305e6: mov eax,[rsi+0xf54] ; cmp eax,1 ; jne ; cmp [rdi],0 ; jne -> call the grab update (rva 1a52da0)
        constexpr const char* kSlotPattern =
            "48 8D BE 80 0D 00 00 48 8D 5F 10 48 8B CB E8 ? ? ? ? 48 89 9D 28 01 00 00 48 89 BD 30 01 00 00 8B 86 54 0F 00 00 83 F8 01 75 07 83 3F 00";
        constexpr uintptr_t kSlotRva  = 0x1a305c5;
        constexpr uintptr_t kSlotHook = 0x21;

        // The world the player's grab steps in, found as the game's drop finds it: the player's cell; an interior's own physics
        // data (rva 510d40 of its +0x50), else the exterior's (the global's); its bhkWorld at +0x50. (The player's update thread.)
        using CellDataFn = uintptr_t (*)(uintptr_t);
        uintptr_t PlayerWorldGuarded(uintptr_t player)
        {
            __try {
                const uintptr_t cell = *reinterpret_cast<const uintptr_t*>(player + kRefCellSlot);
                if (cell == 0) {
                    return 0;
                }
                uintptr_t data = 0;
                if ((*reinterpret_cast<const uint8_t*>(cell + 0x48) & 1) != 0) {
                    data = reinterpret_cast<CellDataFn>(g_fn_cell_world)(*reinterpret_cast<const uintptr_t*>(cell + 0x50));
                } else {
                    const uintptr_t global = *reinterpret_cast<const uintptr_t*>(g_ext_world_global);
                    data                   = global != 0 ? *reinterpret_cast<const uintptr_t*>(global) : 0;
                }
                return data != 0 ? *reinterpret_cast<const uintptr_t*>(data + kWorldOfData) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // An adopted spring taken out of its world, as the drop takes each listed spring out (0x141A53B96..0x141A53C4C): its body, if
        // the world still has it, told it is let go and its "grabbed" flag cleared; the action removed under the world's write lock;
        // the reference released. (The player's update thread, the player's spring list locked by the game, as the drop runs.)
        using BodyValidFn  = int (*)(uintptr_t self, uint32_t body);
        using BodyObjectFn = uintptr_t (*)(uintptr_t world, uint32_t body);
        using ObjectTestFn = uintptr_t (*)(uintptr_t object);
        using NotifyFn     = void (*)(void* functor, uintptr_t object);
        using BodyFlagFn   = void (*)(uintptr_t world, uint32_t body, uint32_t flag, uint32_t clear);
        using LockAtFn     = void (*)(uintptr_t lock);
        using HkWorldFn    = uintptr_t (*)(uintptr_t world);
        using RemoveFn     = void (*)(uintptr_t hk_world, void* out, uintptr_t action);
        using WorldFn      = void (*)(uintptr_t world);
        using ReleaseFn    = void (*)(uintptr_t object);
        bool ReleaseRefGuarded(uintptr_t spring)
        {
            __try {
                reinterpret_cast<ReleaseFn>(g_fn_release)(spring);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool TellLetGoGuarded(uintptr_t world, uint32_t body)
        {
            __try {
                const uintptr_t iface = world + 0x20;
                const uintptr_t table = *reinterpret_cast<const uintptr_t*>(iface);
                const auto      valid = *reinterpret_cast<const BodyValidFn*>(table + 0x58);
                if (valid(iface, body) == 0) {
                    return true; // (the world no longer has it: nothing to tell, as the drop)
                }
                uintptr_t object = reinterpret_cast<BodyObjectFn>(g_fn_body_object)(world, body);
                while (object != 0) {
                    const uint32_t flags = *reinterpret_cast<const uint32_t*>(object + 0x118);
                    if (((flags >> 14) & 1) != 0) {
                        const uintptr_t vt   = *reinterpret_cast<const uintptr_t*>(object);
                        const auto      test = *reinterpret_cast<const ObjectTestFn*>(vt + 0x38);
                        if (test(object) != 0) {
                            break;
                        }
                    }
                    object = *reinterpret_cast<const uintptr_t*>(object + 0x38);
                }
                if (object != 0) {
                    uintptr_t holder = world;
                    struct {
                        uintptr_t  table;
                        uintptr_t* world;
                    } functor{ g_notify_table, &holder };
                    reinterpret_cast<NotifyFn>(g_fn_notify)(&functor, object);
                }
                reinterpret_cast<BodyFlagFn>(g_fn_body_flag)(world, body, 0x40000000u, 1u);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool LockWorldGuarded(uintptr_t world)
        {
            __try {
                reinterpret_cast<LockAtFn>(g_fn_world_lock)(world + 0xb10);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool RemoveActionGuarded(uintptr_t world, uintptr_t spring)
        {
            __try {
                const uintptr_t hk = reinterpret_cast<HkWorldFn>(g_fn_hk_world)(world);
                alignas(16) uint8_t out[16]{};
                reinterpret_cast<RemoveFn>(g_fn_remove)(hk, out, spring);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool UnlockWorldGuarded(uintptr_t world)
        {
            __try {
                reinterpret_cast<WorldFn>(g_fn_world_unlock)(world);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (`body_ours`: the body still this object's, so it is told it is let go and its flag cleared; else only the action removed:
        // removing an action a world does not have is harmless, 0x14308B8C0 looks for it.) The world's lock always given back, whatever
        // fails in between (the review).
        bool RemoveAdoptedGuarded(uintptr_t world, uintptr_t spring, uint32_t body, bool body_ours)
        {
            bool ok = !body_ours || TellLetGoGuarded(world, body);
            if (!LockWorldGuarded(world)) {
                return false;
            }
            ok = RemoveActionGuarded(world, spring) && ok;
            UnlockWorldGuarded(world);
            return ReleaseRefGuarded(spring) && ok;
        }

        // The spring's own turn for the adopted object: the game's grab update builds it from the rows it is given (stored rows G)
        // times the spring's grab-time turn and the grab's own (fixed: M), S = G x M. M = G^T x S once, from the last rows given and
        // the spring's turn then; afterwards S = G' x M for the rows G' that would hold the object as wanted. (Stored rows: a row of
        // floats is glm's column, as OnRotation writes them.)
        bool AdoptTurn(uintptr_t spring, const glm::mat3& given, glm::mat3& out_k)
        {
            float s[12]{};
            if (!SafeRead(s, spring + kSpringTurn, sizeof(s))) {
                return false;
            }
            // (Each stored vector a glm column, as OnRotation writes the rows: the game's products 0x142E9A740 / 0x142E99E80 make out's
            // vector j = sum over i of a's vector i x b's vector j [i], glm's A x B; so S = G x K and K = G^T x S. The review.)
            glm::mat3 turn{ 1.0f };
            for (int c = 0; c < 3; ++c) {
                for (int i = 0; i < 3; ++i) {
                    turn[c][i] = s[c * 4 + i];
                }
            }
            out_k = glm::transpose(given) * turn;
            // A turn (columns of length 1, square to each other): else the layout is not as expected and the turn is left to the step.
            for (int c = 0; c < 3; ++c) {
                if (!Finite(out_k[c]) || std::abs(glm::length(out_k[c]) - 1.0f) > 0.01f || std::abs(glm::dot(out_k[c], out_k[(c + 1) % 3])) > 0.01f) {
                    return false;
                }
            }
            return true;
        }

        bool WriteGuarded(uintptr_t at, const void* from, size_t n)
        {
            __try {
                std::memcpy(reinterpret_cast<void*>(at), from, n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (g_mutex) The adopted object steered for this frame: its held point where the hand holds it, its turn the hand's.
        void SteerAdopted()
        {
            auto&     a = g_adopted;
            glm::mat3 hr{ 1.0f };
            glm::vec3 ht{ 0.0f };
            if (!HandFrame(a.hand, hr, ht)) {
                return;
            }
            const glm::mat3 obj_r  = hr * a.in_hand_r;
            const glm::vec3 origin = ht + hr * a.in_hand_t;
            const glm::vec3 target = origin + obj_r * (a.scale * a.held);
            if (!Finite(target)) {
                return;
            }
            const float to[3]{ target.x, target.y, target.z };
            WriteGuarded(a.spring + kSpringTarget, to, sizeof(to));
            if (a.have_rows) {
                const glm::mat3 rows = obj_r * glm::transpose(a.rows_to_object); // (glm column r = stored row r)
                float           s[12]{};
                if (SafeRead(s, a.spring + kSpringTurn, sizeof(s))) {
                    const glm::mat3 turn = rows * a.k; // (S' = G' x K, each stored vector a glm column; their fourth floats kept)
                    for (int c = 0; c < 3; ++c) {
                        for (int i = 0; i < 3; ++i) {
                            s[c * 4 + i] = turn[c][i];
                        }
                    }
                    bool finite = true;
                    for (const float v : s) {
                        finite = finite && std::isfinite(v);
                    }
                    if (finite) {
                        WriteGuarded(a.spring + kSpringTurn, s, sizeof(s));
                    }
                }
            }
            // Too far from where the hand holds it for a while (stuck behind something): let go.
            NodeXf physics;
            if (HeldWorld(a.root, physics)) {
                const glm::vec3 held_now = physics.r * (physics.s * a.held) + physics.t;
                a.far_frames             = glm::length(held_now - target) > kAdoptFar ? a.far_frames + 1 : 0;
                if (a.far_frames > kAdoptFarFrames && !a.release) {
                    a.release  = true;
                    a.velocity = glm::vec3{ 0.0f };
                    a.why      = "stuck too far from your hand";
                }
            }
            ++a.steers;
        }

        // (g_mutex; the player's update thread, the spring list locked by the game) The adopted object let go: thrown by its own
        // physics step (OnHeldPoint gives the held point the hand's speed; at most 3 frames), then taken out of the world; its group
        // back once clear of you (UpdateGroup).
        void EndAdopted(uintptr_t player, bool throw_it)
        {
            auto& a = g_adopted;
            if (!a.active) {
                return;
            }
            const uintptr_t world_now = PlayerWorldGuarded(player);
            // (Another loading screen since it was taken (a load, a fast travel, a cell change): its world, or one at its address, is not the one
            // it was added to: only its reference let go; the review.)
            const bool same_world = world_now == a.world && a.world != 0 && GameFlow::lastLoadingScreen() == a.loading;
            if (throw_it && g_step_ready.load() && g_point_hook && same_world && glm::length(a.velocity) > 0.3f) {
                std::scoped_lock track{ g_track_mutex };
                if (!a.thrown_armed) {
                    a.thrown_armed           = true;
                    g_adopt_throw_velocity   = a.velocity;
                    g_adopt_throw_done       = false;
                    g_adopt_throw_us         = NowUs();
                    g_adopt_throw_spring.store(a.spring, std::memory_order_release);
                    return; // (its step gives it the speed; taken out at the next frame)
                }
                if (!g_adopt_throw_done && ++a.release_frames <= 3) {
                    return;
                }
            }
            g_adopt_throw_spring.store(0, std::memory_order_release);
            bool removed = false;
            if (same_world) {
                // (Its body told and its flag cleared only while it is still this object's: the world's id could be another body's now.)
                // (Handed over to the game's grab: its body still held, so not told it is let go; only this spring removed.)
                removed = RemoveAdoptedGuarded(a.world, a.spring, a.body, !a.handover && HeldCollision(a.root, a.body) != 0);
            } else {
                ReleaseRefGuarded(a.spring); // (another world now: the old one's own reference goes with it)
            }
            {
                std::scoped_lock group{ g_group_mutex };
                g_group_adopt_end = true; // (its group change to the let-go slot: UpdateGroup)
            }
            const int n = g_adoptions.load();
            if (n <= 20 || n % 10 == 0) {
                spdlog::info("[Grab] (fix170) the object in your {} hand let go ({}): {} after {} frames held on its own{}", a.hand == 0 ? "left" : "right",
                    a.why, removed ? "taken out of the world as the game's drop does" : (!same_world ? "another physics world or a load since: its reference let go" : "taking it out failed"),
                    a.steers, throw_it && g_adopt_throw_done ? fmt::format(", thrown at {:.1f} m/s", glm::length(a.velocity)) : std::string{});
            }
            {
                std::scoped_lock box{ g_adopt_box_mutex };
                g_adopt_box = AdoptBox{};
            }
            a = Adopted{};
            g_adopted_hand = -1;
        }

        // (g_mutex; the player's update thread, with the game holding the spring list's lock) The held object's spring adopted for
        // the hand holding it: out of the list, the game's grab reset by its own drop on the empty list, steered here from now on.
        bool AdoptHeld(uintptr_t player, const AdoptAsk& ask, std::string& why)
        {
            const uintptr_t spring = CurrentSpring(player);
            if (!g_adopt_ready.load() || g_adopted.active) {
                why = "not ready";
                return false;
            }
            if (SpringCount(player) != 1 || spring == 0 || spring != g_spring || !g_use_taken || g_pull.active || g_release) {
                why = SpringCount(player) > 1 ? "it is held by several springs (an object of several parts)" : "the hold is not one the hand holds now";
                return false;
            }
            Track track;
            {
                std::scoped_lock t{ g_track_mutex };
                track = g_track;
                if (g_throw.spring != 0) {
                    why = "it is being thrown";
                    return false;
                }
            }
            NodeXf    xf;
            glm::mat3 hr{ 1.0f };
            glm::vec3 ht{ 0.0f };
            uint32_t  body = 0;
            if (!track.have_held || !DrawnWorld(g_taken.root, xf) || !HandFrame(ask.holder, hr, ht) || !SafeRead(&body, spring + kSpringBody, sizeof(body))) {
                why = "where it is held is not known";
                return false;
            }
            const uintptr_t world = PlayerWorldGuarded(player);
            if (world == 0) {
                why = "the physics world was not found";
                return false;
            }
            Adopted a{};
            a.hand      = ask.holder;
            a.spring    = spring;
            a.body      = body;
            a.world     = world;
            a.ref       = g_taken.ref;
            a.root      = g_taken.root;
            a.scale     = xf.s;
            a.held      = track.held;
            a.in_hand_r = glm::transpose(hr) * xf.r;
            a.in_hand_t = glm::transpose(hr) * (xf.t - ht);
            a.have_rows = g_have_given && g_have_object && AdoptTurn(spring, g_last_given, a.k);
            a.rows_to_object = g_rows_to_object;
            a.box       = track.box;
            a.cylinder  = track.cylinder;
            a.axis      = track.axis;
            a.radius    = track.radius;
            a.lo        = track.lo;
            a.hi        = track.hi;
            a.serial    = track.serial;
            a.us        = NowUs();
            a.loading   = GameFlow::lastLoadingScreen();
            // Its group change kept aside first (UpdateGroup), so the game's grab reset below is never taken for the hold's end.
            {
                std::scoped_lock group{ g_group_mutex };
                g_group_adopt_ask = spring;
            }
            // Out of the list (the game holds its lock here), then the game's own drop on the empty list: its grab state reset, the
            // spring left in the world and its reference ours.
            const uint32_t none = 0;
            if (!WriteGuarded(player + kSpringCount, &none, sizeof(none))) {
                std::scoped_lock group{ g_group_mutex };
                g_group_adopt_ask = 0;
                why               = "the spring list could not be changed";
                return false;
            }
            if (!CallPlayer(g_drop_fn, player)) {
                // (Kept as adopted, so it can still be let go as the drop does; grip grabbing off for the session, as a failed start does.)
                g_adopt_ready = false;
                g_grip_ready  = false;
                spdlog::error("[Grab] (fix170) the game's drop failed with an exception while keeping a held object in its hand: grip grabbing and "
                              "one object in each hand are off for this session");
            }
            // The mod's hold of the game's grab ends here (as at a drop).
            g_spring       = 0;
            g_track_spring = 0;
            g_have_placed  = false;
            g_support      = Support{};
            g_support_hand = -1;
            EndPresent();
            {
                std::scoped_lock debug{ g_debug_mutex };
                g_debug = DebugHeldInfo{};
            }
            a.active          = true;
            g_adopted         = a;
            g_adopted_hand    = a.hand;
            {
                std::scoped_lock box{ g_adopt_box_mutex };
                g_adopt_box = AdoptBox{ a.box, a.root, a.lo, a.hi, a.cylinder, a.axis, a.radius, a.serial };
            }
            g_adopt_other     = ask.other;
            const int n       = g_adoptions.fetch_add(1) + 1;
            if (n <= 20 || n % 10 == 0) {
                spdlog::info("[Grab] (fix170) the object in your {} hand is held on its own (its spring kept from the game's grab: world {:x}, its turn "
                             "steered {}); your {} hand can grab something else",
                    a.hand == 0 ? "left" : "right", world, a.have_rows ? "too" : "no (only its place)", ask.other == 0 ? "left" : "right");
            }
            return true;
        }

        // In the player's update just before its grab test, with the game holding the spring list's lock (the player's update thread):
        // an adoption asked for is made; the adopted object steered, checked, let go.
        void OnGrabSlot(safetyhook::Context& ctx)
        {
            const uintptr_t player = ctx.rsi;
            if ((!g_adopt_asked.load(std::memory_order_acquire) && g_adopted_hand.load(std::memory_order_acquire) < 0) || !IsPlayer(player)) {
                return; // (nothing asked, nothing held on its own)
            }
            std::scoped_lock _{ g_mutex };
            if (g_adopt_ask.pending) {
                const AdoptAsk ask = g_adopt_ask;
                g_adopt_ask        = AdoptAsk{};
                g_adopt_asked      = false;
                std::string why;
                if (NowUs() - ask.us >= 300'000) {
                    why = "asked too long ago";
                } else if (!Active()) {
                    why = "the hand grab is not active now";
                }
                if (why.empty() && AdoptHeld(player, ask, why)) {
                    g_adopt_result = 1;
                } else {
                    g_adopt_result = -1;
                    static int told = 0;
                    if (++told <= 20) {
                        spdlog::info("[Grab] (fix170) the held object could not be kept in its hand on its own ({}): passed to the other hand as before", why);
                    }
                }
            }
            auto& a = g_adopted;
            if (!a.active) {
                return;
            }
            // Gone, or no longer the same: its reference unloaded or another 3D, the spring holding another body, another world.
            uintptr_t  loaded = 0, root = 0;
            uint32_t   body   = 0;
            const bool there  = a.ref != 0 && SafeRead(&loaded, a.ref + kRefLoaded, sizeof(loaded)) && loaded != 0 &&
                               SafeRead(&root, loaded + kLoaded3D, sizeof(root)) && root == a.root && SafeRead(&body, a.spring + kSpringBody, sizeof(body)) &&
                               body == a.body;
            if (!there && !a.release) {
                a.release  = true;
                a.velocity = glm::vec3{ 0.0f };
                a.why      = "it is gone or changed (picked up, unloaded)";
            }
            // (Taken by your other hand's grab meanwhile (its pick found it): the game's grab holds it from now on, its group change kept for
            // it (UpdateGroup); two springs never pull at it. The review.)
            if (!a.release) {
                const uintptr_t current = CurrentSpring(player);
                uint32_t        held_by = 0;
                if (current != 0 && current != a.spring && SafeRead(&held_by, current + kSpringBody, sizeof(held_by)) && held_by == a.body) {
                    a.release  = true;
                    a.velocity = glm::vec3{ 0.0f };
                    a.why      = "taken by your other hand: held by the game's grab from now on";
                    a.handover = true;
                }
            }
            // (Not for menus, conversations or the scanner: the game keeps its own grab through them too; the review.)
            const bool weapon_out = GameFlow::isWeaponDrawn() && !HandsFreeNow();
            if (!a.release && (!Active() || weapon_out || GameFlow::isPilotingShip() || !GameFlow::gStore.internalSettings.oneInEachHand)) {
                a.release  = true;
                a.velocity = glm::vec3{ 0.0f };
                a.why      = !GameFlow::gStore.internalSettings.oneInEachHand ? "the setting turned off"
                             : weapon_out                                       ? "a weapon out"
                             : GameFlow::isPilotingShip()                       ? "the pilot's seat"
                                                                                : "the hand grab is not active now";
            }
            if (a.release) {
                EndAdopted(player, glm::length(a.velocity) > 0.0f && there);
                return;
            }
            SteerAdopted();
        }

        // (Main thread) The other palm at the held object: it joins as the second hand. False: not near (or not to be joined).
        bool JoinSupport(int holder, int other)
        {
            std::scoped_lock _{ g_mutex };
            if (g_spring == 0 || !g_use_taken || g_pull.active || g_release || g_need_contact || !g_taken.have_contact) {
                return false;
            }
            NodeXf    xf;
            GripFrame second, first;
            if (!DrawnWorld(g_taken.root, xf) || !HandGripWorld(other, second) || !HandGripWorld(holder, first)) {
                return false;
            }
            const glm::vec3 p    = ToNode(xf, second.surface);
            const glm::vec3 on   = glm::clamp(p, g_taken.lo, g_taken.hi);
            const float     gap  = glm::length(on - p) * xf.s;
            if (!(gap <= kSupportReach)) {
                return false;
            }
            const glm::vec3 pivot = first.surface + first.facing * g_taken.lift;
            const glm::vec3 grip  = xf.r * (xf.s * on) + xf.t;
            const float     reach = glm::length(grip - pivot);
            g_support          = Support{};
            g_support.active   = true;
            g_support.hand     = other;
            g_support.serial   = g_taken.serial;
            g_support.join_us  = NowUs();
            g_support.grip     = on;
            g_support.aims     = reach >= kSupportAimMin;
            g_support_hand     = other;
            static int told = 0;
            if (++told <= 20) {
                spdlog::info("[Grab] (fix170) your {} hand joins: both hands hold it (its palm {:.2f} m from the object, the grips {:.2f} m apart: {})",
                    other == 0 ? "left" : "right", gap, reach, g_support.aims ? "the second hand turns it with the first" : "too close to turn it: it rides along");
            }
            return true;
        }

        // (Main thread, g_mutex held) The second hand lets go: held by the first as it lies now (its turn kept: rebased).
        void EndSupport(const char* why)
        {
            if (!g_support.active) {
                g_support_hand = -1;
                return;
            }
            const int hand = g_support.hand;
            g_support      = Support{};
            g_support_hand = -1;
            g_rebase       = true;
            g_turn         = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }; // (the turn so far is in the rows kept: nothing more to ease in)
            g_turn_us      = 0;
            static int told = 0;
            if (++told <= 20) {
                spdlog::info("[Grab] (fix170) your {} hand lets go of it ({}): the other holds it as it lies", hand == 0 ? "left" : "right", why);
            }
        }

        void OnTarget(safetyhook::Context& ctx)
        {
            if (!IsPlayer(ctx.r14) || !Active()) {
                return;
            }
            const uintptr_t player = ctx.r14;
            const uintptr_t spring = CurrentSpring(player);
            if (spring != 0) {
                uint32_t body = 0;
                if (SafeRead(&body, spring + kSpringBody, sizeof(body))) {
                    std::scoped_lock debug{ g_debug_mutex };
                    g_debug = DebugHeldInfo{ spring, body, NowUs() }; // (the world is found from the 3D by DebugHeld)
                }
            }
            std::scoped_lock _{ g_mutex };

            if (g_release) {
                const bool was_pulling = g_pull.active;
                const bool keep_speed  = g_release_keep;
                uint32_t   body        = 0;
                uintptr_t  world       = 0;
                const bool have_body   = spring != 0 && SafeRead(&body, spring + kSpringBody, sizeof(body)) &&
                                       SafeRead(&world, ctx.rbp + kWorldSlot, sizeof(world)) && world != 0;
                uintptr_t       table = 0, function = 0;
                const uintptr_t bodies       = world + kWorldBodies;
                const bool      set_velocity = have_body && SafeRead(&table, bodies, sizeof(table)) &&
                                          SafeRead(&function, table + kSetVelocity, sizeof(function)) && InImage(function);
                // Without the velocity function, the spring itself throws it: hold on for one more physics step (at most a few
                // frames), which gives the held point the throw's velocity, then drop.
                const bool can_throw = !was_pulling && !keep_speed && !set_velocity && spring != 0 && spring == g_spring && g_point_hook &&
                                       g_step_ready.load();
                bool       thrown = false, wait = false;
                {
                    std::scoped_lock track{ g_track_mutex };
                    if (can_throw) {
                        if (g_throw.spring != spring) {
                            g_throw = Throw{ spring, g_throw_linear, false, 0, NowUs() };
                            g_throw_spring.store(spring, std::memory_order_release);
                            wait = true;
                        } else if (!g_throw.done && ++g_throw.frames <= 3) {
                            wait = true;
                        }
                    }
                    if (!wait) {
                        thrown  = can_throw && g_throw.spring == spring && g_throw.done;
                        g_throw = Throw{}; // whatever happens next, nothing is left armed
                        g_throw_spring.store(0, std::memory_order_release);
                    }
                }
                if (wait) {
                    // Held on for the throw's step: the springs keep this hold's target meanwhile (not the game's own, which
                    // its "too far from the target" drop would measure against).
                    if (g_have_placed) {
                        auto* slot = reinterpret_cast<float*>(ctx.rbp + kTargetSlot);
                        slot[0]    = g_placed.x;
                        slot[1]    = g_placed.y;
                        slot[2]    = g_placed.z;
                    }
                    return;
                }
                g_release      = false;
                g_track_spring = 0;
                g_release_keep = false;
                g_pull         = Pull{};
                if (spring == 0) {
                    return; // already let go by the game
                }
                if (!CallPlayer(g_drop_fn, player)) {
                    spdlog::error("[Grab] dropping the object failed with an exception");
                    return;
                }
                g_spring = 0;
                {
                    std::scoped_lock debug{ g_debug_mutex };
                    g_debug = DebugHeldInfo{};
                }
                EndPresent();
                if (was_pulling || keep_speed) {
                    if (was_pulling) {
                        spdlog::info("[Grab] let go while it was flying to your hand: it flies on with its own speed");
                    }
                    return; // not the hand's speed: it keeps its own
                }
                if (thrown) {
                    const int throws = g_throws.fetch_add(1) + 1;
                    if (throws <= 20 || throws % 10 == 0) {
                        spdlog::info("[Grab] let go: thrown at {:.1f} m/s (the spring's last physics step gave it your hand's speed)", glm::length(g_throw_linear));
                    }
                    return;
                }
                if (!set_velocity) {
                    // It keeps the speed the hold gave it.
                    static bool told = false;
                    if (!told) {
                        told = true;
                        spdlog::info("[Grab] let go: the object keeps the speed it had in your hand (no throw step: the physics world's velocity function is "
                                     "not where the grab update keeps its world, and the spring's step could not be used)");
                    }
                    return;
                }
                alignas(16) float linear[4]{ g_throw_linear.x, g_throw_linear.y, g_throw_linear.z, 0.0f };
                alignas(16) float angular[4]{ g_throw_angular.x, g_throw_angular.y, g_throw_angular.z, 0.0f };
                if (!CallVelocity(function, bodies, body, linear, angular)) {
                    spdlog::error("[Grab] setting the thrown object's speed failed with an exception");
                    return;
                }
                const int throws = g_throws.fetch_add(1) + 1;
                if (throws <= 20 || throws % 10 == 0) {
                    spdlog::info("[Grab] let go: the object leaves your hand at {:.1f} m/s, spinning {:.1f} rad/s", glm::length(g_throw_linear),
                        glm::length(g_throw_angular));
                }
                return;
            }

            const int  hand = g_hand.load();
            glm::vec3  palm{};
            float      scale = 1.0f;
            if (spring == 0 || !PalmWorld(hand == 0, palm, scale)) {
                g_track_spring = 0;
                if (spring == 0) {
                    EndPresent(); // (a moment without tracking keeps drawing it where it was)
                }
                return;
            }
            // The play area as the hand is read (checked again once the place in the hand is worked out).
            glm::vec3  anchor_at_read{};
            const auto room_at_read = tracking::RoomRotation();
            const bool area_at_read = AnchorWorld(anchor_at_read) && room_at_read.has_value();
            auto*           target = reinterpret_cast<float*>(ctx.rbp + kTargetSlot);
            const glm::vec3 game_target{ target[0], target[1], target[2] };
            const int64_t   now = NowUs();
            if (spring != g_spring || now - g_last_us > 500'000) {
                // The same grab after a pause (a menu) still carries the spring we set; a new one has the game's values.
                float force = 0.0f;
                SafeRead(&force, spring + kSpringForce, sizeof(force));
                const bool resumed = spring == g_spring && force == g_written_force;
                if (!resumed) {
                    if (g_stats_pending && g_last_us != 0) {
                        LogHolding(g_last_us, true); // the previous hold, ended without a frame in between
                    }
                    g_spring      = spring;
                    g_have_base   = false;
                    g_have_placed = false;
                    {
                        std::scoped_lock track{ g_track_mutex };
                        g_throw = Throw{}; // a new grab: nothing of the last one's throw applies
                        g_throw_spring.store(0, std::memory_order_release);
                    }
                    g_support        = Support{}; // (fix170: a new grab: no second hand yet, nothing of the last one's seating)
                    g_support_hand   = -1;
                    g_seat_keep_tilt = false;
                    g_rebase    = false;
                    g_start_us  = now;
                    g_stats_us  = now;
                    g_stats_logs = 0;
                    g_stats_pending = true;
                    g_hold_total    = HoldStats{};
                    const int n = g_grabs.fetch_add(1) + 1;
                    if (n <= 20 || n % 10 == 0) {
                        spdlog::info("[Grab] grab {}: the held object goes to your {} hand, {:.2f} m from where the game holds it", n,
                            hand == 0 ? "left" : "right", glm::length(palm - game_target));
                    }
                    PlanStiffness(spring, n);
                    // The object the grab start just took (its box), if it is this grab's.
                    g_use_taken    = g_taken.valid && now - g_taken.us < 2'000'000;
                    g_held_object  = now - g_taken.us < 2'000'000 ? g_taken.object : 0;
                    g_turn         = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                    g_turn_us      = now;
                    g_need_contact = g_use_taken;
                    g_taken.valid  = false; // used once
                    if (g_use_taken) {
                        AskGroup(spring, g_taken.ref, g_taken.root); // it stops bumping into you from the next frame
                        AskKeepDrawn(spring, g_taken.ref, g_taken.root); // (fix160) its 3D kept following its body
                    }
                    // A pull the grips asked for a moment ago: this grab flies the object to the hand.
                    g_pull = Pull{};
                    if (g_pull_ask.pending && now - g_pull_ask.us < kPullAskUs) {
                        g_pull.active   = true;
                        g_pull.start_us = now;
                    }
                    g_pull_ask = PullAsk{};
                }
                g_have_anchor = false;
            }
            g_last_us = now;
            const float t       = std::clamp(static_cast<float>(now - g_start_us) / static_cast<float>(kRampUs), 0.0f, 1.0f);
            const float ease    = t * t * (3.0f - 2.0f * t);
            const float elastic = ApplyStiffness(spring, ease, g_pull.active);

            // Where the held spot goes: with the object's box, so that the box's point facing the palm rests against the
            // palm; without, the spot itself in the middle of the closed hand.
            glm::vec3 point = palm;
            bool      boxed = false;
            glm::mat3 in_hand_r{ 1.0f }; // the object's rotation and origin in the hand's frame (for drawing it there)
            glm::vec3 in_hand_t{ 0.0f };
            bool      have_in_hand = false;
            glm::vec3 held_world{ 0.0f }; // where the point the springs hold is now (with the box)
            bool      know_held = false;
            glm::vec3 held_object{ 0.0f }; // ...and in the object's own space
            bool      held_measured = false; // ...measured at the springs' first step (else the box's middle stands in)
            if (g_use_taken) {
                NodeXf    xf;
                GripFrame grip;
                // Seated (and turned on) from where it is drawn; physics' own pose for where the springs hold it now.
                NodeXf physics_xf;
                if (DrawnWorld(g_taken.root, xf) && HeldWorld(g_taken.root, physics_xf) && HandGripWorld(hand, grip)) {
                    const glm::vec3 surface = grip.surface;
                    const glm::vec3 facing  = grip.facing;
                    if (!g_need_contact && g_taken.seated && grip.drawn != g_taken.seated_drawn) {
                        // The body was shown or hidden while holding (or the drawn hand was first measured): seated again for
                        // the other way of finding the palm, keeping how it lies now (as when taken into the other hand).
                        g_need_contact = true;
                        g_rebase       = true;
                        static int told = 0;
                        if (++told <= 5) {
                            spdlog::info("[Grab] the held object is seated again: the palm is now {}",
                                grip.drawn ? "the drawn hand's" : "at the controller's grip point (the body is not shown)");
                        }
                    }
                    if (g_need_contact) {
                        // The hand's frame: the palm faces `facing`, the fingers point `forward`, `side` across the palm (the
                        // drawn hand's knuckle line when the body is shown).
                        const glm::vec3 forward = grip.forward;
                        const glm::vec3 side    = grip.side;
                        const bool      square  = GameFlow::gStore.internalSettings.grabAlign && !g_seat_keep_tilt; // (fix170: from both hands: as it lies)
                        g_seat_keep_tilt        = false;
                        g_turn                 = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                        g_turn_us              = now;
                        float     degrees      = 0.0f;
                        const char* how        = "keeps its tilt";
                        const char* along      = "";
                        bool      on_surface   = false; // the palm meets its real collision shape (else its box)
                        if (square) {
                            // Sat in the hand: the turn, then where the palm meets it in that orientation: centred in the palm
                            // (a little toward the fingers, so they close around it), across from your palm along a long
                            // object's length (which end you hold).
                            Seat seat;
                            g_turn              = SquareTurn(xf.r, g_taken, facing, forward, side, seat);
                            degrees             = glm::degrees(glm::angle(g_turn));
                            NodeXf turned       = xf;
                            turned.r            = glm::mat3_cast(g_turn) * xf.r;
                            const glm::vec3 lo  = g_taken.lo, hi = g_taken.hi;
                            const glm::vec3 mid = 0.5f * (lo + hi);
                            const glm::mat3 to_object = glm::transpose(turned.r);
                            const glm::vec3 n_o = to_object * facing;
                            const glm::vec3 f_o = to_object * forward;
                            glm::vec3       c   = mid;
                            if (seat.across >= 0) {
                                // Along its length: with your palm at the object (or when it is passed to the other hand),
                                // across from the palm, the end your hand is at; reaching for it or pulling it, at the middle
                                // of its length (where the pick happened to meet it is no choice of yours: a cup was held by
                                // its lid). Keeping the palm's width on the object.
                                const int       k      = seat.across;
                                const float     margin = std::min(0.5f * (hi[k] - lo[k]), kHalfPalm / xf.s);
                                const glm::vec3 p_obj  = ToNode(xf, surface);
                                const float     gap    = glm::length(glm::clamp(p_obj, lo, hi) - p_obj) * xf.s;
                                const bool      by_palm = g_taken.seated || gap < kNearPalm;
                                c[k]                   = std::clamp(by_palm ? p_obj[k] : mid[k], lo[k] + margin, hi[k] - margin);
                                along                  = by_palm ? ", along it across from your palm" : ", at the middle of its length";
                            }
                            if (g_taken.cylinder) {
                                // On the curved side facing the palm.
                                const int k     = g_taken.axis;
                                glm::vec3 n_cyl = n_o;
                                n_cyl[k]        = 0.0f;
                                n_cyl           = glm::length(n_cyl) > 1e-4f ? glm::normalize(n_cyl) : glm::vec3{ 0.0f };
                                c -= n_cyl * g_taken.radius;
                                how = "lies across your palm";
                            } else {
                                // On the side now flat on the palm.
                                if (seat.palm_axis < 0) {
                                    seat.palm_axis = 0;
                                    for (int i = 1; i < 3; ++i) {
                                        if (std::abs(n_o[i]) > std::abs(n_o[seat.palm_axis])) {
                                            seat.palm_axis = i;
                                        }
                                    }
                                    seat.palm_sign = n_o[seat.palm_axis] >= 0.0f ? 1.0f : -1.0f;
                                }
                                c[seat.palm_axis] = seat.palm_sign > 0.0f ? lo[seat.palm_axis] : hi[seat.palm_axis];
                                how = seat.across >= 0 ? "lies across your palm" : (g_taken.disc ? "lies flat on your palm" : "sits square on your palm");
                            }
                            // Its real surface (its collision shape) there, where the palm's facing meets it, not the box's side.
                            if (glm::vec3 on_shape{}; g_taken.hull.valid && HullContact(g_taken.hull, c, n_o, on_shape)) {
                                c          = on_shape;
                                on_surface = true;
                            }
                            // A little toward the fingers (the object moves that way when the contact moves the other); the drawn
                            // hand's grip point is at the base of the fingers already.
                            if (!grip.drawn) {
                                c -= f_o * (kGripAhead / xf.s);
                            }
                            g_taken.lift    = on_surface ? RestLiftHull(turned, g_taken.hull, c, facing, forward, grip.ahead)
                                              : g_taken.cylinder ? 0.0f
                                                                 : RestLift(turned, lo, hi, c, facing, forward, grip.ahead);
                            g_taken.contact = c;
                        } else {
                            // As picked up: the point of its box facing the palm rests on the palm; the object keeps its tilt,
                            // so it is raised until none of it is under the palm's and open fingers' surface.
                            const glm::vec3 p = ToNode(xf, surface);
                            const glm::vec3 n = glm::normalize(glm::transpose(xf.r) * facing);
                            glm::vec3       on_shape{};
                            on_surface        = g_taken.hull.valid && HullContact(g_taken.hull, p, n, on_shape);
                            g_taken.contact   = on_surface ? on_shape : ContactPoint(p, n, g_taken.lo, g_taken.hi);
                            g_taken.lift      = !Finite(g_taken.contact) ? 0.0f
                                                : on_surface ? RestLiftHull(xf, g_taken.hull, g_taken.contact, facing, forward, grip.ahead)
                                                             : RestLift(xf, g_taken.lo, g_taken.hi, g_taken.contact, facing, forward, grip.ahead);
                        }
                        g_taken.have_contact = Finite(g_taken.contact) && std::isfinite(g_taken.lift);
                        g_taken.seated       = true;
                        g_taken.seated_drawn = grip.drawn;
                        g_need_contact       = false;
                        const int grabs      = g_grabs.load();
                        if (grabs <= 20 || grabs % 10 == 0) {
                            const glm::vec3 size = (g_taken.hi - g_taken.lo) * xf.s;
                            spdlog::info("[Grab] grip: the {} ({:.2f} x {:.2f} x {:.2f} m) {} in your {} hand (turns {:.0f} degrees{}), the palm meets {} "
                                         "{:.2f} m from the middle of its box{}; held {}",
                                g_taken.cylinder ? "round object" : (g_taken.disc ? "flat round object" : "box"), size.x, size.y, size.z, how,
                                hand == 0 ? "left" : "right", degrees, along, on_surface ? "its real surface" : "its box",
                                glm::length(0.5f * (g_taken.lo + g_taken.hi) - g_taken.contact) * xf.s,
                                g_taken.lift > 0.001f ? fmt::format(", raised {:.1f} cm off the palm", g_taken.lift * 100.0f) : "",
                                grip.drawn ? "at the base of the drawn hand's fingers" : "at the controller's grip point (no body shown)");
                        }
                    }
                    // The point the springs hold: the spring maker (rva 1a51a70, +0xc0e..+0x10dc) always makes it the middle
                    // of the object's collision shape's own bounding box, wherever the pick touched it (the touched spot is
                    // only where the game would first have held it). Their first physics step, with the object still at
                    // rest, gives that point exactly; until then, or without it, the middle of the object's box stands in
                    // for it. A reading from a later step, of an object that was moving when it was grabbed (it would have
                    // moved on by then) or one off the object is not used.
                    const glm::vec3   middle   = 0.5f * (g_taken.lo + g_taken.hi);
                    const char* const stand_in = g_holds_middle ? "the middle of its box" : "the touched spot";
                    glm::vec3         held     = g_holds_middle ? middle : g_taken.touched;
                    bool              matched = false, first = false, still = true;
                    float             moved = -1.0f, turned = -1.0f;
                    glm::vec3         measured{ 0.0f };
                    {
                        std::scoped_lock track{ g_track_mutex };
                        if (g_capture.have && g_capture.serial == g_taken.serial) {
                            matched  = true;
                            measured = g_capture.held;
                            first    = g_capture.first;
                            still    = g_capture.still;
                            moved    = g_capture.moved;
                            turned   = g_capture.turned;
                        }
                    }
                    const bool measuring = static_cast<bool>(g_made_hook) && static_cast<bool>(g_point_hook);
                    const bool tell      = !g_taken.measure_told && (matched || (measuring && now - g_start_us > 1'000'000));
                    if (tell) {
                        g_taken.measure_told = true;
                    }
                    if (matched) {
                        const glm::vec3 pad    = 0.25f * (g_taken.hi - g_taken.lo) + glm::vec3{ 0.02f / xf.s };
                        const bool      on_box = glm::all(glm::greaterThanEqual(measured, g_taken.lo - pad)) &&
                                            glm::all(glm::lessThanEqual(measured, g_taken.hi + pad));
                        const bool use = on_box && first && still;
                        held_measured  = use;
                        if (use) {
                            held = measured;
                        }
                        if (tell) {
                            const int n = g_captures.fetch_add(1) + 1;
                            if (n <= 20 || n % 10 == 0) {
                                std::string why;
                                if (!on_box) {
                                    why = "; that is off the object, so ";
                                } else if (!first) {
                                    why = " (measured after the object had started moving); ";
                                } else if (!still) {
                                    why = fmt::format(" (it was {} when you grabbed it: {:.1f} cm and {:.1f} degrees in the frame before); ",
                                        moved >= kStillMove ? "moving" : "turning", moved * 100.0f, glm::degrees(turned));
                                }
                                spdlog::info("[Grab] the springs hold the object {:.2f} m from the middle of its box and {:.2f} m from the spot the pick "
                                             "touched{}{}",
                                    glm::length(measured - middle) * xf.s, glm::length(measured - g_taken.touched) * xf.s,
                                    use ? fmt::format(" (measured at their first physics step, the object {}); it is placed by that point",
                                              moved >= 0.0f ? fmt::format("at rest: {:.1f} mm and {:.2f} degrees in the frame before", moved * 1000.0f,
                                                                  glm::degrees(turned))
                                                            : std::string{ "not known to be moving" })
                                        : why,
                                    use ? std::string{} : fmt::format("{} is used", stand_in));
                            }
                        }
                    } else if (tell) {
                        spdlog::warn("[Grab] the springs' first physics step was not seen within a second; {} is used as the point they hold", stand_in);
                    }
                    held_world  = physics_xf.r * (physics_xf.s * held) + physics_xf.t;
                    know_held   = Finite(held_world);
                    held_object = held;
                    if (g_taken.have_contact && !g_need_contact) {
                        // Placed for the rotation the object is being turned to this frame (the rows given to the game, as
                        // OnRotation builds them, without its lead), not the one it was last drawn with: that one is a frame or
                        // two behind while the hand turns, and any wobble of it would move the held point too.
                        glm::mat3 place_r = xf.r;
                        glm::mat3 hand_r{ 1.0f };
                        if (g_have_base && g_have_object && !g_rebase && HandWorldRotation(hand == 0, hand_r)) {
                            const glm::mat3 r = hand_r * glm::transpose(g_hand0) * EasedTurn(now) * g_camera0 * g_rows_to_object;
                            if (Finite(r[0]) && Finite(r[1]) && Finite(r[2])) {
                                place_r = r;
                            }
                        }
                        // (fix170) Both hands on it: the first hand's grip stays exactly where its hand holds it (the pivot), and the
                        // object is turned by the shortest turn that brings the line from it to the second grip onto the line toward
                        // the second palm (ROCK's shared pose), eased in at the join; the roll stays the first hand's. The second
                        // grip where it then is: the body draws that hand there.
                        const glm::vec3 pivot    = surface + facing * g_taken.lift;
                        glm::mat3       shared_r = place_r;
                        if (g_support.active && g_support.hand == 1 - hand && g_support.serial == g_taken.serial) {
                            // (The frame before's turn is kept when the second palm is not read, or is brought within 0.10 m of the first
                            // grip: the line's direction would then follow the trackers' jitter and spin it; the review.)
                            glm::quat  turn = g_support.aims ? g_support.turn : glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                            GripFrame  second;
                            const bool read = HandGripWorld(g_support.hand, second);
                            if (read && g_support.aims) {
                                const glm::vec3 from = place_r * (xf.s * (g_support.grip - g_taken.contact));
                                const glm::vec3 to   = second.surface - pivot;
                                if (glm::length(from) > 1e-3f && glm::length(to) >= kSupportAimMin) {
                                    const float blend = std::clamp(static_cast<float>(now - g_support.join_us) / static_cast<float>(kSupportBlendUs), 0.0f, 1.0f);
                                    turn              = glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::rotation(glm::normalize(from), glm::normalize(to)),
                                                     blend * blend * (3.0f - 2.0f * blend));
                                }
                            }
                            shared_r                  = glm::mat3_cast(turn) * place_r;
                            g_support.grip_world      = pivot + shared_r * (xf.s * (g_support.grip - g_taken.contact));
                            g_support.have_grip_world = Finite(g_support.grip_world);
                            if (read) {
                                g_support.stretch = glm::length(second.surface - g_support.grip_world);
                            }
                            // For the body: the second grip in the first hand's frame (HandFrame), put back where that hand is as the body
                            // draws it (SupportGrip), as the held object is drawn in the hand: not a frame's walk behind it (the review).
                            glm::mat3 first_r{ 1.0f };
                            glm::vec3 first_t{ 0.0f };
                            if (g_support.have_grip_world && HandFrame(hand, first_r, first_t)) {
                                const glm::vec3  local = glm::transpose(first_r) * (g_support.grip_world - first_t);
                                std::scoped_lock draw{ g_support_draw_mutex };
                                g_support_draw       = local;
                                g_support_draw_first = hand;
                                g_support_draw_hand  = g_support.hand;
                                g_support_draw_us    = now;
                            }
                            g_support.turn       = turn;
                            g_support.turn_frame = g_frame.load(std::memory_order_relaxed);
                        }
                        const glm::vec3 placed = pivot + shared_r * (xf.s * (held - g_taken.contact));
                        if (Finite(placed)) {
                            point = placed;
                            boxed = true;
                            glm::mat3 hand_frame_r{ 1.0f };
                            glm::vec3 hand_frame_t{ 0.0f };
                            if (HandFrame(hand, hand_frame_r, hand_frame_t)) {
                                const glm::vec3 origin = placed - shared_r * (xf.s * held);
                                in_hand_r              = glm::transpose(hand_frame_r) * shared_r;
                                in_hand_t              = glm::transpose(hand_frame_r) * (origin - hand_frame_t);
                                have_in_hand           = Finite(in_hand_t) && Finite(in_hand_r[0]) && Finite(in_hand_r[1]) && Finite(in_hand_r[2]);
                            }
                        }
                    }
                }
            }

            // Pulled (gravity gloves): until it reaches the hand, the held point flies from where it was to its place in
            // the hand (where that is now) on an arc: s = time / flight time, the straight line at s plus g T^2 s (1 - s) / 2
            // upward (a thrown object's path, its top g T^2 / 8 above the line). The spring holds it at full strength, so
            // it keeps to the path; letting go of the grip on the way does not stop it (a squeeze catches it, else it drops at the hand).
            glm::vec3 pull_velocity{ 0.0f };
            float     pull_share = 0.0f; // how much of the hand's own motion the path carries now (s)
            bool      pulling    = false;
            if (g_pull.active) {
                if (!g_pull.have_from) {
                    glm::vec3 from      = held_world;
                    bool      have_from = know_held;
                    if (!have_from) {
                        float first[3]{}; // no box: the springs' first target, the game's own idea of where it is held
                        have_from = SafeRead(first, spring + kSpringTarget, sizeof(first));
                        from      = glm::vec3{ first[0], first[1], first[2] };
                        have_from = have_from && Finite(from);
                    }
                    if (have_from) {
                        g_pull.from      = from;
                        g_pull.have_from = true;
                        g_pull.distance  = glm::length(point - from);
                        g_pull.duration  = std::max(PullTime(g_pull.distance), g_pull.distance / kPullMaxSpeed);
                        const int n      = g_pulls.load();
                        if (n <= 20 || n % 10 == 0) {
                            spdlog::info("[Grab] pull: the object flies {:.1f} m to your {} hand in {:.2f} s", g_pull.distance, hand == 0 ? "left" : "right",
                                g_pull.duration);
                        }
                    } else {
                        g_pull.active = false;
                        spdlog::warn("[Grab] pull: where the object is could not be read; it is held as a normal grab");
                    }
                }
                if (g_pull.active) {
                    const float T = g_pull.duration;
                    const float s = std::clamp(static_cast<float>(now - g_pull.start_us) * 1e-6f / T, 0.0f, 1.0f);
                    if (s >= 1.0f) {
                        g_pull.active = false;
                        g_caught_hand = hand;
                        const int n   = g_pulls.load();
                        if (n <= 20 || n % 10 == 0) {
                            spdlog::info("[Grab] pull: it reached your hand after {:.2f} s{}", T,
                                know_held ? fmt::format(" ({:.2f} m from its place in the hand)", glm::length(held_world - point)) : std::string{});
                        }
                    } else {
                        const glm::vec3 up{ 0.0f, 0.0f, 1.0f };
                        const glm::vec3 delta = point - g_pull.from;
                        const float     rise  = 0.5f * kPullGravity * T * T;
                        const glm::vec3 arc   = g_pull.from + delta * s + up * (rise * s * (1.0f - s));
                        if (Finite(arc)) {
                            point         = arc;
                            pull_velocity = (delta + up * (rise * (1.0f - 2.0f * s))) / T;
                            pull_share    = s;
                            pulling       = true;
                        }
                    }
                }
            }

            // Pulled as the grip was let go, and now at the hand: it waits there a moment for a squeeze to catch it.
            if (!pulling && g_needs_catch.load(std::memory_order_relaxed) && g_catch_deadline_us.load(std::memory_order_relaxed) == 0) {
                g_catch_deadline_us = now + kCatchWindowUs;
            }

            // Where the object sits in the hand, for the scene writer to draw it there (see OnWriter). It stops being drawn there
            // when it flies, is not the grab's own box, or the setting is off (a frame whose place could not be worked out keeps
            // the one before). Only an object whose root's own collision object moves the held body is drawn in the hand (the
            // writer is called with that one for the root); found once a grab.
            {
                const bool  ends    = pulling || !g_use_taken || !GameFlow::gStore.internalSettings.grabInHand;
                const bool  in_hand = !ends && have_in_hand && g_writer_ready.load(std::memory_order_relaxed);
                bool        missing = false, no_collision = false;
                uint32_t    held_body = 0;
                if (ends) {
                    EndPresent();
                } else if (in_hand) {
                    std::scoped_lock present{ g_present_mutex };
                    auto& p = g_present;
                    if (p.root != g_taken.root || p.serial != g_taken.serial) {
                        // Another object or grab: it starts from where physics has it. The same object taken again (still fading
                        // out, its collision object the same) keeps how its node sits on its body and what is left of the
                        // difference, so it does not jump.
                        const uintptr_t collision =
                            spring != 0 && SafeRead(&held_body, spring + kSpringBody, sizeof(held_body)) ? HeldCollision(g_taken.root, held_body) : 0;
                        const Present before = p;
                        const bool    again  = before.root == g_taken.root && collision != 0 && before.collision == collision && before.offset_known &&
                                           !before.off;
                        p           = Present{};
                        p.root      = g_taken.root;
                        p.serial    = g_taken.serial;
                        p.collision = collision;
                        if (again) {
                            p.offset         = before.offset;
                            p.offset_known   = true;
                            p.diff_t         = before.diff_t;
                            p.diff_q         = before.diff_q;
                            p.physics_weight = before.physics_weight;
                            p.drawn_frame    = before.drawn_frame;
                            p.physics        = before.physics;
                            p.drawn          = before.drawn;
                        }
                        no_collision = p.collision == 0;
                    }
                    p.hand      = hand;
                    p.holding   = true;
                    p.frame     = g_frame.load(std::memory_order_relaxed);
                    p.in_hand_r = in_hand_r;
                    p.in_hand_t = in_hand_t;
                    // (Again each frame: a hold that ended for a moment and faded had them cleared.)
                    g_present_collision.store(p.collision, std::memory_order_release);
                    g_present_root.store(p.collision != 0 ? p.root : 0, std::memory_order_release);
                    if (p.collision != 0 && !p.off && p.drawn_frame == 0 && !p.told_missing && now - g_start_us > 1'500'000) {
                        p.told_missing = true;
                        missing        = true;
                    }
                }
                if (no_collision && g_present_fail_tells.fetch_add(1) < 20) {
                    spdlog::info("[Grab] the held object's body {:x} is not moved by its 3D root's own collision object alone (a part of it has the body, or "
                                 "it has several), so it is drawn where physics has it",
                        held_body);
                } else if (missing && g_present_fail_tells.fetch_add(1) < 20) {
                    spdlog::info("[Grab] the game's scene writer was not called for the held object in its first 1.5 s (its collision object's world "
                                 "update did not take the moving body's way), so it is drawn where physics has it");
                }
            }

            // The player's walking speed (for throws, and for the flight's path between frames).
            glm::vec3  anchor{};
            const auto room_now  = tracking::RoomRotation();
            const bool area_now  = AnchorWorld(anchor) && room_now.has_value();
            // The same play area as when the hand was read (else this frame is not measured: it moved in between).
            const bool have_area = area_now && area_at_read && glm::length(anchor - anchor_at_read) < 0.001f &&
                                   TurnCos(*room_now * glm::transpose(*room_at_read)) > 0.99999f;
            if (area_now) {
                if (g_have_anchor) {
                    const float dt = static_cast<float>(now - g_anchor_us) * 1e-6f;
                    if (dt > 0.004f && dt < 0.2f) {
                        glm::vec3 walk = (anchor - g_anchor_prev) / dt;
                        if (!Finite(walk) || glm::length(walk) > 30.0f) {
                            walk = glm::vec3{ 0.0f }; // a jump (loading, fast travel): not walking
                        }
                        g_walk += (walk - g_walk) * 0.5f;
                    }
                } else {
                    g_walk = glm::vec3{ 0.0f };
                }
                g_anchor_prev = anchor;
                g_anchor_us   = now;
                g_have_anchor = true;
            }
            static auto vr = VR::get();
            glm::vec3   hand_velocity{ 0.0f };
            if (room_now) {
                hand_velocity = *room_now * tracking::ToHavokVector(glm::vec3{ vr->get_velocity(static_cast<uint32_t>(ControllerIndex(hand))) }) * scale;
            }
            if (!Finite(hand_velocity) || glm::length(hand_velocity) > 30.0f) {
                hand_velocity = glm::vec3{ 0.0f };
            }
            glm::vec3 placed = point; // where the held point goes this frame
            // Your head (the rendered camera), for telling which way contacts push the held object.
            glm::vec3 body{ 0.0f };
            bool      have_body = false;
            if (auto root = CreationEngineSingletonManager::GetSceneGraphRoot(); root != nullptr && root->worldCamera != nullptr) {
                const auto& at = root->worldCamera->world.translate;
                body           = glm::vec3{ at.x, at.y, at.z };
                have_body      = Finite(body);
            }
            {
                std::scoped_lock track{ g_track_mutex };
                // Held (not flying): it goes where the body draws the hand this frame. The play area moves (you walk, turn)
                // between this update and the body drawing the hand from the same controller sample, so that move is added:
                // measured when the body drew the hand before this update; otherwise the move of the frame before (as it
                // was, or per physics step, whichever has foretold it better), as HIGGS moves held objects with the player's
                // own movement each frame. With no body shown, the play area's move between updates stands in. When drawn
                // objects show the frame before's physics, it is led one more frame of the hand's motion.
                const uint64_t frame = g_frame.load(std::memory_order_relaxed);
                g_read_before        = g_read;
                g_read               = FrameRead{};
                glm::vec3 move{ 0.0f }, step_move{ 0.0f }, lead{ 0.0f };
                bool      per_step = false;
                bool      led      = false; // led by FrameLead with the motion's parts
                if (!pulling && area_now) {
                    // (have_area false: the play area moved while this update ran; the frame is not measured, only foretold.)
                    const int key = ((g_use_taken ? g_taken.serial : 0) << 3) | (hand << 2) | (boxed ? 2 : 0) | (held_measured ? 1 : 0);
                    if (have_area) {
                        g_read = FrameRead{ frame, true, anchor, *room_now, point, key, now };
                    }
                    glm::vec3 drawn{};
                    if (have_area && g_draw.valid && g_draw.frame == frame) {
                        if (MeasureMove(drawn)) {
                            move = drawn - point;
                            lead = FrameLead();
                            led  = g_follow.shows == 2 && g_follow.have_parts;
                            ++g_follow_stats.exact;
                        }
                    } else if (g_follow.have_move && g_follow.move_steps >= 0 && frame - g_follow.move_frame <= 2) {
                        // Foretold from a frame whose hand was drawn after its grab update, like this one will be.
                        per_step  = g_follow.per_step && g_follow.move_steps > 0;
                        move      = g_follow.move;
                        step_move = per_step ? g_follow.move / static_cast<float>(std::min(g_follow.move_steps, kMaxStepsAhead)) : glm::vec3{ 0.0f };
                        if (g_follow.shows == 2 && g_follow.have_drawn_lead && g_follow.drawn_frame + 1 == frame && g_follow.drawn_key == key) {
                            lead = FrameLead(); // the hand's motion over the frame before
                            led  = g_follow.have_parts;
                        }
                    } else if (have_area && (!g_draw.valid || g_draw.frame + 30 < frame) && g_read_before.valid && g_read_before.frame + 1 == frame) {
                        move = anchor - g_read_before.anchor;
                        if (!Finite(move) || glm::length(move) > kFollowJump) {
                            move = glm::vec3{ 0.0f };
                        }
                        ++g_follow_stats.fallback;
                    }
                    placed = point + move + lead;
                    if (!Finite(placed)) {
                        move = step_move = lead = glm::vec3{ 0.0f };
                        per_step                = false;
                        led                     = false;
                        placed                  = point;
                    }
                    g_follow.led_frames[frame & 3]  = led ? frame : 0;
                    g_follow.led_elastic[frame & 3] = elastic;
                }
                g_track.palm      = point;
                g_track.body      = body;
                g_track.have_body = have_body;
                g_track.velocity  = pulling ? pull_velocity + (hand_velocity + g_walk) * pull_share : glm::vec3{ 0.0f };
                g_track.walk      = g_walk;
                g_track.move      = move;
                g_track.step_move = step_move;
                g_track.per_step  = per_step;
                g_track.lead      = lead;
                g_track.frame     = frame;
                g_track.steps     = 0;
                g_track.held      = held_object;
                g_track.have_held = know_held && g_use_taken;
                g_track.elastic   = elastic;
                g_track.us        = now;
                g_track.pulling   = pulling;
                g_in_flight       = pulling;
                g_track.box       = boxed;
                g_track.root      = g_taken.root;
                g_track.lo        = g_taken.lo;
                g_track.hi        = g_taken.hi;
                g_track.serial    = g_taken.serial;
                g_track.cylinder  = g_taken.cylinder;
                g_track.axis      = g_taken.axis;
                g_track.radius    = g_taken.radius;
            }
            g_track_spring = spring;
            target[0]      = placed.x;
            target[1]      = placed.y;
            target[2]      = placed.z;
            g_placed       = placed;
            g_have_placed  = true;

            // How the held object follows the hand (for tuning): a few lines each grab, then one now and then.
            ++g_updates;
            if (g_step_ready.load() && now - g_stats_us > (g_stats_logs < 6 ? 3'000'000 : 15'000'000)) {
                LogHolding(now, false);
            }
        }

        // Every physics step of the held spring: held, the target is where the body draws the hand this frame (see the grab
        // update); flying to the hand, the path moved on by how far it goes from when it was read to the step's end.
        void OnSpringStep(safetyhook::Context& ctx)
        {
            t_step_spring          = 0;
            const uintptr_t spring = ctx.rcx;
            // (fix170) An adopted object's throw step: its dt, for OnHeldPoint.
            if (spring != 0 && spring == g_adopt_throw_spring.load(std::memory_order_acquire)) {
                const float step_dt = ctx.xmm2.f32[0];
                if (step_dt > 0.0f && step_dt < 0.1f) {
                    t_step_spring = spring;
                    t_step_dt     = step_dt;
                }
                return;
            }
            if (spring == 0 || spring != g_track_spring.load(std::memory_order_acquire)) {
                return;
            }
            const float dt = ctx.xmm2.f32[0];
            if (!(dt > 0.0f && dt < 0.1f)) {
                return;
            }
            t_step_spring     = spring;
            t_step_dt         = dt;
            const int64_t now = NowUs();
            Track         track;
            glm::vec3     target{ 0.0f };
            int64_t       age  = 0;
            float         soft = 1.0f; // the elasticity cap while the object touches something
            // How far the object was off the target at the step before (kept by the spring): far off, something blocks it.
            const auto* error = reinterpret_cast<const float*>(spring + kSpringError);
            const float off   = std::sqrt(error[0] * error[0] + error[1] * error[1] + error[2] * error[2]);
            {
                std::scoped_lock _{ g_track_mutex };
                age = now - g_track.us;
                if (g_track.us == 0 || age < 0 || age > 200'000) {
                    return; // the grab update has stopped (paused): leave the spring as it is
                }
                const int step = ++g_track.steps;
                track          = g_track;
                if (track.pulling) {
                    target = track.palm + track.velocity * std::clamp(static_cast<float>(age) * 1e-6f + dt, 0.0f, kMaxLead);
                } else {
                    target = track.palm + (track.per_step ? track.step_move * static_cast<float>(std::min(step, kMaxStepsAhead)) : track.move) + track.lead;
                }
                if (!Finite(target)) {
                    return;
                }
                // Kept for telling whether drawn objects show this frame's physics or the frame before's.
                if (g_follow.target_frame != track.frame) {
                    g_follow.target_before       = g_follow.target;
                    g_follow.target_before_frame = g_follow.target_frame;
                    g_follow.target_frame        = track.frame;
                }
                g_follow.target = target;
                // Touching something (see OnHeldPoint): held more softly, eased in fast and out slowly (HIGGS).
                if (!track.pulling && g_contact.spring == spring) {
                    const float want = g_contact.contact_until >= g_contact.steps ? kContactElastic : 1.0f;
                    g_contact.soft += (want - g_contact.soft) * (want < g_contact.soft ? 0.5f : 0.1f);
                    soft = g_contact.soft;
                    if (soft < 0.95f) {
                        ++g_follow_stats.soft_steps;
                    }
                }
                if (!track.pulling && ((std::isfinite(off) && off > kBlockedError) || soft < 0.95f)) {
                    g_follow.disturbed_frame = track.frame;
                }
            }
            auto* out = reinterpret_cast<float*>(spring + kSpringTarget);
            out[0]    = target.x;
            out[1]    = target.y;
            out[2]    = target.z;

            // Something holds the object back (a wall, a heavier thing): push gently instead of at full strength. (Not while
            // it flies to the hand: it is then a step's travel off by design, and must keep to its path.)
            const bool blocked = !track.pulling && std::isfinite(off) && off > kBlockedError;
            if (track.elastic > 0.0f) {
                float elastic = blocked ? std::min(track.elastic, kBlockedElastic) : track.elastic;
                if (!track.pulling) {
                    elastic = std::min(elastic, soft);
                }
                *reinterpret_cast<float*>(spring + kSpringElastic) = elastic;
            }
            g_steps.fetch_add(1, std::memory_order_relaxed);
            if (blocked) {
                g_blocked_steps.fetch_add(1, std::memory_order_relaxed);
            }
            g_step_dt_us.fetch_add(static_cast<int64_t>(dt * 1e6f), std::memory_order_relaxed);
            g_step_age_us.fetch_add(age, std::memory_order_relaxed);
        }

        // Just before the target orientation is built: the camera rotation it starts from becomes the one at the
        // start of the grab, turned by how far the holding hand has turned since.
        void OnRotation(safetyhook::Context& ctx)
        {
            if (!IsPlayer(ctx.r14) || !Active()) {
                return;
            }
            auto* rows = reinterpret_cast<float*>(ctx.rbp + kRowsSlot); // three rows of 4 floats (w = 0)
            std::scoped_lock _{ g_mutex };
            if (g_spring == 0) {
                return;
            }
            // The holding hand, read once (under the lock, so a pass to the other hand cannot come in between).
            const int side = g_hand.load();
            glm::mat3 hand;
            if (!HandWorldRotation(side == 0, hand)) {
                return;
            }
            NodeXf object;
            const bool have_object = g_use_taken && DrawnWorld(g_taken.root, object); // (as you see it)
            NodeXf     physics_object;
            const bool have_physics_object = g_use_taken && HeldWorld(g_taken.root, physics_object); // (for the turn check)
            if (!g_have_base) {
                // The game copies the camera's stored rotation rows here (1a5363e..1a536e9), and a stored row is a column of
                // the column-vector rotation (as for every node: body::FromNi, NodeWorld): glm column r = stored row r.
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        g_camera0[r][c] = rows[r * 4 + c];
                    }
                }
                g_hand0       = hand;
                g_have_base   = true;
                g_rebase      = false;
                g_object0     = have_object ? object.r : glm::mat3{ 1.0f };
                g_have_object = have_object;
                g_turn_told   = false;
                // (The game keeps the object at this rotation relative to the rows for the whole grab.)
                g_rows_to_object = glm::transpose(g_camera0) * g_object0;
            } else if (g_rebase) {
                // Taken into the other hand: it keeps its orientation and turns with the new hand from here.
                g_camera0     = g_last_turned;
                g_hand0       = hand;
                g_rebase      = false;
                g_object0     = have_object ? object.r : glm::mat3{ 1.0f };
                g_have_object = have_object;
                g_turn_told   = false;
            }
            // The turn that sits it square in the hand, eased in (a world turn applied at the grab's start).
            const int64_t   now    = NowUs();
            const float     t_turn = std::clamp(static_cast<float>(now - g_turn_us) / static_cast<float>(kRampUs), 0.0f, 1.0f);
            const glm::mat3 turn   = EasedTurn(now);
            glm::mat3       turned = hand * glm::transpose(g_hand0) * turn * g_camera0;
            // (fix170) Both hands on it: turned toward the second hand as OnTarget worked out this frame (kept in the rows on a rebase,
            // so the first hand holds it as it lies when the second lets go).
            if (g_support.active && g_support.turn_frame == g_frame.load(std::memory_order_relaxed)) {
                turned = glm::mat3_cast(g_support.turn) * turned;
            }
            g_last_turned = turned;

            // The hand as read this frame and the frame before (the latest of each frame).
            const uint64_t frame = g_frame.load(std::memory_order_relaxed);
            const auto     room  = tracking::RoomRotation();
            if (g_turn_seen.frame != frame) {
                g_turn_seen_before = g_turn_seen;
            }
            g_turn_seen = TurnSeen{ room.has_value(), frame, side, g_spring, hand, room.value_or(glm::mat3{ 1.0f }) };
            const TurnSeen& before = g_turn_seen_before;
            const bool      known  = room.has_value() && before.valid && before.frame + 1 == frame && before.hand == side && before.spring == g_spring;
            const glm::mat3 hand_turn = hand * glm::transpose(before.hand_rot); // the hand's turn (world) over the last frame
            const bool      area_jump = known && TurnCos(*room * glm::transpose(before.room)) < kFollowJumpCos; // a snap turn

            // Drawn objects show the frame before's physics: like its place, its orientation is led one more frame of the
            // hand's turn, so the object as drawn turns with the hand as drawn (not a frame behind it). Not across a snap
            // turn, a change of hand or grab, or a turn too fast to be the hand's own (over 30 degrees in a frame).
            int  shows     = 0;
            bool in_flight = false;
            {
                std::scoped_lock track{ g_track_mutex };
                shows     = g_follow.shows;
                in_flight = g_track.pulling;
            }
            glm::mat3 given = turned;
            if (shows == 2 && !in_flight && known && !area_jump && TurnCos(hand_turn) >= kLeadTurnCos) {
                given = hand_turn * turned;
                std::scoped_lock track{ g_track_mutex };
                ++g_follow_stats.rot_led;
            }
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    rows[r * 4 + c] = given[r][c]; // stored row r = column r
                }
                rows[r * 4 + 3] = 0.0f;
            }
            g_last_given = given; // (fix170: for an adopted spring's own turn)
            g_have_given = true;
            // For the log: the orientation the hand holds it in this frame (once the turn into the hand is done), and how fast
            // the hand turns.
            {
                std::scoped_lock track{ g_track_mutex };
                g_follow.have_want  = g_have_object && t_turn >= 1.0f;
                g_follow.want       = hand * glm::transpose(g_hand0) * glm::mat3_cast(g_turn) * g_object0;
                g_follow.want_frame = frame;
                g_follow.hand_turn  = known && !area_jump ? glm::degrees(std::acos(TurnCos(hand_turn))) : -1.0f;
            }

            // Once a grab has turned in: how far the object is from the orientation the hand should hold it in (the hand's
            // turn since the grab, and the turn into the hand, applied to how it lay then). A few degrees: it follows.
            if (!g_turn_told && g_have_object && have_physics_object && now - g_turn_us > 600'000) {
                g_turn_told           = true;
                const glm::mat3 want  = hand * glm::transpose(g_hand0) * glm::mat3_cast(g_turn) * g_object0;
                const glm::mat3 delta = physics_object.r * glm::transpose(want);
                const float     cos_a = std::clamp(0.5f * (delta[0][0] + delta[1][1] + delta[2][2] - 1.0f), -1.0f, 1.0f);
                const int       n     = g_turn_checks.fetch_add(1) + 1;
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] turn check: the held object is {:.0f} degrees from the orientation your hand holds it in (planned turn {:.0f} "
                                 "degrees, your hand turned {:.0f} degrees since the grab)",
                        glm::degrees(std::acos(cos_a)), glm::degrees(glm::angle(g_turn)),
                        glm::degrees(std::acos(std::clamp(0.5f * (glm::dot(hand[0], g_hand0[0]) + glm::dot(hand[1], g_hand0[1]) +
                                                                     glm::dot(hand[2], g_hand0[2]) - 1.0f),
                            -1.0f, 1.0f))));
                }
            }
        }

        // ---- Reading the held object's collision shape ----
        // Its 3D's collision object -> its physics system's body -> the body's record: its transform at +0 (rotation, then its
        // place at +0x30), its box in the world at +0x60, its shape at +0x78. A single convex shape (hknpConvexShape, as
        // Starfield's own Havok type data lays it out; the Starfield mesh exporter "MeshConverter" reads the same): its convex
        // radius at +0x20, its hull at +0x3c: corners hkRelArray<hkFloat3> (offset from the array itself, count), then its
        // faces' planes hkRelArray<hkVector4> at +0x44. Nothing is used unless it checks out against the body's own box in the
        // world: every corner inside it and filling it, every face with the corners on its inside.
        uintptr_t        g_convex_vtable{ 0 };
        uintptr_t        g_convex_family[4]{}; // hknpConvexShape and the shapes that are one (box, capsule, sphere)
        uintptr_t        g_scaled_vtable{ 0 };  // hknpScaledConvexShape: a convex shape scaled (a placed object resized)

        bool IsConvexTable(uintptr_t table)
        {
            for (const uintptr_t t : g_convex_family) {
                if (t != 0 && t == table) {
                    return true;
                }
            }
            return false;
        }
        std::atomic<int> g_hulls_told{ 0 };
        constexpr uintptr_t kRecordShape   = 0x78;
        constexpr uintptr_t kShapeRadius   = 0x20;
        constexpr uintptr_t kShapeCorners  = 0x3c;
        constexpr uintptr_t kShapePlanes   = 0x44;

        // The type of a C++ object with virtual functions (MSVC: its type table's object locator, then the type descriptor's
        // name), without ".?AV" and "@@"; "" when unknown.
        std::string TypeNameOf(uintptr_t object)
        {
            uintptr_t table = 0, locator = 0;
            int32_t   col[6]{};
            if (object == 0 || !SafeRead(&table, object, sizeof(table)) || !InImage(table) || !SafeRead(&locator, table - 8, sizeof(locator)) ||
                !InImage(locator) || !SafeRead(col, locator, sizeof(col)) || col[0] != 1 || col[3] <= 0) {
                return {};
            }
            char name[96]{};
            if (!SafeRead(name, Base() + static_cast<uint32_t>(col[3]) + 0x10, sizeof(name) - 1)) {
                return {};
            }
            std::string s{ name };
            if (s.rfind(".?AV", 0) == 0) {
                s = s.substr(4);
            }
            if (const auto at = s.find("@@"); at != std::string::npos) {
                s = s.substr(0, at);
            }
            return s;
        }

        // For the log (finding out how a shape that is not one convex shape is made, to hold it by it later): its type, and for
        // a compound shape (as the Starfield character controller's in the physics probe's dumps: its pieces hkFreeListArray
        // at +0x40, their count at +0x48, its box at +0x70 / +0x80) the layout of its pieces: the spacing at which each holds
        // a pointer to a shape at the same place (as Fallout 4's hknpShapeInstance: 0x80 bytes, transform at +0, scale at
        // +0x40, shape at +0x50, ROCK), each piece's type, place and, for a convex piece, its corners' box.
        std::string DescribeShape(uintptr_t shape)
        {
            const std::string type = TypeNameOf(shape);
            if (type.find("Compound") == std::string::npos) {
                return fmt::format("a {}", type.empty() ? std::string{ "shape of unknown type" } : type);
            }
            uint8_t head[0x90]{};
            if (!SafeRead(head, shape, sizeof(head))) {
                return fmt::format("a {} (not readable)", type);
            }
            uintptr_t data = 0;
            int32_t   count = 0;
            float     box[8]{};
            std::memcpy(&data, head + 0x40, sizeof(data));
            std::memcpy(&count, head + 0x48, sizeof(count));
            std::memcpy(box, head + 0x70, sizeof(box));
            std::string out = fmt::format("a {}: {} pieces, its box ({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f})", type, count, box[0], box[1],
                box[2], box[4], box[5], box[6]);
            if (count < 1 || count > 64 || data == 0) {
                return out;
            }
            for (const int stride : { 0x80, 0x60, 0x70, 0x90, 0xa0, 0xb0, 0xc0, 0x100 }) {
                std::vector<uint8_t> raw(static_cast<size_t>(count) * stride);
                if (!SafeRead(raw.data(), data, raw.size())) {
                    continue;
                }
                for (int at = 0; at < stride; at += 8) {
                    std::vector<std::string> types;
                    for (int i = 0; i < count; ++i) {
                        uintptr_t p = 0;
                        std::memcpy(&p, raw.data() + static_cast<size_t>(i) * stride + at, sizeof(p));
                        const std::string t = p != 0 && p != shape ? TypeNameOf(p) : std::string{};
                        if (t.rfind("hknp", 0) != 0 || t.find("Shape") == std::string::npos) {
                            break;
                        }
                        types.push_back(t);
                    }
                    if (static_cast<int>(types.size()) != count) {
                        continue;
                    }
                    out += fmt::format("; pieces {:#x} bytes apart, their shapes at +{:#x}:", stride, at);
                    for (int i = 0; i < count && i < 8; ++i) {
                        const uint8_t* e = raw.data() + static_cast<size_t>(i) * stride;
                        float          f[24]{};
                        std::memcpy(f, e, std::min<size_t>(sizeof(f), static_cast<size_t>(stride)));
                        const glm::mat3 r{ glm::vec3{ f[0], f[1], f[2] }, glm::vec3{ f[4], f[5], f[6] }, glm::vec3{ f[8], f[9], f[10] } };
                        uintptr_t       p = 0;
                        std::memcpy(&p, e + at, sizeof(p));
                        std::string corners;
                        uint8_t     ph[0x48]{};
                        int32_t     c_at = 0, c_n = 0;
                        if (types[i].find("Compound") == std::string::npos && SafeRead(ph, p, sizeof(ph))) {
                            std::memcpy(&c_at, ph + kShapeCorners, sizeof(c_at));
                            std::memcpy(&c_n, ph + kShapeCorners + 4, sizeof(c_n));
                            std::vector<float> v(static_cast<size_t>(std::clamp(c_n, 0, 1024)) * 3);
                            if (c_n >= 1 && c_n <= 1024 && std::abs(c_at) <= (1 << 20) &&
                                SafeRead(v.data(), p + kShapeCorners + static_cast<intptr_t>(c_at), v.size() * sizeof(float))) {
                                glm::vec3 lo{ std::numeric_limits<float>::max() }, hi{ -std::numeric_limits<float>::max() };
                                for (int k = 0; k < c_n; ++k) {
                                    const glm::vec3 q{ v[k * 3], v[k * 3 + 1], v[k * 3 + 2] };
                                    lo = glm::min(lo, q);
                                    hi = glm::max(hi, q);
                                }
                                corners = fmt::format(", {} corners from ({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f})", c_n, lo.x, lo.y, lo.z, hi.x,
                                    hi.y, hi.z);
                            }
                        }
                        out += fmt::format(" [{}: {}, place ({:.2f}, {:.2f}, {:.2f}), turn {}, then ({:.2f}, {:.2f}, {:.2f}, {:.2f}){}]", i, types[i], f[12],
                            f[13], f[14], Rigid(r) ? "rigid" : "not rigid", f[16], f[17], f[18], f[19], corners);
                    }
                    return out;
                }
            }
            // Not recognised: the first piece's words, to work it out from.
            uint32_t words[32]{};
            if (SafeRead(words, data, sizeof(words))) {
                out += "; pieces' layout not recognised; the first 0x80 bytes:";
                for (const uint32_t w : words) {
                    out += fmt::format(" {:08x}", w);
                }
            }
            return out;
        }

        bool ReadHull(uintptr_t root, const NodeXf& xf, Hull& out, std::string& why)
        {
            out = Hull{};
            if (g_convex_vtable == 0) {
                why = "the game's convex shape type was not found";
                return false;
            }
            uintptr_t collision = 0, system = 0, instance = 0, ids = 0, world18 = 0;
            int32_t   index = -1;
            uint32_t  count = 0, body = 0;
            if (!SafeRead(&collision, root + kNodeCollision, sizeof(collision)) || collision == 0 ||
                !SafeRead(&system, collision + kCollisionSystem, sizeof(system)) || !SafeRead(&index, collision + kCollisionIndex, sizeof(index)) ||
                !SafeRead(&instance, system + kSystemInstance, sizeof(instance)) || !SafeRead(&count, instance + kInstanceCount, sizeof(count)) ||
                !SafeRead(&ids, instance + kInstanceIds, sizeof(ids)) || index < 0 || static_cast<uint32_t>(index) >= count ||
                !SafeRead(&body, ids + static_cast<uintptr_t>(index) * sizeof(uint32_t), sizeof(body)) ||
                !SafeRead(&world18, instance + kInstanceWorld, sizeof(world18)) || world18 < kWorldFromInstance) {
                why = "its physics body was not found from its 3D";
                return false;
            }
            BodyRef b;
            if (!ResolveBodyIn(world18 - kWorldFromInstance, body, b) || b.collision != collision) {
                why = "its physics body's record did not check out";
                return false;
            }
            if (b.bodies != 1) {
                why = fmt::format("it is made of {} physics bodies", b.bodies);
                return false;
            }
            float     rec[32]{}; // the record's first 0x80 bytes
            uintptr_t shape = 0, table = 0;
            if (!SafeRead(rec, b.record, sizeof(rec))) {
                why = "its physics body could not be read";
                return false;
            }
            std::memcpy(&shape, reinterpret_cast<const uint8_t*>(rec) + kRecordShape, sizeof(shape));
            if (shape == 0 || !SafeRead(&table, shape, sizeof(table))) {
                why = "its shape could not be read";
                return false;
            }
            // Where its corners are: in the shape itself when it is a convex shape (a box, a capsule or a sphere is one too, as
            // Starfield's type data has them: hknpBoxShape and hknpCapsuleShape derive from hknpConvexShape), or in the convex
            // shape a scaled shape wraps (a placed object resized in the world). Havok's hknpScaledConvexShape holds its child
            // shape, the child's size, a scale and a shift applied after it (corner' = scale * corner + shift), after the shape
            // header; their exact places are not in the published type data, so the places that fit that layout are tried, and
            // the one whose corners fill the body's box is taken.
            struct Source {
                uintptr_t convex{ 0 };
                glm::vec3 scale{ 1.0f };
                glm::vec3 shift{ 0.0f };
                int       child_at{ -1 }, scale_at{ -1 }, shift_at{ -1 };
            };
            std::vector<Source> sources;
            const bool          scaled = g_scaled_vtable != 0 && table == g_scaled_vtable;
            if (IsConvexTable(table)) {
                sources.push_back(Source{ shape });
            } else if (scaled) {
                uint8_t sh[0x90]{};
                if (SafeRead(sh, shape, sizeof(sh))) {
                    for (int at = 0x38; at <= 0x58; at += 8) {
                        uintptr_t child = 0, child_table = 0;
                        std::memcpy(&child, sh + at, sizeof(child));
                        if (child == 0 || child == shape || !SafeRead(&child_table, child, sizeof(child_table)) || !IsConvexTable(child_table)) {
                            continue;
                        }
                        for (int s_at = (at + 8 + 15) & ~15; s_at + 12 <= static_cast<int>(sizeof(sh)); s_at += 16) {
                            glm::vec3 scale{};
                            std::memcpy(&scale, sh + s_at, sizeof(scale));
                            if (!Finite(scale) || glm::any(glm::lessThan(scale, glm::vec3{ 0.01f })) || glm::any(glm::greaterThan(scale, glm::vec3{ 100.0f }))) {
                                continue;
                            }
                            sources.push_back(Source{ child, scale, glm::vec3{ 0.0f }, at, s_at, -1 });
                            if (s_at + 16 + 12 <= static_cast<int>(sizeof(sh))) {
                                glm::vec3 shift{};
                                std::memcpy(&shift, sh + s_at + 16, sizeof(shift));
                                if (Finite(shift) && glm::length(shift) < 10.0f) {
                                    sources.push_back(Source{ child, scale, shift, at, s_at, s_at + 16 });
                                }
                            }
                        }
                    }
                }
                if (sources.empty()) {
                    why = "its collision is a scaled shape whose convex shape and scale were not found";
                    return false;
                }
            } else {
                why = fmt::format("its collision is not a convex shape: it is {}", DescribeShape(shape));
                return false;
            }
            // The body in the world: its rotation (columns at +0, +0x10, +0x20), place and box.
            const glm::vec3 c0{ rec[0], rec[1], rec[2] }, c1{ rec[4], rec[5], rec[6] }, c2{ rec[8], rec[9], rec[10] };
            const glm::vec3 place{ rec[12], rec[13], rec[14] };
            const glm::vec3 box_a{ rec[24], rec[25], rec[26] }, box_b{ rec[28], rec[29], rec[27] };
            const glm::vec3 wlo = glm::min(box_a, box_b), whi = glm::max(box_a, box_b);
            const glm::mat3 cols{ c0, c1, c2 };
            if (!Finite(c0) || !Finite(c1) || !Finite(c2) || !Finite(place) || !Finite(wlo) || !Finite(whi) || !Rigid(cols) ||
                glm::length(whi - wlo) > 20.0f) {
                why = "its physics body's place or box looks wrong";
                return false;
            }
            // The body's turn as Havok stores it (columns) or the other way (rows): the one that matches where the object's 3D is
            // turned (they are the same transform for a one-body object); the body must also be where the 3D is (an object
            // still moving between them is not read).
            const glm::mat3  rows      = glm::transpose(cols);
            const glm::mat3* turn      = nullptr;
            float            turn_off  = 180.0f;
            for (const glm::mat3* m : { &cols, &rows }) {
                const float off = glm::degrees(std::acos(TurnCos(glm::transpose(xf.r) * *m)));
                if (off < turn_off) {
                    turn_off = off;
                    turn     = m;
                }
            }
            const float place_off = glm::length(place - xf.t);
            if (!turn || !(turn_off < 5.0f) || !(place_off < 0.1f + 0.05f * glm::length(whi - wlo))) {
                why = fmt::format("its physics body is not where its 3D is ({:.0f} degrees, {:.2f} m apart: still moving?)", turn_off, place_off);
                return false;
            }
            // Its corners: hkFloat3 (12 bytes: Starfield's type data); 16 (hkVector4, older Havok) as a fallback. They must sit in
            // the body's box in the world (Havok keeps that box a few centimetres bigger than the shape, the same on every side):
            // none outside it, none far inside; of all the ways tried, the one whose margins are the most even.
            const glm::vec3        we          = whi - wlo;
            std::vector<glm::vec3> local;       // the corners, scaled as the body has them (the shape's space)
            std::vector<glm::vec3> child_local; // ...and as the convex shape has them (for its faces)
            const Source*          used        = nullptr;
            int                    stride      = 0;
            float                  radius      = 0.0f;
            int32_t                corner_count = 0, plane_at = 0, plane_count = 0;
            float                  best_spread = std::numeric_limits<float>::max();
            for (const Source& src : sources) {
                uint8_t head[0x4c]{};
                if (!SafeRead(head, src.convex, sizeof(head))) {
                    continue;
                }
                float   r = 0.0f;
                int32_t c_at = 0, c_n = 0, p_at = 0, p_n = 0;
                std::memcpy(&r, head + kShapeRadius, sizeof(r));
                std::memcpy(&c_at, head + kShapeCorners, sizeof(c_at));
                std::memcpy(&c_n, head + kShapeCorners + 4, sizeof(c_n));
                std::memcpy(&p_at, head + kShapePlanes, sizeof(p_at));
                std::memcpy(&p_n, head + kShapePlanes + 4, sizeof(p_n));
                if (!(r >= 0.0f && r < 0.5f) || c_n < 1 || c_n > 1024 || std::abs(c_at) > (1 << 20)) {
                    continue;
                }
                const float out_tol = r + 0.01f + 0.02f * std::max({ we.x, we.y, we.z });
                const float in_tol  = r + 0.15f;
                for (const int st : { 12, 16 }) {
                    std::vector<float> raw(static_cast<size_t>(c_n) * st / 4);
                    if (!SafeRead(raw.data(), src.convex + kShapeCorners + static_cast<intptr_t>(c_at), raw.size() * sizeof(float))) {
                        continue;
                    }
                    std::vector<glm::vec3> these, child;
                    bool                   finite = true;
                    glm::vec3              lo{ std::numeric_limits<float>::max() }, hi{ -std::numeric_limits<float>::max() };
                    for (int i = 0; i < c_n; ++i) {
                        const float*    f = raw.data() + static_cast<size_t>(i) * st / 4;
                        const glm::vec3 v{ f[0], f[1], f[2] };
                        const glm::vec3 sv = src.scale * v + src.shift;
                        finite             = finite && Finite(v) && glm::length(sv) < 50.0f;
                        child.push_back(v);
                        these.push_back(sv);
                        const glm::vec3 w = *turn * sv + place;
                        lo                = glm::min(lo, w);
                        hi                = glm::max(hi, w);
                    }
                    if (!finite) {
                        continue;
                    }
                    const glm::vec3 margin_lo = lo - wlo, margin_hi = whi - hi;
                    const float least = std::min(glm::min(margin_lo.x, glm::min(margin_lo.y, margin_lo.z)), glm::min(margin_hi.x, glm::min(margin_hi.y, margin_hi.z)));
                    const float most  = std::max(glm::max(margin_lo.x, glm::max(margin_lo.y, margin_lo.z)), glm::max(margin_hi.x, glm::max(margin_hi.y, margin_hi.z)));
                    if (least < -out_tol || most > in_tol || most - least >= best_spread) {
                        continue;
                    }
                    best_spread  = most - least;
                    used         = &src;
                    stride       = st;
                    radius       = r;
                    corner_count = c_n;
                    plane_at     = p_at;
                    plane_count  = p_n;
                    local        = std::move(these);
                    child_local  = std::move(child);
                }
            }
            if (!used) {
                why = fmt::format("its shape's corners do not fit its physics body's box ({}{} way(s) tried)", scaled ? "scaled; " : "", sources.size() * 2);
                return false;
            }
            // Its faces (on the convex shape): every corner on the inside of each (n.v + w <= 0) and some on it; the other sign if
            // that is how they are. Then scaled as its corners are (a plane's normal by the inverse scale).
            std::vector<glm::vec4> faces;
            if (plane_count >= 4 && plane_count <= 2048 && std::abs(plane_at) <= (1 << 20)) {
                std::vector<float> raw(static_cast<size_t>(plane_count) * 4);
                if (SafeRead(raw.data(), used->convex + kShapePlanes + static_cast<intptr_t>(plane_at), raw.size() * sizeof(float))) {
                    glm::vec3 llo{ std::numeric_limits<float>::max() }, lhi{ -std::numeric_limits<float>::max() };
                    for (const glm::vec3& v : child_local) {
                        llo = glm::min(llo, v);
                        lhi = glm::max(lhi, v);
                    }
                    const float on  = 0.003f + 0.01f * glm::length(lhi - llo);
                    int         outward = 0, inward = 0;
                    for (int i = 0; i < plane_count; ++i) {
                        const glm::vec3 n{ raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2] };
                        const float     w = raw[i * 4 + 3];
                        if (!Finite(n) || !std::isfinite(w) || std::abs(glm::length(n) - 1.0f) > 0.03f) {
                            break;
                        }
                        float most = -std::numeric_limits<float>::max(), least = std::numeric_limits<float>::max();
                        for (const glm::vec3& v : child_local) {
                            const float e = glm::dot(n, v) + w;
                            most          = std::max(most, e);
                            least         = std::min(least, e);
                        }
                        outward += most <= on && most >= -on ? 1 : 0;
                        inward += least >= -on && least <= on ? 1 : 0;
                    }
                    const float sign = outward == plane_count ? 1.0f : (inward == plane_count ? -1.0f : 0.0f);
                    if (sign != 0.0f) {
                        for (int i = 0; i < plane_count; ++i) {
                            const glm::vec3 n = sign * glm::vec3{ raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2] };
                            const float     w = sign * raw[i * 4 + 3];
                            const glm::vec3 n_scaled = glm::normalize(n / used->scale);
                            const glm::vec3 on_plane = used->scale * (-w * n) + used->shift;
                            faces.emplace_back(n_scaled, -glm::dot(n_scaled, on_plane));
                        }
                    }
                }
            }
            // Into the 3D's own space (the box's), the faces with the convex radius out.
            const glm::mat3 to_node = glm::transpose(xf.r);
            auto node_point = [&](const glm::vec3& v) { return to_node * ((*turn * v + place) - xf.t) / xf.s; };
            out.radius      = radius / xf.s;
            out.lo          = glm::vec3{ std::numeric_limits<float>::max() };
            out.hi          = glm::vec3{ -std::numeric_limits<float>::max() };
            for (const glm::vec3& v : local) {
                const glm::vec3 p = node_point(v);
                out.corners.push_back(p);
                out.lo = glm::min(out.lo, p);
                out.hi = glm::max(out.hi, p);
            }
            out.lo -= glm::vec3{ out.radius };
            out.hi += glm::vec3{ out.radius };
            for (const glm::vec4& f : faces) {
                const glm::vec3 n_node = glm::normalize(to_node * (*turn * glm::vec3{ f }));
                const glm::vec3 p_node = node_point(-f.w * glm::vec3{ f });
                out.planes.emplace_back(n_node, -glm::dot(n_node, p_node) - out.radius);
            }
            out.valid = Finite(out.lo) && Finite(out.hi);
            if (!out.valid) {
                why = "its shape did not turn into the object's space";
                return false;
            }
            const int n = g_hulls_told.fetch_add(1) + 1;
            if (n <= 10) {
                const glm::vec3 size = (out.hi - out.lo) * xf.s;
                spdlog::info("[Grab] its collision shape: {} corners, {} faces{} (convex radius {:.3f} m), {:.2f} x {:.2f} x {:.2f} m (corners {} bytes "
                             "apart, turned by the rotation's {}{})",
                    corner_count, out.planes.size(), faces.empty() && plane_count > 0 ? fmt::format(" (its {} planes did not check out)", plane_count) : "",
                    radius, size.x, size.y, size.z, stride, turn == &cols ? "columns" : "rows",
                    used->child_at < 0 ? std::string{}
                                       : fmt::format("; a scaled shape: its convex shape at +{:#x}, scale ({:.3f}, {:.3f}, {:.3f}) at +{:#x}, shift {}",
                                             used->child_at, used->scale.x, used->scale.y, used->scale.z, used->scale_at,
                                             used->shift_at < 0 ? std::string{ "none" }
                                                                : fmt::format("({:.3f}, {:.3f}, {:.3f}) at +{:#x}", used->shift.x, used->shift.y,
                                                                      used->shift.z, used->shift_at)));
            }
            return true;
        }

        // Round or square, from the shape (as from the bounding sphere): the box's half diagonal over how far the corners
        // inside the box reach from its middle (a box's corners fill it: about 1; a cylinder's do not). -1: too few corners.
        float HullFit(const Taken& taken)
        {
            // The corners inside the box (its part of the shape), their own box and how far they reach from its middle: a box
            // shape's corners are its box's (exactly 1), a round one's are not.
            const glm::vec3 pad = glm::vec3{ taken.hull.radius + 0.01f };
            glm::vec3       lo{ std::numeric_limits<float>::max() }, hi{ -std::numeric_limits<float>::max() };
            std::vector<glm::vec3> in;
            for (const glm::vec3& v : taken.hull.corners) {
                if (glm::all(glm::greaterThanEqual(v, taken.lo - pad)) && glm::all(glm::lessThanEqual(v, taken.hi + pad))) {
                    const glm::vec3 c = glm::clamp(v, taken.lo, taken.hi);
                    in.push_back(c);
                    lo = glm::min(lo, c);
                    hi = glm::max(hi, c);
                }
            }
            if (in.size() < 4) {
                return -1.0f;
            }
            const glm::vec3 mid   = 0.5f * (lo + hi);
            float           reach = 0.0f;
            for (const glm::vec3& c : in) {
                reach = std::max(reach, glm::length(c - mid));
            }
            return reach > 1e-4f ? 0.5f * glm::length(hi - lo) / reach : -1.0f;
        }

        // In the game's grab start, just before it makes the springs: which object is taken, its box, and the spot the
        // pick touched (the springs' first target; both in the object's own space).
        void OnSprings(safetyhook::Context& ctx)
        {
            Taken     taken{};
            uintptr_t ref = 0, base = 0, loaded = 0, root = 0;
            float     touched[3]{}, sphere[4]{};
            float     record[16]{}; // the base record from +0x40, where the bounds are
            NodeXf    xf;
            taken.us = NowUs();
            const bool read = SafeRead(&ref, ctx.rsp + kTakenRefSlot, sizeof(ref)) && ref != 0 && SafeRead(touched, ctx.rsp + kTouchedSlot, sizeof(touched)) &&
                              SafeRead(&base, ref + kRefBase, sizeof(base)) && base != 0 && SafeRead(record, base + kRecordFrom, sizeof(record)) &&
                              SafeRead(&loaded, ref + kRefLoaded, sizeof(loaded)) && loaded != 0 && SafeRead(&root, loaded + kLoaded3D, sizeof(root)) &&
                              root != 0 && NodeWorld(root, xf) && SafeRead(sphere, root + kNodeBound, sizeof(sphere));
            const glm::vec3 spot{ touched[0], touched[1], touched[2] };
            taken.object = ref; // (0 if it could not be read)
            // (fix172) A person (a corpse, or someone knocked down): held as a body by the bone the hand touched, not as a box (Physical).
            const bool body = Physical::OnGrabTaken(ref, t_hand_start);
            // The bounds: at +0x50 by the class sizes of CommonLibSF (its +0x48 comment is older); +0x48 as a fallback.
            auto bounds_at = [&](uintptr_t offset, glm::vec3& lo, glm::vec3& hi) {
                const float* f = record + (offset - kRecordFrom) / sizeof(float);
                lo             = { f[0], f[1], f[2] };
                hi             = { f[3], f[4], f[5] };
                const glm::vec3 size = hi - lo;
                return Finite(lo) && Finite(hi) && size.x >= 0.0f && size.y >= 0.0f && size.z >= 0.0f && glm::length(size) > 1e-3f &&
                       glm::length(size) < 1e4f;
            };
            glm::vec3   lo{ 0.0f }, hi{ 0.0f };
            uintptr_t   used = 0;
            const char* why  = nullptr;
            if (!read) {
                why = "the object or its 3D could not be read";
            } else if (body) {
                why = "(fix172) a person: held as a body, by the bone your hand touched";
            } else if (!Finite(spot)) {
                why = "the touched spot looks wrong";
            } else {
                for (const uintptr_t offset : { kBoundMin, kBoundMinOld }) {
                    if (bounds_at(offset, lo, hi)) {
                        used = offset;
                        break;
                    }
                }
                if (used == 0) {
                    why = "its bounds look wrong";
                    if (g_boxes.load() < 5) {
                        uint8_t type = 0;
                        SafeRead(&type, base + 0x2e, sizeof(type));
                        spdlog::warn("[Grab] the object's base record (form type {}) from +0x40: {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} "
                                     "{:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}; bounding sphere radius {:.3f}",
                            type, record[0], record[1], record[2], record[3], record[4], record[5], record[6], record[7], record[8], record[9],
                            record[10], record[11], record[12], record[13], record[14], record[15], sphere[3]);
                    }
                }
            }
            float fit = 0.0f;
            if (!why) {
                // Very thin sides still get some thickness for the fingers.
                for (int a = 0; a < 3; ++a) {
                    if (hi[a] - lo[a] < 0.005f) {
                        const float mid = 0.5f * (hi[a] + lo[a]);
                        lo[a]           = mid - 0.0025f;
                        hi[a]           = mid + 0.0025f;
                    }
                }
                // The box must fit the object's bounding sphere; if its units are off, it is scaled to fit.
                const float half   = 0.5f * glm::length(hi - lo) * xf.s;
                const float radius = sphere[3];
                fit                = radius > 1e-3f ? half / radius : 1.0f;
                if (radius > 1e-3f && (fit < 0.25f || fit > 4.0f)) {
                    lo /= fit;
                    hi /= fit;
                }
                if (glm::length(hi - lo) * xf.s > 10.0f) {
                    why = "it is too big to hold in a hand";
                }
            }
            std::string hull_why;
            bool        hulled = false;
            glm::vec3   size_box{ 0.0f };
            if (!why) {
                taken.valid = true;
                taken.ref   = ref;
                taken.root  = root;
                taken.lo    = lo;
                taken.hi    = hi;
                taken.touched = ToNode(xf, spot);
                taken.fit   = fit;
                size_box    = (hi - lo) * xf.s;
                // Its real collision shape: the box becomes the shape's part of the game's size box (a potted plant's pot,
                // not its leaves), and the hand meets that surface.
                Hull hull;
                if (ReadHull(root, xf, hull, hull_why)) {
                    const glm::vec3 both_lo = glm::max(lo, hull.lo), both_hi = glm::min(hi, hull.hi);
                    if (glm::any(glm::lessThan(both_hi - both_lo, glm::vec3{ 0.01f / xf.s }))) {
                        hull_why = "its collision shape and its size box do not overlap"; // one of them is not what it seems
                    } else {
                        taken.lo   = both_lo;
                        taken.hi   = both_hi;
                        taken.hull = std::move(hull);
                        if (const float hull_fit = HullFit(taken); hull_fit > 0.0f) {
                            taken.fit = hull_fit;
                        }
                        hulled = true;
                    }
                }
                ClassifyShape(taken);
            }
            const int n = g_boxes.fetch_add(1) + 1;
            if (n <= 20 || n % 10 == 0) {
                if (why) {
                    spdlog::warn("[Grab] taking an object: no box for the hand to close around ({}); held as before", why);
                } else {
                    const glm::vec3 size = (taken.hi - taken.lo) * xf.s;
                    spdlog::info("[Grab] taking an object: box {:.2f} x {:.2f} x {:.2f} m ({}; the game's size box {:.2f} x {:.2f} x {:.2f} m, bounds at "
                                 "+{:x}, {:.2f} x its bounding sphere{}), touched {:.2f} m from its centre",
                        size.x, size.y, size.z, hulled ? "from its real collision shape" : fmt::format("its collision shape not used: {}", hull_why),
                        size_box.x, size_box.y, size_box.z, used, fit, fit < 0.25f || fit > 4.0f ? fmt::format(", so scaled by {:.3f}", 1.0f / fit) : "",
                        glm::length(spot - (xf.r * (xf.s * 0.5f * (lo + hi)) + xf.t)));
                }
            }
            int serial = 0;
            {
                std::scoped_lock _{ g_mutex };
                taken.serial = ++g_taken_serial;
                serial       = taken.serial;
                g_taken      = taken; // (copied: taken is read below)
            }
            // For the springs-made hook a moment later in this same call: the object's 3D now, and whether it is at rest
            // (its 3D where it was the frame before; a moving object would have moved on by the springs' first step).
            t_made        = Made{};
            t_made.valid  = taken.valid;
            t_made.serial = serial;
            t_made.r      = xf.r;
            t_made.t      = xf.t;
            t_made.s      = xf.s;
            NodeXf before;
            if (taken.valid && NodeTransform(root, kNodeWorldBefore, before)) {
                float turned = 0.0f;
                for (int a = 0; a < 3; ++a) {
                    turned = std::max(turned, glm::length(before.r[a] - xf.r[a]));
                }
                const float moved = glm::length(before.t - xf.t);
                // A jump (placed, loaded, teleported, the world's origin moved) is not motion: then it is not known.
                if (moved < kJumpMove && turned < kJumpTurn) {
                    t_made.moved  = moved;
                    t_made.turned = turned;
                    t_made.still  = moved < kStillMove && turned < kStillTurn;
                }
            }
        }

        // In the game's grab start, right after it made the springs (rsi = the player's spring list): the first spring
        // and the object's 3D as it is now, at rest, so the spring's first physics step can tell where it holds it.
        void OnSpringsMade(safetyhook::Context& ctx)
        {
            const Made made = std::exchange(t_made, Made{});
            if (!made.valid || !IsPlayer(ctx.r15)) {
                return; // no box for this grab (nothing to place by), or not the player's grab
            }
            uintptr_t data = 0, spring = 0;
            uint32_t  count = 0;
            if (!SafeRead(&count, ctx.rsi, sizeof(count)) || count == 0 || count > 8 || !SafeRead(&data, ctx.rsi + 8, sizeof(data)) ||
                !SafeRead(&spring, data, sizeof(spring)) || spring == 0) {
                return;
            }
            float      error[3]{};
            const bool read_error = SafeRead(error, spring + kSpringError, sizeof(error));
            std::scoped_lock _{ g_track_mutex };
            g_capture        = Capture{};
            g_capture.armed  = true;
            g_capture.serial = made.serial;
            g_capture.spring = spring;
            g_capture.r      = made.r;
            g_capture.t      = made.t;
            g_capture.s      = made.s;
            g_capture.still  = made.still;
            g_capture.moved  = made.moved;
            g_capture.turned = made.turned;
            // Its kept error before the first step: the spring maker's starting value; without that, what it is now.
            if (g_know_fresh) {
                std::memcpy(g_capture.fresh, g_fresh_error, sizeof(g_capture.fresh));
                g_capture.know_fresh = true;
                g_capture.stepped    = read_error && std::memcmp(error, g_fresh_error, sizeof(error)) != 0;
            } else if (read_error) {
                std::memcpy(g_capture.fresh, error, sizeof(g_capture.fresh));
                g_capture.know_fresh = true;
            }
            g_capture_spring.store(spring, std::memory_order_release);
        }

        // Each physics step of the held spring, once it put its held point in the game world ([rbp+0x60], before it moves it):
        // where that point is vs where the step before sent it. With damping 1 a step leaves the point at held + elasticity x
        // (target - held) (unless its force cap stopped it), so anything more is something it touches pushing it: counted, and
        // the object is then held more softly (OnSpringStep) so it rests against the obstacle instead of shaking.
        void MeasureContact(const safetyhook::Context& ctx, uintptr_t spring)
        {
            float point[3]{}, target[3]{}, elastic = 0.0f, damping = 0.0f, force = 0.0f;
            if (!SafeRead(point, ctx.rbp + kStepPointSlot, sizeof(point)) || !SafeRead(target, spring + kSpringTarget, sizeof(target)) ||
                !SafeRead(&elastic, spring + kSpringElastic, sizeof(elastic)) || !SafeRead(&damping, spring + kSpringDamping, sizeof(damping)) ||
                !SafeRead(&force, spring + kSpringForce, sizeof(force))) {
                return;
            }
            const glm::vec3 held{ point[0], point[1], point[2] };
            const glm::vec3 aim{ target[0], target[1], target[2] };
            const float     dt = t_step_dt;
            if (!Finite(held) || !Finite(aim)) {
                return;
            }
            std::scoped_lock _{ g_track_mutex };
            auto& c = g_contact;
            if (c.spring != spring || c.serial != g_track.serial || g_track.pulling) {
                c        = Contact{};
                c.spring = spring;
                c.serial = g_track.serial;
                if (g_track.pulling) {
                    return; // (flying to the hand: off its path by design)
                }
            } else if (c.have_prev && c.prev_damping >= 0.999f && c.prev_dt > 0.0f) {
                const glm::vec3 asked  = c.prev_target - c.prev_held;
                const float     travel = c.prev_elastic * glm::length(asked);
                if (travel <= c.prev_force * c.prev_dt * c.prev_dt) { // (not stopped short by the force cap)
                    glm::vec3 push = held - (c.prev_held + c.prev_elastic * asked);
                    if (push.z < 0.0f) {
                        push.z = std::min(0.0f, push.z + kFallAllowance);
                    }
                    const float size   = glm::length(push);
                    const bool  pushed = std::isfinite(size) && size > kPushed;
                    auto&       f      = g_follow_stats;
                    ++f.push_steps;
                    if (pushed) {
                        ++f.pushed;
                        f.push_sum += size;
                        f.push_max = std::max(f.push_max, size);
                        // Which way: up or down, else away from you, toward you or sideways (level, from your head).
                        if (std::abs(push.z) > 0.7f * size) {
                            ++(push.z > 0.0f ? f.push_up : f.push_down);
                        } else if (g_track.have_body) {
                            glm::vec3 out = held - g_track.body;
                            out.z         = 0.0f;
                            glm::vec3 flat{ push.x, push.y, 0.0f };
                            const float along = glm::length(out) > 1e-3f ? glm::dot(flat, glm::normalize(out)) : 0.0f;
                            const float level = glm::length(flat);
                            ++(along > 0.5f * level ? f.push_away : (along < -0.5f * level ? f.push_toward : f.push_side));
                        } else {
                            ++f.push_side;
                        }
                    }
                    c.recent = ((c.recent << 1) | (pushed ? 1u : 0u)) & 0xFu;
                    if (std::popcount(c.recent) >= 2) {
                        c.contact_until = c.steps + kContactHoldSteps;
                    }
                }
            }
            c.prev_held    = held;
            c.prev_target  = aim;
            c.prev_elastic = elastic;
            c.prev_damping = damping;
            c.prev_force   = force;
            c.prev_dt      = dt;
            c.have_prev    = true;
            ++c.steps;
        }

        // In a spring's physics step, just after it put its held point in the game world ([rbp+0x60]): for the spring
        // of a new grab (its first step, the object still at rest) that point in the object's own space. Read only.
        void OnHeldPoint(safetyhook::Context& ctx)
        {
            const uintptr_t spring = ctx.rdi;
            if (spring != 0 && spring == g_throw_spring.load(std::memory_order_acquire)) {
                // Let go, to be thrown: every step until the drop gives the held point the throw's velocity (its target, read
                // right after this, is the point moved on by that velocity x the step).
                float       point[3]{};
                const float dt = t_step_spring == spring ? t_step_dt : 0.0f;
                if (dt > 0.0f && dt < 0.1f && SafeRead(point, ctx.rbp + kStepPointSlot, sizeof(point))) {
                    std::scoped_lock _{ g_track_mutex };
                    const glm::vec3 to = glm::vec3{ point[0], point[1], point[2] } + g_throw.velocity * dt;
                    if (g_throw.spring == spring && Finite(to) && NowUs() - g_throw.us < 150'000) {
                        auto* out = reinterpret_cast<float*>(spring + kSpringTarget);
                        out[0]    = to.x;
                        out[1]    = to.y;
                        out[2]    = to.z;
                        auto& force = *reinterpret_cast<float*>(spring + kSpringForce);
                        force       = std::max(force, kLockForce);
                        *reinterpret_cast<float*>(spring + kSpringElastic) = kLockElastic;
                        *reinterpret_cast<float*>(spring + kSpringDamping) = kLockDamping;
                        g_throw.done                                       = true;
                    }
                }
            }
            // (fix170) An adopted object let go, to be thrown: its own step gives the held point the hand's speed (as above).
            if (spring != 0 && spring == g_adopt_throw_spring.load(std::memory_order_acquire)) {
                float point[3]{};
                if (SafeRead(point, ctx.rbp + kStepPointSlot, sizeof(point))) {
                    std::scoped_lock _{ g_track_mutex };
                    const float     dt = t_step_spring == spring ? t_step_dt : 0.0f;
                    const glm::vec3 to = glm::vec3{ point[0], point[1], point[2] } + g_adopt_throw_velocity * dt;
                    if (dt > 0.0f && Finite(to) && NowUs() - g_adopt_throw_us < 150'000) {
                        auto* out = reinterpret_cast<float*>(spring + kSpringTarget);
                        out[0]    = to.x;
                        out[1]    = to.y;
                        out[2]    = to.z;
                        auto& force = *reinterpret_cast<float*>(spring + kSpringForce);
                        force       = std::max(force, kLockForce);
                        *reinterpret_cast<float*>(spring + kSpringElastic) = kLockElastic;
                        *reinterpret_cast<float*>(spring + kSpringDamping) = kLockDamping;
                        g_adopt_throw_done                                 = true;
                    }
                }
            }
            if (spring != 0 && spring == g_track_spring.load(std::memory_order_acquire) && t_step_spring == spring) {
                MeasureContact(ctx, spring);
            }
            if (spring == 0 || spring != g_capture_spring.load(std::memory_order_acquire)) {
                return;
            }
            float point[3]{};
            float previous[3]{};
            if (!SafeRead(point, ctx.rbp + kStepPointSlot, sizeof(point))) {
                return;
            }
            // A spring that has not stepped yet still keeps the error the spring maker started it with.
            const bool      read_previous = SafeRead(previous, spring + kSpringError, sizeof(previous));
            const glm::vec3 world{ point[0], point[1], point[2] };
            std::scoped_lock _{ g_track_mutex };
            if (!g_capture.armed || spring != g_capture.spring) {
                return;
            }
            NodeXf xf;
            xf.r            = g_capture.r;
            xf.t            = g_capture.t;
            xf.s            = g_capture.s;
            g_capture.first = read_previous && g_capture.know_fresh && !g_capture.stepped &&
                              std::memcmp(previous, g_capture.fresh, sizeof(previous)) == 0;
            g_capture.held  = ToNode(xf, world);
            g_capture.world = world;
            g_capture.have  = Finite(world) && Finite(g_capture.held);
            g_capture.armed = false;
            g_capture_spring.store(0, std::memory_order_release);
        }

        // In the grab start, after its own ray from the camera: a grab a grip started keeps the hand pick's target and point
        // (the jump the game takes when that ray hits nothing before the pick's target).
        void OnStartRepick(safetyhook::Context& ctx)
        {
            if (t_hand_start && g_start_fn != 0) {
                ctx.rip = g_start_fn + kStartRepickSkip;
                g_repicks.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Does the interaction pick have something (the use prompt's target)?
        bool PickHasTarget()
        {
            uintptr_t caster = 0;
            uint32_t  handle = 0, null_handle = 0;
            return SafeRead(&caster, g_caster_global, sizeof(caster)) && caster != 0 && SafeRead(&handle, caster + kCasterTarget, sizeof(handle)) &&
                   SafeRead(&null_handle, g_null_global, sizeof(null_handle)) && handle != null_handle && handle != 0;
        }

        // (fix172) The pick's target's handle (0: none), and a grab Physical asks to start again (a person a grip just knocked down is down):
        // the grip, if still held, counts as squeezed afresh at the next frame's start.
        uint32_t PickHandle()
        {
            uintptr_t caster = 0;
            uint32_t  handle = 0, null_handle = 0;
            return SafeRead(&caster, g_caster_global, sizeof(caster)) && caster != 0 && SafeRead(&handle, caster + kCasterTarget, sizeof(handle)) &&
                           SafeRead(&null_handle, g_null_global, sizeof(null_handle)) && handle != null_handle
                       ? handle
                       : 0;
        }
        std::atomic<bool> g_grab_again[2]{};

        // The hand's velocity in the game world (averaged over the last few samples) plus the player's walking.
        void ThrowVelocity(int hand, glm::vec3& linear, glm::vec3& angular)
        {
            glm::vec3 lin{ 0.0f }, ang{ 0.0f };
            for (int i = 0; i < kHistory; ++i) {
                lin += g_linear_history[hand][i];
                ang += g_angular_history[hand][i];
            }
            lin /= static_cast<float>(kHistory);
            ang /= static_cast<float>(kHistory);
            linear  = glm::vec3{ 0.0f };
            angular = glm::vec3{ 0.0f };
            const auto room = tracking::RoomRotation();
            glm::vec3  unused_point{}, unused_direction{};
            float      scale = 1.0f;
            if (!room || !tracking::StageToWorld(glm::vec3{ 0.0f }, glm::vec3{ 0.0f, 0.0f, -1.0f }, unused_point, unused_direction, &scale)) {
                return;
            }
            glm::vec3 walk{ 0.0f };
            {
                std::scoped_lock _{ g_track_mutex };
                walk = g_track.walk;
            }
            linear  = *room * tracking::ToHavokVector(lin) * scale * kThrowBoost + walk;
            angular = *room * tracking::ToHavokVector(ang);
        }

        // At a frame's start (main thread): the frame before has been drawn, so the held object is where it was drawn. How far
        // that is from where the hand held it, and whether it shows that frame's physics (the last step's target) or the frame
        // before's.
        void MeasureDrawn()
        {
            if (g_track_spring.load(std::memory_order_relaxed) == 0) {
                return;
            }
            const uint64_t ended = g_frame.load(std::memory_order_relaxed) - 1;
            uintptr_t      root  = 0;
            glm::vec3      held{ 0.0f };
            {
                std::scoped_lock _{ g_track_mutex };
                if (!g_track.have_held || g_track.us == 0) {
                    return;
                }
                root = g_track.root;
                held = g_track.held;
            }
            // Where physics had it (drawn there, or drawn in the hand instead: then whether that stayed until now).
            NodeXf xf;
            {
                bool   presented = false;
                NodeXf drawn{};
                {
                    std::scoped_lock present{ g_present_mutex };
                    if (g_present.root == root && g_present.drawn_frame == ended) {
                        presented = true;
                        xf        = g_present.physics;
                        drawn     = g_present.drawn;
                    }
                }
                NodeXf now{};
                if (presented && NodeWorld(root, now)) {
                    const bool kept = glm::length(now.t - drawn.t) < 0.001f && AngleBetween(now.r, drawn.r) < 0.01f;
                    (kept ? g_present_kept : g_present_lost).fetch_add(1, std::memory_order_relaxed);
                }
                if (!presented && !HeldWorld(root, xf)) {
                    return;
                }
            }
            const glm::vec3 at = xf.r * (xf.s * held) + xf.t;
            if (!Finite(at)) {
                return;
            }
            std::scoped_lock _{ g_track_mutex };
            if (g_track.root != root) {
                return;
            }
            bool voted = false;
            if (g_follow.target_frame == ended && g_follow.target_before_frame + 1 == ended) {
                // Steps used that frame's reading of the hand and the one before's: which one is it drawn at?
                const float apart = glm::length(g_follow.target - g_follow.target_before);
                if (apart > kMoving) {
                    if (glm::length(at - g_follow.target) < 0.3f * apart) {
                        ++g_follow.fresh_votes;
                        voted = true;
                    } else if (glm::length(at - g_follow.target_before) < 0.3f * apart) {
                        ++g_follow.stale_votes;
                        voted = true;
                    }
                }
            } else if (g_follow.target_frame + 1 == ended && g_follow.have_drawn_lead && g_follow.drawn_frame == ended &&
                       glm::length(g_follow.drawn_lead) > kMoving) {
                // No physics step used that frame's reading yet (the steps ran before its grab update): it is drawn at the
                // frame before's, while the hand moved.
                ++g_follow.stale_votes;
                voted = true;
            }
            if (const int votes = g_follow.fresh_votes + g_follow.stale_votes; voted && votes >= kVotesNeeded) {
                const int shows = g_follow.stale_votes >= kVoteShare * votes ? 2 : (g_follow.fresh_votes >= kVoteShare * votes ? 1 : g_follow.shows);
                if (shows != g_follow.shows) {
                    g_follow.shows = shows;
                    spdlog::info("[Grab] drawn objects show {} ({} of {} frames of motion): {}",
                        shows == 2 ? "the frame before's reading of your hand" : "their own frame's reading of your hand",
                        shows == 2 ? g_follow.stale_votes : g_follow.fresh_votes, votes,
                        shows == 2 ? "a held object is led one more frame of your hand's motion" : "a held object goes exactly where your hand is drawn");
                }
                if (votes >= 200) {
                    g_follow.fresh_votes /= 2;
                    g_follow.stale_votes /= 2;
                }
            }
            auto& s = g_follow_stats;
            if (!g_track.pulling && g_follow.have_drawn && g_follow.drawn_frame == ended && g_follow.have_parts && g_follow.parts_frame == ended) {
                // Walking or turning (the play area moved), else moving the hand in it, else still. (The frame's own motion,
                // from the frame before's drawing to this one's: not the move within the frame, which runs backward when the
                // hand is drawn before the grab update reads it.)
                const glm::vec3 gap    = at - g_follow.drawn;
                const float     length = glm::length(gap);
                const float     area   = glm::length(g_follow.area_move);
                const float     own    = glm::length(g_follow.hand_move);
                // Learning the lead: with a lead of k frames, a drawn object ends up (k - k') x the frame's motion ahead of where
                // the hand holds it (behind: below 0), k' being the lead it needs, so k is nudged toward k' by what is measured,
                // a little each frame. Only from frames the lead alone decided: drawn objects show the frame before's physics,
                // the frame before's target was led that way at full stiffness, no physics step was held back around then (by
                // something blocking it, or held softly while touching something), and no jump (more than 30 cm off).
                const uint64_t shown = ended - 1; // the frame whose physics this drawing shows
                const bool     learn = g_follow.shows == 2 && length < 0.3f && ended >= 2 && g_follow.led_frames[shown & 3] == shown &&
                                   g_follow.led_elastic[shown & 3] >= 0.99f && g_follow.disturbed_frame + 2 < ended;
                if (area > kMoving) {
                    ++s.moving_n;
                    s.moving_sum += length;
                    s.moving_max = std::max(s.moving_max, length);
                    const float along = glm::dot(gap, g_follow.area_move / area);
                    s.along_sum += along;
                    s.move_sum += area;
                    if (learn && own < 0.5f * area) { // (the hand's own motion small beside it: its part of the gap is small too)
                        g_follow.lead_area = std::clamp(g_follow.lead_area - 0.03f * std::clamp(along / area, -1.0f, 1.0f), 0.0f, 1.5f);
                    }
                } else if (own > kMoving) {
                    ++s.hand_n;
                    s.hand_sum += length;
                    s.hand_max = std::max(s.hand_max, length);
                    const float along = glm::dot(gap, g_follow.hand_move / own);
                    s.hand_along_sum += along;
                    s.hand_move_sum += own;
                    if (learn) {
                        g_follow.lead_hand = std::clamp(g_follow.lead_hand - 0.03f * std::clamp(along / own, -1.0f, 1.0f), 0.3f, 1.5f);
                    }
                } else {
                    ++s.still_n;
                    s.still_sum += length;
                    s.still_max = std::max(s.still_max, length);
                }
            }
            if (!g_track.pulling && g_follow.have_want && g_follow.want_frame == ended && g_follow.hand_turn >= 0.0f) {
                // Its orientation as drawn vs the one the hand holds it in (read for that frame).
                const float angle = glm::degrees(std::acos(TurnCos(xf.r * glm::transpose(g_follow.want))));
                if (std::isfinite(angle)) {
                    if (g_follow.hand_turn > kTurning) {
                        ++s.rot_turn_n;
                        s.rot_turn_sum += angle;
                        s.rot_turn_max = std::max(s.rot_turn_max, angle);
                        s.rot_rate_sum += g_follow.hand_turn;
                    } else {
                        ++s.rot_still_n;
                        s.rot_still_sum += angle;
                        s.rot_still_max = std::max(s.rot_still_max, angle);
                    }
                }
            }
        }

        void StopTracking()
        {
            g_track_spring = 0;
            g_in_flight    = false;
            EndPresent();
            std::scoped_lock _{ g_track_mutex };
            g_throw = Throw{};
            g_throw_spring.store(0, std::memory_order_release);
            g_track.us      = 0;
            g_track.box     = false;
            g_track.pulling = false;
            if (g_capture.armed) {
                g_capture.armed = false; // the grab ended before its springs ever stepped
                g_capture_spring.store(0, std::memory_order_release);
            }
        }
    }

    void Install()
    {
        // The convex collision shape's type (held objects are placed on their real shape when it is one).
        {
            const char* const family[4] = { ".?AVhknpConvexShape@@", ".?AVhknpBoxShape@@", ".?AVhknpCapsuleShape@@", ".?AVhknpSphereShape@@" };
            for (int i = 0; i < 4; ++i) {
                if (auto vtable = utility::rtti::find_vtable(MemoryScan::mod, family[i])) {
                    g_convex_family[i] = *vtable;
                }
            }
            g_convex_vtable = g_convex_family[0];
            if (auto vtable = utility::rtti::find_vtable(MemoryScan::mod, ".?AVhknpScaledConvexShape@@")) {
                g_scaled_vtable = *vtable;
            }
        }
        spdlog::info("[Grab] held objects placed on their real collision shape: {} (box shape {}, capsule {}, sphere {}, scaled shape {})",
            g_convex_vtable != 0 ? fmt::format("ready (convex shape type at {:x})", g_convex_vtable - Base()) : std::string{ "unavailable (its type was not found)" },
            g_convex_family[1] != 0, g_convex_family[2] != 0, g_convex_family[3] != 0, g_scaled_vtable != 0);
        const auto site = body::ScanUnique("grab target (hand grab)", kTargetPattern, kTargetRva, "Starfield 1.16.236 offset");
        if (!site) {
            g_off_reason = "off: the game's grab update was not found (another game version?)";
            spdlog::error("[Grab] hand grab is off: the game's grab update was not found");
            return;
        }
        if (!Same(site + kRotationFromTarget, kRotationSite, sizeof(kRotationSite)) || !Same(site + kRowStoreFromTarget, kRowStore, sizeof(kRowStore))) {
            g_off_reason = "off: the game's grab update is not the code this was made for";
            spdlog::error("[Grab] hand grab is off: the grab update at {:x} is not laid out like Starfield 1.16.236's", site - Base());
            return;
        }
        g_target_hook   = safetyhook::create_mid(reinterpret_cast<void*>(site + kTargetHook), &OnTarget);
        g_rotation_hook = safetyhook::create_mid(reinterpret_cast<void*>(site + kRotationFromTarget), &OnRotation);
        if (!g_target_hook) {
            g_off_reason = "off: could not hook the game's grab update";
            spdlog::error("[Grab] hand grab is off: could not hook the grab target");
            return;
        }
        g_ready = true;

        // The spring's physics step: the held object follows the hand between frames.
        if (const auto step = body::ScanUnique("grab spring step (hand grab)", kStepPattern, kStepRva, "Starfield 1.16.236 offset")) {
            bool laid_out = true;
            for (const auto& check : kStepChecks) {
                laid_out = laid_out && Same(step + check.offset, check.bytes, sizeof(check.bytes));
            }
            if (laid_out) {
                g_step_hook  = safetyhook::create_mid(reinterpret_cast<void*>(step), &OnSpringStep);
                g_step_ready = static_cast<bool>(g_step_hook);
                if (Same(step + kStepPointCheck, kStepPointBytes, sizeof(kStepPointBytes))) {
                    g_point_hook = safetyhook::create_mid(reinterpret_cast<void*>(step + kStepPoint), &OnHeldPoint);
                }
            } else {
                spdlog::warn("[Grab] the spring's physics step at {:x} is not laid out like Starfield 1.16.236's; held objects follow once a frame",
                    step - Base());
            }
        }

        // Grabbing with the grip: the game's own start and drop, the pick's target and the world kept by the update.
        const bool world_ok = Same(site - kWorldStoreBack, kWorldStore, sizeof(kWorldStore));
        if (Same(site - kDropCallBack, kDropCall, sizeof(kDropCall))) {
            const uintptr_t drop = RelTarget(site - kDropCallBack + 4, site - kDropCallBack + 8);
            if (Same(drop, kDropPrologue, sizeof(kDropPrologue))) {
                g_drop_fn = drop;
            }
        }
        if (const auto start = body::ScanUnique("grab start (hand grab)", kStartPattern, kStartRva, "Starfield 1.16.236 offset")) {
            if (Same(start + kCasterLoad, kCasterBytes, sizeof(kCasterBytes)) && Same(start + kNullLoad, kNullBytes, sizeof(kNullBytes))) {
                g_start_fn      = start;
                g_caster_global = RelTarget(start + kCasterLoad + 3, start + kCasterLoad + 7);
                g_null_global   = RelTarget(start + kNullLoad + 2, start + kNullLoad + 6);
            }
            // Which object a grab takes (for the grip around its box), and the springs once made (where they hold it).
            const bool springs_call = Same(start + kSpringsArgs, kSpringsArgsBytes, sizeof(kSpringsArgsBytes)) &&
                                      Same(start + kSpringsCall, kSpringsCallBytes, sizeof(kSpringsCallBytes));
            if (springs_call) {
                // The spring maker itself: where it makes the springs hold, and the kept error a new spring starts with.
                const uintptr_t maker = RelTarget(start + kMakerCallRel, start + kMakerCallRel + 4);
                if (Same(maker, kMakerPrologue, sizeof(kMakerPrologue))) {
                    g_holds_middle = Same(maker + kMakerShapeBox, kMakerShapeBoxBytes, sizeof(kMakerShapeBoxBytes)) &&
                                     Same(maker + kMakerBoxMiddle, kMakerBoxMiddleBytes, sizeof(kMakerBoxMiddleBytes)) &&
                                     Same(maker + kMakerMiddleStore, kMakerMiddleStoreBytes, sizeof(kMakerMiddleStoreBytes)) &&
                                     Same(maker + kMakerGrabPoint, kMakerGrabPointBytes, sizeof(kMakerGrabPointBytes));
                    if (Same(maker + kMakerFreshError, kMakerFreshLoad, sizeof(kMakerFreshLoad)) &&
                        Same(maker + kMakerFreshError + 8, kMakerFreshStore, sizeof(kMakerFreshStore))) {
                        const uintptr_t from = RelTarget(maker + kMakerFreshError + 4, maker + kMakerFreshError + 8);
                        g_know_fresh         = InImage(from) && InImage(from + sizeof(g_fresh_error) - 1) && SafeRead(g_fresh_error, from, sizeof(g_fresh_error));
                    }
                }
                spdlog::info("[Grab] the spring maker (rva {:x}) {}; a new spring's kept error starts as {}", maker - Base(),
                    g_holds_middle ? "holds objects by the middle of their collision shape's box" : "is not the code this was made for",
                    g_know_fresh ? fmt::format("({:g}, {:g}, {:g})", g_fresh_error[0], g_fresh_error[1], g_fresh_error[2]) : std::string{ "(not found)" });
                g_springs_hook = safetyhook::create_mid(reinterpret_cast<void*>(start + kSpringsCall), &OnSprings);
            }
            if (g_springs_hook && Same(start + kSpringsMade, kSpringsMadeBytes, sizeof(kSpringsMadeBytes))) {
                g_made_hook = safetyhook::create_mid(reinterpret_cast<void*>(start + kSpringsMade), &OnSpringsMade);
            }
            // Grabs a grip starts (and gravity-glove pulls) keep the hand pick's target instead of the head ray's.
            if (g_start_fn != 0 && Same(start + kStartRepick, kStartRepickBytes, sizeof(kStartRepickBytes))) {
                g_repick_hook = safetyhook::create_mid(reinterpret_cast<void*>(start + kStartRepick), &OnStartRepick);
            }
            const float max_mass  = RipFloat(start + kStartMaxMass, kMovssXmm0Rip);
            const float drop_far  = RipFloat(site - kDropFarBack, kComissXmm1Rip);
            const float drop_slow = RipFloat(site - kDropSlowBack, kComissXmm1Rip);
            const float drop_time = RipFloat(site - kDropTimeBack, kMovssXmm0Rip);
            spdlog::info("[Grab] gravity gloves: {}; the game grabs objects of mass up to {:g}, and its grab update drops a held object {:g} m from its "
                         "target ({:g} m with {:g})",
                g_repick_hook ? "ready (grabs your grips start keep the hand's pick)" : "unavailable (the grab start's head ray could not be skipped)",
                max_mass, drop_far, drop_slow, drop_time);
            g_pull_ready = static_cast<bool>(g_repick_hook);
            spdlog::info("[Grab] grip around the object's box: {}; measuring where the springs hold it: {}",
                static_cast<bool>(g_springs_hook) ? "ready" : "unavailable (held in the closed hand instead)",
                g_made_hook && g_point_hook ? "ready" : (g_holds_middle ? "unavailable (the middle of its box is used)" : "unavailable (the touched spot is used)"));
        }
        g_grip_ready = world_ok && g_drop_fn != 0 && g_start_fn != 0;
        // (fix170) One object in each hand: the drop's own calls (each checked at its place in the drop: the call's opcode and where it
        // goes), for taking an adopted spring out of its world as the drop does; and the player's update just before its grab test,
        // where the game holds the spring list's lock (adopting, steering and letting go happen there).
        if (g_grip_ready.load()) {
            // (fix190) `want`: where it must go, as this build's game version has that place (At(its rva): 0 when not known, and then
            // nothing matches).
            const auto at_call = [](uintptr_t off, uintptr_t want, uintptr_t& out) {
                static constexpr uint8_t kCall[] = { 0xE8 };
                if (!Same(g_drop_fn + off, kCall, sizeof(kCall))) {
                    return false;
                }
                const uintptr_t to = RelTarget(g_drop_fn + off + 1, g_drop_fn + off + 5);
                out                = to == want ? to : 0;
                return out != 0;
            };
            const auto at_rip = [](uintptr_t off, const uint8_t* op, size_t n, uintptr_t want, uintptr_t& out) {
                if (!Same(g_drop_fn + off, op, n)) {
                    return false;
                }
                const uintptr_t to = RelTarget(g_drop_fn + off + n, g_drop_fn + off + n + 4);
                out                = to == want ? to : 0;
                return out != 0;
            };
            static constexpr uint8_t kMovRcxRip[] = { 0x48, 0x8B, 0x0D };
            static constexpr uint8_t kLeaR12Rip[] = { 0x4C, 0x8D, 0x25 };
            const bool calls = at_call(kDropCellWorldCall, At(kCellWorldRva), g_fn_cell_world) &&
                               at_rip(kDropExtWorldLoad, kMovRcxRip, 3, At(kExtWorldRva), g_ext_world_global) &&
                               at_rip(kDropNotifyTable, kLeaR12Rip, 3, At(kNotifyTableRva), g_notify_table) && at_call(kDropBodyObject, At(kBodyObjectRva), g_fn_body_object) &&
                               at_call(kDropNotify, At(kNotifyRva), g_fn_notify) && at_call(kDropBodyFlag, At(kBodyFlagRva), g_fn_body_flag) &&
                               at_call(kDropWorldLock, At(kWorldLockRva), g_fn_world_lock) && at_call(kDropHkWorld, At(kHkWorldRva), g_fn_hk_world) &&
                               at_call(kDropRemove, At(kRemoveRva), g_fn_remove) && at_call(kDropWorldUnlock, At(kWorldUnlockRva), g_fn_world_unlock) &&
                               at_call(kDropRelease, At(kReleaseRva), g_fn_release);
            const auto slot = calls ? body::ScanUnique("player grab slot (two hands)", kSlotPattern, kSlotRva, "Starfield 1.16.236 offset") : 0;
            if (slot) {
                g_slot_hook   = safetyhook::create_mid(reinterpret_cast<void*>(slot + kSlotHook), &OnGrabSlot);
                g_adopt_ready = static_cast<bool>(g_slot_hook);
            }
            spdlog::info("[Grab] (fix170) both hands on one object: ready; one object in each hand: {}",
                g_adopt_ready.load() ? fmt::format("ready (the player's update hooked at rva {:x}; the drop's own calls found; thrown by its own step: {})",
                                           slot + kSlotHook - Base(), static_cast<bool>(g_point_hook))
                                     : (calls ? "unavailable (the player's update is not the code this was made for)"
                                              : "unavailable (the game's drop is not the code this was made for)"));
        }
        g_group_ready = SameCode(kSetGroupRva, kSetGroupBytes, sizeof(kSetGroupBytes));
        {
            // (fix161) The game's wake-up handler and its tests (UpdateKeepDrawn: a held object put in the moving objects).
            bool       all   = true;
            const auto check = [&](bool same, uintptr_t rva) {
                if (!same) {
                    all = false;
                    spdlog::warn("[Grab] (fix161) the game's wake-up handler is not the code this was made for (rva {:x}): held objects are left to it", rva);
                }
            };
            // (fix190) Each run checked by its own place and bytes (GameCode.h), all of them as before.
            check(SameCode(kJoinRva, kJoinStart, sizeof(kJoinStart)), kJoinRva);
            check(SameCode(kJoinTest20Rva, kJoinTest20, sizeof(kJoinTest20)), kJoinTest20Rva);
            check(SameCode(kJoinTest2000Rva, kJoinTest2000, sizeof(kJoinTest2000)), kJoinTest2000Rva);
            check(SameCode(kJoinTestEffectRva, kJoinTestEffect, sizeof(kJoinTestEffect)), kJoinTestEffectRva);
            check(SameCode(kJoinMovingRva, kJoinMoving, sizeof(kJoinMoving)), kJoinMovingRva);
            g_join_ready = all;
            spdlog::info("[Grab] (fix161) held objects put in the game's moving objects when it does not (its wake-up handler, rva {:x}): {}", kJoinRva,
                all ? "ready" : "off");
        }
        g_layer_ready = SameCode(kSetLayerRva, kSetLayerBytes, sizeof(kSetLayerBytes)) && SameCode(kSetLayerRva + kSetLayerMaskAt, kSetLayerMask, sizeof(kSetLayerMask));
        // Drawn in the hand: the game's scene writer, from its one call in the collision object's world update.
        {
            const uintptr_t writer = At(kWriterRva);
            const uintptr_t call   = At(kWriterCallRva);
            if (SameCode(kWriterRva, kWriterBytes, sizeof(kWriterBytes)) && SameCode(kWriterCallRva, kWriterCallBytes, sizeof(kWriterCallBytes)) &&
                RelTarget(call + sizeof(kWriterCallBytes), call + kWriterCallLength) == writer) {
                // Made disabled and turned on once in place: the writer runs on several threads all the time, and a call between
                // the hook going in and its being stored here would find no original to call.
                g_writer_return = call + kWriterCallLength;
                g_writer_hook   = safetyhook::create_inline(reinterpret_cast<void*>(writer), reinterpret_cast<void*>(&OnWriter),
                    safetyhook::InlineHook::StartDisabled);
                g_writer_ready  = static_cast<bool>(g_writer_hook) && g_writer_hook.enable().has_value();
            }
            spdlog::info("[Grab] held objects drawn exactly in your hand: {}",
                g_writer_ready.load() ? "ready (the game's scene writer, rva 237cb20, given the place in the hand for the held object)"
                                      : "unavailable (the game's scene writer is not the code this was made for)");
        }
        spdlog::info("[Grab] held objects stop bumping into you (put in your collision group while held): {}",
            g_group_ready.load() ? "ready" : "unavailable (the collision object's group function is not the code this was made for)");
        spdlog::info("[Grab] hand grab hooks: target {}, rotation {}, physics step {}; grip grabbing {} (start {:x}, drop {:x}, pick {:x}, world kept {})",
            static_cast<bool>(g_target_hook), static_cast<bool>(g_rotation_hook), g_step_ready.load(), g_grip_ready.load() ? "ready" : "unavailable",
            g_start_fn ? g_start_fn - Base() : 0, g_drop_fn ? g_drop_fn - Base() : 0, g_caster_global ? g_caster_global - Base() : 0, world_ok);
    }

    // Gravity gloves (main thread): stop aiming; the hand's cast goes back to the hand reach, the right hand and the hand.
    // `why` (for the log when nothing was locked on): what ended it; null: a pull or a grab took over.
    static void EndAim(const char* why = nullptr)
    {
        const int hand = g_aim_hand.load();
        if (hand < 0) {
            return;
        }
        if (why != nullptr && g_aim_target == 0 && (++g_aim_ends_told <= 30 || g_aim_ends_told % 10 == 0)) {
            spdlog::info("[Grab] {} hand: the gloves stopped aiming without locking on ({}; aimed {:.1f} s, {} far looks, {} found something; "
                         "{} aimed at the movable object nearest your pointing line, {} of those found it; {} movable objects around; the nearest came "
                         "within {} degrees of your hand's line, {} of your palm's, {} of sighting along your hand; {})",
                hand == 0 ? "left" : "right", why, static_cast<double>(NowUs() - g_aim_start_us) * 1e-6, g_aim_looks, g_aim_finds, g_aim_steer_looks,
                g_aim_steer_finds, g_scan.bodies.size(), NearText(g_aim_near[0]), NearText(g_aim_near[1]), NearText(g_aim_near[2]),
                !GameFlow::isWeaponDrawn() ? "weapon put away" : (HandsFreeNow() ? "fists out" : "a weapon out"));
        }
        if (g_aim_target != 0 && (g_locks <= 20 || g_locks % 10 == 0)) {
            spdlog::info("[Grab] {} hand: the lock ended without a pull (the aim ended; fastest move back toward you {:.1f} m/s, a pull needs {:.1f})",
                hand == 0 ? "left" : "right", g_aim_peak, kFlickSpeed);
        }
        g_aim_peak        = 0.0f;
        g_aim_hand        = -1;
        g_aim_target      = 0;
        g_seen_target     = 0;
        g_seen_frames     = 0;
        g_seen_narrow     = CastSeen{};
        g_seen_wide       = CastSeen{};
        g_seen_steer      = CastSeen{};
        g_seen_steer_body = 0;
        g_steer           = SteerTarget{};
        g_steer_next      = false;
        g_lock_steered    = false;
        g_lock_body       = 0;
        g_lock_misses     = 0;
        g_lock_others     = 0;
        g_have_mark       = false;
        GloveHighlight::Show(0, 0); // (the target's highlight taken off)
        g_far_frames_left = 3; // a grab at the hand must wait for a cast made at the hand again
        PointInteract::SetSteer(false, glm::vec3{ 0.0f }, 0);
        PointInteract::HoldCast(false);
        PointInteract::SetFarCast(0.0f, 0.0f);
        PointInteract::SetPointingHand(false);
    }

    // A grip squeezed with nothing next to the hand: it aims (the hand's cast reaches far) for as long as it is held.
    static void StartAim(int hand)
    {
        const float range = std::clamp(GameFlow::gStore.internalSettings.pullRange, 1.0f, 10.0f);
        g_aim_start_us    = NowUs();
        g_aim_looks       = 0;
        g_aim_finds       = 0;
        g_aim_hand        = hand;
        g_aim_target      = 0;
        g_seen_target     = 0;
        g_seen_frames     = 0;
        g_seen_narrow     = CastSeen{};
        g_seen_wide       = CastSeen{};
        g_seen_steer      = CastSeen{};
        g_seen_steer_body = 0;
        g_steer           = SteerTarget{};
        g_steer_next      = false;
        g_lock_steered    = false;
        g_lock_body       = 0;
        g_lock_misses     = 0;
        g_lock_others     = 0;
        g_aim_steer_looks = 0;
        g_aim_steer_finds = 0;
        g_have_mark       = false;
        GloveHighlight::Show(0, 0); // (the target's highlight taken off)
        for (float& degrees_near : g_aim_near) {
            degrees_near = std::numeric_limits<float>::max();
        }
        g_cast_narrow     = false;
        PointInteract::SetSteer(false, glm::vec3{ 0.0f }, 0);
        PointInteract::HoldCast(false);
        PointInteract::SetPointingHand(hand == 0);
        PointInteract::SetFarCast(range, kPullRadius);
        // The movable objects around: the whole list again if it is old or from another place (later frames keep it fresh).
        const int64_t now = NowUs();
        PlayerCollision you;
        if (g_scan.done_us == 0 || now - g_scan.done_us > kSteerScanStale || (ReadPlayerCollision(you) && you.world != g_scan.world)) {
            g_scan.next  = 0;
            g_scan_built = 0;
            ScanMovable(kBodyIndexMask);
        }
        if (now - g_scan_told_us > 30'000'000) {
            g_scan_told_us = now;
            spdlog::info("[Grab] gloves: {} movable objects in your area ({} physics records looked through{}); the far casts aim at the one "
                         "nearest your pointing line", g_scan.bodies.size(), g_scan.records, g_scan.faulted ? ", some could not be read" : "");
        }
        if (++g_aims <= 10 || g_aims % 20 == 0) {
            spdlog::info("[Grab] {} grip: nothing next to your hand, so it aims: point at an object up to {:.0f} m away, then flick your hand back "
                         "toward you to pull it",
                hand == 0 ? "left" : "right", range);
        }
    }

    // While aiming: the cast follows the hand; something it finds for a few frames running, with the hand steady, is
    // locked (a buzz; the cast then stays on it). Pointing well away from it without flicking aims again; a flick back
    // toward you pulls it. grip_held false: the grip was just let go while locked on (a flick in that moment still pulls,
    // and the object must then be caught).
    static void UpdateAim(int hand, uintptr_t player, bool grip_held)
    {
        uintptr_t  caster = 0;
        uint32_t   handle = 0, null_handle = 0;
        float      hit[3]{};
        const bool hit_something = SafeRead(&caster, g_caster_global, sizeof(caster)) && caster != 0 &&
                                   SafeRead(&handle, caster + kCasterTarget, sizeof(handle)) && SafeRead(&null_handle, g_null_global, sizeof(null_handle)) &&
                                   handle != null_handle && handle != 0 && SafeRead(hit, caster + kCasterPoint, sizeof(hit));
        // The game would not take it before; (fix172) or a person (bodies are grabbed with the hand, not flown by a limb: Physical).
        const bool refused = hit_something && (Refused(handle) || Physical::RefusePull(handle));
        bool       found   = hit_something && !refused;
        glm::vec3 point{ hit[0], hit[1], hit[2] };
        const float range       = std::clamp(GameFlow::gStore.internalSettings.pullRange, 1.0f, 10.0f);
        const float cast_scale  = std::max(0.1f, tracking::TrackingScale());
        const CastSeen* picked = nullptr;
        ++g_aim_frames;
        float cast_radius = 0.0f;
        int   cast_count  = 0;
        uint32_t cast_steer = 0;
        PointInteract::LastCast(cast_radius, cast_count, &cast_steer);
        const bool new_cast = cast_count != g_cast_count_seen;
        g_cast_count_seen   = cast_count;
        const int line = std::clamp(GameFlow::gStore.internalSettings.gloveAimLine, 0, tracking::kGloveLines - 1);
        glm::vec3 line_origin{}, line_dir{};
        const bool have_line = tracking::GloveRayWorld(line, hand == 0, line_origin, line_dir);
        if (g_aim_target == 0) {
            // The movable object nearest the gloves' line now (the list kept fresh a chunk a frame); and, for the log, how
            // near the nearest one comes to each of the lines.
            ScanMovable(kSteerScanChunk);
            {
                SteerTarget t;
                float       near_deg = std::numeric_limits<float>::max();
                if (have_line && ChooseSteer(line_origin, line_dir, range, cast_scale, g_steer.body, t, &near_deg)) {
                    t.misses = t.body == g_steer.body ? g_steer.misses : 0;
                    g_steer  = t;
                } else {
                    g_steer = SteerTarget{};
                }
                g_aim_near[line] = std::min(g_aim_near[line], near_deg);
                for (int other = 0; other < tracking::kGloveLines && g_aim_frames % 3 == 0; ++other) { // (for the log: every third frame)
                    glm::vec3   o{}, d{};
                    SteerTarget unused;
                    float       deg = std::numeric_limits<float>::max();
                    if (other != line && tracking::GloveRayWorld(other, hand == 0, o, d)) {
                        ChooseSteer(o, d, range, cast_scale, 0, unused, &deg);
                        g_aim_near[other] = std::min(g_aim_near[other], deg);
                    }
                }
            }
            // Not locked: a new result is the aimed, the narrow or the wide cast's by the radius of the cast that made it (a
            // result of the near cast, from before the aim, is none of them). The aimed cast's counts only when it hit the
            // object it was aimed at (near its box); one it keeps missing is skipped for a moment. What the aimed cast found
            // goes first (as HIGGS: the object nearest the line), then the narrow one's (pointed straight at), then the wide.
            if (new_cast) {
                if (cast_steer != 0 && std::abs(cast_radius - kSteerRadius) < 0.005f) {
                    ++g_aim_looks;
                    if (cast_steer == g_steer.body) {
                        ++g_aim_steer_looks;
                        const float off_box = hit_something && Finite(point) ? DistanceToBox(point, g_steer.lo, g_steer.hi) : -1.0f;
                        const bool  at_it   = off_box >= 0.0f && off_box <= (kSteerHitNear + kSteerRadius) * cast_scale;
                        if (at_it && refused) {
                            SkipSteer(cast_steer, kSteerRefusedUs); // the game would not let you pull it before
                            g_seen_steer = CastSeen{};
                            g_steer      = SteerTarget{};
                        } else if (at_it) {
                            g_seen_steer      = CastSeen{ handle, point, g_aim_frames };
                            g_seen_steer_body = cast_steer;
                            g_steer.misses    = 0;
                            ++g_aim_finds;
                            ++g_aim_steer_finds;
                            if (++g_steer_found_told <= 10) {
                                spdlog::info("[Grab] gloves: aimed at the movable object nearest your pointing line ({:.1f} m away, {:.0f} degrees off it, "
                                             "layer {}): the game's pick found it ({:.2f} m from its box)",
                                    g_steer.distance, g_steer.degrees, g_steer.layer, off_box / cast_scale);
                            }
                        } else {
                            g_seen_steer = CastSeen{};
                            if (++g_steer.misses >= kSteerMisses) {
                                if (++g_steer_skip_told <= 10) {
                                    spdlog::info("[Grab] gloves: the game's pick did not find the object aimed at ({:.1f} m away, {:.0f} degrees off your "
                                                 "pointing line, layer {}; {}); trying the next one",
                                        g_steer.distance, g_steer.degrees, g_steer.layer,
                                        !hit_something ? "nothing it can take there"
                                                       : fmt::format("it found something {:.2f} m from the object's box", off_box / cast_scale));
                                }
                                SkipSteer(g_steer.body, kSteerSkipUs);
                                g_steer = SteerTarget{};
                            }
                        }
                    } else {
                        g_seen_steer = CastSeen{}; // aimed at one that is no longer the choice
                    }
                } else if (CastSeen* from = cast_steer != 0                                   ? nullptr
                                            : std::abs(cast_radius - kPullNarrowRadius) < 0.005f ? &g_seen_narrow
                                            : std::abs(cast_radius - kPullRadius) < 0.005f       ? &g_seen_wide
                                                                                                 : nullptr) {
                    *from = found && Finite(point) ? CastSeen{ handle, point, g_aim_frames } : CastSeen{};
                    ++g_aim_looks;
                    g_aim_finds += from->handle != 0 ? 1 : 0;
                }
                // The next cast: aimed at the object every other cast while there is one; between them, the narrow and the
                // wide cast in turn.
                if (g_steer.body != 0 && !g_steer_next) {
                    g_steer_next = true;
                } else {
                    g_steer_next  = false;
                    g_cast_narrow = !g_cast_narrow;
                }
            }
            if (g_steer_next && g_steer.body == 0) {
                g_steer_next = false; // nothing to aim at any more
            }
            // (The far cast's own radius stays the narrow or the wide one: the aimed cast has its radius with its aim, so a cast
            // made as one is switched to the other is still one or the other.)
            PointInteract::SetFarCast(range, g_cast_narrow ? kPullNarrowRadius : kPullRadius);
            if (g_steer_next) {
                PointInteract::SetSteer(true, g_steer.at, g_steer.body, kSteerRadius); // where it is now
            } else {
                PointInteract::SetSteer(false, glm::vec3{ 0.0f }, 0);
            }
            // Something else pointed straight at (the narrow cast) goes before an aimed object the line does not pass through.
            // A find stays until the next cast of its kind (they take turns, one in two or one in four): kFreshFrames.
            auto       fresh  = [](const CastSeen& seen) { return seen.handle != 0 && g_aim_frames - seen.frame <= kFreshFrames; };
            const bool steer  = fresh(g_seen_steer) && g_seen_steer_body == g_steer.body;
            const bool narrow = fresh(g_seen_narrow);
            picked = steer && !(narrow && g_seen_narrow.handle != g_seen_steer.handle && g_steer.degrees > kSteerOnLine) ? &g_seen_steer
                     : narrow                                                                                           ? &g_seen_narrow
                     : steer                                                                                            ? &g_seen_steer
                                                                                                                        : (fresh(g_seen_wide) ? &g_seen_wide : nullptr);
            found      = picked != nullptr;
            handle = picked ? picked->handle : 0;
            point  = picked ? picked->point : glm::vec3{ 0.0f };
            // The marker: a ring on what was found (the box of an object aimed at), else a dot along the line.
            g_mark      = GloveMark{};
            g_mark.hand = hand;
            if (picked == &g_seen_steer) {
                g_mark.kind   = 1;
                g_mark.point  = g_steer.at;
                g_mark.radius = BoxRingRadius(g_steer.lo, g_steer.hi, cast_scale);
            } else if (picked) {
                g_mark.kind   = 1;
                g_mark.point  = picked->point;
                g_mark.radius = kMarkFoundRadius * cast_scale;
            } else {
                g_mark.kind  = 3;
                g_mark.point = line_origin + line_dir * (kMarkPointer * cast_scale);
            }
            g_have_mark = have_line || picked != nullptr;
            // (On the object itself: blue on what was found.)
            GloveHighlight::Show(picked != nullptr ? 1 : 0, picked != nullptr ? picked->handle : 0);
        } else if (g_lock_steered) {
            // Locked on through the aimed cast: it follows the object (its body read again each frame); a miss or two in a
            // row (it rolled, a thin part) is let go, more and it is lost.
            // Only a cast aimed at it says whether it is still there: another (made as it locked on) is waited out, no pull.
            BodyLook look;
            if (!LookAtBody(g_lock_body, cast_scale, look)) {
                found = false;
            } else {
                g_mark      = GloveMark{ hand, 2, look.at, BoxRingRadius(look.lo, look.hi, cast_scale) };
                GloveHighlight::Show(2, g_aim_target); // (white once locked on)
                g_have_mark = true;
                PointInteract::SetSteer(true, look.at, g_lock_body, kSteerRadius);
                if (cast_steer != g_lock_body) {
                    g_lock_others += new_cast ? 1 : 0;
                    if (g_lock_others <= kLockOthersMax) {
                        return;
                    }
                    found = false; // the aimed casts never came: lost
                } else {
                    g_lock_others = 0;
                    if (!found || handle != g_aim_target) {
                        g_lock_misses += new_cast ? 1 : 0;
                        if (g_lock_misses < kLockMissesMax) {
                            return; // not yet: no pull this frame
                        }
                    } else {
                        g_lock_misses = 0;
                    }
                }
            }
        } else {
            // Locked on what a cast along the line found: the ring stays where it was last found.
            if (found && handle == g_aim_target && Finite(point)) {
                g_lock_point = point;
            }
            g_mark      = GloveMark{ hand, 2, g_lock_point, kMarkFoundRadius * cast_scale };
            GloveHighlight::Show(2, g_aim_target);
            g_have_mark = true;
        }
        // skip: an object locked on by aiming at it is passed over for a moment (lost, or something in the way of it).
        auto unlock = [hand](const char* why, bool skip) {
            if (g_aim_target != 0 && (g_locks <= 20 || g_locks % 10 == 0)) {
                spdlog::info("[Grab] {} hand: the lock ended without a pull ({}; fastest move back toward you {:.1f} m/s, a pull needs {:.1f})",
                    hand == 0 ? "left" : "right", why, g_aim_peak, kFlickSpeed);
            }
            g_aim_target  = 0;
            g_seen_target = 0;
            g_seen_frames = 0;
            g_aim_peak    = 0.0f;
            g_seen_narrow = CastSeen{};
            g_seen_wide   = CastSeen{};
            g_seen_steer  = CastSeen{};
            if (g_lock_steered && skip) {
                SkipSteer(g_lock_body, kSteerSkipUs);
            }
            g_lock_steered = false;
            g_lock_body    = 0;
            g_lock_misses  = 0;
            g_lock_others  = 0;
            g_steer_next   = false;
            PointInteract::SetSteer(false, glm::vec3{ 0.0f }, 0);
            PointInteract::HoldCast(false);
        };
        glm::vec3  palm{};
        float      scale = 1.0f;
        const auto room  = tracking::RoomRotation();
        if (!found || !Finite(point) || !room || !PalmWorld(hand == 0, palm, scale)) {
            if (g_aim_target != 0) {
                unlock("it was lost from sight", true); // it moved, or something came in between: aim again
            }
            g_seen_target = 0;
            g_seen_frames = 0;
            return;
        }
        glm::vec3   to_hand  = palm - point;
        const float distance = glm::length(to_hand);
        if (!(distance > 0.05f) || !std::isfinite(distance)) {
            return;
        }
        to_hand /= distance;
        // Toward you: toward your head (the rendered camera) from the hand.
        glm::vec3 to_head = to_hand;
        if (auto* root = CreationEngineSingletonManager::GetSceneGraphRoot(); root && root->worldCamera) {
            const auto&     eye = root->worldCamera->world.translate;
            const glm::vec3 d   = glm::vec3{ eye.x, eye.y, eye.z } - palm;
            if (glm::length(d) > 0.05f && Finite(d)) {
                to_head = glm::normalize(d);
            }
        }
        // How fast the hand moves back toward you (the most of the last three samples): along the line from the object (as
        // HIGGS and ROCK measure it) or toward your head (pulling back toward your chest), by the controller itself (an arm
        // yank) or the fingertips (a wrist flick, as in Half-Life: Alyx: the controller's speed plus its turn times the reach
        // to the fingertips).
        const glm::vec3 fingers = glm::normalize(-glm::vec3{ tracking::AimPose(hand == 0)[2] }) * kFingerReach;
        float           speed   = -1.0f;
        for (int i = 1; i <= 3; ++i) {
            const int       k     = (g_history_next[hand] - i + kHistory) % kHistory;
            const glm::vec3 v     = g_linear_history[hand][k];
            const glm::vec3 v_tip = v + glm::cross(g_angular_history[hand][k], fingers);
            for (const glm::vec3& stage : { v, v_tip }) {
                const glm::vec3 world = *room * tracking::ToHavokVector(stage) * scale;
                if (Finite(world)) {
                    speed = std::max(speed, std::max(glm::dot(world, to_hand), glm::dot(world, to_head)));
                }
            }
        }
        const glm::vec3 ray      = line_dir; // the gloves' line
        const bool      have_ray = have_line;
        if (g_aim_target == 0) {
            if (!grip_held) {
                return; // let go: nothing to lock on any more
            }
            // Not locked: the same thing for a few frames running, the hand steady.
            if (handle != g_seen_target) {
                g_seen_target = handle;
                g_seen_frames = 1;
                return;
            }
            if (++g_seen_frames < kLockFrames || speed > kFlickStart * scale) {
                return;
            }
            g_aim_target   = handle;
            g_aim_peak     = 0.0f;
            g_lock_ray     = have_ray ? ray : -to_hand;
            g_lock_steered = picked == &g_seen_steer;
            g_lock_misses  = 0;
            g_lock_others  = 0;
            g_lock_point   = point;
            if (++g_lock_lines_told <= 30) {
                // Which line matches how you point: how far what you locked on is off each (its box when aimed at).
                const bool      box = g_lock_steered && g_steer.body == g_seen_steer_body;
                const glm::vec3 lo  = box ? g_steer.lo : point, hi = box ? g_steer.hi : point;
                spdlog::info("[Grab] gloves: what you locked on is {:.0f} degrees off your hand's line, {:.0f} off your palm's, {:.0f} off sighting along "
                             "your hand (pointing with your {})",
                    LineDegrees(0, hand, lo, hi), LineDegrees(1, hand, lo, hi), LineDegrees(2, hand, lo, hi), LineName(line));
            }
            if (g_lock_steered) {
                // Found by aiming at it: the thin cast keeps aiming at it (wherever it goes) while the hand flicks.
                g_lock_body   = g_seen_steer_body;
                g_lock_radius = kSteerRadius;
                PointInteract::SetFarCast(range, kPullNarrowRadius);
                PointInteract::SetSteer(true, g_steer.body == g_lock_body ? g_steer.at : point, g_lock_body, kSteerRadius);
            } else {
                g_lock_body   = 0;
                g_lock_radius = picked == &g_seen_narrow ? kPullNarrowRadius : kPullRadius;
                PointInteract::SetSteer(false, glm::vec3{ 0.0f }, 0);
                PointInteract::SetFarCast(range, g_lock_radius); // the cast that found it...
                PointInteract::HoldCast(true);                   // ...stays on it while the hand flicks
            }
            haptics::MeleePulse(hand == 0, 0.5f, 0.08f);
            if (++g_locks <= 20 || g_locks % 10 == 0) {
                spdlog::info("[Grab] {} hand: locked on something {:.1f} m away{}; flick your hand back toward you to pull it", hand == 0 ? "left" : "right",
                    distance, g_lock_steered ? " (the object nearest your pointing line)" : (g_lock_radius == kPullNarrowRadius ? " (pointed straight at)" : ""));
            }
            return;
        }
        if (handle != g_aim_target) {
            unlock("something else came in the way", true); // aim again
            return;
        }
        g_aim_peak = std::max(g_aim_peak, speed / scale);
        if (!(speed > kFlickSpeed * scale)) {
            // Pointing well away from it, not flicking: the player is aiming at something else.
            if (grip_held && have_ray && speed < kFlickStart * scale && glm::dot(ray, g_lock_ray) < kUnlockCos) {
                unlock("you pointed elsewhere", false);
            }
            return;
        }
        uint32_t mode = 1;
        SafeRead(&mode, player + kGrabMode, sizeof(mode));
        if (mode != 0) {
            return;
        }
        // Pull: the game's grab starts on it (through the held cast), and the grab update flies it to the hand.
        {
            std::scoped_lock _{ g_mutex };
            g_release      = false;
            g_release_keep = false;
            g_pull_ask     = PullAsk{ true, NowUs() };
        }
        g_hand        = hand;
        g_pull_handle = g_aim_target;
        g_pull_body   = g_lock_steered ? g_lock_body : 0;
        // (fix161) The glow off before the grab wakes the object (its effect, still playing, keeps the game from moving it with its body).
        GloveHighlight::Show(0, 0);
        t_hand_start  = true;
        const bool ok = CallPlayer(g_start_fn, player);
        t_hand_start  = false;
        g_aim_target  = 0; // pulled: not a lock that ended without one
        EndAim();
        if (!ok) {
            g_grip_ready = false;
            spdlog::error("[Grab] starting a grab failed with an exception; grip grabbing is off for this session");
            return;
        }
        g_grip_grab         = true;
        g_catch_deadline_us = 0;
        g_needs_catch       = !grip_held; // let go as it flicked: squeeze again to catch it
        const int n   = g_pulls.fetch_add(1) + 1;
        haptics::MeleePulse(hand == 0, 0.35f, 0.12f);
        if (n <= 20 || n % 10 == 0) {
            spdlog::info("[Grab] {} hand: flick ({:.1f} m/s toward you): pulling what you locked on, {:.1f} m away{}", hand == 0 ? "left" : "right",
                speed / scale, distance, grip_held ? "" : " (you let go of the grip as you flicked: squeeze it to catch the object)");
        }
    }

    // ---- Eating and drinking (Consume.h): food or a drink held at your mouth, as HIGGS ----
    // The mouth: in front of and below your eyes (HIGGS: 10 Skyrim units forward and 9 down from the headset, 14 and 13 cm); the
    // palm within kMouthRadius of it (HIGGS 11 units, 16 cm), the hand moving slower than kMouthSpeed on average (HIGGS 2 m/s).
    constexpr float kMouthForward = 0.13f;
    constexpr float kMouthDown    = 0.12f;
    constexpr float kMouthRadius  = 0.17f;   // (the test build's log: held to the mouth, the palm came 8-15 cm from the point)
    constexpr int64_t kMouthGraceUs = 300'000; // let go within this long of being at the mouth still counts (the hand moves as it opens)
    constexpr float kMouthSpeed   = 2.0f;
    constexpr float kMouthBuzz    = 0.15f; // the steady buzz while it is there (HIGGS 0.3 in Skyrim's scale)
    struct MouthState {
        bool      at{ false };   // it is at your mouth now
        uintptr_t ref{ 0 };      // the held object last checked...
        int       serial{ -1 };  // ...in this grab...
        bool      food{ false }; // ...and whether it is food or a drink
        uint32_t  base_id{ 0 };
        float     distance{ 0.0f };
        bool      live{ false };     // the last check got as far as measuring (food held, eating on, not flying)
        int64_t   at_us{ 0 };        // when it was last at your mouth (this object, this hand)
        int       hand{ -1 };        // the hand last checked
        float     speed{ 0.0f };     // its speed then (m/s, average of the last frames)
        // The closest the palm came to the mouth point since the last log line, where it was then (in the headset's frame,
        // metres: x right, y up, z back) and how fast the hand moved, for setting the mouth point from real use.
        float     closest{ 1e9f };
        glm::vec3 closest_local{ 0.0f };
        float     closest_speed{ 0.0f };
        int64_t   log_us{ 0 };
    };
    static MouthState g_mouth{}; // main thread

    // ---- What a grip holds: its kind, for eating (above), the bag and picked-up weapons (below) ----
    struct HeldItem {
        bool      live{ false };   // read this frame: held by a grip, not flying to the hand
        uintptr_t ref{ 0 };
        uintptr_t spring{ 0 };     // (a new grab: a new spring)
        uint8_t   type{ 0 };       // its base form's type (0 unreadable)
        uintptr_t base{ 0 };
        uint32_t  base_id{ 0 };
        uint32_t  weapon_type{ 0xffffffffu }; // a weapon's type (Fallout 4's numbering: 1-6 melee)
    };
    static HeldItem g_item{}; // main thread

    // Reads what the grip holds (main thread); false if nothing (or it still flies to the hand).
    static bool ReadHeldItem()
    {
        g_item.live = false;
        uintptr_t ref = 0, spring = 0;
        {
            std::scoped_lock _{ g_mutex };
            if (g_pull.active || g_pull_ask.pending) {
                return false;
            }
            ref    = g_held_object;
            spring = g_spring;
        }
        if (ref == 0) {
            return false;
        }
        if (ref != g_item.ref || spring != g_item.spring) {
            g_item         = HeldItem{};
            g_item.ref     = ref;
            g_item.spring  = spring;
            g_item.type    = Consume::BaseOf(ref, &g_item.base, &g_item.base_id);
            if (g_item.type == kFormTypeWeapon && g_item.base != 0) {
                uintptr_t instance = 0, general = 0;
                uint32_t  weapon_type = 0xffffffffu;
                if (SafeRead(&instance, g_item.base + kWeaponInstance, sizeof(instance)) && instance != 0 &&
                    SafeRead(&general, instance + kInstanceGeneral, sizeof(general)) && general != 0 &&
                    SafeRead(&weapon_type, general + kGeneralType, sizeof(weapon_type))) {
                    g_item.weapon_type = weapon_type;
                }
            }
        }
        g_item.live = true;
        return true;
    }

    // Average hand speed over the last frames (m/s).
    static float HandSpeed(int hand)
    {
        float speed = 0.0f;
        for (int i = 0; i < kHistory; ++i) {
            speed += glm::length(g_linear_history[hand][i]);
        }
        return speed / static_cast<float>(kHistory);
    }

    // What the hand holds (its grab started by a grip) is food or a drink, at your mouth (main thread; ReadHeldItem first).
    static bool AtMouth(int hand)
    {
        g_mouth.live = false;
        if (!GameFlow::gStore.internalSettings.mouthEat || hand < 0 || hand > 1 || !g_item.live) {
            return false;
        }
        const int serial = static_cast<int>(g_item.spring & 0x7FFFFFFF);
        if (g_item.ref != g_mouth.ref || serial != g_mouth.serial) {
            g_mouth.ref     = g_item.ref;
            g_mouth.serial  = serial;
            g_mouth.base_id = g_item.base_id;
            g_mouth.food    = g_item.type == 0x36;
            g_mouth.closest = 1e9f;
            g_mouth.at_us   = 0;
        }
        if (hand != g_mouth.hand) {
            g_mouth.hand  = hand;
            g_mouth.at_us = 0; // (passed to the other hand)
        }
        if (!g_mouth.food) {
            return false;
        }
        const glm::mat4 head = tracking::HeadPose(); // (stage space, metres: the same as the controllers)
        const glm::vec3 eyes{ head[3] };
        const glm::vec3 forward = -glm::vec3{ head[2] }; // the headset looks along its -Z
        const glm::vec3 up{ head[1] };
        const glm::vec3 mouth = eyes + kMouthForward * forward - kMouthDown * up;
        const glm::vec3 palm{ tracking::GripPose(hand == 0)[3] };
        if (!Finite(mouth) || !Finite(palm) || glm::length(forward) < 0.5f) {
            return false;
        }
        const float speed = HandSpeed(hand);
        g_mouth.speed     = speed;
        g_mouth.distance  = glm::length(palm - mouth);
        if (g_mouth.distance < g_mouth.closest) {
            g_mouth.closest       = g_mouth.distance;
            g_mouth.closest_local = glm::transpose(glm::mat3{ head }) * (palm - eyes);
            g_mouth.closest_speed = speed;
        }
        g_mouth.live = true;
        return g_mouth.distance < kMouthRadius && speed < kMouthSpeed;
    }

    // ---- The bag: let go of something you can carry behind either shoulder or at either hip (HIGGS's shoulder stash) ----
    // The shoulders: in your head's level frame (its yaw only), as the grenade's (HandActions; HIGGS: 25 cm out, 7 back, 10 down,
    // 16 cm across), with the hand at least 4 cm behind your eyes. They move with your head, as your shoulders do.
    // The hips: seated, your hips stay on the chair while your head leans and turns, so they are placed from your neck (a point
    // behind and below your eyes that stays put when you look down or turn your head: the head turns about it) in your body's
    // facing (which follows your head only when it turns well away, or slowly), and they are the whole side of your hip: from
    // 28 to 75 cm below your neck (waist to seat, seated or standing), 6 to 50 cm out to the side, and from 45 cm behind to 15
    // cm in front of your neck (leaning forward moves your neck ahead of your hips). Where you hold things in front of you stays
    // outside. A slow hand (HIGGS: 2 m/s).
    constexpr float   kBagShoulderSide   = 0.20f;
    constexpr float   kBagShoulderDown   = 0.10f;
    constexpr float   kBagShoulderBack   = 0.10f;
    constexpr float   kBagShoulderRadius = 0.18f;
    constexpr float   kBagShoulderFront  = 0.04f;
    constexpr float   kNeckBack          = 0.08f; // the neck's turning point from the eyes, in the head's frame
    constexpr float   kNeckDown          = 0.11f;
    constexpr float   kBagHipDownMin     = 0.28f; // below the neck
    constexpr float   kBagHipDownMax     = 0.75f;
    constexpr float   kBagHipSideMin     = 0.06f; // out to the side from the neck
    constexpr float   kBagHipSideMax     = 0.50f;
    constexpr float   kBagHipFrontMin    = -0.45f; // (behind the neck)
    constexpr float   kBagHipFrontMax    = 0.15f;
    constexpr float   kBodyYawFollow     = glm::radians(45.0f); // the body turns with the head beyond this...
    constexpr float   kBodyYawRelaxS     = 4.0f;                // ...and slowly toward it within it
    constexpr float   kBagSpeed          = 2.0f;
    constexpr float   kBagBuzz           = 0.12f; // (HIGGS 0.2 in Skyrim's scale)
    constexpr int64_t kBagGraceUs        = 500'000; // let go this soon after leaving the zone still counts (a tracking hiccup)...
    constexpr float   kBagGraceSlowOutside = 0.25f; // ...with the hand slow and near the zone,
    constexpr float   kBagGraceOutside   = 0.10f;   // or right next to it and not much faster (a toss is not the bag)
    constexpr float   kBagGraceSpeed     = 2.5f;
    constexpr const char* kZoneNames[]   = { "none", "left shoulder", "right shoulder", "left hip", "right hip" };

    // Your body's facing (stage space, level), each frame (main thread).
    struct BodyFacing {
        bool      have{ false };
        glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
        int64_t   us{ 0 };
    };
    static BodyFacing g_body_facing{};

    static void UpdateBodyFacing(int64_t now)
    {
        const glm::mat4 head = tracking::HeadPose();
        glm::vec3       look = -glm::vec3{ head[2] };
        look.y               = 0.0f;
        const float     dt   = g_body_facing.us != 0 ? std::clamp(static_cast<float>(now - g_body_facing.us) * 1e-6f, 0.0f, 0.1f) : 0.0f;
        g_body_facing.us     = now;
        if (!Finite(look) || glm::length(look) < 0.2f) {
            return; // looking straight down or up: the facing stays
        }
        look = glm::normalize(look);
        if (!g_body_facing.have) {
            g_body_facing.have    = true;
            g_body_facing.forward = look;
            return;
        }
        // Signed angle from the body's facing to the head's (about up).
        const glm::vec3 f     = g_body_facing.forward;
        float           angle = std::atan2(f.x * look.z - f.z * look.x, glm::dot(f, look));
        float           turn  = 0.0f;
        if (std::abs(angle) > kBodyYawFollow) {
            turn = angle - std::copysign(kBodyYawFollow, angle);
            angle -= turn;
        }
        turn += angle * (1.0f - std::exp(-dt / kBodyYawRelaxS));
        const float c = std::cos(turn), s = std::sin(turn);
        // Turning by +angle goes from f toward look (the same sense as the angle above): x' = x c - z s, z' = x s + z c.
        glm::vec3 next{ f.x * c - f.z * s, 0.0f, f.x * s + f.z * c };
        if (Finite(next) && glm::length(next) > 0.5f) {
            g_body_facing.forward = glm::normalize(next);
        }
    }

    struct BagZoneHit {
        int       zone{ 0 };        // 1-4 as kZoneNames (0: none)
        int       nearest{ 0 };     // the nearest zone...
        float     outside{ 1e9f };  // ...and how far outside it the hand is (0 or less: inside)
        float     zone_outside[5]{ 1e9f, 1e9f, 1e9f, 1e9f, 1e9f }; // the same for each zone
        glm::vec3 body{ 0.0f };     // the hand from your neck in your body's facing (x right, y up, z forward), metres
    };

    // How far a value is outside [lo, hi] (negative inside: the margin to the nearer edge).
    static float OutsideRange(float v, float lo, float hi)
    {
        return std::max(lo - v, v - hi);
    }

    // Which bag zone a hand (stage space) is in.
    static BagZoneHit BagZone(const glm::vec3& hand)
    {
        BagZoneHit      hit;
        const glm::mat4 head = tracking::HeadPose();
        const glm::vec3 eyes{ head[3] };
        glm::vec3       forward = -glm::vec3{ head[2] };
        forward.y               = 0.0f;
        if (!Finite(hand) || !Finite(eyes)) {
            return hit;
        }
        // Looking (nearly) straight down or up: no level facing for the head, so only the hips (from the body's facing) count.
        const bool head_level = glm::length(forward) >= 0.2f;
        if (head_level) {
            forward = glm::normalize(forward);
        } else if (!g_body_facing.have) {
            return hit;
        }
        const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        const glm::vec3 right = head_level ? glm::normalize(glm::cross(forward, up)) : glm::vec3{ 0.0f };
        const glm::vec3 rel   = hand - eyes;
        const float     back  = head_level ? -glm::dot(rel, forward) : 0.0f;
        // The neck (stays put as the head turns about it) and the body's facing, for the hips.
        const glm::vec3 neck  = eyes + glm::vec3{ head[2] } * kNeckBack - glm::vec3{ head[1] } * kNeckDown;
        const glm::vec3 body_forward = g_body_facing.have ? g_body_facing.forward : forward;
        const glm::vec3 body_right   = glm::normalize(glm::cross(body_forward, up));
        const glm::vec3 from_neck    = hand - neck;
        hit.body = glm::vec3{ glm::dot(from_neck, body_right), from_neck.y, glm::dot(from_neck, body_forward) };
        for (int zone = 1; zone <= 4; ++zone) {
            const bool  shoulder = zone <= 2;
            if (shoulder && !head_level) {
                continue;
            }
            const float side     = (zone % 2 == 0) ? 1.0f : -1.0f; // (2, 4: right)
            float       outside  = 0.0f;
            if (shoulder) {
                const glm::vec3 centre = eyes + right * (side * kBagShoulderSide) - up * kBagShoulderDown - forward * kBagShoulderBack;
                outside                = std::max(glm::length(hand - centre) - kBagShoulderRadius, kBagShoulderFront - back);
            } else {
                outside = std::max({ OutsideRange(-hit.body.y, kBagHipDownMin, kBagHipDownMax),
                    OutsideRange(hit.body.x * side, kBagHipSideMin, kBagHipSideMax), OutsideRange(hit.body.z, kBagHipFrontMin, kBagHipFrontMax) });
                if (outside > 0.0f) {
                    // Outside the box: the distance to it.
                    const float dy = std::max(0.0f, OutsideRange(-hit.body.y, kBagHipDownMin, kBagHipDownMax));
                    const float dx = std::max(0.0f, OutsideRange(hit.body.x * side, kBagHipSideMin, kBagHipSideMax));
                    const float dz = std::max(0.0f, OutsideRange(hit.body.z, kBagHipFrontMin, kBagHipFrontMax));
                    outside        = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            hit.zone_outside[zone] = outside;
            if (outside < hit.outside) {
                hit.nearest = zone;
                hit.outside = outside;
            }
            if (outside <= 0.0f && hit.zone == 0) {
                hit.zone = zone;
            }
        }
        return hit;
    }

    struct BagState {
        bool      at{ false };      // the held thing is in a bag zone now
        int       zone{ 0 };
        int       last_zone{ 0 };   // the zone it was last in
        int64_t   at_us{ 0 };       // last in a zone (this object, this hand)
        uintptr_t ref{ 0 };
        int       hand{ -1 };
        float     speed{ 0.0f };
        BagZoneHit now{};           // this frame
        // The nearest any zone came since the last log line (how far outside its edge), for setting the zones from real use.
        float     closest{ 1e9f };
        int       closest_zone{ 0 };
        glm::vec3 closest_body{ 0.0f };
        int64_t   log_us{ 0 };
    };
    static BagState g_bag{}; // main thread

    // The held thing (ReadHeldItem first) can go in the bag and is in a bag zone, slowly (main thread).
    static bool AtBag(int hand)
    {
        if (!GameFlow::gStore.internalSettings.bagStash || hand < 0 || hand > 1 || !g_item.live || !Consume::IsCarried(g_item.type)) {
            return false;
        }
        if (g_item.ref != g_bag.ref || hand != g_bag.hand) {
            g_bag.ref       = g_item.ref;
            g_bag.hand      = hand;
            g_bag.at_us     = 0;
            g_bag.last_zone = 0;
            g_bag.closest   = 1e9f;
        }
        const BagZoneHit hit = BagZone(glm::vec3{ tracking::GripPose(hand == 0)[3] });
        g_bag.now            = hit;
        g_bag.speed          = HandSpeed(hand);
        g_bag.zone           = hit.zone;
        if (hit.nearest != 0 && hit.outside < g_bag.closest) {
            g_bag.closest      = hit.outside;
            g_bag.closest_zone = hit.nearest;
            g_bag.closest_body = hit.body;
        }
        return hit.zone != 0 && g_bag.speed < kBagSpeed;
    }

    // Where the hand was, for the log (from your neck, in your body's facing).
    static std::string BagWhere(const glm::vec3& body)
    {
        return fmt::format("{:.2f} m to the {}, {:.2f} m {} and {:.2f} m {} your neck", std::abs(body.x), body.x >= 0.0f ? "right" : "left", std::abs(body.y),
            body.y <= 0.0f ? "below" : "above", std::abs(body.z), body.z >= 0.0f ? "in front of" : "behind");
    }

    // ---- Picked-up weapons: pull the trigger with a weapon in your grip and it is equipped; your weapon
    // before comes back when it leaves your hands (dropped from the inventory) or goes in the bag (the gun hand behind a shoulder:
    // squeeze and let go of the grip there; with no weapon before, it is unequipped (put away if that is unavailable)). Equipping is by the weapon's base form, as
    // script EquipItem does: with two of the same weapon the game picks which one. ----
    struct AutoWeapon {
        int       state{ 0 };       // 0 none, 1 taking and equipping, 2 equipped (not yet seen in your hands), 3 in your hands
        uintptr_t base{ 0 };
        uint32_t  base_id{ 0 };
        uintptr_t previous{ 0 };    // your weapon before (0: fists or none)
        uint32_t  previous_id{ 0 };
        int64_t   since_us{ 0 };
        int64_t   away_us{ 0 };     // since when another (fists, nothing) is in your hands instead
        bool      grip_was{ false };
        bool      bag_armed{ false }; // the gun hand's grip squeezed in a bag zone
        int64_t   bag_at_us{ 0 };
        int       bag_zone{ 0 };
        bool      menu_seen{ false }; // a menu was open since it was last in your hands (dropping it takes the inventory)
        int64_t   loading{ 0 };       // the last loading screen when it started (another one: forgotten)
        // Ready to fire: once equipped (the game swaps it at once; a moment later) a put-away weapon is drawn with a tap of
        // reload (the game's "press reload to draw"); once it is out, a right trigger still held since it equipped the weapon
        // fires it (a fresh press for the game). 0 done, 1 waiting to draw, 2 drawing, 3 firing.
        int       ready{ 0 };
        int64_t   ready_us{ 0 };
        int64_t   out_us{ 0 };           // when it was first seen out (the draw then plays a moment before it can fire)
        int64_t   tap_us{ 0 };           // the last reload tap
        int       taps{ 0 };
        bool      fire_on_draw{ false }; // equipped by the right trigger (the game's fire)
    };
    static AutoWeapon        g_auto{};                       // main thread
    static std::atomic<bool> g_auto_in_hands{ false };       // state 3 with it drawn (any thread)
    static std::atomic<bool> g_auto_bag_zone{ false };       // ...and the gun hand in a bag zone
    static std::atomic<bool> g_trigger_block[2]{};           // this trigger started an equip: not the game's until let go
    static std::atomic<bool> g_weapon_in_grip[2]{};          // the grip holds a weapon
    static std::atomic<int64_t> g_holster_until_us{ 0 };     // hold the game's holster button (X, reload held) until then
    static std::atomic<int64_t> g_reload_tap_until_us{ 0 };  // tap reload (X) until then: draws a put-away weapon
    static std::atomic<int>     g_fire_override{ -1 };       // the game's fire (RT): -1 as the trigger says, 1 pressed...
    static std::atomic<int64_t> g_fire_until_us{ 0 };        // ...while renewed by the frame's update (it lapses if that stops)
    constexpr int64_t           kReadyDelayUs  = 250'000;     // after the equip before drawing (the game swaps at once; the log: ~30 ms)
    constexpr int64_t           kReloadTapUs   = 150'000;
    constexpr int64_t           kRetapUs       = 600'000;     // still put away this long after a tap: tapped once more
    constexpr int64_t           kDrawTimeoutUs = 3'000'000;
    constexpr int64_t           kDrawSettleUs  = 400'000;     // seen out until the fire press (the draw animation finishing)
    static bool              g_trigger_down[2]{ false, false };
    // (fix170 with fix171; the merge review) A left-grip squeeze while the right hand holds something, waiting (see Update), and how long.
    struct OtherAsk {
        bool    pending{ false };
        int     holder{ -1 };
        int64_t us{ 0 };
    };
    static OtherAsk          g_other_ask{};
    static constexpr int64_t kOtherWaitUs = 200'000;
    static int64_t           g_grip_pressed_us[2]{ 0, 0 }; // (fix171 with fix172: when each grip went down)
    constexpr float          kTriggerOn  = 0.7f;
    constexpr float          kTriggerOff = 0.4f;
    constexpr int64_t        kHolsterUs   = 900'000;

    // A trigger's pull (0..1), by the hand grab's hands (they follow "Swap Controller Inputs").
    static float TriggerValue(int hand)
    {
        static auto vr = VR::get();
        return vr->get_finger_state(ControllerIndex(hand) == 1).trigger;
    }

    static void SwitchBack(const char* why, bool bagged)
    {
        auto& a = g_auto;
        if (a.previous != 0) {
            if (Consume::QueueEquip(a.previous, "bringing back your weapon")) {
                spdlog::info("[Grab] picked-up weapon {:08X} {}: your weapon {:08X} is brought back", a.base_id, why, a.previous_id);
            }
        } else if (bagged && Consume::Unequip(a.base, "put in your bag with no weapon before it")) {
            spdlog::info("[Grab] picked-up weapon {:08X} {}: you had no weapon in your hands before (fists), so it is unequipped (your fists come "
                         "back)",
                a.base_id, why);
        } else if (bagged) {
            g_holster_until_us = NowUs() + kHolsterUs;
            spdlog::info("[Grab] picked-up weapon {:08X} {}: you had no weapon in your hands before (fists), so it is put away", a.base_id, why);
        } else {
            spdlog::info("[Grab] picked-up weapon {:08X} {}: you had no weapon in your hands before (fists), so nothing is brought back", a.base_id, why);
        }
        a = AutoWeapon{};
        g_auto_in_hands = false;
        g_auto_bag_zone = false;
        g_fire_override = -1;
    }

    // Ready to fire (see AutoWeapon::ready), each frame while a picked-up weapon is equipped (main thread).
    static void UpdateReady(AutoWeapon& a, int64_t now, bool drawn, uint32_t drawn_id)
    {
        // The weapon's state: 0 put away, 1-2 drawing, 3 out, 4-5 putting away.
        const int state = GameFlow::weaponState();
        switch (a.ready) {
        case 1:
            if (now - a.ready_us < kReadyDelayUs || GameFlow::isShowingMenu() || GameFlow::isWorkshopShown()) { // (fix175: nor build mode: X deletes there)
                return;
            }
            if (state == 4 || state == 5) {
                if (now - a.ready_us < kDrawTimeoutUs) {
                    return; // (being put away: drawn once it is)
                }
            } else if (state == 0) {
                g_reload_tap_until_us = now + kReloadTapUs;
                a.tap_us              = now;
                a.taps                = 1;
                spdlog::info("[Grab] picked-up weapon {:08X}: drawing it (a tap of reload, as the game draws a put-away weapon)", a.base_id);
            }
            a.ready    = 2;
            a.ready_us = now;
            return;
        case 2:
            if (drawn && drawn_id == a.base_id) {
                if (a.out_us == 0) {
                    a.out_us = now;
                }
                if (now - a.out_us >= kDrawSettleUs) {
                    a.ready = 3;
                    spdlog::info("[Grab] picked-up weapon {:08X} is out ({:.2f} s after it was equipped){}", a.base_id, (now - a.since_us) * 1e-6,
                        a.fire_on_draw && g_trigger_block[1].load() ? "; your trigger is still held: it fires" : "");
                }
                return;
            }
            a.out_us = 0;
            if (state == 0 && a.taps < 2 && now - a.tap_us > kRetapUs && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown()) {
                g_reload_tap_until_us = now + kReloadTapUs; // (the tap was missed: a hitch as the weapon loaded)
                a.tap_us              = now;
                ++a.taps;
                spdlog::info("[Grab] picked-up weapon {:08X}: still put away, reload tapped again", a.base_id);
            } else if (now - a.ready_us > kDrawTimeoutUs) {
                a.ready = 0;
                spdlog::info("[Grab] picked-up weapon {:08X}: not out after {:.0f} s (pull the trigger to draw it)", a.base_id, kDrawTimeoutUs * 1e-6);
            }
            return;
        case 3:
            // The right trigger held since it equipped the weapon (its block lifts when let go): the game's fire, freshly pressed.
            if (a.fire_on_draw && g_trigger_block[1].load() && drawn && drawn_id == a.base_id) {
                g_fire_override = 1;
                g_fire_until_us = now + 150'000;
            } else {
                g_fire_override = -1;
                a.ready         = 0;
            }
            return;
        default:
            g_fire_override = -1;
            return;
        }
    }

    // Once per frame (main thread), after the grips: the picked-up weapon's life.
    static void UpdateAutoWeapon()
    {
        auto&         a   = g_auto;
        const int64_t now = NowUs();
        // A trigger that started an equip is the game's again once let go.
        for (int hand = 0; hand < 2; ++hand) {
            if (g_trigger_block[hand].load() && TriggerValue(hand) < kTriggerOff) {
                g_trigger_block[hand] = false;
            }
        }
        if (a.state == 0) {
            g_auto_in_hands = false;
            g_auto_bag_zone = false;
            g_fire_override = -1;
            return;
        }
        if (!GameFlow::gStore.internalSettings.autoEquipWeapons || GameFlow::lastLoadingScreen() != a.loading) {
            spdlog::info("[Grab] picked-up weapon {:08X}: {}, so your weapon before is not brought back later", a.base_id,
                GameFlow::gStore.internalSettings.autoEquipWeapons ? "a loading screen came" : "the setting was turned off");
            a               = AutoWeapon{};
            g_auto_in_hands = false;
            g_auto_bag_zone = false;
            g_fire_override = -1;
            return;
        }
        const bool grip_now = g_grip_down[1];
        const bool grip_was = a.grip_was;
        a.grip_was          = grip_now;
        if (a.state == 1) {
            uintptr_t  base = 0;
            const int  result = Consume::WeaponResult(base);
            if (result < 0 || (result != 0 && base != a.base)) {
                spdlog::info("[Grab] picked-up weapon {:08X}: not equipped", a.base_id);
                a = AutoWeapon{};
            } else if (result > 0) {
                a.state    = 2;
                a.since_us = now;
                a.ready    = 1;
                a.ready_us = now;
                spdlog::info("[Grab] picked-up weapon {:08X}: equipped (your weapon before: {})", a.base_id,
                    a.previous != 0 ? fmt::format("{:08X}", a.previous_id) : std::string{ "fists or none" });
            } else if (now - a.since_us > 60'000'000) {
                a = AutoWeapon{}; // (a safety net: every way the taking ends gives a result)
            }
            return;
        }
        if (GameFlow::isShowingMenu()) {
            a.menu_seen = true;
        }
        const bool drawn    = GameFlow::isWeaponDrawn() && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown(); // (fix175: nor build mode)
        uint32_t   drawn_id = 0;
        if (drawn) {
            std::scoped_lock _{ g_armed_mutex };
            drawn_id = ArmedNowLocked().weapon_id; // (0: fists or none)
        }
        UpdateReady(a, now, drawn, drawn_id);
        if (!drawn) {
            a.bag_armed     = false;
            a.away_us       = 0;
            g_auto_in_hands = false;
            g_auto_bag_zone = false;
            return;
        }
        if (a.state == 2) {
            if (drawn_id == a.base_id) {
                a.state = 3;
                spdlog::info("[Grab] picked-up weapon {:08X} is in your hands (put it in the bag: your gun hand behind a shoulder, squeeze and "
                             "let go of the grip there)",
                    a.base_id);
            } else if (drawn_id != 0 && now - a.since_us > 3'000'000) {
                spdlog::info("[Grab] picked-up weapon {:08X}: another weapon ({:08X}) is in your hands, so yours is not brought back later", a.base_id, drawn_id);
                a = AutoWeapon{};
            }
            return;
        }
        // In your hands (state 3).
        if (drawn_id == a.base_id) {
            a.away_us       = 0;
            a.menu_seen     = false;
            g_auto_in_hands = true;
            // The bag for the weapon in your hands: behind either shoulder only (a lowered gun hand rests at the hip), the hand slow.
            const BagZoneHit hit  = BagZone(glm::vec3{ tracking::GripPose(false)[3] }); // (the gun hand)
            const bool       zone = (hit.zone == 1 || hit.zone == 2) && GameFlow::gStore.internalSettings.bagStash && HandSpeed(1) < kBagSpeed;
            g_auto_bag_zone       = zone;
            if (zone) {
                a.bag_at_us = now;
                a.bag_zone  = hit.zone;
                haptics::MeleePulse(false, kBagBuzz, 0.05f);
            }
            if (grip_now && !grip_was) {
                a.bag_armed = zone; // squeezed in the zone
            }
            // (Let go there, or just after with the hand still slow and next to it: a fast let-go is a throw, not the bag.)
            if (!grip_now && grip_was && a.bag_armed && now - a.bag_at_us < kBagGraceUs &&
                (zone || (HandSpeed(1) < kBagSpeed && std::min(hit.zone_outside[1], hit.zone_outside[2]) < kBagGraceOutside))) {
                haptics::MeleePulse(false, 0.5f, 0.2f);
                SwitchBack(fmt::format("put in your bag ({})", kZoneNames[a.bag_zone]).c_str(), true);
                return;
            }
            if (!grip_now) {
                a.bag_armed = false;
            }
            return;
        }
        g_auto_in_hands = false;
        g_auto_bag_zone = false;
        if (drawn_id != 0) {
            spdlog::info("[Grab] picked-up weapon {:08X}: you took another weapon ({:08X}) into your hands, so yours is not brought back", a.base_id,
                drawn_id);
            a = AutoWeapon{};
            return;
        }
        // Fists or nothing out instead of it, for a second, after a menu (dropping it takes the inventory): it left your hands.
        // (Without a menu in between it was only put away and your fists brought out.)
        if (!a.menu_seen) {
            a.away_us = 0;
            return;
        }
        if (a.away_us == 0) {
            a.away_us = now;
        }
        if (now - a.away_us > 1'000'000) {
            SwitchBack("left your hands (dropped)", false);
        }
    }

    // A weapon in the grip: pull the trigger to equip it (main thread, in the grips' update).
    static bool StartWeaponEquip(int hand)
    {
        uintptr_t previous    = 0;
        uint32_t  previous_id = 0;
        if (g_auto.state != 0) {
            previous    = g_auto.previous; // (a weapon picked up while another picked-up one is yours: yours from before comes back)
            previous_id = g_auto.previous_id;
        } else {
            std::scoped_lock _{ g_armed_mutex };
            const auto& seen = ArmedNowLocked();
            previous         = seen.weapon_form; // (0: fists or none)
            previous_id      = seen.weapon_id;
        }
        const uintptr_t ref = g_item.ref;
        if (!Consume::AfterRelease(Consume::Action::Weapon, ref, hand)) {
            return false;
        }
        // Let go of it (no throw); this squeeze is then nobody's until the grip is let go.
        {
            std::scoped_lock _{ g_mutex };
            g_release       = true;
            g_release_keep  = false;
            g_throw_linear  = glm::vec3{ 0.0f };
            g_throw_angular = glm::vec3{ 0.0f };
            g_support       = Support{}; // (fix170: taken as your weapon: no second hand on it; the review)
            g_support_hand  = -1;
        }
        g_grip_grab = false;
        LeaveGrip(hand == 0);
        g_trigger_block[hand] = true;
        g_auto                = AutoWeapon{ 1, g_item.base, g_item.base_id, previous == g_item.base ? 0 : previous, previous == g_item.base ? 0 : previous_id,
                           NowUs() };
        g_auto.loading        = GameFlow::lastLoadingScreen();
        // (A melee weapon does not attack with the trigger still held once it is out: you swing it.)
        const bool melee      = g_item.weapon_type >= kWeaponMeleeFirst && g_item.weapon_type <= kWeaponMeleeLast;
        g_auto.fire_on_draw   = hand == 1 && !melee;
        g_fire_override       = -1;
        haptics::MeleePulse(hand == 0, 0.45f, 0.08f);
        spdlog::info("[Grab] {} hand: trigger pulled with weapon {:08X} (type {}{}) in your grip: equipping it (your weapon now: {})",
            hand == 0 ? "left" : "right", g_item.base_id, g_item.weapon_type, melee ? ", melee" : "",
            previous != 0 ? fmt::format("{:08X}", previous_id) : std::string{ "fists or none" });
        return true;
    }

    static void UpdateGrips()
    {
        static auto vr       = VR::get();
        const auto& settings = GameFlow::gStore.internalSettings;
        auto*       player   = CreationEngineSingletonManager::GetPlayerRef();
        const bool  use_grip = g_ready.load() && g_grip_ready.load() && settings.gripGrab && Active() && player != nullptr;

        // Hand velocities for throwing and flicks (stage space, metres and radians per second): the runtime's, or, when it
        // gives none (as impact melee allows for), from the controller's last two poses (they change once per new pose).
        const int64_t now_us = NowUs();
        for (int hand = 0; hand < 2; ++hand) {
            const auto      index = static_cast<uint32_t>(ControllerIndex(hand));
            const glm::mat4 grip  = tracking::GripPose(hand == 0);
            const glm::vec3 at{ grip[3] };
            const glm::mat3 turn{ grip };
            auto&           pose = g_pose_track[hand];
            if (Finite(at) && (!pose.have || at != pose.at || turn != pose.turn)) {
                const float dt = static_cast<float>(now_us - pose.us) * 1e-6f;
                if (pose.have && dt > 0.002f && dt < 0.2f) {
                    pose.linear       = (at - pose.at) / dt;
                    const glm::quat q = glm::normalize(glm::quat_cast(turn * glm::transpose(pose.turn)));
                    pose.angular      = glm::axis(q) * (glm::angle(q) > glm::pi<float>() ? glm::angle(q) - glm::two_pi<float>() : glm::angle(q)) / dt;
                    if (!Finite(pose.linear) || !Finite(pose.angular)) {
                        pose.linear = pose.angular = glm::vec3{ 0.0f };
                    }
                    // (fix189) A jump of over 0.3 m from one pose to the next, faster than any hand moves (the tracking lost and
                    // found again: the log's "the poses move 50.43 m/s"): no speed from it, rather than a throw flung its way.
                    if (glm::length(at - pose.at) > kPoseJump && glm::length(pose.linear) > kMaxHandSpeed) {
                        pose.linear = pose.angular = glm::vec3{ 0.0f };
                    }
                } else {
                    pose.linear = pose.angular = glm::vec3{ 0.0f };
                }
                pose.have = true;
                pose.at   = at;
                pose.turn = turn;
                pose.us   = now_us;
            }
            glm::vec3  linear{ vr->get_velocity(index) };
            glm::vec3  angular{ vr->get_angular_velocity(index) };
            const bool runtime = glm::length(linear) > 1e-5f || glm::length(angular) > 1e-5f;
            if (!runtime) {
                // No new pose for a while (tracking lost, or still): no speed rather than the last one again.
                const bool fresh = now_us - pose.us < 50'000;
                linear           = fresh ? pose.linear : glm::vec3{ 0.0f };
                angular          = fresh ? pose.angular : glm::vec3{ 0.0f };
            }
            // Which it is, once the hand first moves briskly.
            if (!g_speed_source_told && glm::length(pose.linear) > 0.5f) {
                g_speed_source_told = true;
                spdlog::info("[Grab] hand speeds: {} (the runtime gives {:.2f} m/s while the poses move {:.2f} m/s)",
                    runtime ? "from the runtime" : "from the controller poses (the runtime gives none)", glm::length(glm::vec3{ vr->get_velocity(index) }),
                    glm::length(pose.linear));
            }
            // (fix189) Each sample at most kMaxHandSpeed (the runtime's too): a throw is the average of four, a glitch's speed
            // no longer flings it. Real hands stay well under it, so normal throws are as before.
            if (const float fast = glm::length(linear); fast > kMaxHandSpeed && std::isfinite(fast)) {
                linear *= kMaxHandSpeed / fast;
            }
            g_linear_history[hand][g_history_next[hand]]  = linear;
            g_angular_history[hand][g_history_next[hand]] = angular;
            g_history_next[hand]                          = (g_history_next[hand] + 1) % kHistory;
        }
        bool pressed[2]{ false, false }, released[2]{ false, false };
        for (int hand = 0; hand < 2; ++hand) {
            // Finger input is by physical controller; hands here follow "Swap Controller Inputs" like the poses.
            const float grip  = vr->get_finger_state(ControllerIndex(hand) == 1).grip;
            const bool  down  = g_grip_down[hand] ? grip > kGripOff : grip > kGripOn;
            pressed[hand]     = down && !g_grip_down[hand];
            released[hand]    = !down && g_grip_down[hand];
            g_grip_down[hand] = down;
            if (pressed[hand]) {
                g_grip_pressed_us[hand] = now_us; // (fix171 with fix172: the left grip's grab waits from here)
            }
            // (fix172) Physical asked for this grip's grab again (a person it knocked down is down now): taken as a fresh squeeze.
            if (g_grab_again[hand].exchange(false) && down) {
                pressed[hand] = true;
            }
        }
        // (fix154) The left grip is the left hand's weapon's while it holds one (dual wielding): its squeeze starts no grab and no gloves'
        // aim; the left grip's d-pad stays (the review: LeaveGrip would take that too).
        if (g_left_weapon_grip.load(std::memory_order_relaxed)) {
            pressed[0] = false;
            if (g_aim_hand.load() == 0) {
                EndAim("the left hand holds its weapon");
            }
            if (g_start_hand.load() == 0) {
                g_start_hand = -1;
                PointInteract::SetPointingHand(false);
            }
        }
        for (int hand = 0; hand < 2; ++hand) {
            const int alone = g_left_alone[hand].load();
            if (alone == 0) {
                continue;
            }
            if (!g_grip_down[hand]) {
                // Let go: the next squeeze is the hand grab's again (one not seen squeezed yet is waited for a few frames).
                if (alone == 2 || g_frame.load(std::memory_order_relaxed) > g_left_alone_frame[hand].load() + 5) {
                    g_left_alone[hand] = 0;
                }
                continue;
            }
            g_left_alone[hand] = 2;
            pressed[hand] = false; // another action has this squeeze
            if (g_aim_hand.load() == hand) {
                // (fix189) Climbing takes it too (it runs after the grab each frame): the log no longer names only two of them.
                EndAim("another hand action took the grip (climbing, a power push, a grenade, the wheel or the scanner)");
            }
            if (g_start_hand.load() == hand) {
                g_start_hand = -1;
                PointInteract::SetPointingHand(false);
            }
        }
        if (!g_grip_down[1]) {
            g_right_latched = false;
        } else if (pressed[1] && GripGrabsHere()) {
            g_right_latched = true;
        }
        // A grip squeezed with a weapon out does not grab: say so now and then (fists not recognised: every time, 10 s apart;
        // another weapon: the first few times, its right grip aims).
        if ((pressed[0] || pressed[1]) && use_grip && GameFlow::isWeaponDrawn() && !HandsFreeNow()) {
            static int64_t last_us = 0;
            static int     told    = 0;
            std::string    what;
            bool           unknown = false;
            {
                std::scoped_lock _{ g_armed_mutex };
                what    = g_armed.what;
                unknown = g_armed.armed == Armed::Unknown;
            }
            if ((unknown && now_us - last_us > 10'000'000) || (!unknown && told < 3)) {
                last_us = now_us;
                ++told;
                spdlog::info("[Grab] {} grip: your weapon is out ({}), so it does not grab or pull; hold the reload button (fix194: X as printed, "
                             "Y in the old layout) to put it away",
                    pressed[0] ? "left" : "right", what);
            }
        }
        // A grip squeezed with the hands free that cannot grab because hand interaction is off this frame: say why, now and then.
        if ((pressed[0] || pressed[1]) && use_grip && HandsFreeNow() && !tracking::HandInteract()) {
            static int64_t last_us = 0;
            if (now_us - last_us > 5'000'000) {
                last_us = now_us;
                spdlog::info("[Grab] {} grip: hand interaction is off right now (weapon {}, first person {}, menu {}, build mode {}, conversation {}, "
                             "pilot's seat {}, hand interaction setting {}), so it does not grab or aim",
                    pressed[0] ? "left" : "right", GameFlow::isWeaponDrawn() ? "out (fists)" : "put away", GameFlow::isInFirstPerson(),
                    GameFlow::isShowingMenu(), GameFlow::isWorkshopShown(), GameFlow::isInDialogue(), GameFlow::isPilotingShip(), settings.handInteract);
            }
        }
        const bool grabbing = player != nullptr && SpringCount(reinterpret_cast<uintptr_t>(player)) > 0;
        if (!grabbing) {
            StopTracking(); // dropped (the use button, too far away, a menu): the hand lets go
            std::scoped_lock _{ g_mutex };
            g_support        = Support{}; // (fix170: nothing held for a second hand; the review)
            g_support_hand   = -1;
            g_seat_keep_tilt = false;
            g_release      = false; // nothing left to let go of
            g_release_keep = false;
            if (player == nullptr || SpringCount(reinterpret_cast<uintptr_t>(player)) == 0) { // (still none: no grab started meanwhile)
                if (g_stats_pending) {
                    LogHolding(now_us, true); // the hold that just ended, however short
                }
                // The game let it go: the next grab is a new one even if its spring is made where this one was.
                g_spring      = 0;
                g_have_placed = false;
                g_held_object = 0;
            }
        }
        if (const int caught = g_caught_hand.exchange(-1); caught >= 0) {
            haptics::MeleePulse(caught == 0, 0.22f, 0.05f); // a pulled object reached the hand
        }
        if (g_far_frames_left > 0) {
            --g_far_frames_left;
        }
        if (!use_grip) {
            g_weapon_in_grip[0] = false;
            g_weapon_in_grip[1] = false;
            EndAim("grip grabbing is off here (a menu, the flat screen, the setting)");
            if (g_start_hand.load() >= 0 || PointInteract::PointingHandIsLeft()) {
                g_start_hand = -1;
                PointInteract::SetPointingHand(false);
            }
            if (!grabbing) {
                g_grip_grab = false;
                g_hand      = 1;
            }
            return;
        }
        const auto p = reinterpret_cast<uintptr_t>(player);

        // (fix170) The object a hand holds on its own (adopted): its grip let go -> thrown at the hand's speed and let go (done in the
        // player's update, OnGrabSlot).
        if (const int ah = g_adopted_hand.load(); ah >= 0 && (released[ah] || !g_grip_down[ah])) {
            glm::vec3 linear{}, angular{};
            ThrowVelocity(ah, linear, angular);
            {
                std::scoped_lock _{ g_mutex };
                if (g_adopted.active && !g_adopted.release) {
                    g_adopted.release  = true;
                    g_adopted.velocity = Finite(linear) ? linear : glm::vec3{ 0.0f };
                    g_adopted.why      = "you let go";
                    haptics::MeleePulse(ah == 0, 0.2f, 0.03f); // (once: the let-go itself waits for the player's update)
                }
            }
        }
        // (fix170) The held object kept by its hand on its own: the other hand starts its own grab with the game's (or, not kept,
        // it is passed to the other hand as before).
        if (const int result = g_adopt_result.exchange(0); result != 0) {
            const int other = g_adopt_other.load();
            if (result == 1 && other >= 0) {
                g_grip_grab   = false;
                g_needs_catch = false;
                g_hand        = other;
                // (Not into a grip another action took meanwhile: the favorites wheel's; the merge review.)
                if (g_grip_down[other] && g_left_alone[other].load() == 0 && g_start_hand.load() < 0 && tracking::HandInteract() && HandsFreeNow()) {
                    g_start_hand         = other;
                    const bool switching = PointInteract::PointingHandIsLeft() != (other == 0);
                    PointInteract::SetPointingHand(other == 0);
                    g_start_wait = std::max(switching ? 3 : 0, g_far_frames_left);
                }
                haptics::MeleePulse(other == 0, 0.35f, 0.04f);
            } else if (result == -1 && other >= 0 && g_grip_grab.load() && grabbing && g_grip_down[other] && g_left_alone[other].load() == 0 &&
                       g_hand.load() != other) {
                g_hand = other;
                std::scoped_lock _{ g_mutex };
                g_rebase       = true;
                g_need_contact = g_use_taken; // the new palm meets the box elsewhere
            }
        }

        // What the grip holds (for eating, the bag and picked-up weapons).
        g_mouth.live     = false;
        const bool held  = g_grip_grab.load() && grabbing && !g_needs_catch.load() && ReadHeldItem();
        for (int hand = 0; hand < 2; ++hand) {
            g_weapon_in_grip[hand] = held && g_hand.load() == hand && g_item.type == kFormTypeWeapon && settings.autoEquipWeapons && Consume::Ready();
        }
        // Food or a drink held at your mouth (as HIGGS): a steady light buzz while it is there; let go there to eat or drink it.
        const bool at_mouth = held && AtMouth(g_hand.load());
        // Something you can carry behind a shoulder or at a hip: a lighter buzz; let go there and it goes in your bag.
        const bool at_bag = held && !at_mouth && AtBag(g_hand.load());
        if (at_bag) {
            g_bag.at_us     = now_us;
            g_bag.last_zone = g_bag.zone;
        }
        if (at_bag != g_bag.at) {
            g_bag.at = at_bag;
            static int told = 0;
            if (at_bag && ++told <= 20) {
                spdlog::info("[Bag] what your {} hand holds ({:08X}) is at your {}: let go to put it in your bag", g_hand.load() == 0 ? "left" : "right",
                    g_item.base_id, kZoneNames[g_bag.zone]);
            }
        }
        // While holding something you can carry: now and then, how near the nearest bag zone your hand came.
        if (held && Consume::IsCarried(g_item.type) && GameFlow::gStore.internalSettings.bagStash && now_us - g_bag.log_us > 3'000'000 &&
            g_bag.closest < 0.5f) {
            g_bag.log_us     = now_us;
            static int lines = 0;
            if (++lines <= 40) {
                spdlog::info("[Bag] holding {:08X}: your hand came {:.2f} m {} the {} zone, at its nearest {}", g_item.base_id, std::abs(g_bag.closest),
                    g_bag.closest <= 0.0f ? "inside" : "outside", kZoneNames[g_bag.closest_zone], BagWhere(g_bag.closest_body));
            }
            g_bag.closest = 1e9f;
        }
        // A weapon in the grip: the trigger pulled equips it. (Melee weapons too: swinging one and letting go throws it, as any
        // object you hold.)
        for (int hand = 0; hand < 2; ++hand) {
            const float trigger  = TriggerValue(hand);
            const bool  was_down = g_trigger_down[hand];
            g_trigger_down[hand] = was_down ? trigger > kTriggerOff : trigger > kTriggerOn;
            const bool pulled    = g_trigger_down[hand] && !was_down;
            const bool mine      = held && g_hand.load() == hand && g_item.type == kFormTypeWeapon && settings.autoEquipWeapons && Consume::Ready();
            if (mine && pulled) {
                if (StartWeaponEquip(hand)) {
                    return; // (it is let go: nothing else for the grips this frame)
                }
            }
        }
        // While holding food or a drink: now and then, the closest your palm came to the mouth point.
        if (g_grip_grab.load() && g_mouth.live && g_mouth.food && now_us - g_mouth.log_us > 3'000'000 && g_mouth.closest < 0.6f) {
            g_mouth.log_us   = now_us;
            static int lines = 0;
            if (++lines <= 40) {
                const glm::vec3 at = g_mouth.closest_local;
                spdlog::info("[Eat] holding food or a drink ({:08X}): the palm came within {:.2f} m of the mouth point (counts below {:.2f}), "
                             "{:.2f} m {} and {:.2f} m below your eyes, {:.2f} m to the {}, the hand at {:.1f} m/s (counts below {:.1f})",
                    g_mouth.base_id, g_mouth.closest, kMouthRadius, std::abs(at.z), at.z <= 0.0f ? "in front" : "behind", -at.y, std::abs(at.x),
                    at.x >= 0.0f ? "right" : "left", g_mouth.closest_speed, kMouthSpeed);
            }
            g_mouth.closest = 1e9f;
        }
        if (at_mouth) {
            g_mouth.at_us = now_us;
        }
        if (at_mouth != g_mouth.at) {
            g_mouth.at = at_mouth;
            static int told = 0;
            if (at_mouth && ++told <= 20) {
                spdlog::info("[Eat] the food or drink in your {} hand ({:08X}) is at your mouth (palm {:.2f} m from the mouth point): let go "
                             "to eat or drink it",
                    g_hand.load() == 0 ? "left" : "right", g_mouth.base_id, g_mouth.distance);
            }
        }

        // Holding with a grip: letting go drops (and throws) it in the next grab update.
        if (!g_grip_grab.load()) {
            g_other_ask = OtherAsk{}; // (fix170: a waiting left squeeze is for a hold that has ended)
        }
        if (g_grip_grab.load()) {
            EndAim();
            const int hand = g_hand.load();
            // (fix170) Both grips let go at once while both hands hold it: the second's hold ends here and the release below throws it
            // (the first's release was lost behind the second's; the review). The setting turned off: the second hand lets go.
            if (const int support = g_support_hand.load(); support >= 0) {
                if (released[hand] && (released[support] || !g_grip_down[support])) {
                    std::scoped_lock _{ g_mutex };
                    g_support      = Support{};
                    g_support_hand = -1;
                } else if (!settings.twoHandHold) {
                    std::scoped_lock _{ g_mutex };
                    EndSupport("the setting turned off");
                }
            }
            if (at_mouth && g_grip_down[hand]) {
                haptics::MeleePulse(hand == 0, kMouthBuzz, 0.05f);
            } else if (at_bag && g_grip_down[hand]) {
                haptics::MeleePulse(hand == 0, kBagBuzz, 0.05f);
            }
            if (!grabbing) {
                g_grip_grab   = false; // the game dropped it (too far away, or the use button), or a pull it did not take
                g_hand        = 1;
                g_needs_catch = false;
                bool refused  = false;
                {
                    std::scoped_lock _{ g_mutex };
                    g_support      = Support{}; // (fix170: nothing for a second hand to hold)
                    g_support_hand = -1;
                    if (g_pull_ask.pending) {
                        g_pull_ask = PullAsk{};
                        refused    = true;
                    }
                }
                if (refused) {
                    // A long soft buzz (not the lock's short one); the aim skips it from now on, and goes on while the grip is held.
                    if (g_pull_handle != 0) {
                        g_refused[g_refused_next]    = g_pull_handle;
                        g_refused_us[g_refused_next] = now_us;
                        g_refused_next               = (g_refused_next + 1) % kRefusedKept;
                    }
                    SkipSteer(g_pull_body, kSteerRefusedUs);
                    haptics::MeleePulse(hand == 0, 0.25f, 0.3f);
                    const bool again = g_grip_down[hand] && settings.gravityGloves && g_pull_ready.load() && tracking::HandInteract() &&
                                       HandsFreeNow();
                    spdlog::info("[Grab] pull: the game did not take hold of it (too heavy, or not something it lets you grab); the aim skips it "
                                 "for a minute{}",
                        again ? ", and your hand aims again" : "");
                    if (again) {
                        StartAim(hand);
                    }
                }
                g_pull_handle = 0;
                g_pull_body   = 0;
            } else if (g_needs_catch.load()) {
                // Pulled as the grip was let go: squeezing either grip catches it; otherwise, once it has waited at the hand a
                // moment, it drops.
                // (The other hand catches with a new squeeze, not one it was already holding, e.g. for the d-pad.)
                const int catcher = g_grip_down[hand] ? hand : (pressed[1 - hand] ? 1 - hand : -1);
                if (catcher >= 0) {
                    g_needs_catch = false;
                    if (catcher != hand) {
                        g_hand = catcher;
                        std::scoped_lock _{ g_mutex };
                        g_rebase       = true;
                        g_need_contact = g_use_taken; // the new palm meets the box elsewhere
                    }
                    haptics::MeleePulse(catcher == 0, 0.35f, 0.04f);
                    spdlog::info("[Grab] {} grip: caught it", catcher == 0 ? "left" : "right");
                } else if (const int64_t deadline = g_catch_deadline_us.load(); deadline != 0 && now_us > deadline) {
                    {
                        std::scoped_lock _{ g_mutex };
                        g_release      = true;
                        g_release_keep = true;
                    }
                    g_grip_grab   = false;
                    g_needs_catch = false;
                    spdlog::info("[Grab] pull: not caught (no grip squeezed as it arrived), so it drops");
                }
            } else if (released[hand] && [] {
                           std::scoped_lock _{ g_mutex };
                           return g_pull.active || g_pull_ask.pending;
                       }()) {
                // Let go while it flies to the hand (people open the hand after the flick): it keeps coming, and a squeeze of
                // either grip catches it, on the way or for a second once it is at the hand (HIGGS); otherwise it drops there.
                g_catch_deadline_us = 0; // counted from when it reaches the hand...
                g_needs_catch       = true;
                static int told     = 0;
                if (++told <= 10) {
                    spdlog::info("[Grab] {} grip let go while the pull flies: it comes on to your hand; squeeze to catch it", hand == 0 ? "left" : "right");
                }
            } else if (const int second = g_support_hand.load();
                       released[hand] && second >= 0 && second != hand && g_grip_down[second] && !released[second]) {
                // (fix170) The first hand lets go while the second holds: taken into the second hand as it lies (hand to hand).
                g_hand = second;
                {
                    std::scoped_lock _{ g_mutex };
                    g_support        = Support{};
                    g_support_hand   = -1;
                    g_rebase         = true;
                    g_need_contact   = g_use_taken; // seated in the new palm...
                    g_seat_keep_tilt = true;        // ...keeping how it lies
                }
                haptics::MeleePulse(second == 0, 0.25f, 0.03f);
                static int told = 0;
                if (++told <= 20) {
                    spdlog::info("[Grab] (fix170) your {} hand let go: your {} hand holds it on its own", hand == 0 ? "left" : "right", second == 0 ? "left" : "right");
                }
            } else if (const int support = g_support_hand.load(); support >= 0 && (released[support] || !g_grip_down[support] || [] {
                           std::scoped_lock _{ g_mutex };
                           return g_support.active && g_support.have_grip_world && g_support.stretch > kSupportStretch;
                       }())) {
                // (fix170) The second hand lets go (or is pulled 25 cm off its grip): the first holds it as it lies.
                std::scoped_lock _{ g_mutex };
                EndSupport(released[support] || !g_grip_down[support] ? "you let go" : "pulled away from its grip");
            } else if (released[hand]) {
                glm::vec3 linear{}, angular{};
                ThrowVelocity(hand, linear, angular);
                {
                    std::scoped_lock _{ g_mutex };
                    g_release       = true;
                    g_release_keep  = false;
                    g_throw_linear  = linear;
                    g_throw_angular = angular;
                    g_support       = Support{}; // (fix170: both let go: thrown)
                    g_support_hand  = -1;
                }
                g_grip_grab = false;
                // At your mouth, or there a moment ago with the same food still in this hand and still close and slow (the hand
                // moves as it opens; a throw is not eating).
                const bool eat = at_mouth || (g_mouth.live && g_mouth.food && now_us - g_mouth.at_us < kMouthGraceUs &&
                                                 g_mouth.distance < 1.5f * kMouthRadius && g_mouth.speed < kMouthSpeed);
                // ...or in a bag zone (or there a moment ago: still slow, or still next to it, as a tracking hiccup leaves it).
                const bool carried = !eat && held && Consume::IsCarried(g_item.type) && GameFlow::gStore.internalSettings.bagStash;
                const float from_last = g_bag.last_zone != 0 ? g_bag.now.zone_outside[g_bag.last_zone] : 1e9f;
                const bool  graced    = carried && !at_bag && g_bag.ref == g_item.ref && g_bag.hand == hand && g_bag.last_zone != 0 &&
                                    now_us - g_bag.at_us < kBagGraceUs &&
                                    ((g_bag.speed < kBagSpeed && from_last < kBagGraceSlowOutside) ||
                                        (g_bag.speed < kBagGraceSpeed && from_last < kBagGraceOutside));
                const bool bag     = carried && (at_bag || graced);
                if (carried) {
                    // Every let-go of something you can carry: where the hand was, so a missed bag shows why.
                    static int lines = 0;
                    if (++lines <= 60) {
                        if (bag) {
                            spdlog::info("[Bag] let go of {:08X} at your {}{}: it goes in your bag", g_item.base_id,
                                kZoneNames[at_bag ? g_bag.zone : g_bag.last_zone],
                                graced ? fmt::format(" (it was there {} ms before)", (now_us - g_bag.at_us) / 1000) : std::string{});
                        } else {
                            spdlog::info("[Bag] let go of {:08X} outside the bag zones: your hand was {:.2f} m outside the {} zone, {}; the hand at "
                                         "{:.1f} m/s (counts below {:.1f})",
                                g_item.base_id, std::max(g_bag.now.outside, 0.0f), kZoneNames[g_bag.now.nearest], BagWhere(g_bag.now.body), g_bag.speed,
                                kBagSpeed);
                        }
                    }
                }
                if (eat && Consume::AfterRelease(Consume::Action::Eat, g_mouth.ref, hand)) {
                    haptics::MeleePulse(hand == 0, 0.5f, 0.2f); // (HIGGS: 0.5 for 0.2 s)
                } else if (bag && Consume::AfterRelease(Consume::Action::Bag, g_item.ref, hand)) {
                    haptics::MeleePulse(hand == 0, 0.5f, 0.2f); // (HIGGS: 0.5 for 0.2 s)
                } else {
                    haptics::MeleePulse(hand == 0, 0.2f, 0.03f);
                }
            } else if (pressed[1 - hand] || (g_other_ask.pending && g_other_ask.holder == hand)) {
                const int other = 1 - hand;
                // (fix170 with fix171 and fix172; the merge review) The left grip is also the favorites wheel's first half: a left squeeze
                // while the right hand holds something waits 0.2 s, and a left trigger pulled meanwhile (the wheel's gesture), the wheel
                // open, or the grip let go drops it, so opening the wheel never joins, adopts or passes what the right hand holds.
                bool act = pressed[other];
                if (pressed[other] && other == 0 && settings.favoritesWheel) {
                    g_other_ask = OtherAsk{ true, hand, now_us };
                    act         = false;
                } else if (!pressed[other]) {
                    act = false;
                    if (!g_grip_down[other] || g_trigger_down[other] || FavoritesWheel::IsOpen() || g_left_alone[other].load() != 0) {
                        g_other_ask = OtherAsk{};
                    } else if (now_us - g_other_ask.us >= kOtherWaitUs) {
                        g_other_ask = OtherAsk{};
                        act         = true;
                    }
                }
                if (!act) {
                    // (waiting, or dropped)
                } else if (Physical::HoldingBody()) {
                    // (fix172's bodies: the second grip does nothing on a held person; passed, its spring's target would jump to the other
                    // palm and the auto-drop let it go; the merge review)
                } else if (settings.twoHandHold && JoinSupport(hand, other)) {
                    // (fix170) Its palm at the held object: both hands hold it.
                    haptics::MeleePulse(other == 0, 0.35f, 0.04f);
                } else if (settings.oneInEachHand && g_adopt_ready.load() && g_adopted_hand.load() < 0 && !g_needs_catch.load()) {
                    // (fix170) Away from it: this hand keeps it on its own (the player's update adopts its spring) and the other grabs
                    // what it is next to; not kept, it is passed as before (OnGrabSlot's answer, handled above).
                    std::scoped_lock _{ g_mutex };
                    g_adopt_ask   = AdoptAsk{ true, hand, other, now_us };
                    g_adopt_other = other;
                    g_adopt_asked = true;
                } else {
                    // The other grip takes it over into that hand.
                    g_hand = other;
                    {
                        std::scoped_lock _{ g_mutex };
                        g_rebase       = true;
                        g_need_contact = g_use_taken; // the new palm meets the box elsewhere
                    }
                    haptics::MeleePulse(hand == 1, 0.35f, 0.04f);
                    spdlog::info("[Grab] passed to your {} hand", hand == 1 ? "left" : "right");
                }
            }
            return;
        }

        // Held by the use button: squeezing a grip takes it into that hand (then letting go drops or throws it).
        if (grabbing) {
            EndAim();
            if (g_start_hand.load() >= 0) {
                g_start_hand = -1; // a reach that the grab overtook
                PointInteract::SetPointingHand(false);
            }
            {
                std::scoped_lock _{ g_mutex };
                if (g_release) {
                    return; // being let go right now
                }
            }
            for (const int hand : { 1, 0 }) {
                if (pressed[hand]) {
                    const bool other = g_hand.load() != hand;
                    g_hand           = hand;
                    g_grip_grab      = true;
                    if (other) {
                        std::scoped_lock _{ g_mutex };
                        g_rebase       = true;
                        g_need_contact = g_use_taken; // the new palm meets the box elsewhere
                    }
                    haptics::MeleePulse(hand == 0, 0.35f, 0.04f);
                    const int n = g_takeovers.fetch_add(1) + 1;
                    if (n <= 20 || n % 10 == 0) {
                        spdlog::info("[Grab] {} grip: took hold of what you are carrying", hand == 0 ? "left" : "right");
                    }
                    break;
                }
            }
            return;
        }
        if (g_hand.load() != 1 && g_start_hand.load() < 0 && g_aim_hand.load() < 0) {
            g_hand = 1; // a grab started by the use button is in the right hand
        }

        // Gravity gloves: a hand aiming (its grip held since nothing was next to it) locks on and pulls with a flick.
        if (const int aim = g_aim_hand.load(); aim >= 0) {
            // The left grip also makes the left stick a d-pad: the stick pushed before anything is locked on means the d-pad.
            const auto stick     = vr->get_left_stick_axis();
            const bool dpad_used = aim == 0 && g_aim_target == 0 && (std::abs(stick.x) >= 0.5f || std::abs(stick.y) >= 0.5f);
            // Let go while locked on: a flick in that same moment still pulls (people let go as they flick).
            if (released[aim] && g_aim_target != 0) {
                g_aim_release_us = now_us;
            }
            const bool grace = !g_grip_down[aim] && g_aim_target != 0 && now_us - g_aim_release_us < kReleaseGraceUs;
            if ((!g_grip_down[aim] && !grace) || !settings.gravityGloves || !g_pull_ready.load() || !tracking::HandInteract() || !HandsFreeNow() ||
                pressed[1 - aim] || dpad_used) {
                // Let go, turned off, a weapon out, the other hand reaches for something (handled below), the d-pad.
                EndAim(!g_grip_down[aim] && !grace ? "you let go"
                       : !settings.gravityGloves   ? "the gravity gloves are turned off"
                       : !g_pull_ready.load()      ? "pulling is not available"
                       : !tracking::HandInteract() ? "hand interaction stopped (a menu, a conversation, the pilot's seat, or a weapon out)"
                       : !HandsFreeNow()           ? "a weapon in your hands"
                       : pressed[1 - aim]          ? "the other grip was squeezed"
                                                   : "the left stick was pushed (the d-pad)");
            } else {
                UpdateAim(aim, p, g_grip_down[aim]);
                return;
            }
        }

        // A grip squeezed with nothing held: the pick follows that hand for a moment, then the game's grab
        // starts on whatever it found.
        if (g_start_hand.load() < 0 && tracking::HandInteract() && HandsFreeNow()) {
            for (const int hand : { 1, 0 }) {
                if (pressed[hand]) {
                    g_start_hand = hand;
                    const bool switching = PointInteract::PointingHandIsLeft() != (hand == 0);
                    PointInteract::SetPointingHand(hand == 0);
                    g_start_wait = std::max(switching ? 3 : 0, g_far_frames_left); // a fresh cast at this hand
                    break;
                }
            }
        }
        const int start_hand = g_start_hand.load();
        if (start_hand < 0) {
            return;
        }
        if (!g_grip_down[start_hand]) {
            g_start_hand = -1;
            PointInteract::SetPointingHand(false);
            return;
        }
        // (fix171 with fix172; the merge review) The left grip is also the favorites wheel's first half: its grab waits 0.2 s from the
        // squeeze, and a left trigger pulled meanwhile (the wheel's gesture) or the wheel open cancels it, so the gesture never grabs,
        // pulls, or knocks a person down (Physical::GripGrab) first.
        if (start_hand == 0 && settings.favoritesWheel) {
            if (g_trigger_down[0] || FavoritesWheel::IsOpen()) {
                g_start_hand = -1;
                PointInteract::SetPointingHand(false);
                return;
            }
            if (now_us - g_grip_pressed_us[0] < kOtherWaitUs) {
                return;
            }
        }
        if (g_start_wait > 0) {
            --g_start_wait;
            return;
        }
        uint32_t mode = 1;
        SafeRead(&mode, p + kGrabMode, sizeof(mode));
        if (PickHasTarget() && mode == 0) {
            // (fix172) A standing person under the hand is knocked down first (grabbed once down); a person down is taken with the game's
            // grab of the living turned on for this one start (Physical; put back as this block ends).
            const Physical::GripGrab physical(PickHandle(), start_hand);
            if (physical.Handled()) {
                g_start_hand = -1;
                PointInteract::SetPointingHand(false);
                return;
            }
            g_hand = start_hand;
            {
                std::scoped_lock _{ g_mutex };
                g_release      = false;     // nothing left over from an earlier grab
                g_release_keep = false;
                g_pull_ask     = PullAsk{}; // a grab at the hand, not a pull
            }
            g_needs_catch = false;
            t_hand_start  = true; // the grab start keeps the hand pick's target (not the head ray's)
            const bool ok = CallPlayer(g_start_fn, p);
            t_hand_start  = false;
            if (ok) {
                g_grip_grab = true;
                const int n = g_grip_grabs.fetch_add(1) + 1;
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Grab] {} grip: grabbing what your hand is next to", start_hand == 0 ? "left" : "right");
                }
                haptics::MeleePulse(start_hand == 0, 0.35f, 0.04f);
            } else {
                g_grip_ready = false;
                spdlog::error("[Grab] starting a grab failed with an exception; grip grabbing is off for this session");
            }
        } else if (mode == 0 && settings.gravityGloves && g_pull_ready.load()) {
            g_start_hand = -1;
            StartAim(start_hand); // the pick stays on this hand, reaching far, while the grip is held
            return;
        } else if (NowUs() - g_last_nothing_log > 5'000'000) {
            g_last_nothing_log = NowUs();
            spdlog::info("[Grab] {} grip: nothing to grab next to your hand{}", start_hand == 0 ? "left" : "right", mode != 0 ? " (already grabbing)" : "");
        }
        g_start_hand = -1;
        PointInteract::SetPointingHand(false);
    }

    void Update()
    {
        g_frame.fetch_add(1, std::memory_order_relaxed);
        UpdateBodyFacing(NowUs()); // (the hips of the bag)
        if (g_ready.load()) {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            const uintptr_t spring_now = player != 0 ? CurrentSpring(player) : 0;
            UpdateGroup(spring_now);
            UpdateKeepDrawn(spring_now); // (fix160)
        }
        SweepPresent();
        MeasureDrawn();
        // (fix175) Build mode opening (no menu by the count, the world as in play): what a hand holds is let go as it opens (dropped where it
        // is, not thrown), as a held person is when a menu opens (Physical, which lets them go in build mode too); no new grab or gloves' pull
        // starts while it is open (tracking::HandInteract is off there).
        {
            static bool workshop_was = false;
            const bool  workshop     = GameFlow::isWorkshopShown();
            if (workshop && !workshop_was) {
                LetGo("(fix175) build mode opened");
            }
            workshop_was = workshop;
        }
        UpdateGrips();
        {
            // Eating what was let go at your mouth: once the game's grab has let go of it (Consume.h).
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            Consume::Update(player != 0 && SpringCount(player) > 0, GameFlow::isShowingMenu());
        }
        UpdateAutoWeapon();
        if (g_holster_until_us.load() != 0 && (!GameFlow::isWeaponDrawn() || GameFlow::isShowingMenu())) {
            g_holster_until_us = 0; // put away (or a menu came): the button is let go
        }
        {
            const int64_t now     = NowUs();
            const int     held    = g_hand.load();
            const bool    holding = g_grip_grab.load() || g_track_spring.load(std::memory_order_relaxed) != 0;
            for (int hand = 0; hand < 2; ++hand) {
                // (fix170: also a hand holding an object on its own, and the second hand on the held object: their swings are no punches; the review)
                if ((holding && held == hand) || g_aim_hand.load() == hand || g_start_hand.load() == hand || g_adopted_hand.load() == hand ||
                    g_support_hand.load() == hand) {
                    g_busy_until[hand].store(now + kBusyAfterUs, std::memory_order_relaxed);
                }
            }
        }
        // The left grip turns the left stick into a d-pad again once it is clear it is not grabbing anything (nor aiming
        // to pull: pushing the stick before anything is locked on ends that).
        // (A grip another hand action took keeps the stick walking too: it was not squeezed for the d-pad.)
        const bool left_busy = g_start_hand.load() == 0 || (g_grip_grab.load() && g_hand.load() == 0) || g_aim_hand.load() == 0 || g_left_alone[0].load() != 0 ||
                               g_adopted_hand.load() == 0 || g_support_hand.load() == 0; // (fix170: holding on its own, or as the second hand)
        if (!g_grip_down[0]) {
            g_left_free = false;
        } else if (!left_busy) {
            g_left_free = true;
        }
    }

    bool HandsFree()
    {
        return HandsFreeNow();
    }

    bool HandBusy(bool left)
    {
        return NowUs() < g_busy_until[left ? 0 : 1].load(std::memory_order_relaxed);
    }

    void MarkBusy(bool left)
    {
        g_busy_until[left ? 0 : 1].store(NowUs() + kBusyAfterUs, std::memory_order_relaxed);
    }

    void SetLeftGripWeapon(bool on) { g_left_weapon_grip.store(on, std::memory_order_relaxed); }

    void LeaveGrip(bool left)
    {
        const int hand = left ? 0 : 1;
        if (g_left_alone[hand].load() == 0) {
            g_left_alone_frame[hand] = g_frame.load(std::memory_order_relaxed);
            g_left_alone[hand]       = 1;
        }
    }

    // (fix179, for Climbing) That grip's squeeze is still being decided (the pick following its hand, the left grip's wait for the wheel).
    bool Deciding(bool left) { return g_start_hand.load(std::memory_order_relaxed) == (left ? 0 : 1); }

    bool HoldingWith(bool left)
    {
        const int hand = left ? 0 : 1;
        // (fix170) An object held on its own by this hand, or this hand the second on the held object.
        if (g_adopted_hand.load(std::memory_order_relaxed) == hand || g_support_hand.load(std::memory_order_relaxed) == hand) {
            return true;
        }
        return g_hand.load(std::memory_order_relaxed) == hand &&
               (g_grip_grab.load(std::memory_order_relaxed) || g_track_spring.load(std::memory_order_relaxed) != 0 ||
                   g_needs_catch.load(std::memory_order_relaxed));
    }

    // ---- (fix172) For Physical: a body let go, and a grip's grab started again ----
    void LetGo(const char* why)
    {
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        if (!g_ready.load() || player == nullptr || SpringCount(reinterpret_cast<uintptr_t>(player)) == 0) {
            return;
        }
        {
            std::scoped_lock _{ g_mutex };
            g_release      = true; // (in the next grab update, as the grip's own let-go)
            g_release_keep = true; // (it keeps its own speed: not thrown)
        }
        g_grip_grab   = false;
        g_needs_catch = false;
        static int told = 0;
        if (++told <= 20) {
            spdlog::info("[Grab] let go: {}", why);
        }
    }

    bool DropNow(const char* why)
    {
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        if (!g_ready.load() || g_drop_fn == 0 || player == nullptr) {
            return false;
        }
        const auto p = reinterpret_cast<uintptr_t>(player);
        if (SpringCount(p) == 0) {
            return false;
        }
        // The game's drop, as the grab update's own let-go calls it (the game calls it from its input and its player's update too, all on
        // the main thread): every spring of the list taken out of the world, the grab's state cleared.
        bool ok = false;
        {
            std::scoped_lock _{ g_mutex };
            g_release      = false;
            g_release_keep = false;
            g_pull         = Pull{};
            g_pull_ask     = PullAsk{};
            g_track_spring = 0;
            g_support      = Support{}; // (fix170's: nothing left for a second hand or an adoption)
            g_support_hand = -1;
            g_adopt_ask    = AdoptAsk{};
            g_adopt_asked  = false;
            {
                std::scoped_lock track{ g_track_mutex };
                g_throw = Throw{};
                g_throw_spring.store(0, std::memory_order_release);
            }
        }
        // (The drop locks the player's spring list itself; OnTarget and OnGrabSlot run with that lock held and then take g_mutex: called
        // with no lock of the mod's held, else the two could wait on each other (the merge review). Nothing in between matters: the
        // grab update either steers as before or no longer runs once the list is empty.)
        ok = CallPlayer(g_drop_fn, p);
        if (ok) {
            std::scoped_lock _{ g_mutex };
            g_spring = 0;
            {
                std::scoped_lock debug{ g_debug_mutex };
                g_debug = DebugHeldInfo{};
            }
            EndPresent();
        }
        g_grip_grab   = false;
        g_needs_catch = false;
        static int told = 0;
        if (++told <= 20) {
            spdlog::info("[Grab] dropped at once: {}{}", why, ok ? "" : " (the game's drop failed with an exception)");
        }
        return ok;
    }

    void GrabAgain(bool left) { g_grab_again[left ? 0 : 1].store(true); }

    bool GunOut()
    {
        if (!GameFlow::isWeaponDrawn()) {
            return false;
        }
        std::scoped_lock _{ g_armed_mutex };
        const auto& armed = ArmedNowLocked();
        return armed.armed == Armed::Weapon && armed.ranged;
    }

    bool GunKnownOut()
    {
        if (!GameFlow::isWeaponDrawn()) {
            return false;
        }
        std::scoped_lock _{ g_armed_mutex };
        const auto& armed = ArmedNowLocked();
        return armed.armed == Armed::Weapon && armed.gun_known;
    }

    uint32_t MeleeWeaponOut()
    {
        if (!GameFlow::isWeaponDrawn()) {
            return 0;
        }
        std::scoped_lock _{ g_armed_mutex };
        const auto& armed = ArmedNowLocked();
        return armed.armed == Armed::Weapon && !armed.ranged && armed.weapon_form != 0 ? armed.weapon_id : 0;
    }

    bool OneHandedMeleeOut()
    {
        if (!GameFlow::isWeaponDrawn()) {
            return false;
        }
        uintptr_t form = 0;
        {
            std::scoped_lock _{ g_armed_mutex };
            const auto& armed = ArmedNowLocked();
            if (armed.armed != Armed::Weapon || armed.ranged || armed.gun_known || armed.weapon_form == 0) {
                return false;
            }
            form = armed.weapon_form;
        }
        uintptr_t instance = 0, general = 0;
        uint32_t  type     = 0xffffffffu;
        return SafeRead(&instance, form + kWeaponInstance, sizeof(instance)) && instance != 0 && SafeRead(&general, instance + kInstanceGeneral, sizeof(general)) &&
               general != 0 && SafeRead(&type, general + kGeneralType, sizeof(type)) && type >= 1 && type <= 4;
    }

    bool GripDown(bool left)
    {
        return g_grip_down[left ? 0 : 1];
    }

    float TriggerPull(bool left)
    {
        return TriggerValue(left ? 0 : 1);
    }

    bool ThrowableEquipped()
    {
        std::scoped_lock _{ g_armed_mutex };
        return ArmedNowLocked().throwable;
    }

    uintptr_t ThrowableForm()
    {
        std::scoped_lock _{ g_armed_mutex };
        return ArmedNowLocked().throwable_form;
    }

    bool HoldFrame(bool left, glm::vec3& surface, glm::vec3& facing, glm::vec3& forward, glm::vec3& side)
    {
        GripFrame frame;
        if (!HandGripWorld(left ? 0 : 1, frame) || !Finite(frame.surface) || !Finite(frame.facing) || !Finite(frame.forward) || !Finite(frame.side)) {
            return false;
        }
        surface = frame.surface;
        facing  = frame.facing;
        forward = frame.forward;
        side    = frame.side;
        return true;
    }

    int PutInYourGroup(uintptr_t root, bool ask, std::string& what)
    {
        if (!g_group_ready.load()) {
            what = "the game's collision group function is not as in Starfield 1.16.236";
            return -1;
        }
        uintptr_t collision = 0, table = 0, function = 0, system = 0, instance = 0, ids = 0;
        int32_t   index = -1;
        uint32_t  count = 0, body = 0;
        if (root == 0 || !SafeRead(&collision, root + kNodeCollision, sizeof(collision)) || collision == 0) {
            what = "its 3D has no collision object of its own (yet)";
            return 0;
        }
        if (!SafeRead(&table, collision, sizeof(table)) || !SafeRead(&function, table + kSetGroupSlot * sizeof(uintptr_t), sizeof(function)) ||
            function != At(kSetGroupRva)) {
            what = "its collision object is not the kind whose group the game's function changes";
            return -1;
        }
        if (!SafeRead(&system, collision + kCollisionSystem, sizeof(system)) || system == 0 || !SafeRead(&index, collision + kCollisionIndex, sizeof(index)) ||
            !SafeRead(&instance, system + kSystemInstance, sizeof(instance)) || instance == 0 || !SafeRead(&count, instance + kInstanceCount, sizeof(count)) ||
            !SafeRead(&ids, instance + kInstanceIds, sizeof(ids)) || index < 0 || static_cast<uint32_t>(index) >= count ||
            !SafeRead(&body, ids + static_cast<uintptr_t>(index) * sizeof(uint32_t), sizeof(body))) {
            what = "its body is not in a physics world (yet)";
            return 0;
        }
        PlayerCollision you;
        if (!ReadPlayerCollision(you)) {
            what = "your own collision was not found";
            return 0;
        }
        BodyRef b;
        if (!ResolveBodyIn(you.world, body, b) || b.collision != collision) {
            what = "its body is not in your physics world (yet)";
            return 0;
        }
        if (b.bodies != 1) {
            // (As for a held object: only an object of one body is put in your group.)
            what = fmt::format("it is made of {} physics bodies", b.bodies);
            return -1;
        }
        if ((b.filter >> 16) == you.group) {
            what = fmt::format("in your collision group ({}) (its filter {:#x})", you.group, b.filter);
            return 1;
        }
        if (!ask) {
            what = fmt::format("not in your collision group ({}) yet (its filter {:#x})", you.group, b.filter);
            return 0;
        }
        const uint32_t before = b.filter;
        const bool     called = CallSetGroup(collision, you.group);
        if (ResolveBodyIn(you.world, body, b) && b.collision == collision && (b.filter >> 16) == you.group) {
            what = fmt::format("put in your collision group ({}) (its filter {:#x} -> {:#x})", you.group, before, b.filter);
            return 1;
        }
        what = fmt::format("asked to go in your collision group ({}), not done yet (its filter {:#x}; the game's function said {})", you.group, before, called);
        return 0;
    }

    namespace
    {
        // (fix155) A node is a NiNode (has children), read under a guard.
        bool IsNodeGuarded(const void* object)
        {
            __try {
                return utility::rtti::derives_from(object, "class NiNode");
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A collision object whose group and layer the game's functions change (the plain kind: slots 64 and 65 are those functions).
        bool Settable(uintptr_t collision)
        {
            uintptr_t table = 0, group_fn = 0, layer_fn = 0;
            return collision != 0 && SafeRead(&table, collision, sizeof(table)) &&
                   SafeRead(&group_fn, table + kSetGroupSlot * sizeof(uintptr_t), sizeof(group_fn)) && group_fn == At(kSetGroupRva) &&
                   SafeRead(&layer_fn, table + kSetLayerSlot * sizeof(uintptr_t), sizeof(layer_fn)) && layer_fn == At(kSetLayerRva);
        }

        using SetLayerFn = bool (*)(void*, uint32_t);
        // (As CallSetGroup: no handler around the game's own function.)
        bool CallSetLayer(uintptr_t collision, uint32_t layer) { return reinterpret_cast<SetLayerFn>(At(kSetLayerRva))(reinterpret_cast<void*>(collision), layer); }

        // (fix187) A copy's physics bodies (PutApart's, and PartsMotion's): the collision objects on its 3D's nodes (a proxy's part
        // object in its place), then every body of their physics systems, by the collision object each belongs to (each of the
        // game's functions changes the one body of the object it is called on).
        struct CopyBody
        {
            uintptr_t collision{ 0 };
            uintptr_t record{ 0 };
            uint32_t  body{ 0 };
            uint32_t  filter{ 0 };
        };
        struct CopyBodies
        {
            std::vector<uintptr_t> objects;
            std::vector<CopyBody>  owned;
            int                    visited{ 0 }, proxies{ 0 };
            int                    waiting{ 0 }; // (objects or bodies not in your world yet: not done until they are; the review)
        };

        void FindCopyBodies(uintptr_t root, const PlayerCollision& you, CopyBodies& out)
        {
            // Its collision objects: on its nodes (the root and below), a proxy's part object in its place.
            std::vector<uintptr_t> stack{ root };
            while (!stack.empty() && out.visited < kApartNodes) {
                const uintptr_t node = stack.back();
                stack.pop_back();
                ++out.visited;
                uintptr_t collision = 0, table = 0;
                if (node != 0 && SafeRead(&collision, node + kNodeCollision, sizeof(collision)) && collision != 0 && SafeRead(&table, collision, sizeof(table))) {
                    if (table == At(kCollProxyTableRva)) {
                        uintptr_t moved = 0, target = 0;
                        collision = SafeRead(&moved, collision + kCollProxyNode, sizeof(moved)) && moved == node &&
                                            SafeRead(&target, collision + kCollProxyTarget, sizeof(target))
                                      ? target
                                      : 0;
                        out.proxies += collision != 0 ? 1 : 0;
                    }
                    if (Settable(collision) && std::find(out.objects.begin(), out.objects.end(), collision) == out.objects.end()) {
                        out.objects.push_back(collision);
                    }
                }
                if (node != 0 && IsNodeGuarded(reinterpret_cast<const void*>(node))) {
                    RE::NiAVObject* kids[128]{};
                    const auto      n = body::ReadChildren(reinterpret_cast<const RE::NiAVObject*>(node), kids, 128);
                    for (uint16_t i = 0; i < n; ++i) {
                        stack.push_back(reinterpret_cast<uintptr_t>(kids[i]));
                    }
                }
            }
            for (const uintptr_t object : out.objects) {
                uintptr_t system = 0, instance = 0, ids = 0;
                uint32_t  count = 0;
                if (!SafeRead(&system, object + kCollisionSystem, sizeof(system)) || system == 0 || !SafeRead(&instance, system + kSystemInstance, sizeof(instance)) ||
                    instance == 0 || !SafeRead(&count, instance + kInstanceCount, sizeof(count)) || !SafeRead(&ids, instance + kInstanceIds, sizeof(ids)) || ids == 0) {
                    ++out.waiting;
                    continue;
                }
                for (uint32_t i = 0; i < std::min(count, kApartBodies); ++i) {
                    uint32_t body = 0;
                    BodyRef  b;
                    if (!SafeRead(&body, ids + static_cast<uintptr_t>(i) * sizeof(uint32_t), sizeof(body)) || !ResolveBodyIn(you.world, body, b) ||
                        !Settable(b.collision)) {
                        ++out.waiting;
                        continue;
                    }
                    if (std::none_of(out.owned.begin(), out.owned.end(), [&](const CopyBody& o) { return o.body == body; })) {
                        out.owned.push_back(CopyBody{ b.collision, b.record, body, b.filter });
                    }
                }
            }
        }
    }

    int PartsMotion(uintptr_t root, int& moving, int& keyframed, std::string& what)
    {
        moving = keyframed = 0;
        PlayerCollision you;
        if (!ReadPlayerCollision(you)) {
            what = "your own collision was not found";
            return 0;
        }
        CopyBodies found;
        FindCopyBodies(root, you, found);
        if (found.owned.empty()) {
            what = fmt::format("its bodies were not found in your physics world ({} collision object(s))", found.objects.size());
            return 0;
        }
        // A body's flags (+0x54, as the far casts' look through the world's bodies reads them): in use bits 0-1; static 1, moved by
        // physics 2 alone, keyframed 4 (with 2).
        int other = 0;
        for (const auto& o : found.owned) {
            uint32_t flags = 0;
            if (!SafeRead(&flags, o.record + kBodyFlags, sizeof(flags)) || (flags & 3u) == 0) {
                ++other;
            } else if ((flags & 4u) != 0) {
                ++keyframed;
            } else if ((flags & 7u) == 2u) {
                ++moving;
            } else {
                ++other;
            }
        }
        what = fmt::format("{} of its {} bod{} keyframed, {} moved by physics (falling){}", keyframed, found.owned.size(), found.owned.size() == 1 ? "y" : "ies",
            moving, other > 0 ? fmt::format(", {} static or unread", other) : std::string{});
        return 1;
    }

    int PutApart(uintptr_t root, bool ask, std::string& what)
    {
        if (!g_group_ready.load() || !g_layer_ready.load()) {
            what = "the game's collision group or layer function is not as in Starfield 1.16.236";
            return -1;
        }
        PlayerCollision you;
        if (!ReadPlayerCollision(you)) {
            what = "your own collision was not found";
            return 0;
        }
        CopyBodies found;
        FindCopyBodies(root, you, found);
        const auto& objects = found.objects;
        auto&       owned   = found.owned;
        const int   visited = found.visited, proxies = found.proxies, waiting = found.waiting;
        if (objects.empty()) {
            what = fmt::format("no collision object of its own found in its 3D (yet; {} nodes looked at)", visited);
            return 0;
        }
        if (owned.empty()) {
            what = fmt::format("its bodies are not in your physics world (yet; {} collision object(s){})", objects.size(), proxies > 0 ? ", on its parts" : "");
            return 0;
        }
        const auto apart = [&](uint32_t filter) { return (filter >> 16) == you.group && (filter & 0x7fu) == kLayerNonCollidable; };
        size_t     done  = 0;
        uint32_t   first_before = owned.front().filter;
        for (auto& o : owned) {
            if (apart(o.filter)) {
                ++done;
                continue;
            }
            if (!ask) {
                continue;
            }
            CallSetGroup(o.collision, you.group);
            CallSetLayer(o.collision, kLayerNonCollidable);
            if (BodyRef b; ResolveBodyIn(you.world, o.body, b)) {
                o.filter = b.filter;
            }
            done += apart(o.filter) ? 1 : 0;
        }
        what = fmt::format("{} of its {} bod{} in your collision group ({}) on the layer that touches nothing ({}){}{} (the first's filter {:#x} -> {:#x})",
            done, owned.size(), owned.size() == 1 ? "y" : "ies", you.group, kLayerNonCollidable, proxies > 0 ? ", its collision on its parts" : "",
            waiting > 0 ? fmt::format("; {} part(s) not in your world yet", waiting) : std::string{}, first_before, owned.front().filter);
        return done == owned.size() && waiting == 0 ? 1 : 0;
    }

    uint32_t WeaponInHandsId()
    {
        std::scoped_lock _{ g_armed_mutex };
        return ArmedNowLocked().weapon_id;
    }

    uintptr_t WeaponInHandsForm()
    {
        return g_weapon_form.load(std::memory_order_relaxed);
    }

    bool SuppressRightGripTrigger()
    {
        // With the weapon put away the right grip grabs; it must not also aim (which draws the weapon). A squeeze
        // that started as a grab stays one until it is let go.
        // A grenade taken from behind the shoulder does not aim either.
        // A picked-up weapon in your hands with the gun hand in a bag zone: that squeeze puts it in the bag, it does not aim.
        return GripGrabsHere() || g_right_latched.load(std::memory_order_relaxed) ||
               (g_grip_grab.load(std::memory_order_relaxed) && g_hand.load(std::memory_order_relaxed) == 1) || HandActions::RightGripTaken() ||
               (g_auto_in_hands.load(std::memory_order_relaxed) && g_auto_bag_zone.load(std::memory_order_relaxed));
    }

    bool SuppressRightTrigger()
    {
        // A weapon in the right grip: its trigger equips it (and must not draw your own weapon or punch); after that, until the
        // trigger is let go.
        return g_weapon_in_grip[1].load(std::memory_order_relaxed) || g_trigger_block[1].load(std::memory_order_relaxed);
    }

    bool PressHolster()
    {
        // (Never in a menu, where the button means something else; the frame's update ends it once the weapon is put away.)
        return NowUs() < g_holster_until_us.load(std::memory_order_relaxed) && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown(); // (fix175: X deletes in build mode)
    }

    bool PressReload()
    {
        return NowUs() < g_reload_tap_until_us.load(std::memory_order_relaxed) && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown(); // (fix175)
    }

    int FireOverride()
    {
        if (GameFlow::isShowingMenu() || GameFlow::isWorkshopShown() || NowUs() > g_fire_until_us.load(std::memory_order_relaxed)) { // (fix175: RT rotates in build mode)
            return -1;
        }
        return g_fire_override.load(std::memory_order_relaxed);
    }

    bool PickedWeaponInHands()
    {
        return g_auto_in_hands.load(std::memory_order_relaxed) && GameFlow::gStore.internalSettings.bagStash;
    }

    bool SuppressLeftGripDpad()
    {
        // While the left grip may be grabbing (from the squeeze until it is clear there is nothing to grab), aims to pull
        // something, or holds something, the left stick keeps walking.
        // (Nor while another hand action has the grip: a power pushed out with it.)
        return g_start_hand.load(std::memory_order_relaxed) == 0 || (g_grip_grab.load(std::memory_order_relaxed) && g_hand.load(std::memory_order_relaxed) == 0) ||
               g_aim_hand.load(std::memory_order_relaxed) == 0 || (GripGrabsHere() && !g_left_free.load(std::memory_order_relaxed)) ||
               g_left_alone[0].load(std::memory_order_relaxed) != 0 || g_adopted_hand.load(std::memory_order_relaxed) == 0 ||
               g_support_hand.load(std::memory_order_relaxed) == 0; // (fix170)
    }

    void OnHandsPlaced()
    {
        if (g_track_spring.load(std::memory_order_relaxed) == 0 || !g_step_ready.load(std::memory_order_relaxed)) {
            return;
        }
        glm::vec3  anchor{};
        const auto room = tracking::RoomRotation();
        if (!room || !AnchorWorld(anchor)) {
            return;
        }
        const uint64_t frame = g_frame.load(std::memory_order_relaxed);
        const int64_t  now   = NowUs();
        std::scoped_lock _{ g_track_mutex };
        if (g_draw.valid && g_draw.frame == frame) {
            return; // once a frame
        }
        g_draw.frame  = frame;
        g_draw.valid  = true;
        g_draw.anchor = anchor;
        g_draw.room   = *room;
        g_draw.us     = now;
        g_draw.steps  = -1;
        if (g_track.pulling) {
            return; // flying to the hand: not measured (and not foretold from)
        }
        if (g_read.valid && g_read.frame == frame) {
            // After this frame's grab update: the physics steps that already ran this frame used the foretold move; the
            // ones still to come go exactly where the hand is drawn.
            g_draw.steps = g_track.frame == frame ? g_track.steps : 0;
            auto& s      = g_follow_stats;
            ++s.drawn;
            s.delay_us += static_cast<double>(now - g_read.us);
            s.steps_before += g_draw.steps;
            glm::vec3 drawn{};
            if (MeasureMove(drawn)) {
                if (g_draw.steps == 0) {
                    ++s.drawn_before_steps;
                    ++s.exact;
                } else {
                    ++s.predicted;
                }
            }
        } else {
            g_draw.steps = -1; // before this frame's grab update, which then uses it
            ++g_follow_stats.drawn_first;
        }
    }

    float HeldCurl(bool left)
    {
        if (GrenadeHand::InHand(left)) {
            return kHeldCurl; // a grenade taken from your shoulder (its copy in your hand)
        }
        // (fix170) An object held on its own by this hand; this hand the second on the held object (while it is held).
        if (g_adopted_hand.load(std::memory_order_relaxed) == (left ? 0 : 1) && GameFlow::gStore.internalSettings.handGrab) {
            return kHeldCurl;
        }
        if (g_support_hand.load(std::memory_order_relaxed) == (left ? 0 : 1) && g_track_spring.load(std::memory_order_relaxed) != 0 &&
            GameFlow::gStore.internalSettings.handGrab) {
            return kHeldCurl;
        }
        if (g_track_spring.load(std::memory_order_relaxed) == 0 || g_in_flight.load(std::memory_order_relaxed) ||
            g_needs_catch.load(std::memory_order_relaxed) || g_throw_spring.load(std::memory_order_relaxed) != 0 ||
            !GameFlow::gStore.internalSettings.handGrab) {
            return 0.0f; // not holding, or what it pulls is still on its way (or waits at the open hand to be caught), or thrown
        }
        return (g_hand.load(std::memory_order_relaxed) == 0) == left ? kHeldCurl : 0.0f;
    }

    bool AimMark(GloveMark& out)
    {
        if (!g_have_mark || g_aim_hand.load() < 0) {
            return false;
        }
        out = g_mark;
        return true;
    }

    bool SupportGrip(bool left, glm::vec3& grip)
    {
        const int hand = left ? 0 : 1;
        if (g_support_hand.load(std::memory_order_relaxed) != hand) {
            return false;
        }
        glm::vec3 local{ 0.0f };
        int       first = -1;
        {
            std::scoped_lock _{ g_support_draw_mutex };
            if (g_support_draw_hand != hand || NowUs() - g_support_draw_us > 150'000 || !Finite(g_support_draw)) {
                return false;
            }
            local = g_support_draw;
            first = g_support_draw_first;
        }
        glm::mat3 r{ 1.0f };
        glm::vec3 t{ 0.0f };
        if (first < 0 || !HandFrame(first, r, t)) {
            return false;
        }
        grip = t + r * local; // (the first hand as the body draws it now)
        return Finite(grip);
    }

    bool HeldBox(bool left, HeldShape& out)
    {
        if (GrenadeHand::HeldShape(left, out)) {
            return true;
        }
        // (fix170) An object held on its own by this hand: its own box.
        if (g_adopted_hand.load(std::memory_order_relaxed) == (left ? 0 : 1)) {
            AdoptBox box;
            {
                std::scoped_lock _{ g_adopt_box_mutex };
                box = g_adopt_box;
            }
            NodeXf xf;
            if (!box.valid || !GameFlow::gStore.internalSettings.handGrab || !DrawnWorld(box.root, xf)) {
                return false;
            }
            out.r        = xf.r;
            out.t        = xf.t;
            out.s        = xf.s;
            out.min      = box.lo;
            out.max      = box.hi;
            out.grab     = box.serial;
            out.cylinder = box.cylinder;
            out.axis     = box.axis;
            out.radius   = box.radius;
            return true;
        }
        // (fix170) This hand the second on the held object: the held object's box too (the fingers close round it where this hand is).
        const bool second = g_support_hand.load(std::memory_order_relaxed) == (left ? 0 : 1);
        if (g_track_spring.load(std::memory_order_relaxed) == 0 || ((g_hand.load(std::memory_order_relaxed) == 0) != left && !second) ||
            g_in_flight.load(std::memory_order_relaxed) || g_needs_catch.load(std::memory_order_relaxed) ||
            g_throw_spring.load(std::memory_order_relaxed) != 0 || !GameFlow::gStore.internalSettings.handGrab) {
            return false;
        }
        uintptr_t root = 0;
        glm::vec3 lo{}, hi{};
        int       serial = 0;
        {
            std::scoped_lock _{ g_track_mutex };
            if (!g_track.box) {
                return false;
            }
            root   = g_track.root;
            lo     = g_track.lo;
            hi     = g_track.hi;
            serial = g_track.serial;
            out.cylinder = g_track.cylinder;
            out.axis     = g_track.axis;
            out.radius   = g_track.radius;
        }
        NodeXf xf;
        if (!DrawnWorld(root, xf)) {
            return false;
        }
        out.r    = xf.r;
        out.t    = xf.t;
        out.s    = xf.s;
        out.min  = lo;
        out.max  = hi;
        out.grab = serial;
        return true;
    }

    namespace
    {
        // What the writer does with a call for the held object's collision object.
        struct WritePlan {
            enum class What { Pass, Measure, Present } what{ What::Pass };
            uintptr_t node{ 0 };
            NodeXf    body{};  // Measure: the body's pose the writer is given
            NodeXf    drawn{}; // Present: where the node is to be drawn
            uint64_t  frame{ 0 };
        };

        // `in` is the writer's transform (the body's pose, game units), `owner` the node the collision object moves; for Present,
        // `out` the one that draws the node where the hand holds it (eased with physics as above). Takes g_present_mutex (last).
        WritePlan PlanWrite(uintptr_t collision, uintptr_t owner, const float* in, float* out)
        {
            float    soft      = 1.0f;
            uint64_t disturbed = 0;
            bool     pulling   = false;
            {
                std::scoped_lock track{ g_track_mutex };
                soft      = g_contact.soft;
                disturbed = g_follow.disturbed_frame;
                pulling   = g_track.pulling;
            }
            const bool     held  = g_track_spring.load(std::memory_order_relaxed) != 0 && !pulling;
            const uint64_t frame = g_frame.load(std::memory_order_relaxed);
            WritePlan      plan;
            plan.frame = frame;
            std::scoped_lock _{ g_present_mutex };
            auto& p = g_present;
            if (p.root == 0 || p.collision != collision || owner != p.root || p.off) {
                return plan;
            }
            const NodeXf body = FromWriter(in);
            if (!FiniteXf(body) || !Rigid(body.r)) {
                return plan;
            }
            plan.node = p.root;
            if (!p.offset_known) {
                plan.what = WritePlan::What::Measure;
                plan.body = body;
                return plan;
            }
            const NodeXf physics   = Compose(body, p.offset); // the node where physics has it (as the game would draw it)
            const bool   new_frame = p.drawn_frame != frame;
            NodeXf       drawn     = physics;
            if (held && p.holding && p.frame + 3 >= frame && GameFlow::gStore.internalSettings.grabInHand) {
                glm::mat3 hand_r{ 1.0f };
                glm::vec3 hand_t{ 0.0f };
                if (HandFrame(p.hand, hand_r, hand_t)) {
                    const NodeXf in_hand{ hand_r * p.in_hand_r, hand_r * p.in_hand_t + hand_t, physics.s };
                    const float  gap      = glm::length(in_hand.t - physics.t);
                    const float  angle    = AngleBetween(in_hand.r, physics.r);
                    const bool   touching = soft < 0.95f || disturbed + 3 > frame;
                    const float  want     = touching || !(gap <= kInHandMaxGap) || !(angle <= kInHandMaxAngle) ? 1.0f : 0.0f;
                    if (new_frame) {
                        p.physics_weight += (want - p.physics_weight) * (want > p.physics_weight ? kToPhysicsRate : kToHandRate);
                        p.physics_weight = std::clamp(p.physics_weight, 0.0f, 1.0f);
                    }
                    const float     w      = p.physics_weight;
                    const glm::quat q_phys = glm::quat_cast(physics.r);
                    drawn.t                = glm::mix(in_hand.t, physics.t, w);
                    drawn.r                = glm::mat3_cast(glm::slerp(glm::quat_cast(in_hand.r), q_phys, w));
                    p.diff_t               = drawn.t - physics.t;
                    p.diff_q               = glm::quat_cast(drawn.r) * glm::inverse(q_phys);
                } else {
                    // The hand could not be read this time: the difference from physics stays as it was.
                    drawn.t = physics.t + p.diff_t;
                    drawn.r = glm::mat3_cast(p.diff_q * glm::quat_cast(physics.r));
                }
                if (new_frame) {
                    g_present_frames.fetch_add(1, std::memory_order_relaxed);
                    if (p.physics_weight < 0.05f) {
                        g_present_in_hand.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_present_offset_um.fetch_add(static_cast<int64_t>(glm::length(p.diff_t) * 1e6f), std::memory_order_relaxed);
                }
            } else {
                // Let go, flying, or no longer updated by the grab: what is left of the difference fades out.
                if (new_frame) {
                    p.diff_t *= kFadeRate;
                    p.diff_q         = glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, p.diff_q, kFadeRate);
                    p.physics_weight = 1.0f;
                }
                const float turn = 2.0f * std::acos(std::clamp(std::abs(p.diff_q.w), 0.0f, 1.0f));
                if ((glm::length(p.diff_t) < 0.001f && turn < 0.002f) || (!p.holding && p.release_frame + kForgetFrames < frame)) {
                    // Faded: drawn where physics has it (as the game would).
                    p.drawn_frame = frame;
                    p.physics     = physics;
                    p.drawn       = physics;
                    if (!p.holding) {
                        g_present_collision.store(0, std::memory_order_release);
                        g_present_root.store(0, std::memory_order_release);
                    }
                    return plan;
                }
                drawn.t = physics.t + p.diff_t;
                drawn.r = glm::mat3_cast(p.diff_q * glm::quat_cast(physics.r));
            }
            const NodeXf drawn_body = Compose(drawn, Inverse(p.offset));
            if (!FiniteXf(drawn) || !FiniteXf(drawn_body) || !Rigid(drawn_body.r)) {
                return plan;
            }
            ToWriter(drawn_body, in, out);
            p.drawn_frame = frame;
            p.physics     = physics;
            p.drawn       = drawn;
            plan.what     = WritePlan::What::Present;
            plan.drawn    = drawn;
            return plan;
        }

        // The writer was given the body's own pose: the node is where physics has it; how the node's pose follows from the
        // body's (three writes alike in a row).
        void MeasureOffset(uintptr_t collision, const WritePlan& plan, bool read, const NodeXf& now)
        {
            std::string tell;
            bool        failed = false;
            {
                std::scoped_lock _{ g_present_mutex };
                auto& p = g_present;
                if (p.collision != collision || p.root != plan.node || p.offset_known || p.off) {
                    return;
                }
                ++p.offset_tries;
                if (read) {
                    p.physics     = now;
                    p.drawn       = now;
                    p.drawn_frame = plan.frame;
                    const NodeXf o = Compose(Inverse(plan.body), now);
                    if (FiniteXf(o) && Rigid(o.r) && glm::length(o.t) < 2.0f) {
                        const bool alike = p.offset_alike > 0 && glm::length(o.t - p.offset_last.t) < kOffsetSameM &&
                                           AngleBetween(o.r, p.offset_last.r) < kOffsetSameRad && std::abs(o.s - p.offset_last.s) < 1e-3f * o.s;
                        p.offset_alike = alike ? p.offset_alike + 1 : 1;
                        p.offset_last  = o;
                    } else {
                        p.offset_alike = 0;
                    }
                    if (p.offset_alike >= kOffsetAlike) {
                        p.offset       = p.offset_last;
                        p.offset_known = true;
                        tell = fmt::format("[Grab] the held object goes through the game's scene writer (from its collision object's world update): its node "
                                           "is {:.1f} mm and {:.2f} degrees from its physics body (scale {:.3f}; the same over {} writes), so it is drawn in your "
                                           "hand from now on",
                            glm::length(p.offset.t) * 1000.0f, glm::degrees(AngleBetween(p.offset.r, glm::mat3{ 1.0f })), p.offset.s, kOffsetAlike);
                    }
                }
                if (!p.offset_known && p.offset_tries >= kOffsetTries) {
                    p.off  = true;
                    failed = true;
                    tell   = fmt::format("[Grab] the held object's node did not follow the game's scene writer the same way over {} writes ({}), so it is drawn where "
                                        "physics has it",
                        kOffsetTries, read ? "how it sits on its body kept changing" : "it could not be read after the writer");
                }
            }
            if (!tell.empty() && (failed ? g_present_fail_tells : g_present_tells).fetch_add(1) < 30) {
                spdlog::info("{}", tell);
            }
        }

        // The writer was given the place in the hand: the node must be there now.
        void CheckWrite(uintptr_t collision, const WritePlan& plan, bool read, const NodeXf& now)
        {
            const float gap   = read ? glm::length(now.t - plan.drawn.t) : -1.0f;
            const float angle = read ? AngleBetween(now.r, plan.drawn.r) : -1.0f;
            const bool  same  = read && gap < kWroteSameM && angle < kOffsetSameRad;
            g_present_wrote.fetch_add(1, std::memory_order_relaxed);
            if (!same) {
                g_present_wrote_off.fetch_add(1, std::memory_order_relaxed);
            }
            std::string tell;
            bool        failed = false;
            {
                std::scoped_lock _{ g_present_mutex };
                auto& p = g_present;
                if (p.collision != collision || p.root != plan.node) {
                    return;
                }
                if (p.wrote < kWroteCheck) {
                    ++p.wrote;
                    p.wrote_off += same ? 0 : 1;
                    if (p.wrote_off >= kWroteOffMax && !p.off) {
                        p.off            = true;
                        p.physics_weight = 1.0f; // (the fingers go back to where physics has it too: DrawnWorld)
                        p.diff_t         = glm::vec3{ 0.0f };
                        p.diff_q         = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                        failed           = true;
                        tell             = fmt::format("[Grab] the game's scene writer did not put the held object where it was drawn in {} of its first {} writes, so it "
                                            "is drawn where physics has it for the rest of this hold",
                            p.wrote_off, p.wrote);
                    }
                }
                if (tell.empty() && !p.told_wrote) {
                    p.told_wrote = true;
                    tell = read ? fmt::format("[Grab] the held object's first write in the hand: after the game's scene writer its node is {:.2f} mm and {:.3f} "
                                              "degrees from where it was drawn",
                                      gap * 1000.0f, glm::degrees(angle))
                                : std::string{ "[Grab] the held object's first write in the hand: its node could not be read after the game's scene writer" };
                }
            }
            if (!tell.empty() && (failed ? g_present_fail_tells : g_present_tells).fetch_add(1) < 30) {
                spdlog::info("{}", tell);
            }
        }

        // The game's scene writer (rva 237cb20; see "Drawing the held object in the hand"). Every other collision object, and
        // calls from anywhere but the collision object's own world update, go through as they are. (unsafe_call: the hook is
        // never removed while the game runs, and the writer runs on several threads at once; call() would make them wait in
        // turn.)
        void OnWriter(void* collision, float* transform)
        {
            const auto c = reinterpret_cast<uintptr_t>(collision);
            if (c == 0 || transform == nullptr || c != g_present_collision.load(std::memory_order_acquire)) {
                g_writer_hook.unsafe_call<void>(collision, transform);
                return;
            }
            const auto from = reinterpret_cast<uintptr_t>(HANDGRAB_RETURN_ADDRESS());
            if (from != g_writer_return) {
                if (g_writer_others.fetch_add(1) == 0) {
                    spdlog::info("[Grab] the game's scene writer was called for the held object from {:x} as well (not its collision object's world update); "
                                 "those calls are left as they are",
                        InImage(from) ? from - Base() : from);
                }
                g_writer_hook.unsafe_call<void>(collision, transform);
                return;
            }
            // (The pointer alone could be a new collision object where a freed one was: its node must be the held 3D.)
            uintptr_t owner = 0;
            if (!SafeRead(&owner, c + kCollisionOwner, sizeof(owner)) || owner == 0) {
                g_writer_hook.unsafe_call<void>(collision, transform);
                return;
            }
            alignas(32) float given[16]{};
            const WritePlan   plan = PlanWrite(c, owner, transform, given);
            g_writer_hook.unsafe_call<void>(collision, plan.what == WritePlan::What::Present ? given : transform);
            if (plan.what == WritePlan::What::Pass) {
                return;
            }
            // Where the writer put the node.
            NodeXf     now{};
            const bool read = NodeWorld(plan.node, now);
            if (plan.what == WritePlan::What::Measure) {
                MeasureOffset(c, plan, read, now);
            } else {
                CheckWrite(c, plan, read, now);
            }
        }
    }

    bool DebugHeld(HeldDebug& out)
    {
        {
            std::scoped_lock debug{ g_debug_mutex };
            if (g_debug.spring == 0 || NowUs() - g_debug.us > 150'000) {
                return false;
            }
            out.spring = g_debug.spring;
            out.body   = g_debug.body;
        }
        {
            std::scoped_lock track{ g_track_mutex };
            out.root      = g_track.box || g_track.have_held ? g_track.root : 0; // (else it may be an earlier grab's)
            out.head      = g_track.body;
            out.have_head = g_track.have_body;
        }
        NodeXf xf;
        out.have_object = out.root != 0 && HeldWorld(out.root, xf) && Finite(xf.t);
        out.object      = out.have_object ? xf.t : glm::vec3{ 0.0f };
        BodyRef         b;
        PlayerCollision you;
        if (out.root != 0 && ResolveBody(out.root, out.body, b)) {
            out.world = b.world;
        } else {
            out.world = ReadPlayerCollision(you) && ResolveBodyIn(you.world, out.body, b) ? b.world : 0;
        }
        return true;
    }

    std::string Status()
    {
        if (!g_ready.load()) {
            return std::string{ "Hand grab is " } + g_off_reason + ".";
        }
        return fmt::format("Ready{}{}{}. This session: {} objects held ({} grabbed with a grip, {} pulled with the gravity gloves, {} taken with a grip), {} let "
                           "go with the grip.",
            g_grip_ready.load() ? "" : " (grip grabbing unavailable: use the use button)", g_step_ready.load() ? "" : " (follows the hand once a frame)",
            g_pull_ready.load() ? "" : " (gravity gloves unavailable)", g_grabs.load(), g_grip_grabs.load(), g_pulls.load(), g_takeovers.load(),
            g_throws.load());
    }
}
