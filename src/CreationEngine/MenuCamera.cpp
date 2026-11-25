#include "MenuCamera.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "CreationEngineCameraManager.h"
#include "CreationEngineSingletonManager.h"
#include "LiveWorld.h"
#include "ViewHistory.h"
#include "NiMath.h"
#include "memory/ScanHelper.h"
#include <mods/VR.hpp>

namespace MenuCamera
{
    namespace
    {
        // The renderer's setup of a view's camera for the frame (Starfield 1.16.236; rcx: the view's part of the frame, its view's
        // handle at +0x24). Its first reads are the view's camera, from the camera views' table, and that camera's place and turn,
        // from the cameras' table, both from the renderer's copies:
        //   mov rax, rsp / ... / sub rsp, 0x3b8
        //   mov r11d, [rcx+0x24]          the view's handle
        //   mov r13, rcx
        //   mov rcx, [cameras]            <- the cameras' table (its address relative to the next instruction: +3)
        //   and r11d, 0xffffff
        constexpr const char* kSetupPattern =
            "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 FD FF FF 48 81 EC B8 03 00 00 "
            "44 8B 59 24 4C 8B E9 48 8B 0D ? ? ? ? 41 81 E3 FF FF FF 00";
        constexpr size_t kSetupCameras = 0x30;
        // ...a little further in the same function:
        //   mov rax, [camera views]       <- the camera views' table (+3)
        //   mov [rsp+0x50], esi
        //   mov r10, [rax+0x120]          its view data
        //   mov r8, [r10+0x40]            the renderer's slots
        //   mov edx, [r8+r11*4]           the view's slot
        constexpr const char* kViewsPattern = "48 8B 05 ? ? ? ? 89 74 24 50 4C 8B 90 20 01 00 00 4D 8B 42 40 43 8B 14 98";
        constexpr size_t      kViewsSearch  = 0x100;
        // The camera's own world update (NiCamera's), as it writes its place and turn into the game's copy:
        //   mov ecx, [rsi+0x130]          its handle
        //   cmp ecx, 0xffffff / je ...    (none: not in the tables)
        //   vmovss xmm10, [...]
        //   mov rax, [cameras]            <- the same cameras' table (+3)
        //   mov rdx, [rax+0x108]          the place-and-turn column
        //   and ecx, 0xffffff
        //   mov rax, [rdx+0x2c8]          the game's slots
        //   mov ecx, [rax+rcx*4]          the camera's slot
        constexpr const char* kWritePattern =
            "8B 8E 30 01 00 00 81 F9 FF FF FF 00 0F 84 ? ? ? ? C5 7A 10 15 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B 90 08 01 00 00 81 E1 FF FF FF 00 "
            "48 8B 82 C8 02 00 00 8B 0C 88";
        constexpr size_t kWriteCameras = 0x1a;

        // The tables' columns (the game's StorageTable::AsyncTableColumn): the slot of each handle in the renderer's copy (+0x40) and in
        // the game's (+0x2c8), and the slots themselves (+0x3c8). The game moves a handle to a new slot when it writes it after the
        // renderer was given the old one (and gives the renderer the new one between frames). Slot 0 is the game's "none".
        // (The lens is a column of its own, +0x100; with the mod's symmetric eye projection both eyes' lens is the same, so the menu's
        // camera keeps the one it has.)
        // The stars' camera (fix122's research): the renderer draws the star field with a camera of its own ("Starfield Camera"),
        // looked up for each view right after the view's camera setup (0x1429242B5..0x1429244D4): the view's scene (its camera view's
        // +0) -> the scenes' table column +0x170 (4 bytes each: the scene's star data) -> the star data's table column +0x108 (4
        // bytes each: its camera), each handle checked by its generation (a table's count at +0xdc, its generations at +8); then that
        // camera's record (the cameras' column +0x108), as for any camera. Behind the menu MenuCamera turns it with the head as well,
        // else the stars stay where they were as the menu opened (and, piloting in space, the whole ship seems to turn with you).
        //   mov r9, [scenes]             <- the scenes' table (+3)
        //   mov r8, [r9+0x170]           its star data column
        //   and ecx, 0xffffff / mov rax, [r8+0x40] / mov edx, [rax+rcx*4] / mov rax, [r8+0x3c8] / mov r8d, [rax+rdx*4]
        //   mov ecx, [rbx+0x24] / cmp [stars on], r15b / je ... / (the view's flags: bit 14, the stars drawn) ...
        //   cmp r8d, 0xffffff / je ... / mov rdx, [star data]   <- the star data's table (+0x7b, +3)
        constexpr const char* kStarsPattern =
            "4C 8B 0D ? ? ? ? 4D 8B 81 70 01 00 00 81 E1 FF FF FF 00 49 8B 40 40 8B 14 88 49 8B 80 C8 03 00 00 44 8B 04 90 8B 4B 24 44 38 3D ? ? ? ? "
            "0F 84 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B 90 08 01 00 00 81 E1 FF FF FF 00 48 8B 42 40 8B 0C 88 48 69 C1 10 02 00 00 48 03 82 C8 03 00 00 "
            "8B 08 48 C1 E9 0E F6 C1 01 0F 84 ? ? ? ? 41 81 F8 FF FF FF 00 0F 84 ? ? ? ? 48 8B 15 ? ? ? ?";
        constexpr size_t    kStarsScenes      = 0x00;  // (the load of the scenes' table)
        constexpr size_t    kStarsData        = 0x7b;  // (the load of the star data's table)
        constexpr uintptr_t kSceneStarsColumn = 0x170; // the scenes': each one's star data, 4 bytes
        constexpr uintptr_t kStarsCameraColumn = 0x108; // the star data's: its camera, 4 bytes
        constexpr uintptr_t kTableCount       = 0xdc;
        constexpr uintptr_t kTableGenerations = 0x8;
        constexpr uintptr_t kTransformColumn = 0x108; // the cameras': place and turn, 160 bytes each
        // The cameras' motion column (64 bytes each, written by NiCamera's world update, 0x142BE9084..0x142BE9207): the camera's speeds,
        // the frame of its update (+0x38) and its cut flag (+0x3c: the camera jumped). The renderer reads the flag of a view's camera
        // just before setting it up (0x142924229: "shl rdx, 6", then "cmp [rdx+rax+0x3c], r15b"): set, it throws away the view's
        // histories (each of its 12 persistent pictures marked to start over, and its "camera cut"). Behind the menu the game no longer
        // updates the frozen camera, so its flag stays as the menu's opening left it.
        constexpr uintptr_t kMotionColumn    = 0xe8;
        constexpr size_t    kMotionSize      = 64;
        constexpr uintptr_t kMotionFrame     = 0x38;
        constexpr uintptr_t kMotionCut       = 0x3c;
        constexpr uintptr_t kViewDataColumn  = 0x120; // the camera views': 0x1c bytes each, the view's camera's handle at +4
        constexpr uintptr_t kRendererSlots   = 0x40;
        constexpr uintptr_t kGameSlots       = 0x2c8;
        constexpr uintptr_t kSlotData        = 0x3c8;
        constexpr size_t    kTransformSize   = 160;
        constexpr size_t    kViewDataSize    = 0x1c;
        constexpr uint32_t  kMaxSlot         = 1u << 22; // (a slot number is at most 22 bits)
        constexpr uintptr_t kViewHandle      = 0x24;     // (the view's part of the frame)
        constexpr uintptr_t kCameraHandle    = 0x130;    // NiCamera: its handle in the tables
        constexpr uintptr_t kOffsetMatrix    = 0x140;    // NiCamera: a 4x4 the world update applies (normally the identity)
        constexpr uint32_t  kNoHandle        = 0xffffff;

        // A camera's place and turn (CameraTransformData), as NiCamera's world update writes it (0x142BE8AA0, after its world transform W
        // times the offset matrix, normally the identity):
        //   floats 0..3    its place (W's translation), and a 0
        //   floats 4..7    its turn, a quaternion (of the axes in another order; which one is found from the game's own records)
        //   floats 8..23   M: rows 0..2 = (axis 2, axis 1, axis 0) component i, 0; row 3 constant (W's axes: the rows of its NiMatrix3)
        //   floats 24..39  M transposed (its inverse), from which and the place the renderer builds the view
        constexpr int kFloats = static_cast<int>(kTransformSize / sizeof(float));
        constexpr int kTurn   = 4;
        constexpr int kM      = 8;
        constexpr int kN      = 24;
        using Record          = std::array<float, kFloats>;

        safetyhook::MidHook g_setup_hook{};
        uintptr_t           g_cameras{ 0 };     // where the game keeps the cameras' table
        uintptr_t           g_views{ 0 };       // ...and the camera views'
        uintptr_t           g_scenes{ 0 };      // ...the scenes' and the star data's (the stars' camera: 0 if not found)
        uintptr_t           g_stars{ 0 };
        std::atomic<bool>   g_installed{ false };
        std::atomic<bool>   g_session{ false }; // a view set up behind the open menu (its counts are for the log when it closes)

