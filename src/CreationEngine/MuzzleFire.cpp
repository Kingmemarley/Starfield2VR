#include "MuzzleFire.h"

#include <safetyhook/easy.hpp>

#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define MUZZLEFIRE_RETURN_ADDRESS() _ReturnAddress()
#else
#define MUZZLEFIRE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include "HandActions.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "body/BodyIK.h"
#include "body/SafeScan.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "vr/TrackingSpace.h"
#include <mods/VR.hpp>

// Ported from SFVR (AetherianArtificer's fork of starfield2vr, src/CreationEngine/MuzzleFire.cpp).
namespace MuzzleFire
{
    namespace
    {
        // ProjectileLaunchData offsets (SFVR, Starfield 1.16.244). The shooter is compared with the
        // player before anything is written, so a different layout only means nothing happens.
        constexpr std::size_t kShooterOffset       = 0x38;
        constexpr std::size_t kAimFromMuzzleOffset = 0xF2;
        constexpr std::size_t kMuzzleNodeOffset    = 0x88;  // NiPointer<NiAVObject>, checked before any lookup
        constexpr std::size_t kAnglesOffset        = 0x24;  // pitch, roll, yaw

        // (fix195) More of the launch data, the projectile's form and the game's beam, as Starfield 1.16.236's code uses them (the same in
        // 1.16.244: its launch function and beam update read them at the same offsets):
        // - the launch's start (+0x00): a place (x, y, z) and at +0x0C the index of the frame of reference it is given in (as the
        //   game's aim point below);
        // - the projectile's form (+0x30); in it, its flags (+0x178; 0x1000 "Continuous Update": the game aims the projectile again
        //   every frame while it lives) and its kind (+0x1B4; 2 a beam);
        // - +0xE4: set, the game tests what the beam hits along the camera's line (from the camera toward the game's aim point)
        //   instead of along the beam itself. With +0xF2 set (as here) the game sets it when its aim point is within 0.85 m of the
        //   weapon's own muzzle (fContinuousBeamOverrideDistance); in plain first person always (a game setting, on by default);
        //   (fix204: read only on the beam's test without its model loaded (0x1b0c3d7); with its model the test starts at the beam's own
        //   node's place, kBeamMuzzleNodeOffset below; the test without the model (0x1b45c30) starts fContinuousBeamSafeZoneDistance,
        //   0.35 m by default, behind it (+0x244, set while +0xF2 is set and +0xE4 not), so its hit distance then includes those 0.35 m);
        // - a beam keeps its own copy of the launch data at +0x290 and the distance to what it hit at +0x224;
        // - the player's aim point, where the game's aim ray from the camera hit (+0x838: x, y, z, and the index at +0x844).
        constexpr std::size_t   kLaunchStartOffset       = 0x00;
        constexpr std::size_t   kProjectileFormOffset    = 0x30;
        constexpr std::size_t   kUseCameraLineOffset     = 0xE4;
        constexpr std::size_t   kFormFlagsOffset         = 0x178;
        constexpr std::size_t   kFormKindOffset          = 0x1B4;
        constexpr std::uint32_t kContinuousUpdateFlag    = 0x1000;
        constexpr std::uint8_t  kBeamKind                = 2;
        constexpr std::size_t   kBeamLaunchDataOffset    = 0x290;
        constexpr std::size_t   kBeamHitDistanceOffset   = 0x224;
        constexpr std::size_t   kPlayerAimPointOffset    = 0x838;
        // (fix195) The muzzle this close to the game's aim point (metres): pushed into the surface the aim point is on.
        constexpr float         kMuzzleAtSurface         = 0.3f;
        // (fix204) The beam's own muzzle node (BeamProjectile +0x1A8, a pointer the beam holds a reference to). Each frame the beam's
        // update (1.16.236 0x1b0a0c3, through 0x1b51a40 and 0x1b517d0) puts the drawn beam, and the start of the test of what it hits
        // (what it cuts and mines), at this node's place. The game's launch (0x1b3e6e0, at 0x1b3fb3f; 1.16.244 0x1b3d1af) copies the
        // launch data's node (+0x88) into it with a reference, so with the muzzle this hook puts there (above, since fix195) it already
        // holds the gun in your hand's muzzle: the write below is only for a beam found without one (not expected), and the log says
        // which. Only empty with no node in the launch data, when the game's own weapon node lookup is used. The beam's clean-up as it is
        // destroyed (0x1b3cdc0, at 0x1b3d15a) drops the reference; BeamProjectile's own setter (0x1b0b7c0) takes one the same way. The
        // same offset and code in 1.16.244 (0x1b08e80, 0x1b3a7d1).
        constexpr std::size_t   kBeamMuzzleNodeOffset    = 0x1A8;
        // (fix204) The game's camera FOVs (PlayerCamera): the world's (+0x280) and the first-person one (+0x298, plus +0x29C and +0x2A4
        // while the byte at +0x2B0 is set). Where they differ, the beam's update (at 0x1b0a10a) runs a first-person beam's start through
        // 0x1e96420: projected with a reference camera ('1st Person Camera - World Reference', made by 0x1e87d50 from the world camera's
        // place and the first-person FOV) and put back along the world camera's ray through the same screen point, at the same distance
        // from it. The mod gives that camera the eye's lens (it takes it for the gun and arms camera by its name), so the two match and the
        // start comes back where it was. For the log only (the same offsets in 1.16.244).
        constexpr std::size_t   kCameraWorldFovOffset       = 0x280;
        constexpr std::size_t   kCameraFirstPersonFovOffset = 0x298;
        constexpr std::size_t   kCameraFovAddOffsetA        = 0x29C;
        constexpr std::size_t   kCameraFovAddOffsetB        = 0x2A4;
        constexpr std::size_t   kCameraFovAddFlagOffset     = 0x2B0;

