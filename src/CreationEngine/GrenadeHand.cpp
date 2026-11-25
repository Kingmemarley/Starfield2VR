#include "GrenadeHand.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <glm/glm.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "Consume.h"
#include "CreationEngineSingletonManager.h"
#include "HandActions.h"
#include "HandGrab.h"
#include "LeftWeapon.h"
#include "MeleeThrow.h"
#include "PointInteract.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"
#include "body/BodyIK.h"
#include "memory/GameCode.h"

namespace GrenadeHand
{
    namespace
    {
        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        template <class T>
        bool Read(uintptr_t at, T& out)
        {
            return SafeRead(&out, at, sizeof(T));
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // ---- The game's code used (Starfield 1.16.236), as the physics test builds dumped it; every piece checked at Install ----
        // PlaceAtMe's helper (self, position, angles, cell, worldspace, form, count, force persist, initially disabled, delete when
        // able, script engine, stack, offsets, alias) -> the new reference (its own hold on it released). Its start up to and with
        // its only use of the script engine (an error for the game's "no cell" value, compared at +0x53):
        constexpr uintptr_t kPlaceRva     = 0x207bcc0;
        constexpr uint8_t   kPlaceStart[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x4C, 0x89, 0x48, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10,
              0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xD8, 0xFB, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF0, 0x04, 0x00,
              0x00, 0xC5, 0xF8, 0x29, 0x70, 0xB8, 0x4D, 0x8B, 0xD1, 0x4D, 0x8B, 0xE8, 0x48, 0x8B, 0xDA, 0x4C, 0x8B, 0xF1, 0x44, 0x8B, 0xBD, 0x60, 0x04, 0x00,
              0x00, 0x4C, 0x8B, 0xA5, 0x58, 0x04, 0x00, 0x00, 0x33, 0xF6, 0x8B, 0x3D, 0x05, 0x70, 0x18, 0x04, 0x4C, 0x3B, 0x0D, 0xAE, 0x30, 0xE1, 0x03, 0x75,
              0x21, 0x44, 0x8B, 0x85, 0x88, 0x04, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x1E, 0xC7, 0xCB, 0x02, 0x48, 0x8B, 0x8D, 0x80, 0x04, 0x00, 0x00, 0xE8, 0x6A,
              0x1D, 0x27, 0xFF, 0x33, 0xC0, 0xE9, 0x9F, 0x0D, 0x00, 0x00 };
        constexpr size_t    kPlaceNoCellCmp = 0x53; // cmp r9,[rip+x]: the "no cell" value (7 bytes)
        // ...and script PlaceAtMe's call of it (rva 207d32f: self, &position, &angles, cell), and the worldspace it passes (rva
        // 207d2db: the function giving a reference's worldspace, called with self).
        constexpr uintptr_t kPlaceCallRva = 0x207d32f;
        constexpr uint8_t   kPlaceCall[]  = { 0x4C, 0x8B, 0xCF, 0x4C, 0x8D, 0x45, 0x90, 0x48, 0x8D, 0x55, 0x80, 0x48, 0x8B, 0x4C, 0x24, 0x70, 0xE8, 0x7C, 0xE9, 0xFF, 0xFF };
        constexpr size_t    kPlaceCallAt  = 0x10;
        constexpr uintptr_t kWorldspaceRva  = 0x207d2db;
        constexpr uint8_t   kWorldspace[]   = { 0x48, 0x8B, 0x4C, 0x24, 0x70, 0xE8, 0x7B, 0x1F, 0xAD, 0xFE };
        constexpr size_t    kWorldspaceCall = 0x05;
        // Script Delete's job (rva 2068840): a reference from its handle (the handle table's lookup with the function object that
        // hands out the reference, a hold taken), ...
        constexpr uintptr_t kDeleteJobRva  = 0x2068840;
        constexpr uint8_t   kDeleteJob[]   = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x53, 0x10, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0xFA, 0x48,
              0x8B, 0xF1, 0x33, 0xED, 0x49, 0x89, 0x6B, 0x20, 0x49, 0x89, 0x6B, 0x08, 0x49, 0x8D, 0x43, 0x08, 0x49, 0x89, 0x43, 0x18, 0x48, 0x8D, 0x05, 0xE2,
              0xF4, 0xA8, 0x02, 0x49, 0x89, 0x43, 0xA8, 0x49, 0x8D, 0x43, 0x18, 0x49, 0x89, 0x43, 0xB0, 0x4D, 0x8D, 0x43, 0xA8, 0x8B, 0x51, 0x10, 0x48, 0x8B,
              0x0D, 0xB8, 0xF8, 0xDF, 0x03, 0xE8, 0x63, 0x87, 0x86, 0x00, 0x48, 0x8B, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x85, 0xDB, 0x0F, 0x95, 0xC0 };
        constexpr size_t    kJobRefTable   = 0x27; // lea rax,[rip+x]: the function object's type table (7 bytes)
        constexpr size_t    kJobManager    = 0x41; // mov rcx,[rip+x]: the handle table (7 bytes)
        constexpr size_t    kJobLookup     = 0x48; // call: the lookup (table, handle, function object)
        // ...the delete itself (rva 20688dd: the null handle, then the handle of the reference handed to the delete, rva 187e370,
        // with false), ...
        constexpr uintptr_t kDeleteCallRva = 0x20688dd;
        constexpr uint8_t   kDeleteCall[]  = { 0x8B, 0x0D, 0x35, 0xA4, 0x19, 0x04, 0x89, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00, 0x8B, 0x53, 0x24, 0x85, 0xD2, 0x74,
              0x40, 0x48, 0x8D, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0x89, 0x5C, 0x24, 0x38, 0x48, 0x8D, 0x05, 0xD6, 0x42,
              0xA9, 0x02, 0x48, 0x89, 0x44, 0x24, 0x40, 0x48, 0x8D, 0x44, 0x24, 0x30, 0x48, 0x89, 0x44, 0x24, 0x48, 0x4C, 0x8D, 0x44, 0x24, 0x40, 0x48, 0x8B,
              0x0D, 0x1B, 0xF8, 0xDF, 0x03, 0xE8, 0xC6, 0x86, 0x86, 0x00, 0x8B, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00, 0x33, 0xD2, 0xE8, 0x38, 0x5A, 0x81, 0xFF };
        constexpr size_t    kCallNullHandle = 0x00; // mov ecx,[rip+x] (6 bytes)
        constexpr size_t    kCallDelete     = 0x56;
        constexpr uintptr_t kDeleteFnRva    = 0x187e370; // (fix188) where that delete is (its check by its own place)
        constexpr uint8_t   kDeleteStart[]  = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0xFA, 0x49, 0xC7, 0x43, 0x20,
              0x00, 0x00, 0x00, 0x00, 0x49, 0xC7, 0x43, 0x18, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8D, 0x43, 0x18, 0x49, 0x89, 0x43, 0xD8, 0x48, 0x8D, 0x05, 0xB2,
              0x99, 0x27, 0x03, 0x49, 0x89, 0x43, 0xE0, 0x49, 0x8D, 0x43, 0xD8, 0x49, 0x89, 0x43, 0xE8, 0x4D, 0x8D, 0x43, 0xE0, 0x8B, 0xD1 };
        // ...and letting go of the reference (rva 2068950: the release of its hold).
        constexpr uintptr_t kReleaseRva  = 0x2068950;
        constexpr uint8_t   kRelease[]   = { 0x48, 0x85, 0xDB, 0x74, 0x08, 0x48, 0x8B, 0xCB, 0xE8, 0x73, 0xC2, 0x31, 0xFE };
        constexpr size_t    kReleaseCall = 0x08;
        // Script SetMotionType's job (rva 2083837): its check that the 3D is in (rva 3537c0, the reference and 1), then the
        // motion type set on the 3D (rva 235a310: the 3D, the type, true, false, not "immediately simulate").
        constexpr uintptr_t kMotionRva     = 0x2083837;
        constexpr uint8_t   kMotion[]      = { 0x48, 0x8B, 0xCB, 0x48, 0x83, 0x7D, 0x48, 0x00, 0x0F, 0x84, 0x23, 0x01, 0x00, 0x00, 0x41, 0x8D, 0x55, 0x02, 0xE8,
              0x72, 0xFF, 0x2C, 0xFE, 0x84, 0xC0, 0x0F, 0x84, 0x06, 0x01, 0x00, 0x00, 0x41, 0x8B, 0xC4, 0x41, 0x38, 0x46, 0x20, 0x0F, 0x94, 0xC0, 0x89, 0x44,
              0x24, 0x20, 0x45, 0x33, 0xC9, 0x41, 0xB0, 0x01, 0x41, 0x8B, 0x56, 0x14, 0x48, 0x8B, 0x4D, 0x48, 0xE8, 0x99, 0x6A, 0x2D, 0x00 };
        constexpr size_t    kMotionLoaded  = 0x12;
        constexpr size_t    kMotionSet     = 0x3b;
        // (fix187) ...and the mark the job then puts on the reference (rva 2083877: its type table's slot 23, the game's change
        // record, rva b25c70, with 4 (r13 is -1 there): "its havok changed"). The mod made only the motion call, so its copies fell:
        // script SetPosition's move ends with the reference's havok set up again (its slot 164, rva b51820; rva 206689f), which for a
        // weapon (and the other kinds physics moves) makes its 3D's bodies dynamic again (rva b521c4: 235a310 with 1) unless the
        // reference has that mark (rva b52189: the same change record asked for flag 4). Each frame the copy was put in your hand,
        // made dynamic again, pulled down by gravity in the physics step and drawn where its body was (a dynamic part's proxy draws
        // it from the body, pushed ahead in time): it sank slowly below the hand, while the root
        // read right after the move was where it was put. (A helper's research of the exe, the key places checked again.)
        constexpr uintptr_t kMarkRva       = 0x2083877;
        constexpr uint8_t   kMark[]        = { 0x48, 0x8B, 0x03, 0x41, 0x8D, 0x55, 0x05, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0xB8, 0x00, 0x00, 0x00 };
        constexpr size_t    kMarkSlot      = 0xb8 / sizeof(uintptr_t); // 23
        constexpr uint32_t  kHavokChanged  = 4;
        constexpr uintptr_t kAddChangeRva  = 0xb25c70;
        constexpr uint8_t   kAddChange[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x89, 0x4C,
                  0x24, 0x20 };
        constexpr uintptr_t kResetCheckRva = 0xb52189; // (the havok set-up's ask for the mark, checked only to say so in the log)
        constexpr uint8_t   kResetCheck[]  = { 0x48, 0x8B, 0x05, 0x10, 0x33, 0x32, 0x05, 0x41, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x8B, 0x56, 0x28 };
        // The motion call keyframes with "force" (as the havok set-up calls it): without it the call does nothing unless the 3D's
        // root has bit 2 (havok) in its BSX flags (rva 235a32d), and says nothing.
        constexpr int       kMotionWatchMoves = 30; // its bodies' motion read after each of its first moves...
        constexpr int       kMotionCheckEvery = 15; // ...then every this many frames
        constexpr uintptr_t kLoadedFnRva   = 0x3537c0;  // (fix188) where that 3D check is (its check by its own place)
        constexpr uint8_t   kLoadedStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC,
              0x20, 0x48, 0x8D, 0xB9, 0xB8, 0x00, 0x00, 0x00, 0x8B, 0xEA, 0x48, 0x8D, 0x4F, 0x08, 0x32, 0xDB, 0xE8, 0xC8, 0xB1, 0xF7, 0x01, 0x48, 0x8B, 0x07,
              0x48, 0x85, 0xC0, 0x74, 0x06, 0x85, 0x68, 0x20, 0x0F, 0x95, 0xC3 };
        // Script SetPosition's job's move (rva 2068a00): the reference's cell (+0xb0), that cell's worldspace (+0x110, none for an
        // interior: +0x48 bit 0), then the move (rva 20660f0: the reference, &no target, cell, worldspace, &place, &angles).
        constexpr uintptr_t kMoveRva    = 0x2068a00;
        constexpr uint8_t   kMove[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48,
              0x83, 0xEC, 0x30, 0x4D, 0x8B, 0x88, 0xB0, 0x00, 0x00, 0x00, 0x4D, 0x8D, 0xB0, 0x80, 0x00, 0x00, 0x00, 0x33, 0xFF, 0x49, 0x8B, 0xD8, 0x48, 0x8B,
              0xF2, 0x48, 0x8B, 0xE9, 0x4D, 0x8B, 0xD1, 0x4D, 0x85, 0xC9, 0x75, 0x34, 0x0F, 0xB6, 0x43, 0x2C, 0x4D, 0x8B, 0xC1, 0xD0, 0xE8, 0xA8, 0x01, 0x75,
              0x1F, 0xF7, 0x43, 0x20, 0x00, 0x44, 0x00, 0x00, 0x74, 0x16, 0x48, 0x8B, 0x8B, 0xC8, 0x00, 0x00, 0x00, 0xE8, 0x26, 0xAC, 0x4A, 0xFE, 0x4C, 0x8B,
              0x93, 0xB0, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC0, 0x4D, 0x8B, 0xC8, 0x4D, 0x85, 0xC0, 0x74, 0x0E, 0x41, 0xF6, 0x41, 0x48, 0x01, 0x75, 0x07, 0x49,
              0x8B, 0xB9, 0x10, 0x01, 0x00, 0x00, 0x8B, 0x05, 0x98, 0xA2, 0x19, 0x04, 0x48, 0x8D, 0x54, 0x24, 0x58, 0x89, 0x44, 0x24, 0x58, 0x4C, 0x8B, 0xCF,
              0x48, 0x8D, 0x45, 0x20, 0x4C, 0x89, 0x74, 0x24, 0x28, 0x4D, 0x8B, 0xC2, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0xCB, 0xE8, 0x4B, 0xD6, 0xFF,
              0xFF };
        constexpr size_t    kMoveCall   = 0xa0;
        // A `ret` of the game's (the end of script SetPosition's job runner, rva 2066f7c: pop rbp; ret) for a refused activate.
        constexpr uintptr_t kRetRva   = 0x2066f7c;
        constexpr uint8_t   kRetEnd[] = { 0x5D, 0xC3 };

        // References (CommonLibSF's layouts, as the hand grab and the eating code read them).
        constexpr uintptr_t kFormFlags      = 0x20;       // kDeleted 1 << 5, kPersistent 1 << 10, kDisabled 1 << 11
        constexpr uintptr_t kRefHandle      = 0x24;       // its handle in the game's handle table (what script jobs keep)
        constexpr uintptr_t kFormId         = 0x28;
        constexpr uintptr_t kRefCell        = 0xb0;
        constexpr uintptr_t kRefLoaded      = 0xb8;       // loadedData...
        constexpr uintptr_t kLoaded3D       = 0x08;       // ...its 3D
        constexpr uintptr_t kCellFlags      = 0x48;       // bit 0: an interior
        constexpr uintptr_t kCellWorldspace = 0x110;
        constexpr uintptr_t kNodeWorld      = 0x80;       // NiAVObject::world (rotate rows, translate, scale)
        constexpr uintptr_t kNodeBound      = 0x100;      // NiAVObject::worldBound (centre, radius)
        constexpr uint32_t  kPersistent     = 1u << 10;
        constexpr uint32_t  kDeleted        = 1u << 5;
        constexpr uintptr_t kRefBaseObject  = 0x98;       // TESObjectREFR::data.objectReference (its base: HandGrab, Consume)
        constexpr uintptr_t kFormTypeAt     = 0x2e;       // a form's type: a weapon 0x30
        constexpr uint8_t   kWeaponType     = 0x30;
        constexpr uintptr_t kRefLocation    = 0x8c;       // TESObjectREFR::data.location (x y z; game units, metres)
        constexpr float     kStrayNearM     = 4.0f;       // (a copy from a save: this near you after the load)
        constexpr uint32_t  kMotionKeyframed = 2;         // Starfield's Motion_Keyframed (ObjectReference.psc)
        constexpr uint32_t  kLoadedPhysics   = 1;         // script SetMotionType's check

        // Timing and placing.
        constexpr int64_t kLoadTimeoutUs  = 2'000'000; // its 3D must be in by then
        constexpr int64_t kGroupTimeoutUs = 1'000'000; // and in your collision group this long after
        constexpr int     kGroupAskEvery  = 4;         // frames between asks while the change is not done (it may be queued)
        constexpr int     kNoHandFrames   = 30;        // your hand's place not found this many frames running: removed
        constexpr float   kDefaultRadius  = 0.04f;     // m, until its own size is read
        constexpr float   kSinkIn         = 0.8f;      // its middle this share of its bounding radius out from the palm...
        constexpr float   kMinOut         = 0.02f;     // ...within these (m)
        constexpr float   kMaxOut         = 0.07f;
        constexpr float   kBoxShare       = 0.7f;      // the fingers close around a box this share of its radius
        constexpr int     kBoundAfter     = 3;         // its middle and size read again after this many moves (drawn in the hand)

        using PlaceFn      = void* (*)(void*, const float*, const float*, void*, void*, void*, uint32_t, bool, bool, bool, void*, uint32_t, void*, void*);
        using WorldspaceFn = void* (*)(void*);
        struct RefCaptures {
            void** out{ nullptr };
        };
        struct LookupFunctor {
            uintptr_t table{ 0 };
            void*     captures{ nullptr };
        };
        using LookupFn  = void (*)(void*, uint32_t, LookupFunctor*);
        using ReleaseFn = void (*)(void*);
        using DeleteFn  = void (*)(uint32_t, bool);
        using LoadedFn  = bool (*)(void*, uint32_t);
        using MotionFn  = bool (*)(void*, uint32_t, bool, bool, uint32_t);
        using MoveFn    = void (*)(void*, uint32_t*, void*, void*, const float*, const float*);
        using AddChangeFn = bool (*)(void*, uint32_t);

        uintptr_t g_base{ 0 }, g_text_begin{ 0 }, g_text_end{ 0 };
        uintptr_t g_place_fn{ 0 }, g_worldspace_fn{ 0 }, g_no_cell_global{ 0 }, g_lookup_fn{ 0 }, g_manager_global{ 0 }, g_ref_table{ 0 },
            g_null_handle_global{ 0 }, g_delete_fn{ 0 }, g_release_fn{ 0 }, g_loaded_fn{ 0 }, g_motion_fn{ 0 }, g_move_fn{ 0 }, g_ret{ 0 };
        std::atomic<bool> g_ready{ false };
        std::string       g_off{ "not checked yet" };
        bool              g_mark_ready{ false }; // (fix187) the change record's mark as script SetMotionType makes it
        std::string       g_mark_why{ "not checked yet" };

        bool InText(uintptr_t a, size_t n = 1) { return a >= g_text_begin && a < g_text_end && n <= g_text_end - a; }

        bool FindText()
        {
            g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (g_base == 0) {
                return false;
            }
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                return false;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) {
                return false;
            }
            const auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
                if (std::strncmp(reinterpret_cast<const char*>(section->Name), ".text", 8) == 0) {
                    g_text_begin = g_base + section->VirtualAddress;
                    g_text_end   = g_text_begin + section->Misc.VirtualSize;
                    return g_text_end > g_text_begin;
                }
            }
            return false;
        }

        // A rel32 call at `at`: where it goes (0 if it is not one, or not into the game's code).
        uintptr_t CallTarget(uintptr_t at)
        {
            if (!InText(at, 5) || *reinterpret_cast<const uint8_t*>(at) != 0xE8) {
                return 0;
            }
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof(rel));
            const uintptr_t target = at + 5 + static_cast<intptr_t>(rel);
            return InText(target) ? target : 0;
        }

        // A rip-relative operand of the instruction at `at` (`length` bytes, the displacement in its last 4): its address in the
        // game's image (0 if outside it).
        uintptr_t RipTarget(uintptr_t at, size_t length)
        {
            if (!InText(at, length)) {
                return 0;
            }
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + length - 4), sizeof(rel));
            const uintptr_t target = at + length + static_cast<intptr_t>(rel);
            const auto*     dos    = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto*     nt     = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            return target > g_base && target < g_base + nt->OptionalHeader.SizeOfImage ? target : 0;
        }

        // (fix188) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return InText(GameCode::At(rva), n) && GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // Finds everything used, checked against the game's code (see above); false with the reason in g_off.
        bool FindCode()
        {
            if (!SameCode(kPlaceRva, kPlaceStart, sizeof(kPlaceStart))) {
                g_off = fmt::format("PlaceAtMe's helper (rva {:x}) is not as in Starfield 1.16.236", kPlaceRva);
                return false;
            }
            if (!SameCode(kPlaceCallRva, kPlaceCall, sizeof(kPlaceCall)) || CallTarget(At(kPlaceCallRva) + kPlaceCallAt) != At(kPlaceRva)) {
                g_off = "script PlaceAtMe does not call its helper as expected";
                return false;
            }
            g_place_fn       = At(kPlaceRva);
            g_no_cell_global = RipTarget(At(kPlaceRva) + kPlaceNoCellCmp, 7);
            if (!SameCode(kWorldspaceRva, kWorldspace, sizeof(kWorldspace)) || (g_worldspace_fn = CallTarget(At(kWorldspaceRva) + kWorldspaceCall)) == 0) {
                g_off = "script PlaceAtMe's worldspace function is not as expected";
                return false;
            }
            if (!SameCode(kDeleteJobRva, kDeleteJob, sizeof(kDeleteJob)) || !SameCode(kDeleteCallRva, kDeleteCall, sizeof(kDeleteCall)) ||
                !SameCode(kReleaseRva, kRelease, sizeof(kRelease))) {
                g_off = fmt::format("script Delete's job (rva {:x}) is not as in Starfield 1.16.236", kDeleteJobRva);
                return false;
            }
            g_ref_table          = RipTarget(At(kDeleteJobRva) + kJobRefTable, 7);
            g_manager_global     = RipTarget(At(kDeleteJobRva) + kJobManager, 7);
            g_lookup_fn          = CallTarget(At(kDeleteJobRva) + kJobLookup);
            g_null_handle_global = RipTarget(At(kDeleteCallRva) + kCallNullHandle, 6);
            g_delete_fn          = CallTarget(At(kDeleteCallRva) + kCallDelete);
            g_release_fn         = CallTarget(At(kReleaseRva) + kReleaseCall);
            if (g_delete_fn == 0 || g_delete_fn != At(kDeleteFnRva) || !SameCode(kDeleteFnRva, kDeleteStart, sizeof(kDeleteStart))) {
                g_off = "the delete script Delete's job calls is not as in Starfield 1.16.236";
                return false;
            }
            if (!SameCode(kMotionRva, kMotion, sizeof(kMotion))) {
                g_off = fmt::format("script SetMotionType's job (rva {:x}) is not as in Starfield 1.16.236", kMotionRva);
                return false;
            }
            g_loaded_fn = CallTarget(At(kMotionRva) + kMotionLoaded);
            g_motion_fn = CallTarget(At(kMotionRva) + kMotionSet);
            if (g_loaded_fn == 0 || g_loaded_fn != At(kLoadedFnRva) || !SameCode(kLoadedFnRva, kLoadedStart, sizeof(kLoadedStart))) {
                g_off = "script SetMotionType's 3D check is not as in Starfield 1.16.236";
                return false;
            }
            // (fix187) The mark (without it the copies work as before, kept keyframed after every move instead: see Hold).
            g_mark_ready = SameCode(kMarkRva, kMark, sizeof(kMark)) && SameCode(kAddChangeRva, kAddChange, sizeof(kAddChange));
            g_mark_why   = !g_mark_ready ? fmt::format("not made: script SetMotionType's mark (rva {:x}, rva {:x}) is not as in Starfield 1.16.236", kMarkRva,
                                             kAddChangeRva)
                         : SameCode(kResetCheckRva, kResetCheck, sizeof(kResetCheck))
                             ? "ready (script SetMotionType's mark, rva b25c70 with 4: the game's move then leaves their motion alone, rva b52189)"
                             : "ready (script SetMotionType's mark, rva b25c70 with 4; the move's havok set-up is not as expected, rva b52189)";
            if (!SameCode(kMoveRva, kMove, sizeof(kMove))) {
                g_off = fmt::format("script SetPosition's move (rva {:x}) is not as in Starfield 1.16.236", kMoveRva);
                return false;
            }
            g_move_fn = CallTarget(At(kMoveRva) + kMoveCall);
            if (SameCode(kRetRva, kRetEnd, sizeof(kRetEnd))) {
                g_ret = At(kRetRva) + 1;
            }
            if (g_no_cell_global == 0 || g_ref_table == 0 || g_manager_global == 0 || g_lookup_fn == 0 || g_null_handle_global == 0 || g_release_fn == 0 ||
                g_motion_fn == 0 || g_move_fn == 0 || g_ret == 0) {
                g_off = "a function or value the scripts use is not in the game's image";
                return false;
            }
            return true;
        }

        // ---- References by handle ----
        uint32_t NullHandle()
        {
            uint32_t h = 0;
            Read(g_null_handle_global, h);
            return h;
        }

        // The reference with this handle (a hold taken on it: Release it), or 0, as every script job finds its reference.
        uintptr_t Lookup(uint32_t handle)
        {
            uintptr_t manager = 0;
            if (handle == 0 || handle == NullHandle() || !Read(g_manager_global, manager) || manager == 0) {
                return 0;
            }
            void*         out = nullptr;
            RefCaptures   captures{ &out };
            LookupFunctor functor{ g_ref_table, &captures };
            reinterpret_cast<LookupFn>(g_lookup_fn)(reinterpret_cast<void*>(manager), handle, &functor);
            return reinterpret_cast<uintptr_t>(out);
        }

        void Release(uintptr_t ref)
        {
            if (ref != 0) {
                reinterpret_cast<ReleaseFn>(g_release_fn)(reinterpret_cast<void*>(ref));
            }
        }

        // A reference found by its handle, held for this scope.
        struct Found
        {
            uintptr_t ref{ 0 };
            explicit Found(uint32_t handle) : ref(Lookup(handle)) {}
            // (fix154) Only the reference with this form id (a copy's: anything else the handle finds is not it; the review).
            Found(uint32_t handle, uint32_t ref_id) : ref(Lookup(handle))
            {
                uint32_t id = 0;
                if (ref != 0 && ref_id != 0 && (!Read(ref + kFormId, id) || id != ref_id)) {
                    Release(ref);
                    ref = 0;
                }
            }
            ~Found() { Release(ref); }
            Found(const Found&)            = delete;
            Found& operator=(const Found&) = delete;
        };

        uintptr_t Root(uintptr_t ref)
        {
            uintptr_t loaded = 0, root = 0;
            if (ref == 0 || !Read(ref + kRefLoaded, loaded) || loaded == 0 || !Read(loaded + kLoaded3D, root)) {
                return 0;
            }
            return root;
        }

        // ---- Transforms: a point x of the 3D's space is at r * (s * x) + t in the game world (its stored rows are glm's columns) ----
        struct Xf
        {
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
        };

        bool NodeWorld(uintptr_t node, Xf& out)
        {
            float raw[16]{};
            if (node == 0 || !SafeRead(raw, node + kNodeWorld, sizeof(raw))) {
                return false;
            }
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    out.r[a][b] = raw[a * 4 + b];
                }
            }
            out.t = { raw[12], raw[13], raw[14] };
            out.s = raw[15];
            return Finite(out.t) && Finite(out.r[0]) && Finite(out.r[1]) && Finite(out.r[2]) && std::isfinite(out.s) && out.s > 0.01f && out.s < 100.0f;
        }

        // A reference's stored angles (radians) for a rotation. Convention 0: the stored rows (glm's columns of the rotation) are
        // rebuilt as Rz * Ry * Rx (ROCK found this for Fallout 4 VR's references); convention 1: the rotation itself is.
        glm::vec3 Angles(const glm::mat3& m, int convention)
        {
            auto S = [&](int row, int column) { return convention == 0 ? m[row][column] : m[column][row]; };
            const float across = std::hypot(S(0, 0), S(1, 0));
            const float y      = std::atan2(-S(2, 0), across);
            if (across > 1e-6f) {
                return { std::atan2(S(2, 1), S(2, 2)), y, std::atan2(S(1, 0), S(0, 0)) };
            }
            return { 0.0f, y, std::atan2(-S(0, 1), S(1, 1)) };
        }

        float DegreesBetween(const glm::mat3& a, const glm::mat3& b)
        {
            const glm::mat3 d = glm::transpose(a) * b;
            return glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
        }

        // ---- The copies ----
        // (fix154) Two: [0] the grenade in your right hand (as before), [1] the left hand's melee weapon (LeftWeapon: dual wielding). The
        // weapon's is held by its grip (its model's own origin) at the left palm, its blade along the fist's line (across the palm),
        // and is kept as long as the left weapon is out; the pick is blanked only for the grenade's (a weapon held for minutes must not
        // keep you from using anything: its activation is still refused).
        constexpr int kGrenade = 0, kLeftWeapon = 1, kCopies = 2;
        constexpr float kWeaponRadiusMax = 1.6f;  // m: a weapon's bound may be this large (a grenade's 0.3)
        constexpr float kWeaponGripOut   = 0.025f; // its grip this far out from the palm
        enum class Stage { None, Loading, Holding };
        struct Copy
        {
            Stage     stage{ Stage::None };
            bool      weapon{ false }; // (fix154) the left weapon's
            uintptr_t form{ 0 };
            uint32_t  choice{ 0 };     // (fix166) the left weapon's choice it was made for (LeftWeapon::ChoiceSerial: its mods)
            int       serial{ 0 }; // (fix154) its grip for the fingers (HeldShape::grab)
            uintptr_t root_seen{ 0 }; // (fix155) its 3D's root as last held (another one: put apart and keyframed again)
            uint32_t  handle{ 0 };
            uint32_t  ref_id{ 0 };
            uint32_t  form_id{ 0 };
            int64_t   made_us{ 0 };
            int64_t   loaded_us{ 0 };
            int       frames{ 0 };
            int       load_frames{ 0 };
            bool      grouped{ false };
            int       group_asks{ 0 };
            int       since_ask{ 0 };
            std::string group_what;
            bool      keyframed{ false };
            // (fix187) Kept keyframed: marked as script SetMotionType marks it; its bodies' motion read after moves; keyframed again
            // when the game had made them dynamic (and from then after every move).
            bool      marked{ false };
            bool      key_each_move{ false };
            bool      motion_told{ false };
            bool      told_falling{ false };
            int       falling_run{ 0 }; // reads in a row that found it moved by physics
            int       motion_reads{ 0 };
            int       found_falling{ 0 };
            int       rekeys{ 0 };
            std::string first_motion;
            // Its model's middle in its own space and its bounding radius (game world), as read from where it is drawn.
            glm::vec3 centre_local{ 0.0f };
            float     radius{ kDefaultRadius };
            float     scale{ 1.0f };
            bool      bound_read{ false };
            int       no_hand{ 0 };
            // Where it ended up against where it was put (right after each move).
            int       moves{ 0 };
            int       checked{ 0 };
            double    off_sum{ 0.0 };
            float     off_max{ 0.0f };
            double    turn_sum{ 0.0 };
            float     turn_max{ 0.0f };
            Xf        last_to{};
            bool      have_last_to{ false };
            int       logged_moves{ 0 };
        };
        Copy g_copies[kCopies]{};
        bool g_failed_take[kCopies]{}; // making one failed for this grenade (or this left weapon): not again until the next

        // The angle convention: the first a copy follows within 10 degrees (moves whose place landed exactly, so the 3D was
        // updated by the move itself); 0 unless it misses and 1 does better.
        int    g_convention{ 0 };
        int    g_convention_tests[2]{ 0, 0 };
        double g_convention_error[2]{ 0.0, 0.0 };
        bool   g_convention_settled{ false };

        // For other threads (by slot).
        std::atomic<uintptr_t> g_copy_ref[kCopies]{};
        std::atomic<uint32_t>  g_copy_id[kCopies]{};   // its form id (a reference later made at the same address is not refused)
        // A copy whose 3D was still there right after its delete (or that could not be found by its handle): its activation stays
        // refused until it is gone (main thread; the atomics for the activate).
        struct Orphan
        {
            uint32_t  handle{ 0 };
            uint32_t  ref_id{ 0 };
            int64_t   since{ 0 };
            int       retries{ 0 };
        };
        Orphan                 g_orphan[kCopies]{};
        std::atomic<uintptr_t> g_orphan_ref[kCopies]{};
        std::atomic<uint32_t>  g_orphan_id[kCopies]{};
        constexpr int64_t      kOrphanRetryUs = 1'000'000;
        constexpr int          kOrphanRetries = 3;
        std::atomic<bool>      g_picks_off{ false };
        std::atomic<bool>      g_in_hand[kCopies]{};
        std::atomic<bool>      g_flying[kCopies]{}; // (fix156) the left weapon's copy where its throw flies
        std::mutex             g_shape_mutex;
        HandGrab::HeldShape    g_shape[kCopies]{};
        bool                   g_have_shape[kCopies]{};
        int                    g_serial{ 1'000'000 }; // (the hand grab's serials are small)
        // (fix154) The left weapon asked for (LeftWeapon, each frame; 0: none) and the copy's blade for others (main thread).
        std::atomic<uintptr_t> g_left_want{ 0 };
        std::mutex             g_blade_mutex;
        glm::vec3              g_blade_grip{ 0.0f }, g_blade_way{ 0.0f, 0.0f, 1.0f };
        glm::vec3              g_blade_local{ 0.0f, 0.0f, -1.0f }; // (its way in the left controller's aim frame, for the hits: the review)
        bool                   g_blade_local_valid{ false };
        float                  g_blade_length{ 0.0f };
        bool                   g_blade_valid{ false };

        // Counts.
        std::atomic<int> g_made{ 0 };
        std::atomic<int> g_removed{ 0 };
        std::atomic<int> g_failed{ 0 };
        std::atomic<int> g_weapon_made{ 0 };
        std::string      g_last; // the last copy's outcome (main thread)
        std::mutex       g_last_mutex;
        constexpr int    kLogCopies = 20; // copies whose story is logged in full (then every 10th)

        // (fix154) Copies that may be in a save (one there while the game saved: the review: after a load it would lie in the world, a
        // weapon for free): their reference's id and its base's, kept in the settings (StrayCopies), looked for after each load and
        // deleted when found (and never activated meanwhile). The copies alive (or gone a moment ago) are noted as a save starts.
        constexpr int     kStrays      = 128; // (one more each save made with a copy in a hand: room for many; the review)
        constexpr int64_t kGoneRecentUs = 5'000'000;
        struct Stray
        {
            uint32_t ref_id{ 0 };
            uint32_t base_id{ 0 };
        };
        std::mutex            g_stray_mutex;
        Stray                 g_strays[kStrays]{};
        int                   g_stray_next{ 0 };
        std::atomic<bool>     g_strays_changed{ false };
        std::atomic<uint32_t> g_stray_ids[kStrays]{}, g_stray_bases[kStrays]{}; // (for the activate, any thread)
        std::atomic<uint32_t> g_copy_base[kCopies]{};   // (the copy's base's id, with g_copy_id)
        std::atomic<uint32_t> g_orphan_base[kCopies]{};
        std::atomic<uint32_t> g_gone_id[kCopies]{}, g_gone_base[kCopies]{};
        std::atomic<int64_t>  g_gone_us[kCopies]{};

        // (fix154) Saves (the research, checked against 1.16.236). The game's save manager (BGSSaveLoadManager: its singleton at
        // 0x145E754A8, as its task processor 0x141852AB0 reads it each frame on the main thread, `mov r15,[...]; cmp dword [r15+50h],0`):
        // its asked-for tasks at +0x50. An autosave (0x1, then 0x100) and a quicksave (0x8, then 0x200) are asked for 2-3 frames before
        // they are made; the exit saves (0x1000, 0x2000) and a forced save (0x2: script Game.RequestSave, the console, the pause menu)
        // are asked for too. Meanwhile the copies in your hands are taken away and not made again (until the asking ends, at most 5 s),
        // so they are not in the save. Every save is made by 0x14183ED20 (SaveGame; its only callers are those saves): hooked at its
        // start, the copies there then (or gone a moment ago) are noted, to be deleted if a load brings one back.
        constexpr uintptr_t kSaveTasksRva  = 0x1852ac7;
        constexpr uint8_t   kSaveTasks[]   = { 0x4c, 0x8b, 0x3d };             // mov r15, [the manager]
        constexpr uint8_t   kSaveTasksCmp[] = { 0x41, 0x83, 0x7f, 0x50, 0x00 }; // cmp dword [r15+50h], 0
        constexpr uintptr_t kSaveManagerRva = 0x5e754a8;
        constexpr uintptr_t kSaveTasksAt   = 0x50;
        constexpr uint32_t  kSaveBits      = 0x1 | 0x2 | 0x8 | 0x100 | 0x200 | 0x1000 | 0x2000;
        constexpr uintptr_t kSaveGameRva   = 0x183ed20;
        constexpr uint8_t   kSaveGameStart[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                                 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00, 0x45, 0x8b, 0xf1, 0x45, 0x8b,
                                                 0xf8, 0x48, 0x8b, 0xf2, 0x48, 0x8b, 0xf9, 0x83, 0x79, 0x50, 0x00, 0x75, 0x07, 0xb2, 0x05 };
        constexpr size_t    kSaveBeginCall = 0x2f;      // call 0x141853260 (the save's begin event)
        constexpr uintptr_t kSaveBeginRva  = 0x1853260;
        constexpr int64_t   kSaveHoldMaxUs = 5'000'000; // (a save asked for and not made: the copies come back after this)
        constexpr int64_t   kSaveAfterUs   = 300'000;   // (and a moment after the asking ends)
        uintptr_t           g_save_manager_at{ 0 };     // (where the manager's pointer is: 0 if the code is not as expected)
        safetyhook::MidHook g_save_hook{};
        std::atomic<int>    g_saves_seen{ 0 };
        void                InstallSaveWatch(); // (below)

        bool LogThis(int n) { return n <= kLogCopies || n % 10 == 0; }
        const char* Tag(int slot) { return slot == kLeftWeapon ? "[LeftWeapon]" : "[Nade]"; }
        const char* What(int slot) { return slot == kLeftWeapon ? "left weapon" : "grenade"; }

        void SetLast(std::string text)
        {
            std::scoped_lock _{ g_last_mutex };
            g_last = std::move(text);
        }

        void ClearShape(int slot)
        {
            std::scoped_lock _{ g_shape_mutex };
            g_have_shape[slot] = false;
            if (slot == kLeftWeapon) {
                std::scoped_lock b{ g_blade_mutex };
                g_blade_valid       = false;
                g_blade_local_valid = false;
            }
        }

        // (A copy gone: noted a moment, for a save made just then.)
        void NoteGone(int slot, uint32_t ref_id, uint32_t base_id)
        {
            if (ref_id != 0 && base_id != 0) {
                g_gone_base[slot].store(base_id);
                g_gone_id[slot].store(ref_id);
                g_gone_us[slot].store(NowUs());
            }
        }

        void Clear(int slot)
        {
            if (g_orphan_id[slot].load() != g_copies[slot].ref_id) {
                NoteGone(slot, g_copies[slot].ref_id, g_copies[slot].form_id); // (an orphan: noted when it is gone at last)
            }
            g_copies[slot] = Copy{};
            g_copy_ref[slot].store(0);
            g_copy_id[slot].store(0);
            g_copy_base[slot].store(0);
            if (slot == kGrenade) {
                g_picks_off.store(false);
            }
            g_in_hand[slot].store(false);
            ClearShape(slot);
            g_flying[slot].store(false);
        }

        // Where the copy goes now. A grenade: its model's middle out from your palm by about its radius (resting against the palm where
        // a held object rests), turned with your hand (its up along the line across the palm, its forward along the fingers). (fix154)
        // A weapon: its grip (its model's own origin) just out from the left palm, its blade (from the grip to its model's middle) along
        // the fist's line across the palm, its model's other axes turned with the fingers.
        bool Target(const Copy& c, Xf& out)
        {
            glm::vec3 surface{}, facing{}, forward{}, side{};
            if (!HandGrab::HoldFrame(c.weapon, surface, facing, forward, side)) {
                return false;
            }
            // (fix157) Placed before your character moves this frame: moved on by the camera root's expected move, so it is drawn in your hand
            // as the body's hand is, not a frame behind while you move.
            surface += tracking::FrameStartLead();
            if (c.weapon) {
                const float blade_len = glm::length(c.centre_local);
                if (!(blade_len > 0.01f)) {
                    return false;
                }
                // The hand's frame: the blade's way first (the fist's line), the fingers' way made square to it.
                const glm::vec3 way = glm::normalize(side);
                glm::vec3       fwd = forward - way * glm::dot(forward, way);
                if (glm::length(fwd) < 1e-3f) {
                    return false;
                }
                fwd = glm::normalize(fwd);
                const glm::mat3 hand{ glm::cross(fwd, way), fwd, way }; // (columns: x, y = fingers, z = blade)
                // The model's frame the same way: z its blade (to its middle), y its own +y made square to it.
                const glm::vec3 m_way = c.centre_local / blade_len;
                glm::vec3       m_fwd = glm::vec3{ 0.0f, 1.0f, 0.0f } - m_way * m_way.y;
                if (glm::length(m_fwd) < 0.2f) {
                    m_fwd = glm::vec3{ 1.0f, 0.0f, 0.0f } - m_way * m_way.x;
                }
                m_fwd = glm::normalize(m_fwd);
                const glm::mat3 model{ glm::cross(m_fwd, m_way), m_fwd, m_way };
                const glm::mat3 m = hand * glm::transpose(model); // (the model's frame onto the hand's)
                out.r = m;
                out.s = c.scale;
                out.t = surface + facing * kWeaponGripOut;
                return Finite(out.t) && Finite(m[0]) && std::abs(glm::determinant(m) - 1.0f) < 0.05f;
            }
            glm::mat3 m{ 1.0f };
            m[2] = side;
            m[1] = forward;
            m[0] = glm::cross(m[1], m[2]);
            const float     out_from_palm = std::clamp(c.radius * kSinkIn, kMinOut, kMaxOut);
            const glm::vec3 centre        = surface + facing * out_from_palm;
            out.r = m;
            out.s = c.scale;
            out.t = centre - m * (c.centre_local * c.scale);
            return Finite(out.t) && Finite(m[0]) && std::abs(glm::determinant(m) - 1.0f) < 0.05f;
        }

        // Reads its model's middle and size from where its 3D is drawn now (its world and bounding sphere): false if they do not
        // look like a hand-held thing's (a weapon may be larger).
        bool ReadBound(uintptr_t root, Copy& c)
        {
            Xf    xf;
            float sphere[4]{};
            if (!NodeWorld(root, xf) || !SafeRead(sphere, root + kNodeBound, sizeof(sphere))) {
                return false;
            }
            const glm::vec3 centre{ sphere[0], sphere[1], sphere[2] };
            const float     radius = sphere[3];
            const float     most   = c.weapon ? kWeaponRadiusMax : 0.3f;
            if (!Finite(centre) || !std::isfinite(radius) || radius < 0.01f || radius > most || glm::length(centre - xf.t) > most) {
                return false;
            }
            c.centre_local = glm::transpose(xf.r) * (centre - xf.t) / xf.s;
            c.radius       = radius;
            c.scale        = std::clamp(xf.s, 0.5f, 2.0f);
            c.bound_read   = true;
            return true;
        }

        // Gone without us (the game removed it: a cell unloaded, ...): nothing to delete.
        void Gone(int slot, const char* why)
        {
            g_removed.fetch_add(1);
            const std::string text = fmt::format("{} copy {:08X} gone by itself ({}) after {} frames", What(slot), g_copies[slot].ref_id, why,
                g_copies[slot].frames);
            spdlog::info("{} {}", Tag(slot), text);
            SetLast(text);
            Clear(slot);
        }

        void SetOrphan(int slot, uintptr_t ref, uint32_t handle, uint32_t ref_id)
        {
            g_orphan[slot] = Orphan{ handle, ref_id, NowUs(), 0 };
            g_orphan_base[slot].store(g_copies[slot].ref_id == ref_id ? g_copies[slot].form_id : 0);
            g_orphan_id[slot].store(ref_id);
            g_orphan_ref[slot].store(ref);
        }

        void ClearOrphan(int slot)
        {
            NoteGone(slot, g_orphan_id[slot].load(), g_orphan_base[slot].load());
            g_orphan_base[slot].store(0);
            g_orphan_ref[slot].store(0);
            g_orphan_id[slot].store(0);
            g_orphan[slot] = Orphan{};
        }

        // Deletes the copy as script Delete does, and says how its time in your hand went.
        void Remove(int slot, const std::string& why)
        {
            auto&       c = g_copies[slot];
            std::string result;
            {
                Found found(c.handle, c.ref_id);
                if (found.ref != 0) {
                    uint32_t flags = 0, after = 0;
                    Read(found.ref + kFormFlags, flags);
                    reinterpret_cast<DeleteFn>(g_delete_fn)(c.handle, false);
                    Read(found.ref + kFormFlags, after);
                    const bool still_there = Root(found.ref) != 0;
                    result = fmt::format("deleted as script Delete does (form flags {:#x} -> {:#x}{}; its 3D {})", flags, after,
                        (flags & kPersistent) != 0 ? ", it was persistent" : "", still_there ? "still there: its activation stays refused until it is gone" : "gone");
                    if (still_there) {
                        SetOrphan(slot, found.ref, c.handle, c.ref_id);
                    }
                } else {
                    result = "already gone (the game removed it)";
                }
            }
            const int  n    = g_removed.fetch_add(1) + 1;
            const auto text = fmt::format("{} copy {:08X} removed ({}): {}; {} frames, {} moves{}{}", What(slot), c.ref_id, why, result, c.frames, c.moves,
                c.checked > 0 ? fmt::format(", drawn {:.1f} mm from where it was put on average (at most {:.1f}), turned {:.1f} degrees off on average "
                                            "(at most {:.1f}; angle convention {})",
                                    c.off_sum / c.checked * 1000.0, c.off_max * 1000.0f, c.turn_sum / c.checked, c.turn_max, g_convention)
                              : std::string{},
                // (fix187) How it was kept keyframed.
                !c.keyframed ? std::string{}
                             : fmt::format("; (fix187) {}, its motion read {} times{}, found moved by physics {} times, keyframed again {} times{}",
                                   c.marked ? "marked" : "not marked", c.motion_reads, c.first_motion.empty() ? "" : " (the first " + c.first_motion + ")",
                                   c.found_falling, c.rekeys, c.key_each_move ? " (after every move)" : ""));
            if (LogThis(n)) {
                spdlog::info("{} {}", Tag(slot), text);
            }
            SetLast(text);
            Clear(slot);
        }

        // Removed because something went wrong: no other copy until the next grenade is taken (or the left weapon changes).
        void Fail(int slot, const std::string& why)
        {
            g_failed_take[slot] = true;
            g_failed.fetch_add(1);
            Remove(slot, why);
        }

        // A copy that outlived its delete: deleted again a few times, and refused until it is gone (main thread, every frame).
        void UpdateOrphan(int slot)
        {
            auto& o = g_orphan[slot];
            if (o.handle == 0) {
                return;
            }
            Found found(o.handle, o.ref_id);
            if (found.ref == 0 || Root(found.ref) == 0) {
                spdlog::info("{} copy {:08X}: {}", Tag(slot), o.ref_id, found.ref == 0 ? "gone at last" : "its 3D gone at last");
                ClearOrphan(slot);
                return;
            }
            const int64_t now = NowUs();
            if (now - o.since > kOrphanRetryUs && o.retries < kOrphanRetries) {
                ++o.retries;
                o.since = now;
                reinterpret_cast<DeleteFn>(g_delete_fn)(o.handle, false);
                spdlog::info("{} copy {:08X}: deleted again (try {}); its 3D {}", Tag(slot), o.ref_id, o.retries + 1, Root(found.ref) == 0 ? "gone" : "still there");
            }
        }

        // (fix166) The left weapon's copy made with the chosen item's own mods (the left saber was always the default red; a
        // helper's research, checked against 1.16.236: the copy was placed from the base weapon, so the game gave it the weapon's default
        // parts). PlaceAtMe's helper fills the new reference's data at [rbp+0x1C0] (its +0x70: the object-instance extra to give the
        // reference, cleared at 0x14207C094 (rsi is 0 there)) and makes each reference with 0x1405A10E0(manager, &out, &data)
        // (0x14207C110-0x14207C123; r15 counts the references still to make, 0x14207C205). That one (the data in r15, 0x1405A1130) adds a
        // given extra to the new reference's own extra data (0x1405A27DD: [r15+0x70]; added by 0x1402C4160 at 0x1405A27F2, which takes it:
        // freed with the reference) instead of making the weapon's default one. Hooked at 0x14207C117 (r8 the data): a copy of the chosen
        // item's extra (LeftWeapon::CloneModsExtra) given, only on the thread that asked, for a single reference, once.
        constexpr uintptr_t kPlaceDataRva      = 0x207c110;
        constexpr uint8_t   kPlaceData[]       = { 0x4C, 0x8D, 0x85, 0xC0, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x8B, 0x0D };
        constexpr size_t    kPlaceDataHook     = 0x07; // (lea rdx,[rsp+38h]: r8 is the data)
        constexpr size_t    kPlaceDataCreate   = 0x13; // (call 0x1405A10E0)
        constexpr uintptr_t kPlaceDataClearRva = 0x207c094;
        constexpr uint8_t   kPlaceDataClear[]  = { 0x48, 0x89, 0xB5, 0x30, 0x02, 0x00, 0x00 }; // mov [rbp+230h],rsi
        constexpr uintptr_t kPlaceCountRva     = 0x207c205;
        constexpr uint8_t   kPlaceCount[]      = { 0x49, 0x83, 0xEF, 0x01, 0x0F, 0x85, 0x01, 0xFF, 0xFF, 0xFF }; // sub r15,1; jne 0x14207C110
        constexpr uintptr_t kCreateRva         = 0x5a10e0;
        constexpr uint8_t   kCreateStart[]     = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08 };
        constexpr uintptr_t kCreateDataRva     = 0x5a1130;
        constexpr uint8_t   kCreateData[]      = { 0x4D, 0x8B, 0xF8 }; // mov r15,r8
        constexpr uintptr_t kCreateExtraRva    = 0x5a27dd;
        constexpr uint8_t   kCreateExtra[]     = { 0x49, 0x8B, 0x57, 0x70, 0x48, 0x85, 0xD2, 0x74, 0x13, 0x48, 0x8B, 0x44, 0x24, 0x50, 0x48, 0x8B, 0x88,
                                                   0xC8, 0x00, 0x00, 0x00, 0xE8 };
        constexpr size_t    kCreateExtraAdd    = 0x15;
        constexpr uintptr_t kExtraAddRva       = 0x2c4160;
        constexpr uintptr_t kDataGivenExtra    = 0x70;
        safetyhook::MidHook    g_place_data_hook{};
        thread_local uintptr_t t_give_extra{ 0 };
        thread_local bool      t_extra_given{ false };
        thread_local uintptr_t t_give_form{ 0 };  // (the form being placed: only its placement gets the extra; the review)
        constexpr uintptr_t    kDataObject{ 0x30 }; // (the placement's data: its object, written at 0x14207C052 from the form argument)
        std::atomic<int>       g_mods_told{ 0 };

        void OnPlaceData(safetyhook::Context& ctx)
        {
            if (t_give_extra == 0) {
                return;
            }
            // (One reference to make, and nothing given yet: a second reference must never get the same extra.)
            auto* const given = reinterpret_cast<uintptr_t*>(ctx.r8 + kDataGivenExtra);
            uintptr_t   object = 0;
            if (ctx.r15 != 1 || *given != 0 || !Read(ctx.r8 + kDataObject, object) || object != t_give_form) {
                return; // (not our placement: a nested one on this thread keeps its own parts)
            }
            *given        = t_give_extra;
            t_give_extra  = 0;
            t_extra_given = true;
        }

        void InstallPlaceData()
        {
            const uintptr_t at = At(kPlaceDataRva);
            if (SameCode(kPlaceDataRva, kPlaceData, sizeof(kPlaceData)) && CallTarget(at + kPlaceDataCreate) == At(kCreateRva) &&
                SameCode(kPlaceDataClearRva, kPlaceDataClear, sizeof(kPlaceDataClear)) && SameCode(kPlaceCountRva, kPlaceCount, sizeof(kPlaceCount)) &&
                SameCode(kCreateRva, kCreateStart, sizeof(kCreateStart)) && SameCode(kCreateDataRva, kCreateData, sizeof(kCreateData)) &&
                SameCode(kCreateExtraRva, kCreateExtra, sizeof(kCreateExtra)) && CallTarget(At(kCreateExtraRva) + kCreateExtraAdd) == At(kExtraAddRva)) {
                g_place_data_hook = safetyhook::create_mid(reinterpret_cast<void*>(at + kPlaceDataHook), &OnPlaceData);
            }
            if (g_place_data_hook) {
                spdlog::info("[LeftWeapon] (fix166) the left weapon's copy is made with the chosen item's own mods (PlaceAtMe's helper hooked at rva {:x}: "
                             "the game's own copy of the item's mods given to the new reference)",
                    kPlaceDataRva + kPlaceDataHook);
            } else {
                spdlog::warn("[LeftWeapon] (fix166) PlaceAtMe's helper is not as in Starfield 1.16.236 (or could not be hooked): the left weapon's copy keeps "
                             "the weapon's default parts");
            }
        }

        // A copy of this form in your hand, made as script PlaceAtMe does it: the grenade taken (slot 0), or the left weapon (slot 1;
        // (fix166) for the left weapon's choice `choice`).
        void Make(int slot, uintptr_t form, uint32_t choice)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  cell = 0, no_cell = 0;
            uint32_t   form_id = 0;
            Read(form + kFormId, form_id);
            std::string problem;
            Copy        c;
            c.weapon = slot == kLeftWeapon;
            c.form   = form;
            c.choice = choice;
            // (A weapon's grip at the palm from the first frame: its blade's way not known until its 3D is in; a small one meanwhile.)
            if (c.weapon) {
                c.centre_local = glm::vec3{ 0.0f, 0.0f, 0.3f };
                c.radius       = 0.3f;
            }
            Xf at;
            if (!Consume::ActivateHooked() || (slot == kGrenade && !PointInteract::PicksHooked())) {
                problem = "the game's activate or interaction pick is not hooked, so it could not be kept from being taken";
            } else if (g_orphan[slot].handle != 0) {
                problem = "the copy before is not gone yet";
            } else if (form == 0) {
                problem = slot == kGrenade ? "no grenade or mine was found in its slot" : "no weapon given";
            } else if (player == 0 || !Read(player + kRefCell, cell) || cell == 0 || !Read(g_no_cell_global, no_cell) || cell == no_cell) {
                problem = "you are not in a cell the game places things in";
            } else if (!Target(c, at)) {
                problem = "your hand's place was not found";
            }
            if (!problem.empty()) {
                g_failed_take[slot] = true;
                g_failed.fetch_add(1);
                const auto text = fmt::format("no copy of the {} made: {}", What(slot), problem);
                spdlog::info("{} {}", Tag(slot), text);
                SetLast(text);
                return;
            }
            const glm::vec3 angles = Angles(at.r, g_convention);
            const float     pos[3]{ at.t.x, at.t.y, at.t.z };
            const float     rot[3]{ angles.x, angles.y, angles.z };
            void* const     worldspace = reinterpret_cast<WorldspaceFn>(g_worldspace_fn)(reinterpret_cast<void*>(player));
            // (fix166) The left weapon: the game's own copy of its chosen item's mods, given to the placement as it makes the reference.
            uintptr_t   extra = 0;
            std::string chosen_mods;
            if (slot == kLeftWeapon && g_place_data_hook) {
                // (Not waiting on your inventory's lock on this thread: while it is being changed the copy is made a frame later, up to 30
                // frames, then waited for; the review.)
                static int busy_frames = 0;
                bool       busy        = false;
                extra                  = LeftWeapon::CloneModsExtra(form, chosen_mods, busy, busy_frames >= 30);
                if (busy) {
                    ++busy_frames;
                    return;
                }
                busy_frames = 0;
            }
            t_give_extra  = extra;
            t_give_form   = form;
            t_extra_given = false;
            const int64_t   start      = NowUs();
            // (One, not persistent, not disabled, deleted when able: script PlaceAtMe's defaults; no script engine, offsets or alias.)
            void* const made = reinterpret_cast<PlaceFn>(g_place_fn)(reinterpret_cast<void*>(player), pos, rot, reinterpret_cast<void*>(cell), worldspace,
                reinterpret_cast<void*>(form), 1, false, false, true, nullptr, 0, nullptr, nullptr);
            const int64_t took = NowUs() - start;
            const bool    given = t_extra_given;
            t_give_extra        = 0;
            t_extra_given       = false;
            if (extra != 0 && !given) {
                LeftWeapon::FreeModsExtra(extra); // (never handed to the game: ours to free)
            }
            const auto    ref  = reinterpret_cast<uintptr_t>(made);
            uint32_t      handle = 0, ref_id = 0;
            if (extra != 0 && g_mods_told.fetch_add(1) < 20) {
                std::string on_copy;
                const bool  has = given && ref != 0 && LeftWeapon::ReferenceHasExtra(ref, extra, on_copy);
                spdlog::info("{} (fix166) copy of {:08X}: the chosen item's mods [{}] {}; the copy's own mods: [{}]", Tag(slot), form_id, chosen_mods,
                    !given ? "not taken by the placement (freed again)"
                    : has  ? "given to the placement and on the copy"
                           : "given to the placement but not on the copy (left to the game)",
                    on_copy);
            } else if (slot == kLeftWeapon && extra == 0 && g_mods_told.fetch_add(1) < 20) {
                spdlog::info("{} (fix166) copy of {:08X}: made with the weapon's default parts ({})", Tag(slot), form_id,
                    g_place_data_hook ? "no mods of its own found in your inventory" : "the placement could not be given them");
            }
            if (ref == 0 || !Read(ref + kRefHandle, handle) || !Read(ref + kFormId, ref_id)) {
                g_failed_take[slot] = true;
                g_failed.fetch_add(1);
                const auto text = fmt::format("no copy of {} {:08X} made: the game's PlaceAtMe gave nothing", What(slot), form_id);
                spdlog::info("{} {}", Tag(slot), text);
                SetLast(text);
                return;
            }
            {
                Found found(handle);
                if (found.ref != ref) {
                    // It cannot be found (or deleted) by its handle: no more copies this session (it is deleted when able).
                    g_off   = fmt::format("a copy's handle ({:#x}) did not find it", handle);
                    g_ready = false;
                    g_orphan_base[slot].store(form_id);
                    g_orphan_id[slot].store(ref_id); // (its activation stays refused: it is never taken)
                    g_orphan_ref[slot].store(ref);
                    spdlog::error("{} copy {:08X} of {} {:08X} made, but its handle {:#x} finds {}: copies in your hands are off for this "
                                  "session (the copy is left to the game, marked delete-when-able; its activation stays refused)",
                        Tag(slot), ref_id, What(slot), form_id, handle, found.ref == 0 ? std::string{ "nothing" } : fmt::format("another reference ({:#x})", found.ref));
                    SetLast(g_off);
                    return;
                }
            }
            c.serial  = ++g_serial; // (a new grip for the fingers: this copy's own, so the other hand's new copy does not re-grip it)
            c.stage   = Stage::Loading;
            c.handle  = handle;
            c.ref_id  = ref_id;
            c.form_id = form_id;
            c.made_us = NowUs();
            g_copies[slot] = c;
            g_copy_base[slot].store(form_id);
            g_copy_id[slot].store(ref_id);
            g_copy_ref[slot].store(ref);
            if (slot == kGrenade) {
                g_picks_off.store(true);
            }
            const int n = slot == kGrenade ? g_made.fetch_add(1) + 1 : g_weapon_made.fetch_add(1) + 1;
            if (LogThis(n)) {
                uint32_t flags = 0;
                Read(ref + kFormFlags, flags);
                spdlog::info("{} copy {} of {} {:08X} made in your {} hand as script PlaceAtMe does: {:08X} (handle {:#x}, form flags {:#x}), at "
                             "({:.2f} {:.2f} {:.2f}), angles ({:.0f} {:.0f} {:.0f}) degrees, in {:.1f} ms; {}",
                    Tag(slot), n, What(slot), form_id, slot == kLeftWeapon ? "left" : "right", ref_id, handle, flags, at.t.x, at.t.y, at.t.z,
                    glm::degrees(angles.x), glm::degrees(angles.y), glm::degrees(angles.z), took * 1e-3,
                    slot == kGrenade ? "nothing can be picked or activated while it is there" : "it can never be activated (taken)");
            }
        }

        // (fix187) Its 3D's bodies keyframed, its parts too (the call recurses), with "force" as the game's own havok set-up calls it
        // (see kMarkRva). The game's answer (false: nothing done).
        bool SetKeyframed(uintptr_t root)
        {
            return reinterpret_cast<MotionFn>(g_motion_fn)(reinterpret_cast<void*>(root), kMotionKeyframed, true, true, 0);
        }

        // (fix187) The reference marked as script SetMotionType's job marks it ("its havok changed"), so the havok set-up at the end of
        // each move leaves its bodies' motion alone. Only through its own type table's change function when that is the reference's.
        bool Mark(uintptr_t ref, std::string& what)
        {
            if (!g_mark_ready) {
                what = "not marked (" + g_mark_why + ")";
                return false;
            }
            uintptr_t table = 0, change = 0;
            if (!Read(ref, table) || table == 0 || !Read(table + kMarkSlot * sizeof(uintptr_t), change) || change != At(kAddChangeRva)) {
                what = "not marked (its type table's change function is not the reference's)";
                return false;
            }
            const bool took = reinterpret_cast<AddChangeFn>(change)(reinterpret_cast<void*>(ref), kHavokChanged);
            what = took ? "marked as script SetMotionType marks it (its havok changed: the game's moves leave its motion alone)"
                        : "marked as script SetMotionType marks it, but the game's change record did not take it";
            return took;
        }

        // Waiting for its 3D: then out of your way (your collision group) and keyframed, as script SetMotionType does it.
        void Load(int slot)
        {
            auto&         c   = g_copies[slot];
            const int64_t now = NowUs();
            Found         found(c.handle, c.ref_id);
            if (found.ref == 0) {
                Gone(slot, "before its 3D came in");
                return;
            }
            ++c.load_frames;
            const bool      loaded = reinterpret_cast<LoadedFn>(g_loaded_fn)(reinterpret_cast<void*>(found.ref), kLoadedPhysics);
            const uintptr_t root   = loaded ? Root(found.ref) : 0;
            if (root == 0) {
                if (now - c.made_us > kLoadTimeoutUs) {
                    Fail(slot, fmt::format("its 3D did not come in within {:.0f} s", kLoadTimeoutUs * 1e-6));
                }
                return;
            }
            if (c.loaded_us == 0) {
                c.loaded_us = now;
            }
            if (!c.grouped) {
                const bool ask = c.group_asks == 0 || c.since_ask >= kGroupAskEvery;
                // (fix155) A weapon: every body of it, through its parts' collision (its root has only a proxy), on the layer that touches nothing.
                const int  r   = c.weapon ? HandGrab::PutApart(root, ask, c.group_what) : HandGrab::PutInYourGroup(root, ask, c.group_what);
                if (ask) {
                    ++c.group_asks;
                    c.since_ask = 0;
                } else {
                    ++c.since_ask;
                }
                if (r < 0) {
                    Fail(slot, fmt::format("it could not be kept from bumping into you: {}", c.group_what));
                    return;
                }
                if (r == 0) {
                    if (now - c.loaded_us > kGroupTimeoutUs) {
                        Fail(slot, fmt::format("it could not be kept from bumping into you within {:.0f} s: {}", kGroupTimeoutUs * 1e-6, c.group_what));
                    }
                    return;
                }
                c.grouped = true;
            }
            if (!c.keyframed) {
                SetKeyframed(root); // (its answer says nothing with force: true whenever there is a root; the review)
                std::string mark;
                c.marked        = Mark(found.ref, mark); // (fix187: after the motion, as the script's job does)
                c.key_each_move = !c.marked;             // (no mark: the moves would make it dynamic again; keyframed after each)
                c.keyframed     = true;
                c.root_seen  = root;
                ReadBound(root, c);
                if (LogThis(slot == kGrenade ? g_made.load() : g_weapon_made.load())) {
                    spdlog::info("{} copy {:08X}: its 3D came in after {} frames ({:.0f} ms); {}; made keyframed as script SetMotionType does ((fix187) "
                                 "with force), {}{}; its middle ({:.3f} {:.3f} {:.3f}) in its own space, radius {:.3f} m{}",
                        Tag(slot), c.ref_id, c.load_frames, (c.loaded_us - c.made_us) * 1e-3, c.group_what, mark,
                        c.key_each_move ? "; keyframed again after every move" : "", c.centre_local.x, c.centre_local.y, c.centre_local.z, c.radius,
                        c.bound_read ? "" : " (not read yet: its own 3D's bound looked wrong)");
                }
            }
            c.stage = Stage::Holding;
        }

        // In your hand: moved there every frame, as script SetPosition does it.
        void Hold(int slot)
        {
            auto& c = g_copies[slot];
            Found found(c.handle, c.ref_id);
            if (found.ref == 0) {
                Gone(slot, "while in your hand");
                return;
            }
            const uintptr_t root = Root(found.ref);
            if (root == 0) {
                Fail(slot, "its 3D went away");
                return;
            }
            // (fix155) A weapon held for minutes: its bodies looked at twice a second, and put apart again if the game changed them back
            // (the keyframing, a part's 3D made again).
            // (A body changed, or its 3D made anew (another root): made keyframed again too, else a new part's body would fall, on a layer that
            // touches nothing, and its proxy would draw the weapon after it; the review.)
            if (c.weapon && (c.frames % 45 == 0 || root != c.root_seen)) {
                std::string what;
                if (root != c.root_seen || HandGrab::PutApart(root, false, what) != 1) {
                    const int r = HandGrab::PutApart(root, true, what);
                    SetKeyframed(root); // ((fix187) with force)
                    static int told = 0;
                    if (told++ < 10) {
                        spdlog::info("{} copy {:08X}: {}put apart and keyframed again ({}): {}", Tag(slot), c.ref_id,
                            root != c.root_seen && c.root_seen != 0 ? "its 3D made anew: " : "", r == 1 ? "done" : "not all yet", what);
                    }
                }
                c.root_seen = root;
            }
            // (fix156) The left weapon thrown (MeleeThrow): where it flies, not in your hand (the hand open meanwhile).
            Xf         to;
            bool       flying = c.weapon && MeleeThrow::FlightPose(true, to.r, to.t, to.s);
            if (flying && !(Finite(to.t) && Finite(to.r[0]) && Finite(to.r[1]) && Finite(to.r[2]) && std::isfinite(to.s))) {
                return; // (a pose that is not a place: this frame not moved; the review)
            }
            if (!flying && !Target(c, to)) {
                if (++c.no_hand >= kNoHandFrames) {
                    Fail(slot, "your hand's place was not found");
                }
                return;
            }
            c.no_hand = 0;
            uintptr_t cell = 0, worldspace = 0;
            uint8_t   cell_flags = 0;
            if (!Read(found.ref + kRefCell, cell) || cell == 0) {
                Fail(slot, "it is in no cell");
                return;
            }
            if (Read(cell + kCellFlags, cell_flags) && (cell_flags & 1) == 0) {
                Read(cell + kCellWorldspace, worldspace);
            }
            const int       convention = g_convention;
            const glm::vec3 angles     = Angles(to.r, convention);
            const float     pos[3]{ to.t.x, to.t.y, to.t.z };
            const float     rot[3]{ angles.x, angles.y, angles.z };
            uint32_t        no_target = NullHandle();
            reinterpret_cast<MoveFn>(g_move_fn)(reinterpret_cast<void*>(found.ref), &no_target, reinterpret_cast<void*>(cell), reinterpret_cast<void*>(worldspace),
                pos, rot);
            ++c.moves;
            // (fix187) Still keyframed after the move? The move's own havok set-up makes it dynamic again unless the reference is marked
            // (see kMarkRva); unmarked, or found dynamic twice running (a motion change can wait for the physics step: the first read
            // after a keyframing may not show it yet; the review), it is keyframed again right after every move, before the physics step.
            // (Its root read again after the move: another one is left to the next frame's check above; the review.)
            if (Root(found.ref) == root) {
                if (c.key_each_move) {
                    SetKeyframed(root);
                    ++c.rekeys;
                }
                if (c.moves <= kMotionWatchMoves || c.frames % kMotionCheckEvery == 0 || c.falling_run > 0) {
                    int         moving = 0, keyed = 0;
                    std::string what;
                    if (HandGrab::PartsMotion(root, moving, keyed, what) == 1) {
                        ++c.motion_reads;
                        if (c.first_motion.empty()) {
                            c.first_motion = fmt::format("after move {}: {}", c.moves, what);
                        }
                        if (moving > 0) {
                            ++c.found_falling;
                            ++c.falling_run;
                            if (!c.key_each_move) {
                                SetKeyframed(root);
                                ++c.rekeys;
                            }
                            static int falling_lines = 0;
                            if (!c.told_falling && falling_lines++ < 10) {
                                c.told_falling = true;
                                spdlog::info("{} (fix187) copy {:08X} after move {}: moved by physics ({}{}{}): keyframed again", Tag(slot), c.ref_id,
                                    c.moves, what, c.marked ? "; marked" : "; not marked", c.key_each_move ? "; keyframed after every move already" : "");
                            }
                            if (c.falling_run >= 2 && !c.key_each_move) {
                                c.key_each_move = true;
                                spdlog::info("{} (fix187) copy {:08X} after move {}: moved by physics twice running: keyframed right after every move "
                                             "from now",
                                    Tag(slot), c.ref_id, c.moves);
                            }
                        } else {
                            c.falling_run = 0;
                        }
                        if (!c.motion_told && LogThis(slot == kGrenade ? g_made.load() : g_weapon_made.load())) {
                            c.motion_told = true;
                            spdlog::info("{} (fix187) copy {:08X} after move {}: {}{}", Tag(slot), c.ref_id, c.moves, what,
                                moving > 0 ? " (keyframed again)" : c.marked ? " (marked: the game's move left it keyframed)" : "");
                        }
                    }
                }
            }
            // (fix169) The body's left hand put on it from the same place this frame (its fist on the hilt, swinging or running).
            if (slot == kLeftWeapon && !flying) {
                if (glm::vec3 root_used{}; tracking::FrameStartRoot(root_used)) {
                    root_used.z += body::ViewHeightOffset(); // (the view's lowering as the copy's hand frame had it: StageToWorld; the review)
                    body::NoteLeftWeaponRoot(root_used);
                }
            }

            // Where it is now against where it was put.
            Xf drawn;
            if (NodeWorld(root, drawn)) {
                const float off  = glm::length(drawn.t - to.t);
                const float turn = DegreesBetween(drawn.r, to.r);
                if (std::isfinite(off) && std::isfinite(turn)) {
                    ++c.checked;
                    c.off_sum += off;
                    c.off_max = std::max(c.off_max, off);
                    c.turn_sum += turn;
                    c.turn_max = std::max(c.turn_max, turn);
                    // (Only a move that placed it exactly says something about the angles.)
                    if (!g_convention_settled && off < 0.005f) {
                        g_convention_error[convention] += turn;
                        if (++g_convention_tests[convention] >= 10) {
                            const double mean = g_convention_error[convention] / g_convention_tests[convention];
                            if (mean < 10.0) {
                                g_convention_settled = true;
                                spdlog::info("{} the copy turns with your hand as asked (angle convention {}: {:.1f} degrees off on average)", Tag(slot),
                                    convention, mean);
                            } else if (g_convention_tests[1 - convention] == 0) {
                                g_convention = 1 - convention;
                                spdlog::info("{} the copy was {:.0f} degrees off how it was asked to turn (angle convention {}); trying convention {}",
                                    Tag(slot), mean, convention, g_convention);
                            } else {
                                const double other = g_convention_error[1 - convention] / g_convention_tests[1 - convention];
                                g_convention         = other < mean ? 1 - convention : convention;
                                g_convention_settled = true;
                                spdlog::info("{} neither angle convention turned the copy as asked ({:.0f} and {:.0f} degrees off); keeping {}", Tag(slot),
                                    mean, other, g_convention);
                            }
                        }
                    }
                }
                if (c.logged_moves < 3 && LogThis(slot == kGrenade ? g_made.load() : g_weapon_made.load())) {
                    ++c.logged_moves;
                    spdlog::info("{} copy {:08X} move {}: put at ({:.3f} {:.3f} {:.3f}), drawn at ({:.3f} {:.3f} {:.3f}) ({:.1f} mm off, {:.1f} degrees "
                                 "turned off; {:.1f} mm and {:.1f} degrees from the move before's place)",
                        Tag(slot), c.ref_id, c.moves, to.t.x, to.t.y, to.t.z, drawn.t.x, drawn.t.y, drawn.t.z, off * 1000.0f, turn,
                        c.have_last_to ? glm::length(drawn.t - c.last_to.t) * 1000.0f : 0.0f, c.have_last_to ? DegreesBetween(drawn.r, c.last_to.r) : 0.0f);
                }
                if (c.moves == kBoundAfter && off < 0.01f) {
                    ReadBound(root, c); // (its bound, now where it is drawn)
                }
            }
            c.last_to      = to;
            c.have_last_to = true;

            // For the fingers: its box where it is drawn (the place it was put, which is where the body draws your hand). A weapon: its
            // grip, a hand's width of it. (fix156) A weapon flying: none (the hand open), no blade, and nothing told to the throw.
            if (flying) {
                {
                    std::scoped_lock _{ g_shape_mutex };
                    g_have_shape[slot] = false;
                }
                {
                    std::scoped_lock b{ g_blade_mutex };
                    g_blade_valid       = false;
                    g_blade_local_valid = false;
                }
                g_in_hand[slot].store(false);
                g_flying[slot].store(true);
                return;
            }
            g_flying[slot].store(false);
            {
                const float     half = c.weapon ? 0.02f : std::max(c.radius * kBoxShare / std::max(c.scale, 0.01f), 0.005f);
                const glm::vec3 at   = c.weapon ? glm::vec3{ 0.0f } : c.centre_local;
                std::scoped_lock _{ g_shape_mutex };
                g_shape[slot]          = HandGrab::HeldShape{};
                g_shape[slot].r        = to.r;
                g_shape[slot].t        = to.t;
                g_shape[slot].s        = to.s;
                g_shape[slot].min      = at - glm::vec3{ half };
                g_shape[slot].max      = at + glm::vec3{ half };
                g_shape[slot].grab     = c.serial;
                g_shape[slot].cylinder = false; // (a weapon too: its grip's box; the review)
                g_have_shape[slot]     = true;
            }
            // (fix154) The weapon's blade where it is put (its grip, its way, its length to the bound's far side), for its hits and throw.
            if (c.weapon) {
                const float     blade_len = glm::length(c.centre_local);
                const glm::vec3 way       = blade_len > 1e-4f ? glm::normalize(to.r * c.centre_local) : glm::vec3{ 0.0f, 0.0f, 1.0f };
                std::scoped_lock b{ g_blade_mutex };
                g_blade_grip   = to.t;
                g_blade_way    = way;
                g_blade_length = (blade_len + c.radius) * c.scale;
                g_blade_valid  = Finite(way) && Finite(to.t);
                // (Its way in the left controller's aim frame (the game world's aim axes as the hand grab finds them: the room's turn of the
                // tracked axes): a strike this frame or the next along the blade as it is drawn.)
                g_blade_local_valid = false;
                if (const auto room = tracking::RoomRotation(); room && g_blade_valid) {
                    const glm::mat4 aim = tracking::AimPose(true);
                    glm::mat3       hand{ 1.0f };
                    for (int k = 0; k < 3; ++k) {
                        hand[k] = *room * tracking::ToHavokVector(glm::normalize(glm::vec3{ aim[k] }));
                    }
                    const glm::vec3 local = glm::transpose(hand) * way;
                    if (Finite(local) && glm::length(local) > 0.5f) {
                        g_blade_local       = glm::normalize(local);
                        g_blade_local_valid = true;
                    }
                }
            }
            // (fix156) In your left hand: the throw told where it is (its grip, and its middle and size as it is drawn).
            if (c.weapon) {
                MeleeThrow::OnWeaponInHand(true, to.r, to.t, to.s, to.r, to.t, to.s, to.t + to.r * (c.centre_local * to.s), c.radius);
            }
            g_in_hand[slot].store(true);
        }

        // One slot's frame: made when wanted, removed when not (or when another form is wanted, or (fix166) the left weapon chosen anew:
        // `choice`), loaded, then held.
        void UpdateSlot(int slot, bool want, uintptr_t form, uint32_t choice, const char* why_not)
        {
            auto& c = g_copies[slot];
            // (fix154) The left weapon is held for long: after a failure it is tried again after a wait (1 s, doubling to 30 s; its copy
            // before gone first), and at once for another weapon (the review: one failure must not hide it until the next sheathing).
            static int64_t   failed_at[kCopies]{};
            static int       retries[kCopies]{};
            static uintptr_t wanted[kCopies]{};
            static uint32_t  wanted_choice[kCopies]{};
            if (!want || (slot == kLeftWeapon && (form != wanted[slot] || choice != wanted_choice[slot]))) { // (the grenade as before: its latch until it is let go)
                g_failed_take[slot]  = false;
                failed_at[slot]      = 0;
                retries[slot]        = 0;
                wanted[slot]         = want ? form : 0;
                wanted_choice[slot]  = want ? choice : 0;
            }
            if (slot == kLeftWeapon && g_failed_take[slot] && c.stage == Stage::None) {
                const int64_t now = NowUs();
                if (failed_at[slot] == 0) {
                    failed_at[slot] = now;
                }
                const int64_t wait = std::min<int64_t>(1'000'000ll << std::min(retries[slot], 5), 30'000'000);
                if (g_orphan[slot].handle == 0 && now - failed_at[slot] > wait) {
                    g_failed_take[slot] = false;
                    failed_at[slot]     = 0;
                    ++retries[slot];
                }
            }
            if (c.stage != Stage::None && (!want || (slot == kLeftWeapon && (form != c.form || choice != c.choice)))) {
                Remove(slot, !want ? std::string{ why_not } : std::string{ form != c.form ? "another one asked for" : "chosen anew (other mods)" });
                if (!want) {
                    return;
                }
            }
            if (c.stage == Stage::None) {
                if (!want || g_failed_take[slot]) {
                    return;
                }
                Make(slot, form, choice);
                if (c.stage == Stage::None) {
                    return;
                }
            }
            ++c.frames;
            if (c.stage == Stage::Loading) {
                Load(slot);
            }
            if (c.stage == Stage::Holding) {
                Hold(slot);
            }
        }
    }

    bool WithRoot(uint32_t handle, const std::function<void(uintptr_t root)>& use)
    {
        // (As the copies are found: the handle table's lookup, a hold on the reference while it is used.)
        if (g_lookup_fn == 0 || g_ref_table == 0 || g_release_fn == 0 || handle == 0) {
            return false;
        }
        Found           found(handle);
        const uintptr_t root = Root(found.ref);
        if (root == 0) {
            return false;
        }
        use(root);
        return true;
    }

    bool WithReference(uint32_t handle, const std::function<void(uintptr_t ref, uintptr_t root)>& use)
    {
        if (g_lookup_fn == 0 || g_ref_table == 0 || g_release_fn == 0 || handle == 0) {
            return false;
        }
        Found found(handle);
        if (found.ref == 0) {
            return false;
        }
        use(found.ref, Root(found.ref));
        return true;
    }

    void Install()
    {
        if (!FindText()) {
            g_off = "the game's image could not be read";
        } else if (FindCode()) {
            g_off.clear();
            g_ready = true;
        }
        if (g_ready) {
            InstallSaveWatch();
            InstallPlaceData(); // (fix166)
        }
        if (g_ready) {
            spdlog::info("[Nade] copies in your hands are ready (grenades, and the left hand's weapon): made as script PlaceAtMe does (rva {:x}), "
                         "moved as script SetPosition does (rva {:x}), keyframed as script SetMotionType does (rva {:x}), deleted as script Delete "
                         "does (rva {:x}); the game's code checked; (fix187) kept keyframed: the mark {}, their bodies' motion read after "
                         "each of their first {} moves and then every {} frames",
                g_place_fn - g_base, g_move_fn - g_base, g_motion_fn - g_base, g_delete_fn - g_base, g_mark_why, kMotionWatchMoves, kMotionCheckEvery);
        } else {
            spdlog::warn("[Nade] copies in your hands stay off: {}", g_off);
        }
    }

    namespace
    {
        // (Under g_stray_mutex.) One more copy that may be in a save (once each).
        void AddStray(uint32_t ref_id, uint32_t base_id)
        {
            if (ref_id == 0 || base_id == 0) {
                return;
            }
            int empty = -1;
            for (int i = 0; i < kStrays; ++i) {
                if (g_strays[i].ref_id == ref_id && g_strays[i].base_id == base_id) {
                    return;
                }
                if (empty < 0 && g_strays[i].ref_id == 0) {
                    empty = i;
                }
            }
            // (An empty place first; else the oldest.)
            if (empty >= 0) {
                g_stray_next = empty;
            }
            g_strays[g_stray_next] = Stray{ ref_id, base_id };
            g_stray_bases[g_stray_next].store(base_id);
            g_stray_ids[g_stray_next].store(ref_id);
            g_stray_next = (g_stray_next + 1) % kStrays;
            g_strays_changed.store(true);
        }

        // After each load (its loading screen gone, play back): the copies noted at saves looked for for 20 s, and deleted when one
        // is found (its base as noted, a weapon; not one of this session's copies). Main thread.
        void CleanStrays()
        {
            static int64_t seen_loading = 0;
            static int64_t until_us     = 0;
            static int64_t looked_us    = 0;
            static bool    armed        = false;
            const int64_t  now          = NowUs();
            if (const int64_t loading = GameFlow::lastLoadingScreen(); loading != seen_loading) {
                seen_loading = loading;
                armed        = true;
                until_us     = 0;
            }
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  cell   = 0;
            const bool in_cell = player != 0 && Read(player + kRefCell, cell) && cell != 0;
            if (armed && until_us == 0 && !GameFlow::isShowingMenu() && in_cell) {
                until_us = now + 20'000'000;
            }
            if (!armed || until_us == 0 || !in_cell || now - looked_us < 500'000) {
                return;
            }
            looked_us = now;
            if (now > until_us) {
                armed = false;
                return;
            }
            Stray strays[kStrays]{};
            {
                std::scoped_lock _{ g_stray_mutex };
                std::copy(std::begin(g_strays), std::end(g_strays), std::begin(strays));
            }
            for (int i = 0; i < kStrays; ++i) {
                const auto& s = strays[i];
                if (s.ref_id == 0) {
                    continue;
                }
                bool mine = false;
                for (int slot = 0; slot < kCopies; ++slot) {
                    mine = mine || g_copy_id[slot].load() == s.ref_id || g_orphan_id[slot].load() == s.ref_id;
                }
                const uintptr_t ref = mine ? 0 : LeftWeapon::LookupForm(s.ref_id);
                uintptr_t       base = 0;
                uint32_t        id = 0, base_id = 0, handle = 0, flags = 0;
                uint8_t         base_type = 0;
                if (ref == 0 || !Read(ref + kFormId, id) || id != s.ref_id || !Read(ref + kRefBaseObject, base) || base == 0 ||
                    !Read(base + kFormId, base_id) || base_id != s.base_id || !Read(base + kFormTypeAt, base_type) || base_type != kWeaponType ||
                    !Read(ref + kRefHandle, handle) || handle == 0 || !Read(ref + kFormFlags, flags) || (flags & (kDeleted | kPersistent)) != 0) {
                    continue;
                }
                // (Where the copy was as the save was made: in your hand, and the load puts you back there: in your cell, within 4 m.
                // Anything else with its id and base (another thing made later, after an older save) is left alone; the review.)
                uintptr_t ref_cell = 0;
                float     where[3]{}, you[3]{};
                if (!Read(ref + kRefCell, ref_cell) || ref_cell != cell || !Read(ref + kRefLocation, where) || !Read(player + kRefLocation, you)) {
                    continue;
                }
                const float dx = where[0] - you[0], dy = where[1] - you[1], dz = where[2] - you[2];
                if (!(dx * dx + dy * dy + dz * dz < kStrayNearM * kStrayNearM)) {
                    continue;
                }
                // (Its handle finds it: the delete is of this reference.)
                {
                    Found found(handle);
                    if (found.ref != ref) {
                        continue;
                    }
                }
                reinterpret_cast<DeleteFn>(g_delete_fn)(handle, false);
                uint32_t after = 0;
                Read(ref + kFormFlags, after);
                spdlog::info("[GrenadeHand] (fix154) a copy from a save found after the load: {:08X} (a copy of {:08X}), deleted as script Delete "
                             "does (form flags {:#x} -> {:#x})",
                    s.ref_id, s.base_id, flags, after);
                std::scoped_lock _{ g_stray_mutex };
                if (g_strays[i].ref_id == s.ref_id) {
                    g_strays[i] = Stray{};
                    g_stray_ids[i].store(0);
                    g_strays_changed.store(true);
                }
            }
        }
    }

    void NoteSaving(const char* what)
    {
        const int64_t now   = NowUs();
        int           noted = 0;
        std::scoped_lock _{ g_stray_mutex };
        for (int slot = 0; slot < kCopies; ++slot) {
            const uint32_t ids[3][2]{ { g_copy_id[slot].load(), g_copy_base[slot].load() }, { g_orphan_id[slot].load(), g_orphan_base[slot].load() },
                { now - g_gone_us[slot].load() < kGoneRecentUs ? g_gone_id[slot].load() : 0u, g_gone_base[slot].load() } };
            for (const auto& p : ids) {
                if (p[0] != 0 && p[1] != 0) {
                    AddStray(p[0], p[1]);
                    ++noted;
                }
            }
        }
        if (noted > 0) {
            spdlog::info("[GrenadeHand] (fix154) the game saves ({}): {} copy(ies) in your hands noted, to be deleted if found after a load", what, noted);
        }
    }

    void LoadStrays(const std::string& text)
    {
        std::scoped_lock _{ g_stray_mutex };
        for (int i = 0; i < kStrays; ++i) {
            g_strays[i] = Stray{};
            g_stray_ids[i].store(0);
        }
        g_stray_next = 0;
        size_t at    = 0;
        while (at < text.size() && g_stray_next < kStrays) {
            const size_t end  = std::min(text.find(',', at), text.size());
            const auto   item = text.substr(at, end - at);
            at                = end + 1;
            unsigned ref_id = 0, base_id = 0;
            if (sscanf_s(item.c_str(), "%x:%x", &ref_id, &base_id) == 2 && ref_id != 0 && base_id != 0) {
                g_strays[g_stray_next] = Stray{ ref_id, base_id };
                g_stray_bases[g_stray_next].store(base_id);
                g_stray_ids[g_stray_next].store(ref_id);
                ++g_stray_next;
            }
        }
        g_stray_next %= kStrays;
        g_strays_changed.store(false);
    }

    bool TakeStraysChanged(std::string& text)
    {
        if (!g_strays_changed.exchange(false)) {
            return false;
        }
        std::scoped_lock _{ g_stray_mutex };
        text.clear();
        // (Oldest first, so the next one noted replaces the oldest after a restart too.)
        for (int k = 0; k < kStrays; ++k) {
            const auto& s = g_strays[(g_stray_next + k) % kStrays];
            if (s.ref_id != 0) {
                text += fmt::format("{}{:08X}:{:08X}", text.empty() ? "" : ",", s.ref_id, s.base_id);
            }
        }
        return true;
    }

    namespace
    {
        // A save asked for now (the manager's tasks: see kSaveBits). Main thread.
        bool SaveAsked(uint32_t& tasks)
        {
            uintptr_t manager = 0;
            tasks             = 0;
            return g_save_manager_at != 0 && Read(g_save_manager_at, manager) && manager != 0 && Read(manager + kSaveTasksAt, tasks) &&
                   (tasks & kSaveBits) != 0;
        }

        // The copies held back while a save is asked for (and a moment after; at most 5 s for one asking). Main thread.
        bool HoldForSave()
        {
            static int64_t  asked_since = 0, ended_at = 0;
            static uint32_t asked_bits  = 0;
            static int      told        = 0;
            const int64_t   now         = NowUs();
            uint32_t        tasks       = 0;
            if (SaveAsked(tasks)) {
                // (Its 5 s counted afresh as the asking moves on: an autosave asked for can wait long (a menu, the game busy) and then
                // becomes 0x100, made 3 frames later: held again then; the review.)
                if ((tasks & kSaveBits) != asked_bits) {
                    asked_bits  = tasks & kSaveBits;
                    asked_since = 0;
                }
                if (asked_since == 0) {
                    asked_since = now;
                    if (told++ < 20) {
                        spdlog::info("[Nade] (fix154) the game is about to save (its tasks {:#x}): the copies in your hands are taken away until it is made", tasks);
                    }
                }
                ended_at = now;
                return now - asked_since < kSaveHoldMaxUs;
            }
            asked_since = 0;
            return ended_at != 0 && now - ended_at < kSaveAfterUs;
        }

        // The start of every save (SaveGame, 0x14183ED20): the copies there now (or gone a moment ago) noted.
        void OnSaveGame(safetyhook::Context& ctx)
        {
            const int n = g_saves_seen.fetch_add(1) + 1;
            if (n <= 20) {
                spdlog::info("[Nade] (fix154) the game saves (save {} this session; thread {}; name {})", n, GetCurrentThreadId(), ctx.rdx != 0 ? "given" : "none");
            }
            NoteSaving("its save starts");
        }

        void InstallSaveWatch()
        {
            const uintptr_t at = At(kSaveTasksRva);
            if (SameCode(kSaveTasksRva, kSaveTasks, sizeof(kSaveTasks)) && SameCode(kSaveTasksRva + 7, kSaveTasksCmp, sizeof(kSaveTasksCmp)) &&
                RipTarget(at, 7) == At(kSaveManagerRva)) {
                g_save_manager_at = At(kSaveManagerRva);
            }
            if (SameCode(kSaveGameRva, kSaveGameStart, sizeof(kSaveGameStart)) && CallTarget(At(kSaveGameRva) + kSaveBeginCall) == At(kSaveBeginRva)) {
                g_save_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kSaveGameRva)), &OnSaveGame);
            }
            if (g_save_manager_at != 0 && g_save_hook) {
                spdlog::info("[Nade] (fix154) saves watched: the copies in your hands are taken away while one is asked for (the save manager's tasks), "
                             "and noted at every save's start (rva {:x}), to be deleted if a load brings one back",
                    kSaveGameRva);
            } else {
                spdlog::warn("[Nade] (fix154) saves {}{}: a copy in your hand could be kept in a save (deleted after a load only if it was noted)",
                    g_save_manager_at == 0 ? "not watched (the save manager's tasks not found)" : "watched",
                    g_save_hook ? "" : "; the save's start not hooked (the code is not as in 1.16.236)");
            }
        }
    }

    void Update()
    {
        if (!g_ready.load()) {
            return;
        }
        CleanStrays();
        for (int slot = 0; slot < kCopies; ++slot) {
            UpdateOrphan(slot);
        }
        const auto& s = GameFlow::gStore.internalSettings;
        // (fix154) A save asked for: no copies meanwhile (see kSaveBits).
        const bool saving = HoldForSave();
        UpdateSlot(kGrenade, !saving && s.grenadeInHand && s.shoulderGrenade && HandActions::GrenadeInHand(), HandGrab::ThrowableForm(), 0,
            saving                                   ? "the game is about to save"
            : s.grenadeInHand && s.shoulderGrenade ? "the grenade left your hand: thrown, put back, or a menu came"
                                                     : "the setting was turned off");
        // (fix154) The left weapon, as LeftWeapon asks (a menu, a loading screen, the weapon put away, ...: not asked).
        const uintptr_t left = g_left_want.load(std::memory_order_acquire);
        UpdateSlot(kLeftWeapon, !saving && left != 0, left, LeftWeapon::ChoiceSerial(),
            saving ? "the game is about to save" : "the left weapon put away (a menu, the weapon sheathed, or taken out of the left hand)");
    }

    void WantLeftWeapon(uintptr_t form) { g_left_want.store(form, std::memory_order_release); }

    bool LeftWeaponShown() { return g_in_hand[kLeftWeapon].load(std::memory_order_relaxed); }

    bool LeftWeaponOut() { return g_in_hand[kLeftWeapon].load(std::memory_order_relaxed) || g_flying[kLeftWeapon].load(std::memory_order_relaxed); }

    uint32_t LeftWeaponHandle() { return g_copies[kLeftWeapon].stage == Stage::Holding ? g_copies[kLeftWeapon].handle : 0; }

    bool LeftBlade(glm::vec3& grip, glm::vec3& way, float& length)
    {
        std::scoped_lock _{ g_blade_mutex };
        if (!g_blade_valid) {
            return false;
        }
        grip   = g_blade_grip;
        way    = g_blade_way;
        length = g_blade_length;
        return true;
    }

    bool SavePending()
    {
        uint32_t tasks = 0;
        return SaveAsked(tasks);
    }

    bool LeftBladeLocal(glm::vec3& way)
    {
        std::scoped_lock _{ g_blade_mutex };
        if (!g_blade_local_valid) {
            return false;
        }
        way = g_blade_local;
        return true;
    }

    uintptr_t RefuseActivation(uintptr_t object)
    {
        if (object == 0) {
            return 0;
        }
        // (fix154) A copy from a save (noted as the game saved, found after a load): never activated, until it is deleted.
        // (Its base too: another thing made later with the same id, after an older save was loaded, is not refused.)
        if (uint32_t id = 0; Read(object + kFormId, id) && id != 0) {
            for (int i = 0; i < kStrays; ++i) {
                if (g_stray_ids[i].load(std::memory_order_relaxed) != id) {
                    continue;
                }
                uintptr_t base    = 0;
                uint32_t  base_id = 0;
                if (Read(object + kRefBaseObject, base) && base != 0 && Read(base + kFormId, base_id) && base_id == g_stray_bases[i].load()) {
                    return g_ret;
                }
            }
        }
        for (int slot = 0; slot < kCopies; ++slot) {
            uint32_t want = 0;
            if (object == g_copy_ref[slot].load(std::memory_order_acquire)) {
                want = g_copy_id[slot].load();
            } else if (object == g_orphan_ref[slot].load(std::memory_order_acquire)) {
                want = g_orphan_id[slot].load();
            } else {
                continue;
            }
            uint32_t id = 0;
            return want != 0 && Read(object + kFormId, id) && id == want ? g_ret : 0;
        }
        return 0;
    }

    bool PicksOff() { return g_picks_off.load(std::memory_order_relaxed); }

    bool InHand(bool left) { return g_in_hand[left ? kLeftWeapon : kGrenade].load(std::memory_order_relaxed); }

    bool HeldShape(bool left, HandGrab::HeldShape& out)
    {
        const int slot = left ? kLeftWeapon : kGrenade;
        if (!g_in_hand[slot].load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock _{ g_shape_mutex };
        if (!g_have_shape[slot]) {
            return false;
        }
        out = g_shape[slot];
        return true;
    }

    std::string Status()
    {
        if (!g_ready.load()) {
            return fmt::format("Copies in your hands: off ({}).", g_off);
        }
        std::string last;
        {
            std::scoped_lock _{ g_last_mutex };
            last = g_last;
        }
        return fmt::format("Copies in your hands: {} grenades and {} left weapons made, {} removed{}{}.{}", g_made.load(), g_weapon_made.load(),
            g_removed.load(), g_failed.load() > 0 ? fmt::format(", {} not made", g_failed.load()) : std::string{},
            g_in_hand[kGrenade].load() ? " (a grenade in your hand now)" : (g_in_hand[kLeftWeapon].load() ? " (the left weapon in your hand now)" : ""),
            last.empty() ? std::string{} : fmt::format(" Last: {}.", last));
    }
}