        bool InGame(uintptr_t at)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            return at >= base && at < base + MemoryScan::mod_size;
        }

        // The address a "mov reg, [rip+x]" (7 bytes) at "at" reads.
        uintptr_t Relative(uintptr_t at)
        {
            int32_t distance = 0;
            std::memcpy(&distance, reinterpret_cast<const void*>(at + 3), sizeof(distance));
            return at + 7 + static_cast<intptr_t>(distance);
        }

        bool ReadU32(uintptr_t at, uint32_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CopyGuarded(void* to, const void* from, size_t size)
        {
            __try {
                std::memcpy(to, from, size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A camera's slots in one of the cameras' columns, the renderer's and the game's.
        struct Slots
        {
            uint32_t  camera{ kNoHandle };
            uint32_t  renderer{ 0 };
            uint32_t  game{ 0 };
            uintptr_t renderer_at{ 0 };
            uint32_t  scene{ kNoHandle }; // (a view's: its scene's handle, as its camera view has it)
            uintptr_t game_at{ 0 };
        };

        bool ReadColumnSlots(uintptr_t column_at, size_t size, uint32_t camera, Slots& out)
        {
            __try {
                out.camera = camera;
                const uintptr_t cameras = *reinterpret_cast<const uintptr_t*>(g_cameras);
                if (cameras == 0 || camera == kNoHandle) {
                    return false;
                }
                const uintptr_t column = *reinterpret_cast<const uintptr_t*>(cameras + column_at);
                if (column == 0) {
                    return false;
                }
                const uintptr_t renderer_slots = *reinterpret_cast<const uintptr_t*>(column + kRendererSlots);
                const uintptr_t game_slots     = *reinterpret_cast<const uintptr_t*>(column + kGameSlots);
                const uintptr_t items          = *reinterpret_cast<const uintptr_t*>(column + kSlotData);
                if (renderer_slots == 0 || game_slots == 0 || items == 0) {
                    return false;
                }
                out.renderer = reinterpret_cast<const uint32_t*>(renderer_slots)[camera];
                out.game     = reinterpret_cast<const uint32_t*>(game_slots)[camera];
                if (out.renderer == 0 || out.game == 0 || out.renderer >= kMaxSlot || out.game >= kMaxSlot) {
                    return false;
                }
                out.renderer_at = items + out.renderer * size;
                out.game_at     = items + out.game * size;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadCameraSlots(uint32_t camera, Slots& out)
        {
            return ReadColumnSlots(kTransformColumn, kTransformSize, camera, out);
        }

        // A view's camera (as the renderer has it), and that camera's slots.
        bool ReadSlots(uint32_t view, Slots& out)
        {
            uint32_t camera = kNoHandle;
            uint32_t scene  = kNoHandle;
            __try {
                const uintptr_t views = *reinterpret_cast<const uintptr_t*>(g_views);
                if (views == 0) {
                    return false;
                }
                const uintptr_t view_data = *reinterpret_cast<const uintptr_t*>(views + kViewDataColumn);
                if (view_data == 0) {
                    return false;
                }
                const uintptr_t view_slots = *reinterpret_cast<const uintptr_t*>(view_data + kRendererSlots);
                const uintptr_t view_items = *reinterpret_cast<const uintptr_t*>(view_data + kSlotData);
                if (view_slots == 0 || view_items == 0) {
                    return false;
                }
                const uint32_t view_slot = reinterpret_cast<const uint32_t*>(view_slots)[view];
                if (view_slot >= kMaxSlot) {
                    return false;
                }
                camera = *reinterpret_cast<const uint32_t*>(view_items + view_slot * kViewDataSize + 4) & kNoHandle;
                scene  = *reinterpret_cast<const uint32_t*>(view_items + view_slot * kViewDataSize);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            if (camera == kNoHandle || !ReadCameraSlots(camera, out)) {
                return false;
            }
            out.scene = scene;
            return true;
        }

        // The stars' camera of a scene, as the renderer finds it (kStarsPattern): its handle (its index in the cameras' table: the low
        // 24 bits).
        bool StarsCameraOf(uint32_t scene, uint32_t& camera)
        {
            camera = kNoHandle;
            if (g_scenes == 0 || g_stars == 0 || scene == kNoHandle) {
                return false;
            }
            __try {
                const uintptr_t scenes = *reinterpret_cast<const uintptr_t*>(g_scenes);
                const uintptr_t stars  = *reinterpret_cast<const uintptr_t*>(g_stars);
                const uintptr_t cams   = *reinterpret_cast<const uintptr_t*>(g_cameras);
                if (scenes == 0 || stars == 0 || cams == 0) {
                    return false;
                }
                // (A handle: its index in the low 24 bits, its generation in the top 8, checked against the table's.)
                const auto valid = [](uintptr_t table, uint32_t handle) {
                    const uint32_t index      = handle & kNoHandle;
                    const uint8_t  generation = static_cast<uint8_t>(handle >> 24);
                    return handle != kNoHandle && generation != 0 && index < *reinterpret_cast<const uint32_t*>(table + kTableCount) &&
                           generation == *reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(table + kTableGenerations) + index);
                };
                const auto column = [](uintptr_t table, uintptr_t at, uint32_t index, uint32_t& value) {
                    const uintptr_t col   = *reinterpret_cast<const uintptr_t*>(table + at);
                    const uintptr_t slots = col != 0 ? *reinterpret_cast<const uintptr_t*>(col + kRendererSlots) : 0;
                    const uintptr_t items = col != 0 ? *reinterpret_cast<const uintptr_t*>(col + kSlotData) : 0;
                    if (slots == 0 || items == 0) {
                        return false;
                    }
                    const uint32_t slot = reinterpret_cast<const uint32_t*>(slots)[index];
                    if (slot >= kMaxSlot) {
                        return false;
                    }
                    value = *reinterpret_cast<const uint32_t*>(items + slot * 4);
                    return true;
                };
                const uint32_t scene_index = scene & kNoHandle;
                if (scene_index >= *reinterpret_cast<const uint32_t*>(scenes + kTableCount)) {
                    return false;
                }
                uint32_t data = kNoHandle, cam = kNoHandle;
                if (!column(scenes, kSceneStarsColumn, scene_index, data) || !valid(stars, data) ||
                    !column(stars, kStarsCameraColumn, data & kNoHandle, cam) || !valid(cams, cam)) {
                    return false;
                }
                camera = cam; // (the whole handle: its index, and its generation to tell it is still the same camera)
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadRecord(uintptr_t at, Record& out)
        {
            return CopyGuarded(out.data(), reinterpret_cast<const void*>(at), kTransformSize);
        }

        uint32_t HandleOf(const void* camera)
        {
            uint32_t handle = kNoHandle;
            if (camera == nullptr || !ReadU32(reinterpret_cast<uintptr_t>(camera) + kCameraHandle, handle)) {
                return kNoHandle;
            }
            return handle & kNoHandle;
        }

        // The world's camera (the scene's, the one the headset turns): its handle in the tables.
        uint32_t HeadCamera()
        {
            auto* root = CreationEngineSingletonManager::GetSceneGraphRoot();
            return root == nullptr ? kNoHandle : HandleOf(root->worldCamera);
        }

        // ---- A camera record and the pose in it ----

        // A camera's pose: its axes (glm columns, in world coordinates: NiMath's layout of the world transform) and its place.
        struct Pose
        {
            glm::mat3 axes{ 1.0f };
            glm::vec3 place{ 0.0f };
        };

        bool Finite(const Record& r)
        {
            for (float f : r) {
                if (!std::isfinite(f)) {
                    return false;
                }
            }
            return true;
        }

        // The pose in a record, and whether the record is laid out as expected: M's axes of length 1 and square to each other, N its
        // transpose.
        bool PoseOf(const Record& r, Pose& out)
        {
            if (!Finite(r)) {
                return false;
            }
            for (int a = 0; a < 3; ++a) {
                for (int i = 0; i < 3; ++i) {
                    out.axes[a][i] = r[kM + i * 4 + (2 - a)];
                }
            }
            out.place = glm::vec3{ r[0], r[1], r[2] };
            for (int a = 0; a < 3; ++a) {
                if (std::abs(glm::length(out.axes[a]) - 1.0f) > 1e-3f || std::abs(glm::dot(out.axes[a], out.axes[(a + 1) % 3])) > 1e-3f) {
                    return false;
                }
            }
            if (std::abs(glm::determinant(out.axes) - 1.0f) > 1e-3f) {
                return false;
            }
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    if (std::abs(r[kN + i * 4 + j] - r[kM + j * 4 + i]) > 1e-4f) {
                        return false;
                    }
                }
            }
            return true;
        }

        // The quaternion the game writes for these axes: DirectXMath's XMQuaternionRotationMatrix of the matrix whose rows are axes 2,
        // 0 and 1 (0x142BE8DA2..0x142BE8ED5; its constants XMPMMP, XMMPMP, XMMMPP at 0x144B050B0..D0): DirectX's rows are glm's columns.
        glm::quat TurnOf(const glm::mat3& axes)
        {
            return glm::quat_cast(glm::mat3{ axes[2], axes[0], axes[1] });
        }

        float Dot(const glm::quat& q, const Record& r)
        {
            return q.x * r[kTurn] + q.y * r[kTurn + 1] + q.z * r[kTurn + 2] + q.w * r[kTurn + 3];
        }

        // The record's quaternion is the one made from its axes, as above (up to its sign).
        bool TurnFits(const Record& r, const Pose& pose)
        {
            return std::abs(Dot(TurnOf(pose.axes), r)) > 0.9999f;
        }

        // A record with this pose, as the game would write it (the rest as in "like").
        Record RecordOf(const Record& like, const Pose& pose)
        {
            Record r = like;
            r[0] = pose.place.x;
            r[1] = pose.place.y;
            r[2] = pose.place.z;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    r[kM + i * 4 + j] = pose.axes[2 - j][i];
                }
            }
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    r[kN + i * 4 + j] = r[kM + j * 4 + i];
                }
            }
            glm::quat q = TurnOf(pose.axes);
            if (Dot(q, like) < 0.0f) {
                q = -q; // (the same turn: kept on the same side as before)
            }
            r[kTurn]     = q.x;
            r[kTurn + 1] = q.y;
            r[kTurn + 2] = q.z;
            r[kTurn + 3] = q.w;
            return r;
        }

        // (fix169) The stars' camera on a planet (a helper's research): the game turns it as the world's camera times the sky's frame there
        // (0x140D756C0: C * R * P, R from 0x140839E10), and R's second branch (0x14083A053: the rows (d x a, a, d)) is a mirror image: about
        // half the planet's surface gives the stars' camera axes whose determinant is -1. The renderer only reads its place and M (0x1429FFFE0
        // inverts M), so the game never minds, but PoseOf refused it ("its record is not laid out as expected"): the stars stayed as the
        // menu opened. Its own checks: the same as PoseOf's, a mirror allowed; and its quaternion as the game makes it for any matrix.

        // The quaternion as the game writes it: DirectXMath's XMQuaternionRotationMatrix (its SSE form: the row of 4 q q^T picked by the
        // signs of r22, r11 - r00 and r11 + r00, divided by its length; 0x142BE8DA2..0x142BE8ED5) of the matrix whose rows are axes 2, 0
        // and 1, then normalised. For a real turn it is TurnOf's (up to its sign); for a mirror it is still what the game wrote.
        glm::quat GameTurnOf(const glm::mat3& axes)
        {
            const glm::vec3 rows[3]{ axes[2], axes[0], axes[1] };
            const auto      m = [&](int i, int j) { return rows[i][j]; };
            const float     r00 = m(0, 0), r11 = m(1, 1), r22 = m(2, 2);
            float           q[4]{};
            if (r22 <= 0.0f) {
                if (r11 - r00 <= 0.0f) {
                    q[0] = 1.0f + r00 - r11 - r22, q[1] = m(0, 1) + m(1, 0), q[2] = m(0, 2) + m(2, 0), q[3] = m(1, 2) - m(2, 1);
                } else {
                    q[0] = m(0, 1) + m(1, 0), q[1] = 1.0f - r00 + r11 - r22, q[2] = m(1, 2) + m(2, 1), q[3] = m(2, 0) - m(0, 2);
                }
            } else if (r11 + r00 <= 0.0f) {
                q[0] = m(0, 2) + m(2, 0), q[1] = m(1, 2) + m(2, 1), q[2] = 1.0f - r00 - r11 + r22, q[3] = m(0, 1) - m(1, 0);
            } else {
                q[0] = m(1, 2) - m(2, 1), q[1] = m(2, 0) - m(0, 2), q[2] = m(0, 1) - m(1, 0), q[3] = 1.0f + r00 + r11 + r22;
            }
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(length > 1e-12f)) {
                return glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
            }
            return glm::quat{ q[3] / length, q[0] / length, q[1] / length, q[2] / length }; // (glm: w first)
        }

        // The stars' record's pose: as PoseOf, but a mirror image allowed (its determinant +1 or -1: `mirror` says which). `why` says which
        // check failed, with its numbers, for the log.
        bool StarsPoseOf(const Record& r, Pose& out, bool& mirror, std::string& why)
        {
            mirror = false;
            if (!Finite(r)) {
                why = "its numbers are not all finite";
                return false;
            }
            for (int a = 0; a < 3; ++a) {
                for (int i = 0; i < 3; ++i) {
                    out.axes[a][i] = r[kM + i * 4 + (2 - a)];
                }
            }
            out.place = glm::vec3{ r[0], r[1], r[2] };
            for (int a = 0; a < 3; ++a) {
                const float length = glm::length(out.axes[a]);
                const float dot    = glm::dot(out.axes[a], out.axes[(a + 1) % 3]);
                if (std::abs(length - 1.0f) > 1e-3f || std::abs(dot) > 1e-3f) {
                    why = fmt::format("its axis {} is {:.4f} long and {:.4f} off square to the next (a scale or a slant)", a, length, dot);
                    return false;
                }
            }
            const float det = glm::determinant(out.axes);
            if (std::abs(std::abs(det) - 1.0f) > 1e-3f) {
                why = fmt::format("its axes' determinant is {:.4f}", det);
                return false;
            }
            mirror      = det < 0.0f;
            float worst = 0.0f;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    worst = (std::max)(worst, std::abs(r[kN + i * 4 + j] - r[kM + j * 4 + i]));
                }
            }
            if (worst > 1e-4f) {
                why = fmt::format("its second matrix is {:.5f} off the first's transpose", worst);
                return false;
            }
            const float fits = std::abs(Dot(GameTurnOf(out.axes), r));
            if (fits <= 0.9999f) {
                // (A mirror's quaternion is never read by the renderer: noted only. A real turn's must fit, as for any camera.)
                why = fmt::format("its quaternion is {:.5f} off the game's for its axes{}", 1.0f - fits, mirror ? " (a mirror image: taken anyway)" : "");
                return mirror;
            }
            return true;
        }

        // A stars' record with this pose, as the game would write it (RecordOf, with the game's own quaternion: a mirror's too).
        Record StarsRecordOf(const Record& like, const Pose& pose)
        {
            Record    r = RecordOf(like, pose);
            glm::quat q = GameTurnOf(pose.axes);
            if (Dot(q, like) < 0.0f) {
                q = -q; // (the same turn: kept on the same side as before)
            }
            r[kTurn]     = q.x;
            r[kTurn + 1] = q.y;
            r[kTurn + 2] = q.z;
            r[kTurn + 3] = q.w;
            return r;
        }

        float AngleBetween(const glm::mat3& a, const glm::mat3& b)
        {
            const glm::mat3 d = glm::transpose(a) * b;
            return glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
        }

        // ---- Your head, frame by frame (the game's thread) ----

        // The head (and the eye drawn) in the camera's parent's terms, as the camera's local transform is made from it (to_havok_space of
        // the headset's pose times the eye's, tracking scale included): the camera's world turn is then the room's, times this, times
        // the camera's own first local turn ("original"), whatever the camera's parent does with the aim (the local undoes it).
        struct Head
        {
            uint32_t  frame{ 0 };
            bool      valid{ false };
            glm::mat4 head{ 1.0f };
            float     scale{ 1.0f }; // (the camera's parent's scale then)
        };
        // The world's camera's record as the game wrote it in a frame, with the head of that frame.
        struct Written
        {
            uint32_t  frame{ 0 };
            bool      valid{ false };
            Record    record{};
            glm::mat4 head{ 1.0f };
            float     scale{ 1.0f };
        };
        constexpr size_t            kHeads = 512; // (several seconds: the menu's camera is copied a moment before the world is kept drawn)
        std::mutex                  g_head_mtx{};
        std::array<Head, kHeads>    g_heads{};
        std::array<Written, kHeads> g_written{};
        glm::mat3                   g_original{ 1.0f }; // (the world camera's first local turn, as UpdateWorldCamera keeps it)
        bool                        g_offset_identity{ false };

        // The self-test: the world's camera predicted from your head's movement (Moved) against the game's own record, each frame the
        // head has turned enough over the last few frames (in play; and behind a menu, where the head's camera's parent stays still).
        // A check passes when the prediction's turn is off by at most 0.1 degree plus 3% of the head's turn, its place (when the head
        // moved 2 cm or more) by at most 3 mm plus 5% of the head's move, and the game's quaternion is the one made from its axes.
        constexpr float kTestTurn      = 2.0f;  // degrees the head must turn over kTestApart frames
        constexpr float kTestMove      = 0.02f; // metres the head must move for the place to be checked too
        constexpr int   kTestApart     = 8;     // frames
        constexpr int   kTestKept      = 32;    // checks kept
        constexpr auto  kTestRecent    = std::chrono::seconds(60);
        constexpr int   kTestNeeded    = 5;     // checks in the last minute...
        constexpr float kTestPassShare = 0.8f;  // ...of which this share passed
        struct Check
        {
            std::chrono::steady_clock::time_point at{};
            bool                                  passed{ false };
        };
        struct SelfTest
        {
            uint32_t                              checks{ 0 };
            uint32_t                              passed{ 0 };
            uint32_t                              place_checks{ 0 };
            float                                 last_turn{ 0.0f };  // the prediction's error, degrees
            float                                 last_moved{ 0.0f }; // ...metres
            float                                 last_head_turn{ 0.0f };
            float                                 last_head_move{ 0.0f };
            std::array<Check, kTestKept>          recent{};
            int                                   next{ 0 };
            std::chrono::steady_clock::time_point logged_at{};
            int                                   logs{ 0 };
        };
        SelfTest g_test{};

        // The self-test's verdict now: enough checks in the last minute, and most of them passed.
        bool Passing(const SelfTest& t, int* checks = nullptr, int* passed = nullptr)
        {
            const auto now = std::chrono::steady_clock::now();
            int        n = 0, p = 0;
            for (const auto& c : t.recent) {
                if (c.at != std::chrono::steady_clock::time_point{} && now - c.at < kTestRecent) {
                    ++n;
                    p += c.passed ? 1 : 0;
                }
            }
            if (checks != nullptr) {
                *checks = n;
            }
            if (passed != nullptr) {
                *passed = p;
            }
            return n >= kTestNeeded && static_cast<float>(p) >= kTestPassShare * static_cast<float>(n);
        }

        // A pose moved as the head moved from "from" to "to" (both Head::head): its turn by the head's turn, seen in the camera's own
        // axes, its place by the head's move, turned into the world.
        Pose Moved(const Pose& base, const glm::mat4& from, const glm::mat4& to, const glm::mat3& original, float scale)
        {
            const glm::mat3 from_turn{ from };
            const glm::mat3 to_turn{ to };
            const glm::vec3 from_place{ from[3] };
            const glm::vec3 to_place{ to[3] };
            const glm::mat3 back = glm::inverse(original) * glm::transpose(from_turn); // (the room, seen from the camera at "from")
            Pose out{};
            out.axes  = base.axes * back * to_turn * original;
            out.place = base.place + scale * (base.axes * back * (to_place - from_place));
            return out;
        }

        bool HeadAt(uint32_t frame, glm::mat4& out)
        {
            const auto& h = g_heads[frame % kHeads];
            if (!h.valid || h.frame != frame) {
                return false;
            }
            out = h.head;
            return true;
        }

        // (Under g_head_mtx.) With the newest of the world camera's records and one a few frames older: does the camera move as the head
        // did? (The player not walking or turning meanwhile.)
        void TestPrediction(uint32_t newest)
        {
            const auto& b = g_written[newest % kHeads];
            const auto& a = g_written[(newest - kTestApart) % kHeads];
            if (!b.valid || !a.valid || b.frame != newest || a.frame != newest - kTestApart || !g_offset_identity) {
                return;
            }
            const float head_turn = AngleBetween(glm::mat3{ a.head }, glm::mat3{ b.head });
            if (head_turn < kTestTurn) {
                return; // (too little head movement to say anything)
            }
            Pose pa{}, pb{};
            if (!PoseOf(a.record, pa) || !PoseOf(b.record, pb)) {
                return;
            }
            const Pose  predicted = Moved(pa, a.head, b.head, g_original, a.scale);
            const float head_move = a.scale * glm::length(glm::vec3{ b.head[3] } - glm::vec3{ a.head[3] });
            auto&       t         = g_test;
            ++t.checks;
            t.last_turn      = AngleBetween(predicted.axes, pb.axes);
            t.last_moved     = glm::length(predicted.place - pb.place);
            t.last_head_turn = head_turn;
            t.last_head_move = head_move;
            const bool place_checked = head_move >= kTestMove;
            t.place_checks += place_checked ? 1 : 0;
            const bool passed = t.last_turn <= 0.1f + 0.03f * head_turn && (!place_checked || t.last_moved <= 0.003f + 0.05f * head_move) &&
                                TurnFits(b.record, pb);
            t.passed += passed ? 1 : 0;
            t.recent[t.next] = Check{ std::chrono::steady_clock::now(), passed };
            t.next           = (t.next + 1) % kTestKept;
            const auto now   = std::chrono::steady_clock::now();
            if (t.logs < 20 && (t.logs == 0 || now - t.logged_at >= std::chrono::seconds(30))) {
                ++t.logs;
                t.logged_at = now;
                int        n = 0, p = 0;
                const bool ok = Passing(t, &n, &p);
                spdlog::info("[MenuCamera] self-test: the world's camera predicted from your head's movement: {} of {} checks passed ({} of {} in the "
                             "last minute: {}); this time the head turned {:.1f} deg and moved {:.3f} m, the prediction was off by {:.2f} deg and {:.4f} m "
                             "({} checks of the place so far), the game's quaternion {} the one made from its axes",
                    t.passed, t.checks, p, n, ok ? "passing" : "not passing", head_turn, head_move, t.last_turn, t.last_moved, t.place_checks,
                    TurnFits(b.record, pb) ? "is" : "is NOT");
            }
        }

        // ---- Behind the menu (the renderer's threads) ----

        // (Under g_mtx: the views' setups run once a frame each, on the renderer's threads.)
        std::mutex g_mtx{};

        // This menu: the world's camera as the game left it (its record when the menu opened), and the head it was drawn for.
        struct Frozen
        {
            bool      ready{ false };
            bool      decided{ false };
            int       tries{ 0 };
            uint32_t  camera{ kNoHandle };
            Record    record{};
            Pose      pose{};
            glm::mat4 head{ 1.0f };
            float     scale{ 1.0f };
            // The stars' camera as the menu opened (its record, as camera [2]'s was taken: turned from it as camera [2] is, below).
            bool      stars{ false };
            uint32_t  stars_handle{ kNoHandle }; // (its whole handle: found again each frame, the same one)
            Record    stars_record{};
            Pose      stars_pose{};
            Record    stars_last{};              // (the last record written: written again on a frame camera [2] is not moved)
            bool      stars_written{ false };
            // (fix142) The world drawn with the head's camera itself (on a planet): the game keeps turning that camera with your head
            // behind the menu, but no longer places the stars' camera from it (its 'Starfield Camera' frustum sets stop: fix141's log,
            // 03:28:41-03:30:12), so the stars stayed as the menu opened, turning with your view. Then only the stars are turned, as
            // the head's camera has turned since the last frame of play ("pose": its record then).
            bool      head_drawn{ false };
        };
        constexpr int kTries = 60; // (frames the menu's camera is looked at before it is given up on)
        Frozen g_frozen{};
        bool   g_tested{ false }; // (this menu: the self-test has passed; it stays so until the menu closes)
        bool   g_world_logged{ false };
        bool   g_cut_logged{ false }; // (this menu: the frozen camera's cut flag logged once)
        bool    g_cut_ours{ false };   // (this menu: the flag cleared here; its value then)
        uint8_t g_cut_original{ 0 };
        uint32_t g_cut_camera{ 0xffffff };
        int     g_cut_logs{ 0 };
        // The renderer's record of the world's camera behind the menu (the one moved here: its view-constant setups watched, for the
        // log), and what those setups did in the last 10 s.
        std::atomic<uintptr_t> g_world_record{ 0 };
        struct ConstantCounts
        {
            uint32_t setups{ 0 };
            uint32_t copied{ 0 };      // its last frame's camera kept as its previous (as each frame in play)
            uint32_t game_resets{ 0 }; // its history thrown away by the game
            uint32_t mod_resets{ 0 };  // ...by the mod (as the world starts being drawn behind the menu)
            uint32_t still{ 0 };       // afterwards its previous camera is its current one (no camera motion seen)
        };
        std::mutex                            g_constants_mtx{};
        ConstantCounts                        g_constants{};
        std::chrono::steady_clock::time_point g_constants_start{};
        int                                   g_constants_logs{ 0 };
        // This menu's views as first set up, for the log (each view's camera, and whether the renderer's copy of it was the game's).
        constexpr int kViewsNoted = 8;
        uint32_t      g_views_noted[kViewsNoted]{};
        int           g_views_noted_count{ 0 };
        struct Counts
        {
            uint32_t setups{ 0 };      // setups of the world's view
            uint32_t moved{ 0 };       // its camera moved as your head moved since the menu opened
            uint32_t same{ 0 };        // ...already there
            uint32_t no_head{ 0 };     // not done: this frame's head not known (stereo)
            uint32_t not_ready{ 0 };   // not done: the menu's camera not understood (see the menu's first lines)
            uint32_t not_tested{ 0 };  // not done: the self-test not passed (yet)
            uint32_t unreadable{ 0 };  // not done: the tables not readable
            uint32_t too_far{ 0 };     // not done: the result far from where the menu opened (not trusted)
            uint32_t cut_cleared{ 0 }; // its cut flag found set (its histories thrown away each frame) and cleared
            uint32_t cut_restored{ 0 }; // ...put back as the game left it (its histories not kept as its own)
            uint32_t cut_unknown{ 0 }; // ...not readable
            uint32_t stars_moved{ 0 };   // the stars' camera turned with your head too
            uint32_t stars_unknown{ 0 }; // ...not done: its record not readable
            float    max_turn{ 0.0f }; // how far it was turned and moved from where the menu opened (degrees, metres)
            float    max_moved{ 0.0f };
        };
        Counts                                g_window{}; // the last 2 s
        Counts                                g_total{};  // this menu
        std::chrono::steady_clock::time_point g_window_start{};
        int                                   g_window_logs{ 0 };
        int                                   g_sessions_logged{ 0 };
        int                                   g_play_logs{ 0 };
        std::chrono::steady_clock::time_point g_play_logged_at{};
        // The last frame (in play) the world's view was drawn with the head's camera: as the next menu opens, the game copies that camera.
        bool                                  g_play_seen{ false };
        uint32_t                              g_play_frame{ 0 };
        constexpr uint32_t                    kPlayRecentFrames = 90;
        std::string                           g_status{ "not installed yet" };
        std::string                           g_last{};

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_mtx };
            g_status = std::move(text);
        }

        std::string Describe(const Counts& c)
        {
            return fmt::format("the world's camera moved with your head {} times (up to {:.1f} deg and {:.3f} m from where the menu opened; already "
                               "there {}); not done: the self-test not passed (yet) {}, this frame's head not known {}, the menu's camera not "
                               "understood {}, tables not readable {}, too far to trust {}; setups of the world's view {}; its cut flag "
                               "cleared {}, put back {} (not readable {}); the stars' camera turned {} (not readable {})",
                c.moved, c.max_turn, c.max_moved, c.same, c.not_tested, c.no_head, c.not_ready, c.unreadable, c.too_far, c.setups, c.cut_cleared, c.cut_restored,
                c.cut_unknown, c.stars_moved, c.stars_unknown);
        }

        void Add(Counts& to, const Counts& from)
        {
            to.setups += from.setups;
            to.moved += from.moved;
            to.same += from.same;
            to.no_head += from.no_head;
            to.not_ready += from.not_ready;
            to.not_tested += from.not_tested;
            to.unreadable += from.unreadable;
            to.too_far += from.too_far;
            to.cut_cleared += from.cut_cleared;
            to.cut_restored += from.cut_restored;
            to.cut_unknown += from.cut_unknown;
            to.stars_moved += from.stars_moved;
            to.stars_unknown += from.stars_unknown;
            to.max_turn  = std::max(to.max_turn, from.max_turn);
            to.max_moved = std::max(to.max_moved, from.max_moved);
        }

        bool LogSession()
        {
            return g_sessions_logged < 40;
        }

        std::string TestText(const SelfTest& t)
        {
            int        n = 0, p = 0;
            const bool ok = Passing(t, &n, &p);
            return fmt::format("{} of {} checks passed, {} of {} in the last minute ({}; the last: off by {:.2f} deg and {:.4f} m for a head turn of {:.1f} "
                               "deg and move of {:.3f} m)",
                t.passed, t.checks, p, n, ok ? "passing" : "not passing", t.last_turn, t.last_moved, t.last_head_turn, t.last_head_move);
        }

        // (Under g_mtx.) The menu closed (or the world is no longer kept drawn behind it): its counts for the log.
        void RestoreCut(Counts& c);

        void EndSession()
        {
            RestoreCut(g_window); // (the menu's world no longer drawn: the frozen camera's flag as the game left it)
            Add(g_total, g_window);
            g_last = g_world_logged ? Describe(g_total) : std::string{ "the world's view was not set up" };
            if (LogSession()) {
                spdlog::info("[MenuCamera] the menu closed: {}", g_last);
            }
            ++g_sessions_logged;
            g_window    = Counts{};
            g_total     = Counts{};
            g_play_seen = false; // (the next menu's from play after this one)
            g_world_record.store(0, std::memory_order_release);
            g_session.store(false, std::memory_order_release);
        }

        // (Under g_mtx.) The first view set up behind this menu.
        void BeginSession(uint32_t world_view)
        {
            g_session.store(true, std::memory_order_release);
            g_window            = Counts{};
            g_total             = Counts{};
            g_window_start      = std::chrono::steady_clock::now();
            g_views_noted_count = 0;
            g_world_logged      = false;
            g_cut_logged        = false;
            g_cut_ours          = false;
            g_cut_original      = 0;
            g_frozen            = Frozen{};
            SelfTest t{};
            {
                std::scoped_lock _{ g_head_mtx };
                t = g_test;
            }
            g_tested = Passing(t);
            if (!LogSession()) {
                return;
            }
            auto* root = CreationEngineSingletonManager::GetSceneGraphRoot();
            auto& vr   = VR::get();
            spdlog::info("[MenuCamera] behind the menu: the world's view [{}]; cameras: the head's [{}], the scene's full-screen one [{}]; self-test: {}; "
                         "frames: game {}, renderer {}, {}",
                world_view, HeadCamera(), root != nullptr ? HandleOf(root->pFullscreenCamera) : kNoHandle, TestText(t), vr->m_engine_frame_count.load(),
                vr->m_render_frame_count.load(), vr->is_mono() ? "mono" : "stereo");
        }

        // (Under g_mtx.) Each of this menu's views the first time it is set up: its camera, and whether the renderer's copy of that camera
        // is the game's.
        void NoteView(uint32_t view, bool readable, const Slots& slots, uint32_t head, uint32_t world_view)
        {
            for (int i = 0; i < g_views_noted_count; ++i) {
                if (g_views_noted[i] == view) {
                    return;
                }
            }
            if (g_views_noted_count >= kViewsNoted) {
                return;
            }
            g_views_noted[g_views_noted_count++] = view;
            if (!LogSession()) {
                return;
            }
            if (!readable) {
                spdlog::info("[MenuCamera]   view [{}]: its camera not readable from the tables", view);
                return;
            }
            spdlog::info("[MenuCamera]   view [{}]{}: drawn with camera [{}]{}; the renderer's copy {} the game's; thread {}", view,
                view == world_view ? " (the world's)" : "", slots.camera, slots.camera == head ? " (the head's)" : "",
                slots.renderer == slots.game ? "is" : "is not", GetCurrentThreadId());
        }

        // A turn as an angle (degrees) and the axis it is about.
        std::string TurnText(const glm::mat3& turn)
        {
            const glm::quat q    = glm::normalize(glm::quat_cast(turn));
            const float     half = std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f));
            glm::vec3       axis{ q.x, q.y, q.z };
            if (q.w < 0.0f) {
                axis = -axis;
            }
            if (glm::length(axis) > 1e-6f) {
                axis = glm::normalize(axis);
            }
            return fmt::format("{:.1f} deg about ({:.2f} {:.2f} {:.2f})", glm::degrees(2.0f * half), axis.x, axis.y, axis.z);
        }

        std::string PoseText(const Pose& p)
        {
            return fmt::format("at ({:.2f} {:.2f} {:.2f}), axes ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f})", p.place.x, p.place.y,
                p.place.z, p.axes[0].x, p.axes[0].y, p.axes[0].z, p.axes[1].x, p.axes[1].y, p.axes[1].z, p.axes[2].x, p.axes[2].y, p.axes[2].z);
        }

        // (Under g_mtx.) The world's view's setups behind this menu until decided: its camera, the game's frozen copy of the head's camera
        // made as the menu opened (fix118c's log: kept in another frame of reference, turned 178 degrees and 9.5 m from every record of
        // the head's camera, yet drawing the view from before the menu), and the head as the menu opened (the last frame the world's view
        // was drawn with the head's camera). Moved along its own axes as the head moves, the frame of reference it is kept in does not
        // matter. (Looked at again for a while: the game may still draw the world with the head's camera itself for a moment as the menu
        // opens.)
        // (Under g_mtx.) As the menu's camera is understood: the stars' camera of the world view's scene, as it is then (its renderer's
        // record), taken to be turned as camera [2] is; logged with the turn between the two (their frames of reference). Never the
        // world's camera itself, nor the head's.
        void SnapStars(const Slots& world, uint32_t head)
        {
            auto& f = g_frozen;
            f.stars         = false;
            f.stars_written = false;
            uint32_t    stars = kNoHandle;
            Slots       s{};
            Record      r{};
            Pose        p{};
            std::string why{};
            std::string note{}; // (fix169: a mirror's quaternion off the game's: taken, noted)
            bool        mirror = false;
            if (g_scenes == 0) {
                why = "its tables were not found";
            } else if (!StarsCameraOf(world.scene, stars)) {
                why = fmt::format("the scene {:x} has none (indoors, or no stars drawn)", world.scene);
            } else if ((stars & kNoHandle) == world.camera || (stars & kNoHandle) == head) {
                why = fmt::format("it is camera [{}], the world's or the head's", stars & kNoHandle);
            } else if (!ReadCameraSlots(stars & kNoHandle, s)) {
                why = "its slots in the cameras' table are not readable"; // (fix169: each check told apart)
            } else if (!ReadRecord(s.renderer_at, r)) {
                why = "its record is not readable";
            } else if (std::string bad{}; !StarsPoseOf(r, p, mirror, bad)) {
                why = fmt::format("its record is not laid out as expected: {}", bad);
            } else {
                note           = bad;
                f.stars        = true;
                f.stars_handle = stars;
                f.stars_record = r;
                f.stars_pose   = p;
            }
            if (LogSession()) {
                if (f.stars) {
                    // (A mirror's turn between the two is not a turn: its own axes' turn from camera [2]'s left out of the log then.)
                    spdlog::info("[MenuCamera]   the stars' camera [{}] {} (its renderer's slot {}, the game's {}){}{}; from camera [{}]'s axes to its: {} "
                                 "in the world, {} in camera [{}]'s own axes: turned with your head as camera [{}] is",
                        stars & kNoHandle, PoseText(p), s.renderer, s.game, mirror ? "; (fix169) a mirror image (determinant -1), as the game makes it on "
                                                                                     "much of a planet"
                                                                                   : "",
                        note.empty() ? "" : "; " + note, world.camera,
                        mirror ? std::string{ "(a mirror)" } : TurnText(p.axes * glm::transpose(f.pose.axes)),
                        mirror ? std::string{ "(a mirror)" } : TurnText(glm::transpose(f.pose.axes) * p.axes), world.camera, world.camera);
                } else {
                    spdlog::info("[MenuCamera]   the stars' camera: not turned with your head ({})", why);
                }
            }
        }

        // (Under g_mtx.) The stars' camera, found again this frame (the same one as the menu opened: its whole handle), its record written:
        // turned as camera [2] has just been (to `target` from its pose as the menu opened: the same turn in its own axes, its axes as
        // the menu opened times camera [2]'s turn in camera [2]'s own axes; its place kept), or, on a frame camera [2] is not moved
        // (target null), the last one written (camera [2]'s slot keeps its last too). As the game writes a camera's record, into the
        // renderer's copy, before the renderer reads it (right after this view's setup).
        void MoveStars(const Slots& world, const Pose* target, Counts& c)
        {
            auto& f = g_frozen;
            if (!f.stars || (target == nullptr && !f.stars_written)) {
                return;
            }
            uint32_t handle = kNoHandle;
            Slots    s{};
            if (!StarsCameraOf(world.scene, handle) || handle != f.stars_handle || !ReadCameraSlots(handle & kNoHandle, s)) {
                ++c.stars_unknown;
                return;
            }
            Record record = f.stars_last;
            if (target != nullptr) {
                Pose turned{};
                // (The stars' axes are a fixed matrix times camera [2]'s while the game is paused (on a planet the sky's frame, perhaps a
                // mirror, on the left in these axes): turned as camera [2] has turned, a mirror stays one; fix169.)
                turned.axes  = f.stars_pose.axes * (glm::transpose(f.pose.axes) * target->axes);
                turned.place = f.stars_pose.place;
                record       = StarsRecordOf(f.stars_record, turned);
            }
            Record current{};
            if (!Finite(record) || !ReadRecord(s.renderer_at, current)) {
                ++c.stars_unknown;
                return;
            }
            if (std::memcmp(current.data(), record.data(), kTransformSize) != 0 &&
                !CopyGuarded(reinterpret_cast<void*>(s.renderer_at), record.data(), kTransformSize)) {
                ++c.stars_unknown;
                return;
            }
            f.stars_last    = record;
            f.stars_written = true;
            ++c.stars_moved;
        }

        void Understand(const Slots& world, uint32_t head)
        {
            auto& f = g_frozen;
            ++f.tries;
            const bool last_try = f.tries >= kTries;
            if (world.camera == head) {
                // (fix142) The head's camera's record from the last frame of play (the stars' camera was last placed from it then); else
                // its record now (a few frames later: off by what the head turned meanwhile).
                Record      reference{};
                Pose        pose{};
                bool        from_play = false;
                std::string why{};
                {
                    auto& vr = VR::get();
                    std::scoped_lock _{ g_head_mtx };
                    if (g_play_seen && vr->m_render_frame_count.load() - g_play_frame <= kPlayRecentFrames) {
                        for (const uint32_t frame : { g_play_frame, g_play_frame + 1, g_play_frame - 1 }) {
                            const auto& w = g_written[frame % kHeads];
                            if (w.valid && w.frame == frame) {
                                reference = w.record;
                                from_play = true;
                                break;
                            }
                        }
                    }
                }
                if (!from_play && !ReadRecord(world.renderer_at, reference)) {
                    why = "its record is not readable";
                } else if (!PoseOf(reference, pose) || !TurnFits(reference, pose)) {
                    why = "its record is not laid out as expected";
                }
                if (why.empty()) {
                    f.ready      = true;
                    f.decided    = true;
                    f.head_drawn = true;
                    f.camera     = world.camera;
                    f.record     = reference;
                    f.pose       = pose;
                    if (LogSession()) {
                        spdlog::info("[MenuCamera] behind the menu: the world's view is drawn with the head's camera [{}] itself (the game turns it "
                                     "with your head): the stars' camera turned as it turns, from its record {}",
                            head, from_play ? "in the last frame of play" : "now (no frame of play kept)");
                    }
                    SnapStars(world, head);
                    return;
                }
                if (last_try) {
                    f.decided = true;
                    if (LogSession()) {
                        spdlog::info("[MenuCamera] behind the menu: the world's view is drawn with the head's camera [{}] itself: left as it is (the "
                                     "stars not turned: {})",
                            head, why);
                    }
                }
                return;
            }
            std::string why{};
            Pose        pose{};
            Record      record{};
            if (!ReadRecord(world.renderer_at, record) || !PoseOf(record, pose)) {
                why = "its record is not laid out as expected";
            } else if (!TurnFits(record, pose)) {
                why = "its quaternion is not the one made from its axes";
            } else {
                auto&      vr        = VR::get();
                glm::mat4  open{ 1.0f };
                float      scale     = 1.0f;
                bool       have_head = false, from_play = false, have_play = false;
                uint32_t   open_frame = 0;
                Written    play{};
                {
                    std::scoped_lock _{ g_head_mtx };
                    // (Only a frame from just before: the world is kept drawn a few frames after the menu opened.)
                    if (g_play_seen && vr->m_render_frame_count.load() - g_play_frame <= kPlayRecentFrames) {
                        const auto& h = g_heads[g_play_frame % kHeads];
                        if (h.valid && h.frame == g_play_frame) {
                            open       = h.head;
                            scale      = h.scale;
                            open_frame = g_play_frame;
                            have_head = from_play = true;
                        }
                        const auto& w = g_written[g_play_frame % kHeads];
                        if (w.valid && w.frame == g_play_frame) {
                            play      = w;
                            have_play = true;
                        }
                    }
                    if (!have_head) {
                        for (const uint32_t frame : { vr->m_render_frame_count.load(), vr->m_engine_frame_count.load() }) {
                            const auto& h = g_heads[frame % kHeads];
                            if (h.valid && h.frame == frame) {
                                open       = h.head;
                                scale      = h.scale;
                                open_frame = frame;
                                have_head  = true;
                                break;
                            }
                        }
                    }
                }
                if (!have_head) {
                    why = "no head known for the moment the menu opened";
                } else {
                    f.ready   = true;
                    f.decided = true;
                    f.camera  = world.camera;
                    f.record  = record;
                    f.pose    = pose;
                    f.head    = open;
                    f.scale   = scale;
                    if (LogSession()) {
                        spdlog::info("[MenuCamera] behind the menu: the world's view is drawn with camera [{}]; it is moved along its own axes as your head "
                                     "moves from frame {} ({}){}",
                            world.camera, open_frame, from_play ? "the last frame drawn with the head's camera" : "now: no frame of play kept",
                            g_tested ? "" : ", once the self-test passes");
                        Pose p1{};
                        if (have_play && PoseOf(play.record, p1)) {
                            // (Its frame of reference against the head's: the turn taking the head's camera's axes to its own, as seen in the
                            // world and as seen in the camera's own axes, and the places.)
                            spdlog::info("[MenuCamera]   camera [{}] {}; the head's camera [{}] in frame {} {}; from the head's camera's axes to "
                                         "camera [{}]'s: {} in the world, {} in the camera's own axes",
                                world.camera, PoseText(pose), head, play.frame, PoseText(p1), world.camera,
                                TurnText(pose.axes * glm::transpose(p1.axes)), TurnText(glm::transpose(p1.axes) * pose.axes));
                        } else {
                            spdlog::info("[MenuCamera]   camera [{}] {}; no record of the head's camera from that frame", world.camera, PoseText(pose));
                        }
                    }
                    SnapStars(world, head);
                    return;
                }
            }
            if (last_try) {
                f.decided = true;
                if (LogSession()) {
                    spdlog::info("[MenuCamera] behind the menu: the world's view is drawn with camera [{}]; it is NOT moved: {}", world.camera, why);
                }
            }
        }

        // (Under g_mtx.) The frozen camera's cut flag, as the renderer has it: cleared once the camera is moved with the head, while the
        // world's histories are being kept as its own (ViewHistory engaged: it moves smoothly then, and its histories are its own from one
        // frame to the next); put back as the game left it when they no longer are (at once, at the world view's next setup, or as the
        // menu's session ends: else the world would show what the menu's views left in them instead of starting them over). Read just
        // before this point in the frame, so from the next frame on; the game does not write it again behind the menu.
        bool ReadCut(uint32_t camera, Slots& motion, uint8_t& cut, uint32_t& frame)
        {
            return ReadColumnSlots(kMotionColumn, kMotionSize, camera, motion) &&
                   CopyGuarded(&cut, reinterpret_cast<const void*>(motion.renderer_at + kMotionCut), 1) && ReadU32(motion.renderer_at + kMotionFrame, frame);
        }

        void LogCut(uint32_t camera, uint8_t value)
        {
            if (g_cut_logs < 12 && LogSession()) {
                ++g_cut_logs;
                spdlog::info("[MenuCamera] behind the menu: camera [{}]'s cut flag {}", camera,
                    value == 0 ? "cleared (the world's histories kept as its own)" : "put back as the game left it (the world's histories not kept)");
            }
        }

        // (Under g_mtx.) The flag put back, if it was cleared here and is still clear (its slot found again).
        void RestoreCut(Counts& c)
        {
            if (!g_cut_ours) {
                return;
            }
            g_cut_ours = false;
            Slots    motion{};
            uint8_t  cut   = 0;
            uint32_t frame = 0;
            if (!ReadCut(g_cut_camera, motion, cut, frame)) {
                ++c.cut_unknown;
                return;
            }
            if (cut != 0) {
                return; // (set again since: by the game)
            }
            if (!CopyGuarded(reinterpret_cast<void*>(motion.renderer_at + kMotionCut), &g_cut_original, 1)) {
                ++c.cut_unknown;
                return;
            }
            ++c.cut_restored;
            LogCut(g_cut_camera, g_cut_original);
        }

        // (Under g_mtx.) The camera just moved with the head: its flag cleared while the world's histories are kept as its own.
        void ClearCut(uint32_t camera, Counts& c)
        {
            if (!ViewHistory::Engaged()) {
                RestoreCut(c);
                return;
            }
            Slots    motion{};
            uint8_t  cut   = 0;
            uint32_t frame = 0;
            if (!ReadCut(camera, motion, cut, frame)) {
                ++c.cut_unknown;
                return;
            }
            if (!g_cut_logged && LogSession()) {
                g_cut_logged = true;
                spdlog::info("[MenuCamera] behind the menu: camera [{}]'s cut flag is {} (its last update in frame {}; its motion record's slots: "
                             "renderer {}, game {}){}",
                    camera, cut, frame, motion.renderer, motion.game,
                    cut != 0 ? ": the world's histories are thrown away each frame (cleared while they are kept as its own)" : "");
            }
            if (cut == 0) {
                return;
            }
            const uint8_t zero = 0;
            if (!CopyGuarded(reinterpret_cast<void*>(motion.renderer_at + kMotionCut), &zero, 1)) {
                ++c.cut_unknown;
                return;
            }
            // (Its first value kept: if the game sets it again, it is cleared again and counted.)
            if (!g_cut_ours) {
                g_cut_original = cut;
                g_cut_camera   = camera;
                g_cut_ours     = true;
            }
            ++c.cut_cleared;
            LogCut(camera, 0);
        }

        // (Under g_mtx.) Behind the menu, the world's view: its camera moved as your head has moved since the menu opened.
        void MoveFrozen(const Slots& world, uint32_t head)
        {
            auto& vr = VR::get();
            auto& c  = g_window;
            ++c.setups;
            g_world_logged = true;
            // (The world's histories no longer kept as its own: the frozen camera's cut flag put back, if it was cleared here.)
            if (g_cut_ours && !ViewHistory::Engaged()) {
                RestoreCut(c);
            }
            if (!g_frozen.decided) {
                Understand(world, head);
            }
            // (fix142: decided on the head's own camera, the world now drawn with another (the game may switch to its frozen copy a
            // moment after the menu opened): worked out again.)
            if (g_frozen.head_drawn && world.camera != g_frozen.camera) {
                if (LogSession()) {
                    spdlog::info("[MenuCamera] behind the menu: the world's view is now drawn with camera [{}], not the head's [{}]: looked at again",
                        world.camera, g_frozen.camera);
                }
                g_frozen = Frozen{};
                Understand(world, head);
            }
            const auto& f = g_frozen;
            if (!f.ready || world.camera != f.camera) {
                ++c.not_ready;
                return;
            }
            // (fix142) The head's camera itself (the game turns it): only the stars turned as it has, read from its record this frame.
            if (f.head_drawn) {
                Record current{};
                Pose   pose{};
                if (!ReadRecord(world.renderer_at, current) || !PoseOf(current, pose)) {
                    ++c.unreadable;
                    MoveStars(world, nullptr, c);
                    return;
                }
                c.max_turn = std::max(c.max_turn, AngleBetween(f.pose.axes, pose.axes));
                MoveStars(world, &pose, c);
                return;
            }
            // This frame's head: the frame being drawn (mono: the newest; the game waits for each frame to be shown before the next).
            // And the self-test passing (in play, or here: behind the menu the game keeps updating the head's camera, its parent still);
            // once it has in this menu, it stays so until the menu closes.
            glm::mat4 now{ 1.0f };
            glm::mat3 original{ 1.0f };
            bool      known = false;
            SelfTest  t{};
            {
                std::scoped_lock _{ g_head_mtx };
                if (!g_tested) {
                    t = g_test;
                }
                original = g_original;
                known    = HeadAt(vr->m_render_frame_count.load(), now);
                if (!known && vr->is_mono()) {
                    known = HeadAt(vr->m_engine_frame_count.load(), now);
                }
            }
            if (!g_tested) {
                if (!Passing(t)) {
                    ++c.not_tested;
                    return;
                }
                g_tested = true;
                if (LogSession()) {
                    spdlog::info("[MenuCamera] behind the menu: the self-test is passing ({}): the world's camera is moved with your head from now on",
                        TestText(t));
                }
            }
            // (On a frame camera [2] is not moved, the stars keep their last turn as it keeps its own.)
            if (!known) {
                ++c.no_head;
                MoveStars(world, nullptr, c);
                return;
            }
            const Pose  target = Moved(f.pose, f.head, now, original, f.scale);
            const float turn   = AngleBetween(f.pose.axes, target.axes);
            const float moved  = glm::length(target.place - f.pose.place);
            // (Written only within these, and only real numbers: never anything else into the game's camera.)
            if (!(turn <= 150.0f && moved <= 2.0f)) {
                ++c.too_far;
                MoveStars(world, nullptr, c);
                return;
            }
            const Record record = RecordOf(f.record, target);
            if (!Finite(record)) {
                ++c.too_far;
                MoveStars(world, nullptr, c);
                return;
            }
            Record current{};
            if (!ReadRecord(world.renderer_at, current)) {
                ++c.unreadable;
                MoveStars(world, nullptr, c);
                return;
            }
            g_world_record.store(world.renderer_at, std::memory_order_release);
            ClearCut(world.camera, c);
            MoveStars(world, &target, c);
            if (std::memcmp(current.data(), record.data(), kTransformSize) == 0) {
                ++c.same;
                return;
            }
            if (!CopyGuarded(reinterpret_cast<void*>(world.renderer_at), record.data(), kTransformSize)) {
                ++c.unreadable;
                return;
            }
            ++c.moved;
            c.max_turn  = std::max(c.max_turn, turn);
            c.max_moved = std::max(c.max_moved, moved);
        }

        // (Under g_mtx.) In play, now and then: the camera the world's view is drawn with.
        void NotePlay(const Slots& world, uint32_t head)
        {
            if (world.camera == head) {
                g_play_seen  = true;
                g_play_frame = VR::get()->m_render_frame_count.load();
            }
            const auto now = std::chrono::steady_clock::now();
            if (g_play_logs < 12 && now - g_play_logged_at >= std::chrono::seconds(30)) {
                ++g_play_logs;
                g_play_logged_at = now;
                spdlog::info("[MenuCamera] in play: the world's view is drawn with camera [{}] (the head's [{}])", world.camera, head);
            }
        }

        // At the start of the renderer's setup of a view's camera (one of the renderer's threads; each view of the frame).
        void OnSetup(safetyhook::Context& ctx)
        {
            const bool behind = LiveWorld::Active();
            if (!behind && g_session.load(std::memory_order_acquire)) {
                std::scoped_lock _{ g_mtx };
                if (g_session.load(std::memory_order_relaxed)) {
                    EndSession();
                }
            }
            uint32_t view = 0;
            if (!ReadU32(ctx.rcx + kViewHandle, view)) {
                return;
            }
            view &= kNoHandle;
            uint32_t world_view = kNoHandle;
            if (view == kNoHandle || !LiveWorld::WorldViewHandle(world_view)) {
                return;
            }
            world_view &= kNoHandle;
            if (!behind && view != world_view) {
                return; // (in play only the world's view is watched)
            }
            // (Behind a menu only: the setting, as LiveWorld reads it on the renderer's thread too. In play it only looks.)
            auto& vr = VR::get();
            if (vr == nullptr) {
                return;
            }
            if (behind) {
                const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
                if (xr == nullptr || !xr->panel_world_turns.load()) {
                    // (The world's camera's record still known: in stereo its previous camera is kept per eye, the menu's views' not.)
                    if (Slots s{}; view == world_view && ReadSlots(view, s)) {
                        g_world_record.store(s.renderer_at, std::memory_order_release);
                    }
                    return;
                }
            }
            Slots          slots{};
            const bool     readable = ReadSlots(view, slots);
            // (Known as soon as it is read, whether or not the camera is moved this frame: IsWorldRecord.)
            if (behind && readable && view == world_view) {
                g_world_record.store(slots.renderer_at, std::memory_order_release);
            }
            const uint32_t head     = HeadCamera();

            std::scoped_lock _{ g_mtx };
            if (!behind) {
                if (readable && head != kNoHandle) {
                    NotePlay(slots, head);
                }
                return;
            }
            if (!g_session.load(std::memory_order_relaxed)) {
                // (Not just after the menu closed: another thread may have ended this menu's session a moment ago.)
                if (!LiveWorld::Active()) {
                    return;
                }
                BeginSession(world_view);
            }
            NoteView(view, readable, slots, head, world_view);
            if (view != world_view || !readable || head == kNoHandle) {
                return; // (the menu's 3D views keep their own cameras)
            }
            MoveFrozen(slots, head);
            const auto now = std::chrono::steady_clock::now();
            if (now - g_window_start >= std::chrono::seconds(2)) {
                if (g_window_logs < 300) {
                    ++g_window_logs;
                    spdlog::info("[MenuCamera] last 2 s behind the menu: {}", Describe(g_window));
                }
                Add(g_total, g_window);
                g_window       = Counts{};
                g_window_start = now;
            }
        }
    }

    void OnHeadPlaced(uint32_t frame, const glm::mat4& head, const glm::mat3& original, float parent_scale, const RE::NiCamera* camera)
    {
        if (!g_installed.load(std::memory_order_acquire) || camera == nullptr) {
            return;
        }
        // The world camera's record as the game wrote it in the frame before (its world update ran after this point then), with that
        // frame's head.
        const uint32_t handle = HandleOf(camera);
        Slots          slots{};
        Record         written{};
        const bool     have = handle != kNoHandle && ReadCameraSlots(handle, slots) && ReadRecord(slots.game_at, written) && Finite(written);
        float          offset[16]{};
        const bool     offset_read = CopyGuarded(offset, reinterpret_cast<const uint8_t*>(camera) + kOffsetMatrix, sizeof(offset));

        std::scoped_lock _{ g_head_mtx };
        g_original = original;
        if (offset_read) {
            // (The record is the world transform times this matrix; the moves here hold for the identity, with NiMath's node layout.)
            bool identity = true;
            for (int i = 0; i < 16; ++i) {
                identity = identity && std::abs(offset[i] - ((i % 5) == 0 ? 1.0f : 0.0f)) < 1e-5f;
            }
            const bool usable = identity && NiMath::g_rows_are_axes.load();
            if (usable != g_offset_identity) {
                spdlog::info("[MenuCamera] the world camera's offset matrix {} the identity, the node layout {}: the world behind menus {} moved with your "
                             "head",
                    identity ? "is" : "is NOT", NiMath::g_rows_are_axes.load() ? "rows are axes" : "columns are axes", usable ? "can be" : "is NOT");
            }
            g_offset_identity = usable;
        }
        const auto& before = g_heads[(frame - 1) % kHeads];
        if (have && before.valid && before.frame == frame - 1) {
            auto& w          = g_written[(frame - 1) % kHeads];
            w.frame          = frame - 1;
            w.valid          = true;
            w.record         = written;
            w.head           = before.head;
            w.scale          = before.scale;
            TestPrediction(frame - 1);
        }
        auto& h = g_heads[frame % kHeads];
        h.frame = frame;
        h.valid = true;
        h.head  = head;
        h.scale = parent_scale != 0.0f ? parent_scale : 1.0f;
    }

    void Install()
    {
        const auto setup = utility::scan(MemoryScan::mod, kSetupPattern);
        if (!setup) {
            spdlog::warn("[MenuCamera] the renderer's camera setup not found: the world behind menus is drawn from where you looked as the menu opened");
            SetStatus("World behind menus turning with your head: not available (the renderer's camera setup not found: another game version?)");
            return;
        }
        const auto views_at = utility::scan(*setup, kViewsSearch, kViewsPattern);
        const auto write_at = utility::scan(MemoryScan::mod, kWritePattern);
        if (!views_at || !write_at) {
            spdlog::warn("[MenuCamera] the camera tables not found as expected ({}{}): left as it is", views_at ? "" : "the camera views' table",
                write_at ? "" : (views_at ? "the camera's update" : ", the camera's update"));
            SetStatus("World behind menus turning with your head: not available (the camera tables not found)");
            return;
        }
        const uintptr_t cameras         = Relative(*setup + kSetupCameras);
        const uintptr_t cameras_written = Relative(*write_at + kWriteCameras);
        const uintptr_t views           = Relative(*views_at);
        if (!InGame(cameras) || !InGame(views) || cameras != cameras_written) {
            spdlog::warn("[MenuCamera] the camera tables are not where expected (the renderer's {:x}, the camera update's {:x}, the views' {:x}): left as it is",
                cameras, cameras_written, views);
            SetStatus("World behind menus turning with your head: not available (the camera tables not as expected)");
            return;
        }
        g_cameras    = cameras;
        g_views      = views;
        // (The stars' camera's tables: else the stars are left as the game has them.)
        if (const auto stars_at = utility::scan(MemoryScan::mod, kStarsPattern); stars_at) {
            const uintptr_t scenes = Relative(*stars_at + kStarsScenes);
            const uintptr_t stars  = Relative(*stars_at + kStarsData);
            if (InGame(scenes) && InGame(stars)) {
                g_scenes = scenes;
                g_stars  = stars;
            }
        }
        if (g_scenes == 0) {
            spdlog::warn("[MenuCamera] the stars' camera's tables not found: behind see-through menus the stars stay as they were as the menu opened");
        }
        g_setup_hook = safetyhook::create_mid(reinterpret_cast<void*>(*setup), &OnSetup);
        if (!g_setup_hook) {
            spdlog::warn("[MenuCamera] could not watch the renderer's camera setup: left as it is");
            SetStatus("World behind menus turning with your head: not available (could not watch the renderer's camera setup)");
            return;
        }
        g_installed.store(true, std::memory_order_release);
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        spdlog::info("[MenuCamera] the renderer's camera setup found and watched (rva {:x}; the cameras' table at rva {:x}, the camera views' at rva {:x}; "
                     "the scenes' at rva {:x}, the star data's at rva {:x}): the world behind see-through menus can turn with your head",
            *setup - base, cameras - base, views - base, g_scenes != 0 ? g_scenes - base : 0, g_stars != 0 ? g_stars - base : 0);
        SetStatus("World behind menus turning with your head: ready");
    }

    bool IsWorldRecord(uintptr_t record)
    {
        return record != 0 && record == g_world_record.load(std::memory_order_acquire);
    }

    void OnViewConstants(bool copied, bool game_reset, bool mod_reset, bool still)
    {
        std::scoped_lock _{ g_constants_mtx };
        const auto now = std::chrono::steady_clock::now();
        if (g_constants.setups == 0) {
            g_constants_start = now;
        }
        auto& c = g_constants;
        ++c.setups;
        c.copied += copied ? 1 : 0;
        c.game_resets += game_reset ? 1 : 0;
        c.mod_resets += mod_reset ? 1 : 0;
        c.still += still ? 1 : 0;
        if (now - g_constants_start >= std::chrono::seconds(10)) {
            if (g_constants_logs++ < 60) {
                spdlog::info("[MenuCamera] last 10 s, the world's camera's view constants behind the menu: {} setups; its last frame's camera kept "
                             "as its previous {}; its history thrown away by the game {}, by the mod {}; no camera motion seen afterwards {}",
                    c.setups, c.copied, c.game_resets, c.mod_resets, c.still);
            }
            c = ConstantCounts{};
        }
    }

    std::string Status()
    {
        std::scoped_lock _{ g_mtx };
        return g_last.empty() ? g_status : g_status + "; last menu: " + g_last;
    }
}