        // (fix195) Where the game's beam update (BeamProjectile's update, 1.16.236 0x1b09aa0) aims its beam again each frame: it hands
        // its own copy of the launch data (+0x290) to the launch function this hook is on, only for a beam with Continuous Update. Tells
        // those calls apart for certain (the beam's form is read as well, so a build that does not know this place still has the fix as
        // long as the form says beam) and lets the log read the beam's hit distance.
        constexpr uintptr_t kBeamLaunchCallRva = 0x1b0a2ba;
        constexpr uint8_t   kBeamLaunchCall[]  = { 0x48, 0x8D, 0x8F, 0x90, 0x02, 0x00, 0x00, // lea rcx, [rdi+0x290] (its launch data)
                                                   0xE8, 0xDA, 0xAB, 0x04, 0x00 };           // call (the launch function, 0x1b54ea0)

        safetyhook::InlineHook g_compute_launch_origin_hook{};
        std::atomic<bool>      g_follow_barrel{ true };
        uintptr_t              g_beam_return{ 0 }; // (fix195) the return address of the beam update's call; 0: not known in this build

        void CopyLaunchData(std::uint8_t* to, const std::uint8_t* from)
        {
            SIZE_T done = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), from, to, HandActions::kLaunchBytes, &done) || done != HandActions::kLaunchBytes) {
                std::memset(to, 0, HandActions::kLaunchBytes);
            }
        }

        // (fix195) A read of the game's memory that fails rather than crashes (a form or object that is not what it should be).
        bool ReadBytes(uintptr_t from, void* to, std::size_t n)
        {
            SIZE_T done = 0;
            return from != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(from), to, n, &done) && done == n;
        }

        // (fix195) Whether this launch is of a continuous beam (the cutter's): its projectile a beam the game aims again every frame.
        // Other weapons (bullets, other beams, flames, which the game also aims again each frame through this function) are left as before.
        bool ReadForm(const std::uint8_t* launch_data, std::uint32_t& flags, std::uint8_t& kind)
        {
            std::uint64_t form = 0;
            std::memcpy(&form, launch_data + kProjectileFormOffset, sizeof(form));
            return form != 0 && ReadBytes(static_cast<uintptr_t>(form) + kFormFlagsOffset, &flags, sizeof(flags)) &&
                   ReadBytes(static_cast<uintptr_t>(form) + kFormKindOffset, &kind, sizeof(kind));
        }

        bool IsContinuousBeam(const std::uint8_t* launch_data)
        {
            std::uint32_t flags = 0;
            std::uint8_t  kind  = 0;
            return ReadForm(launch_data, flags, kind) && kind == kBeamKind && (flags & kContinuousUpdateFlag) != 0;
        }

        // (fix204) What a beam's own muzzle node (kBeamMuzzleNodeOffset) was found holding, on the frames it was looked at.
        enum class BeamNode
        {
            NotLooked,   // not looked at this frame (not the beam's update, or its muzzle not the gun in your hand)
            Put,         // empty: the gun in your hand put there
            AlreadyOurs, // the gun in your hand already (put on an earlier frame)
            Other,       // another node (left as it was)
            Unreadable,  // could not be read or written (left as it was)
        };

        // (fix204) A pointer of the game's, put there under a guard only if it is empty: `seen` gets what it held (what the swap found).
        // False when it held something else or could not be read or written (a bad address fails instead of crashing the game).
        bool PutPointerIfEmpty(uintptr_t at, void* value, void*& seen, bool& readable)
        {
            seen     = nullptr;
            readable = false;
            if (at < 0x10000) {
                return false;
            }
            __try {
                seen     = *reinterpret_cast<void* volatile*>(at);
                readable = true;
                if (seen != nullptr) {
                    return false;
                }
                seen = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(at), value, nullptr);
                return seen == nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readable = false;
                return false;
            }
        }

        // (fix204) The beam's update, with its muzzle (the launch data's node) the gun in your hand: the beam's own node checked to be that
        // muzzle too (the game's launch copies it there), and set to it if the beam has none, so the game draws the beam and tests what it
        // hits from the gun in your hand from its next frame on. The reference the beam holds is counted here (as for the launch data's node)
        // and dropped by the game as the beam is destroyed. A node already there (ours, as expected, or another) is left. (`beam` is the BeamProjectile: the launch data is its copy,
        // at +0x290, handed over by its own update; `muzzle` is held by that launch data as well, so taking our count back on a failed swap
        // never frees it.)
        BeamNode GiveBeamTheMuzzle(std::uint8_t* beam, RE::NiAVObject* muzzle)
        {
            if (beam == nullptr || muzzle == nullptr) {
                return BeamNode::Unreadable;
            }
            const uintptr_t at  = reinterpret_cast<uintptr_t>(beam) + kBeamMuzzleNodeOffset;
            void*           now = nullptr;
            if (!ReadBytes(at, &now, sizeof(now))) {
                return BeamNode::Unreadable;
            }
            if (now != nullptr) { // (the usual frame once it is put: nothing counted or written)
                return now == muzzle ? BeamNode::AlreadyOurs : BeamNode::Other;
            }
            auto* const count = reinterpret_cast<volatile long*>(reinterpret_cast<std::uint8_t*>(muzzle) + 8);
            InterlockedIncrement(count); // (counted before the beam can be seen holding it)
            void* seen     = nullptr;
            bool  readable = false;
            if (PutPointerIfEmpty(at, muzzle, seen, readable)) {
                return BeamNode::Put;
            }
            InterlockedDecrement(count);
            if (!readable) {
                return BeamNode::Unreadable;
            }
            return seen == muzzle ? BeamNode::AlreadyOurs : BeamNode::Other;
        }

        const char* BeamNodeText(BeamNode state)
        {
            switch (state) {
                case BeamNode::Put:         return "empty (not expected): the gun in your hand's muzzle put there (the beam drawn, and what it hits tested, from it from the next frame on)";
                case BeamNode::AlreadyOurs: return "already the gun in your hand's muzzle (as expected: the game's launch copies it from the launch data)";
                case BeamNode::Other:       return "another node (left as it was: the beam not drawn from the gun in your hand)";
                case BeamNode::Unreadable:  return "not readable (left as it was)";
                default:                    return "not looked at";
            }
        }

        // (fix204) Once (on the first beam frame fired from the gun in your hand): whether the game's own first-person FOV correction of a
        // beam's start (0x1e96420) runs. It runs unless the two FOVs are exactly equal (the game's own test); running, it changes the start
        // only where its reference camera's lens differs from the world camera's (see kCameraWorldFovOffset).
        void LogCameraFovsOnce()
        {
            static std::atomic<bool> logged{ false };
            if (logged.exchange(true)) {
                return;
            }
            const auto  camera = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerCameraSingleton());
            float       world = 0.0f, first = 0.0f, add_a = 0.0f, add_b = 0.0f;
            std::uint8_t flag = 0;
            if (camera == 0 || !ReadBytes(camera + kCameraWorldFovOffset, &world, sizeof(world)) ||
                !ReadBytes(camera + kCameraFirstPersonFovOffset, &first, sizeof(first)) ||
                !ReadBytes(camera + kCameraFovAddOffsetA, &add_a, sizeof(add_a)) || !ReadBytes(camera + kCameraFovAddOffsetB, &add_b, sizeof(add_b)) ||
                !ReadBytes(camera + kCameraFovAddFlagOffset, &flag, sizeof(flag))) {
                spdlog::info("[MuzzleFire] (fix204) the game's camera FOVs could not be read (whether the game moves a beam's start for them not known)");
                return;
            }
            const float used = flag != 0 ? first + (add_a + add_b) : first;
            spdlog::info("[MuzzleFire] (fix204) the game's camera FOVs: the world's {:.2f}, the first person's {:.2f}{}: the game's FOV correction of "
                         "a beam's start {}",
                world, used, flag != 0 ? std::format(" ({:.2f} + {:.2f} + {:.2f})", first, add_a, add_b) : std::string{},
                used == world ? "is off (equal)"
                              : "runs (they differ; it moves the start only where its reference camera's lens differs from the world camera's)");
        }

        // (fix195) A place as the game keeps it (the launch's start, the player's aim point): x, y, z in a frame of reference given by the
        // index after them. Two places are compared only when their index is the same (their difference is then the difference in the
        // world).
        struct Place
        {
            float         x, y, z;
            std::uint32_t index;
        };

        // (fix195) The player's aim point (where the game's aim ray from the camera hit) and the launch's start; false when the aim point
        // cannot be read.
        bool ReadAimAndStart(const std::uint8_t* launch_data, Place& aim, Place& start)
        {
            auto* player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr || !ReadBytes(reinterpret_cast<uintptr_t>(player) + kPlayerAimPointOffset, &aim, sizeof(aim))) {
                return false;
            }
            std::memcpy(&start, launch_data + kLaunchStartOffset, sizeof(start));
            return true;
        }

        // (fix195) How far the launch's start (the muzzle) is from the game's aim point, in metres; < 0 when not known (not read, or in
        // another frame of reference).
        float StartToAimPoint(const std::uint8_t* launch_data)
        {
            Place aim{}, start{};
            if (!ReadAimAndStart(launch_data, aim, start) || aim.index != start.index) {
                return -1.0f;
            }
            const float d[3]{ aim.x - start.x, aim.y - start.y, aim.z - start.z };
            const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            return std::isfinite(n) ? n : -1.0f;
        }

        // A direction from the launch angles (pitch, roll, yaw; Bethesda's forward: sin yaw cos pitch, cos yaw cos pitch, -sin pitch).
        void DirectionOf(const float* angles, float out[3])
        {
            out[0] = std::sin(angles[2]) * std::cos(angles[0]);
            out[1] = std::cos(angles[2]) * std::cos(angles[0]);
            out[2] = -std::sin(angles[0]);
        }

        float DegreesBetween(const float* a, const float* b)
        {
            const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (!(la > 1e-6f) || !(lb > 1e-6f)) {
                return -1.0f;
            }
            const float c = std::clamp((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb), -1.0f, 1.0f);
            return std::acos(c) * 57.2957795f;
        }

        // (fix195) The log of the continuous beam (the cutter): each trigger pull (a beam fired, until 0.3 s without its frames) has its
        // first frames written one by one, then one summary line.
        constexpr int kDetailedFrames = 20;  // frames written one by one, each pull
        constexpr int kDetailedPulls  = 30;  // pulls with those lines
        constexpr int kSummaryPulls   = 300; // pulls with a summary
        constexpr auto kPullGap       = std::chrono::milliseconds(300);

        // (fix195) What became of the game's ask for the camera's line (+0xE4) in a beam frame.
        enum class CameraLine
        {
            NotAsked,
            Cleared, // fired along the barrel: tested along the beam as drawn
            Kept,    // the muzzle at the surface the game's aim point is on: kept (the beam's own test would start inside it)
            Left,    // the frame left to the game
        };

        struct BeamPull
        {
            bool                                  open{ false };
            std::chrono::steady_clock::time_point last{};
            int                                   number{ 0 };
            int                                   frames{ 0 };
            int                                   written{ 0 };      // frames fired along the barrel by this hook
            int                                   camera_line{ 0 };  // frames the game asked for the camera's line
            int                                   camera_kept{ 0 };  // ...of them, kept (the muzzle at the surface)
            bool                                  follow{ true };    // shots follow the barrel, as of this pull's last frame
            float                                 turn_sum{ 0.0f };  // the game's own direction from the barrel, degrees
            float                                 turn_max{ 0.0f };
            int                                   turn_n{ 0 };
            float                                 off_max{ -1.0f };  // the game's aim point from the barrel's line, metres (< 0: never known)
            float                                 off_last{ -1.0f };
            float                                 along_last{ 0.0f }; // ...and how far out along it (with off_last)
            float                                 hit_last{ -1.0f };  // the beam's hit distance, as the game last had it
            BeamNode                              node_first{ BeamNode::NotLooked }; // (fix204) the beam's own node, as first looked at
            int                                   node_put{ 0 };      // (fix204) ...frames the gun's muzzle was put there
            int                                   node_ours{ 0 };     // ...frames it was already there
            int                                   node_other{ 0 };    // ...frames another node was there, or it could not be read
        };
        std::mutex        g_beam_log_mutex;
        BeamPull          g_pull;
        int               g_pulls{ 0 };
        std::atomic<bool> g_pull_open{ false };
        std::atomic<bool> g_beam_log_done{ false }; // (the last pull logged has been summed up: nothing more is read or written for the log)

        // (Under g_beam_log_mutex.) Its summary, and the log closed after the last pull it has room for.
        void WritePullSummary(const BeamPull& p)
        {
            if (p.number >= kSummaryPulls) {
                g_beam_log_done.store(true);
            }
            if (p.number > kSummaryPulls) {
                return;
            }
            spdlog::info("[MuzzleFire] (fix195) continuous beam, pull #{}: {} frames, {} fired along the barrel; the game's own direction (its "
                         "muzzle node) {:.1f} deg from the barrel on average, at most {:.1f}; the game asked for the camera's line on {} frames "
                         "({} of them kept: the muzzle at the surface; else cleared where fired along the barrel); the game's aim point {} from "
                         "the barrel's line at most ({} last, {} out); the beam's last hit distance {}; shots follow the barrel: {}; (fix204) the "
                         "beam's own node: first {}, then the gun's muzzle put there on {} frames, already there on {}, another or unreadable on "
                         "{}{}",
                p.number, p.frames, p.written, p.turn_n > 0 ? p.turn_sum / p.turn_n : 0.0f, p.turn_max, p.camera_line, p.camera_kept,
                p.off_max >= 0.0f ? std::format("{:.3f} m", p.off_max) : std::string{ "unknown" },
                p.off_last >= 0.0f ? std::format("{:.3f} m", p.off_last) : std::string{ "unknown" },
                p.off_last >= 0.0f ? std::format("{:.2f} m", p.along_last) : std::string{ "unknown" },
                p.hit_last >= 0.0f ? std::format("{:.2f} m", p.hit_last) : std::string{ "not known" }, p.follow ? "yes" : "no",
                p.node_first == BeamNode::Put           ? "empty"
                : p.node_first == BeamNode::AlreadyOurs ? "the gun's muzzle"
                : p.node_first == BeamNode::Other       ? "another node"
                : p.node_first == BeamNode::Unreadable  ? "unreadable"
                                                        : "never looked at",
                p.node_put, p.node_ours, p.node_other, p.number == kSummaryPulls ? " (the last pull logged)" : "");
        }

        // (fix195) One frame of the continuous beam, for the log: the angles the game worked out (before this hook wrote any) against the
        // barrel, whether the game asked for the camera's line, the beam's hit distance (its later frames only), and where the game's
        // aim point lies against the launch's start (the muzzle) and the barrel's line.
        void NoteBeamFrame(const std::uint8_t* launch_data, const float* game_angles, CameraLine camera_line, bool written, bool later,
            const char* left, uintptr_t from, BeamNode node_state)
        {
            // (Once the last pull logged is summed up: nothing more read for it, no lock taken.)
            if (g_beam_log_done.load(std::memory_order_relaxed)) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            float      barrel[3]{};
            const bool have_barrel = body::GetBodyAimForward(barrel);
            float      game_way[3]{};
            DirectionOf(game_angles, game_way);
            const float turn = have_barrel ? DegreesBetween(game_way, barrel) : -1.0f;
            // The beam's hit distance: only from its own update, where the launch data is the beam's copy.
            float hit = -1.0f;
            if (from != 0 && from == g_beam_return) {
                float value = 0.0f;
                if (ReadBytes(reinterpret_cast<uintptr_t>(launch_data) - kBeamLaunchDataOffset + kBeamHitDistanceOffset, &value, sizeof(value)) &&
                    std::isfinite(value)) {
                    hit = value;
                }
            }
            // The game's aim point against the launch's start and the barrel's line (Place: compared only in the same frame of reference).
            float       along = 0.0f, off = -1.0f; // (off < 0: not known)
            const char* aim_note = "not read";
            {
                Place      start{}, aim{};
                const bool have_aim = ReadAimAndStart(launch_data, aim, start);
                if (have_aim && have_barrel && aim.index == start.index) {
                    const float d[3]{ aim.x - start.x, aim.y - start.y, aim.z - start.z };
                    const float a = d[0] * barrel[0] + d[1] * barrel[1] + d[2] * barrel[2];
                    const float p[3]{ d[0] - a * barrel[0], d[1] - a * barrel[1], d[2] - a * barrel[2] };
                    const float o = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                    if (std::isfinite(a) && std::isfinite(o)) {
                        along    = a;
                        off      = o;
                        aim_note = "read";
                    }
                } else if (have_aim) {
                    aim_note = have_barrel ? "in another frame of reference than the muzzle" : "no barrel known";
                }
            }
            std::uint32_t form_flags = 0;
            std::uint8_t  form_kind  = 0;
            const bool    have_form  = ReadForm(launch_data, form_flags, form_kind);
            const bool    follow     = g_follow_barrel.load();
            std::scoped_lock _{ g_beam_log_mutex };
            if (!g_pull.open || now - g_pull.last > kPullGap) {
                if (g_pull.open) {
                    WritePullSummary(g_pull);
                    g_pull.open = false;
                    g_pull_open.store(false);
                }
                if (g_beam_log_done.load()) {
                    return;
                }
                g_pull        = {};
                g_pull.open   = true;
                g_pull.number = ++g_pulls;
                g_pull_open.store(true);
            }
            g_pull.last   = now;
            g_pull.follow = follow;
            ++g_pull.frames;
            g_pull.written += written ? 1 : 0;
            g_pull.camera_line += camera_line != CameraLine::NotAsked ? 1 : 0;
            g_pull.camera_kept += camera_line == CameraLine::Kept ? 1 : 0;
            if (turn >= 0.0f) {
                g_pull.turn_sum += turn;
                g_pull.turn_max = std::max(g_pull.turn_max, turn);
                ++g_pull.turn_n;
            }
            if (off >= 0.0f) {
                g_pull.off_max    = std::max(g_pull.off_max, off);
                g_pull.off_last   = off;
                g_pull.along_last = along;
            }
            if (hit >= 0.0f) {
                g_pull.hit_last = hit;
            }
            // (fix204) The beam's own node: counted, and the first time it is looked at in a pull, said in a line of its own.
            if (node_state != BeamNode::NotLooked) {
                g_pull.node_put += node_state == BeamNode::Put ? 1 : 0;
                g_pull.node_ours += node_state == BeamNode::AlreadyOurs ? 1 : 0;
                g_pull.node_other += node_state == BeamNode::Other || node_state == BeamNode::Unreadable ? 1 : 0;
                if (g_pull.node_first == BeamNode::NotLooked) {
                    g_pull.node_first = node_state;
                    spdlog::info("[MuzzleFire] (fix204) continuous beam, pull #{} frame {}: the beam's own node (where the game draws it and tests "
                                 "what it hits from) was {}",
                        g_pull.number, g_pull.frames, BeamNodeText(node_state));
                }
            }
            if (g_pull.frames <= kDetailedFrames && g_pull.number <= kDetailedPulls) {
                spdlog::info("[MuzzleFire] (fix195) continuous beam, pull #{} frame {} ({}{}; its form: {}): {}; the game's own direction {} from "
                             "the barrel (pitch {:.1f}, yaw {:.1f} deg); camera's line asked {}; the beam's hit distance {}; the game's aim point {}; "
                             "shots follow the barrel: {}",
                    g_pull.number, g_pull.frames, later ? "a later frame" : "fired", from != 0 && from == g_beam_return ? ", the beam's update" : "",
                    have_form ? std::format("kind {}, flags {:#x}", static_cast<int>(form_kind), form_flags) : std::string{ "not read" },
                    written ? std::string{ "fired along the barrel" } : std::format("left to the game ({})", left),
                    turn >= 0.0f ? std::format("{:.1f} deg", turn) : std::string{ "unknown" }, game_angles[0] * 57.2957795f,
                    game_angles[2] * 57.2957795f,
                    camera_line == CameraLine::NotAsked ? "no"
                    : camera_line == CameraLine::Cleared ? "yes (cleared: tested along the beam)"
                    : camera_line == CameraLine::Kept    ? "yes (kept: the muzzle at the surface the game's aim point is on)"
                                                         : "yes (left to the game)",
                    hit >= 0.0f ? std::format("{:.2f} m", hit) : std::string{ "not known" },
                    off >= 0.0f ? std::format("{:.3f} m off the barrel's line, {:.2f} m out", off, along) : std::string{ aim_note },
                    follow ? "yes" : "no");
            }
        }

        // Player shots take the projectile-node path when this flag is set; NPCs never read it.
        bool ComputeLaunchOrigin(std::uint8_t* launch_data)
        {
            static auto vr  = VR::get();
            const auto  from = reinterpret_cast<uintptr_t>(MUZZLEFIRE_RETURN_ADDRESS());
            bool        aim = false;
            const auto  player = launch_data ? CreationEngineSingletonManager::GetPlayerRef() : nullptr;
            const bool  by_player = player && *reinterpret_cast<void**>(launch_data + kShooterOffset) == player;
            if (by_player) {
                haptics::PlayerFired();
            }
            // A grenade thrown from the shoulder or a power pushed out (HandActions): the launch data as it came (for the log of
            // the first few grenades, and to tell what is launched).
            const bool    grenade_pending = by_player && HandActions::ThrowPending();
            const bool    power_moment    = by_player && HandActions::PowerAimActive();
            std::uint8_t  before[HandActions::kLaunchBytes]{};
            if (grenade_pending || power_moment) {
                CopyLaunchData(before, launch_data);
            }
            // Only with the weapon out: with it put away the hand aims for pointing at things (grenades are left alone). Not a
            // grenade from the shoulder (it goes along the hand's throw, from where the game throws it), nor, just after a power
            // was pushed out, anything that is not the gun's (the power goes along the push).
            bool not_the_gun = grenade_pending && HandActions::IsThrownLaunch(before);
            if (!not_the_gun && power_moment) {
                const auto gun  = static_cast<std::uint64_t>(HandGrab::WeaponInHandsForm());
                bool       mine = false;
                for (std::size_t at = 0; gun != 0 && at + 8 <= HandActions::kLaunchBytes && !mine; at += 8) {
                    std::uint64_t value = 0;
                    std::memcpy(&value, before + at, sizeof(value));
                    mine = value == gun;
                }
                not_the_gun = !mine;
            }
            const bool aiming = by_player && !not_the_gun && tracking::AimingWithHand() && vr->is_using_controllers() && GameFlow::isWeaponDrawn();
            if (aiming && !ModConstants::shootFromMuzzle) {
                g_follow_barrel = false;
            }
            // (fix195) A continuous beam (every such weapon: the cutter, the Arc Welder and the like): the game aims it again every frame
            // through this function, with its own copy of the launch data, which still holds the muzzle given at the first frame (the
            // ProjectileNode of the gun in the body's hand). Before, only that first frame was fired along the barrel: later frames went
            // along that node's own axis, which the weapon's firing animation can swing (the cutter's first-person model was seen swinging
            // about 18 degrees within a tenth of a second; the body's gun is the same weapon model), and the gun sight's laser was told
            // each frame that shots no longer follow the barrel. Its later frames are fired along the barrel too while they still start
            // at the gun in your hand.
            // (The beam update's own call is one for certain: the game makes it only for a beam with Continuous Update.)
            const bool  beam_update = from != 0 && from == g_beam_return;
            const bool  beam  = aiming && ModConstants::shootFromMuzzle && (beam_update || IsContinuousBeam(launch_data));
            bool        later = false;    // (fix195) a continuous beam's later frame (its muzzle given before)
            bool        ours  = false;    // (fix195) ...and that muzzle is the gun in your hand, found this frame
            const char* left  = "";       // (fix195) why a frame is left to the game (the log)
            BeamNode    beam_node = BeamNode::NotLooked; // (fix204) the beam's own node, when looked at this frame
            if (aiming && ModConstants::shootFromMuzzle) {
                {
                    launch_data[kAimFromMuzzleOffset] = 1;
                    // Fire from the weapon in the shown body's hand; the launch data releases this reference.
                    auto& node = *reinterpret_cast<RE::NiAVObject**>(launch_data + kMuzzleNodeOffset);
                    if (!node) {
                        if (auto muzzle = body::GetBodyMuzzle()) {
                            InterlockedIncrement(reinterpret_cast<volatile long*>(reinterpret_cast<std::uint8_t*>(muzzle) + 8));
                            node = muzzle;
                            aim  = true;
                        } else {
                            left = "the gun in your hand not found this frame";
                        }
                    } else if (beam) {
                        // (Only compared: the launch data holds its own reference to the node it has.)
                        later                 = true;
                        const auto muzzle_now = body::GetBodyMuzzle();
                        ours                  = muzzle_now != nullptr && node == muzzle_now;
                        left                  = muzzle_now == nullptr ? "the gun in your hand not found this frame" : "not fired from the gun in your hand";
                        // (fix204) In the beam's own update (its launch data the beam's copy, at +0x290), fired from the gun in your hand:
                        // the beam's own node (where the game draws it and starts the test of what it hits) checked to be that gun's muzzle,
                        // and set to it if the beam has none (the game's launch normally copies it there already). (Not with the body off,
                        // or another muzzle: `ours` is false then; never for other projectiles: only the beam's update makes this call.)
                        if (ours && beam_update) {
                            beam_node = GiveBeamTheMuzzle(launch_data - kBeamLaunchDataOffset, muzzle_now);
                            LogCameraFovsOnce();
                        }
                    }
                    body::NotifyPlayerFired();
                    if (beam) {
                        body::NotifyBeamFrame(); // (fix206: a multi-beam weapon's firing hold is learned only on a continuous beam's frames)
                    }
                }
            }
            const bool result = g_compute_launch_origin_hook.call<bool>(launch_data);

            // (fix195) What the game worked out, before anything is written (for the log).
            float        game_angles[3]{};
            std::uint8_t game_camera_line = 0;
            if (beam) {
                std::memcpy(game_angles, launch_data + kAnglesOffset, sizeof(game_angles));
                game_camera_line = launch_data[kUseCameraLineOffset];
            }

            // Direction along the aim ray, whatever the muzzle node's own axis: pitch = -asin(z), yaw = atan2(x, y).
            float forward[3];
            const bool barrel = (aim || (later && ours)) && body::GetBodyAimForward(forward);
            if (barrel) {
                const float two_pi = 6.28318530718f;
                auto        angles = reinterpret_cast<float*>(launch_data + kAnglesOffset);
                angles[0]          = -std::asin(std::clamp(forward[2], -1.0f, 1.0f));
                angles[1]          = 0.0f;
                angles[2]          = std::fmod(std::atan2(forward[0], forward[1]) + two_pi, two_pi);
            }
            // (fix195) A continuous beam fired along the barrel: what it hits is tested along the beam as drawn, not along the camera's line
            // (from your view toward the game's aim point, which lies beside the barrel's line by your hand's distance from it). Except
            // with the muzzle pushed into a surface: the game asked for the camera's line (it does when its aim point is close to the
            // weapon) and the muzzle is within kMuzzleAtSurface of the game's aim point (the surface it is on): the beam's own test would
            // start inside it, so the game's camera line is kept there.
            CameraLine camera_line = game_camera_line == 0 ? CameraLine::NotAsked : CameraLine::Left;
            if (barrel && beam && game_camera_line != 0) {
                const float to_aim = StartToAimPoint(launch_data);
                if (to_aim >= 0.0f && to_aim < kMuzzleAtSurface) {
                    camera_line = CameraLine::Kept;
                } else {
                    launch_data[kUseCameraLineOffset] = 0;
                    camera_line                       = CameraLine::Cleared;
                }
            }
            if (aiming && ModConstants::shootFromMuzzle) {
                if (!later) {
                    g_follow_barrel = barrel; // what the gun sight follows
                } else if (barrel) {
                    // (fix195) A continuous beam's later frames: only ever told that it follows the barrel (a frame it could not be fired
                    // along it leaves what its first frame said).
                    g_follow_barrel = true;
                }
            }
            if (beam) {
                NoteBeamFrame(launch_data, game_angles, camera_line, barrel, later,
                    barrel ? "" : (later ? ours : aim) ? "the barrel not known this frame" : left, from, beam_node);
            }
            // Last, so a grenade thrown from the shoulder goes along the hand's throw.
            if (grenade_pending) {
                HandActions::OnPlayerLaunch(launch_data, before);
            }
            return result;
        }
    }

    bool ShotsFollowBarrel()
    {
        return g_follow_barrel.load();
    }

    void ResetShotsFollowBarrel()
    {
        g_follow_barrel = true;
    }

    void Update()
    {
        // (fix195) A pull of the continuous beam that has had no frame for 0.3 s: its summary written.
        if (!g_pull_open.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock _{ g_beam_log_mutex };
        if (g_pull.open && std::chrono::steady_clock::now() - g_pull.last > kPullGap) {
            WritePullSummary(g_pull); // (closes the log after the last pull it has room for)
            g_pull.open = false;
            g_pull_open.store(false);
        }
    }

    void Install()
    {
        const auto address = body::ScanUnique("ComputeLaunchOrigin (shots from the gun muzzle)",
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FA FF FF 48 81 EC 08 06 00 00", 0x1b52510);
        if (!address) {
            return;
        }
        // (fix195) The beam update's call (for the log only): its code checked, and it must call the function hooked here.
        if (GameCode::Same(kBeamLaunchCallRva, kBeamLaunchCall)) {
            const uintptr_t site = GameCode::At(kBeamLaunchCallRva);
            std::int32_t    rel  = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(site + 8), sizeof(rel));
            if (site + 12 + static_cast<std::intptr_t>(rel) == address) {
                g_beam_return = site + 12;
            }
        }
        spdlog::info("[MuzzleFire] (fix195) a continuous beam (the cutter) is fired along the barrel in every frame, what it hits tested along "
                     "it; the beam update's call {}",
            g_beam_return != 0 ? "found (its hit distance logged; (fix204) the beam's own node checked to be the gun in your hand's muzzle)"
                               : "not known in this build (its hit distance not logged; (fix204) the beam's own node not checked)");
        g_compute_launch_origin_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ComputeLaunchOrigin));
        if (!g_compute_launch_origin_hook) {
            spdlog::error("[MuzzleFire] Failed to hook ComputeLaunchOrigin");
        }
    }
}
