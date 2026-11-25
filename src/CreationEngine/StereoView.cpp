#include "StereoView.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <malloc.h>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "CreationEngineSingletonManager.h"
#include "NiMath.h"
#include "body/BodyIK.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <CreationEngine/models/GameFlow.h>
#include <ModSettings.h>
#include <RE/M/Main.h>
#include <RE/N/NiCamera.h>
#include <mods/VR.hpp>
#include <nvidia/UpscalerAfrNvidiaModule.h>

namespace StereoView
{
    namespace
    {
        // Starfield 1.16.236 (rvas from the game's base), as the menus' 3D views are made (0x142552B20) and the frame's views listed
        // (0x140840A10); each function checked by its first bytes, each hooked place by its instruction.
        // (fix190) Each one's rva and bytes as constants of their own, checked by them through GameCode (the port tool reads them there).
        struct Code
        {
            uintptr_t   rva;
            const char* what;
        };
        constexpr uintptr_t kPoolInitRva     = 0x22C9D20;
        constexpr uint8_t   kPoolInitBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04 };
        constexpr Code      kPoolInit        = { kPoolInitRva, "the cameras' pool's start" };
        constexpr uintptr_t kAllocRva     = 0x22CA0C0;
        constexpr uint8_t   kAllocBytes[] = { 0x44, 0x88, 0x4C, 0x24, 0x20, 0x53, 0x55, 0x56 };
        constexpr Code      kAlloc        = { kAllocRva, "the pool's allocation" };
        constexpr uintptr_t kSmallAllocRva     = 0x22C9910;
        constexpr uint8_t   kSmallAllocBytes[] = { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41 };
        constexpr Code      kSmallAlloc        = { kSmallAllocRva, "the small allocation" };
        constexpr uintptr_t kCameraCtorRva     = 0x2BE6F30;
        constexpr uint8_t   kCameraCtorBytes[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83 };
        constexpr Code      kCameraCtor        = { kCameraCtorRva, "NiCamera's constructor" };
        constexpr uintptr_t kCameraRegRva     = 0x2BE71C0;
        constexpr uint8_t   kCameraRegBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };
        constexpr Code      kCameraReg        = { kCameraRegRva, "a camera's registration" };
        constexpr uintptr_t kCameraUpdateRva     = 0x2BE8AA0;
        constexpr uint8_t   kCameraUpdateBytes[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55 };
        constexpr Code      kCameraUpdate        = { kCameraUpdateRva, "NiCamera's world update" };
        constexpr uintptr_t kViewCreateRva     = 0x7CA610;
        constexpr uint8_t   kViewCreateBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 };
        constexpr Code      kViewCreate        = { kViewCreateRva, "a camera view's making" };
        constexpr uintptr_t kViewDataRva     = 0x841BA0;
        constexpr uint8_t   kViewDataBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
        constexpr Code      kViewData        = { kViewDataRva, "a view's data writer" };
        constexpr uintptr_t kFeaturesRva     = 0x248F480;
        constexpr uint8_t   kFeaturesBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
        constexpr Code      kFeatures        = { kFeaturesRva, "a view's features writer" };
        constexpr uintptr_t kImageSpaceRva     = 0x841E50;
        constexpr uint8_t   kImageSpaceBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
        constexpr Code      kImageSpace        = { kImageSpaceRva, "a view's image space writer" };
        constexpr uintptr_t kReadbacksRva     = 0x841A90;
        constexpr uint8_t   kReadbacksBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
        constexpr Code      kReadbacks        = { kReadbacksRva, "a view's light readings writer" };
        constexpr uintptr_t kShadowRva     = 0xD784A0;
        constexpr uint8_t   kShadowBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
        constexpr Code      kShadow        = { kShadowRva, "a view's shadow settings writer" };
        // mov rcx, [rax+0x120] (the viewports' lists), just before the frame's list is handed over (call 0x142918E40); r8 = the list.
        constexpr uintptr_t kListAtRva     = 0x840C4F;
        constexpr uint8_t   kListAtBytes[] = { 0x48, 0x8B, 0x88, 0x20, 0x01, 0x00, 0x00, 0xE8 };
        constexpr Code      kListAt        = { kListAtRva, "the frame's view list" };
        // jbe (one view or none: the upscaler as chosen), after cmp [rdx+rax], 1 (the viewport's list's count).
        constexpr uintptr_t kUpscaleAtRva     = 0x298CC36;
        constexpr uint8_t   kUpscaleAtBytes[] = { 0x0F, 0x86, 0x6C, 0x01, 0x00, 0x00, 0x83, 0x7B };
        constexpr Code      kUpscaleAt        = { kUpscaleAtRva, "the upscale's one-view check" };
        // (fix153) jne (more than one view: the whole DLSS block skipped), after cmp [rdx+rcx], 1 (the same count) in the view's scene setup
        // (SceneSetupRenderPass function 6, 0x142923D40): the only call that hands DLSS a view's camera, jitter and reset (0x142A735D0, its
        // slSetConstants at 0x142A73C76, and the options after it) is behind it. In full stereo it skipped both eyes every frame: DLSS ran on
        // the constants of the frame before full stereo (no new jitter, old matrices; fix151's counters: 0-3 a 30 s), and the laser got
        // no camera. Each view's own camera record and jitter are made before it, so let through as the upscale's check is.
        constexpr uintptr_t kConstantsAtRva     = 0x2925647;
        constexpr uint8_t   kConstantsAtBytes[] = { 0x0F, 0x85, 0x8D, 0x00, 0x00, 0x00, 0x41, 0x83 };
        constexpr Code      kConstantsAt        = { kConstantsAtRva, "the scene setup's DLSS one-view check" };
        // (fix136) The views' "last frame" pictures (the anti-aliasing's, AO's, refraction's, the volumetric light's... 12 slots of picture
        // pairs, read from one, written to the other, by the frame's stamp): one list for every view, made with the root graph
        // (0x142AA3270: 0x68 bytes, its constructor 0x142A66270, at its owner +0x18), each view's record pointing at it (+0x40) as its
        // parent's (RenderViewSubGraph's function 10, 0x142B7BE10 -> 0x142AA30A0, makes the view's record from its parent's). With both
        // eyes' views in it, the left eye read the right eye's last frame (the shimmer and smears of fix135's test). The right eye's view
        // gets a list of its own: the game fills it itself, at its own sizes; nothing copied.
        constexpr uintptr_t kRecordEntryRva = 0x4EE7AD0; // RenderViewSubGraph's type table, function 10
        constexpr uintptr_t kRecordFnRva     = 0x2B7BE10;
        constexpr uint8_t   kRecordFnBytes[] = { 0x48, 0x8B, 0xC2, 0x8B, 0x91, 0x28, 0x0D, 0x00 };
        constexpr Code      kRecordFn        = { kRecordFnRva, "a view's record maker" };
        constexpr uintptr_t kListCtorRva     = 0x2A66270;
        constexpr uint8_t   kListCtorBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74 };
        constexpr Code      kListCtor        = { kListCtorRva, "the history list's constructor" };
        constexpr uintptr_t kGraphView    = 0xD28; // (a RenderViewSubGraph: its view's handle)
        constexpr uintptr_t kRecordList   = 0x40;  // (a view's record: its history list)
        constexpr uintptr_t kListCut      = 0x60;  // (the list's camera-cut flag: the game clears only its own list's, 0x142AA8D6C)
        constexpr size_t    kListSlots    = 12;
        constexpr uintptr_t kPoolStateRva  = 0x5E68158;
        constexpr uintptr_t kPoolRva       = 0x5E68180;
        constexpr uintptr_t kReadbackTable = 0x4B4CCB0; // (the light reading's shared block's type table, as the menus make it)
        constexpr uintptr_t kViewsTableRva = 0x597D178; // the camera views' table
        constexpr size_t    kCameraUpdateSlot = 79;     // NiCamera's world update in its type table
        constexpr uintptr_t kUpdateStampRva   = 0x5E821E0; // (the update stamp the game gives its objects' updates)

        // The scene root (Main::SceneGraphRoot, [0x145FB8668]): the world's view (+0x3d0; +0x3d4 the space scene's, drawn instead when
        // +0x4e0 is set: aboard a ship in space, the same scene (the ship's inside in it) through the galaxy camera; fix135), the cameras
        // they are drawn with (+0x80 the world camera; +0xa8 the galaxy camera, unparented, its own lens and far plane: 0x1423448F0),
        // whether the world is drawn this frame (+0x4e1), its image space (+0x64).
        constexpr uintptr_t kWorldView   = 0x3d0;
        constexpr uintptr_t kSpaceView   = 0x3d4;
        constexpr uintptr_t kWorldCameraAt  = 0x80;
        constexpr uintptr_t kGalaxyCameraAt = 0xa8;
        constexpr uintptr_t kSpaceFlag   = 0x4e0;
        constexpr uintptr_t kWorldDrawn  = 0x4e1;
        constexpr uintptr_t kImageSpaceAt = 0x64;
        // The camera views' table's columns (the game's own copy of each: StorageTable::AsyncTableColumn, as MenuCamera reads them).
        constexpr uintptr_t kViewDataColumn   = 0x120; // 0x1c bytes: +0 the scene, +4 the camera, +8, +0xc fov, near, far, +0x18, +0x19
        constexpr uintptr_t kFeaturesColumn   = 0x108; // 0x210 bytes (its writer copies 4 x 0x80 + 0x10: 0x14248F515)
        constexpr uintptr_t kImageSpaceColumn = 0x110; // 4 bytes
        constexpr uintptr_t kReadbacksColumn  = 0xF8;  // two shared pointers to a light reading
        constexpr uintptr_t kShadowColumn     = 0xF0;  // 0x3c bytes: DirectionalShadowData (+0 the cascades' update mode: 0 time-sliced, 1, 2 all
                                                       // every frame; set for the space view as 2, 0x1408402D6; the world view's from settings)
        constexpr uintptr_t kGameSlots        = 0x2c8;
        constexpr uintptr_t kSlotData         = 0x3c8;
        constexpr size_t    kViewDataSize     = 0x1c;
        constexpr size_t    kFeaturesSize     = 0x210;
        constexpr size_t    kReadbacksSize    = 0x20;
        constexpr size_t    kShadowSize       = 0x3c;
        constexpr uint32_t  kNoHandle         = 0xffffff;
        constexpr uint32_t  kMaxSlot          = 1u << 22;
        // NiCamera: its handle (+0x130); the lens and the rest copied from the world's camera each frame (its offset matrix, +0x140;
        // its frustum to its level of detail, +0x1c0..+0x208; not its lock at +0x208).
        constexpr uintptr_t kCameraHandle = 0x130;
        constexpr uintptr_t kWorldTransform = 0x80; // (NiAVObject: its world transform, an NiTransform)
        constexpr uintptr_t kOffsetMatrix = 0x140;
        constexpr uintptr_t kLensStart    = 0x1c0;
        constexpr uintptr_t kLensEnd      = 0x208;
        constexpr uintptr_t kCameraCut    = 0x210; // (NiCamera: a cut forced at its next world update)

        using PoolInitFn   = void (*)(void* pool, void* state);
        using AllocFn      = void* (*)(void* pool, uint32_t size, uint32_t align, bool flag);
        using SmallAllocFn = void* (*)(uint64_t size);
        using CtorFn       = void* (*)(void* memory);
        using RegisterFn   = void (*)(void* camera);
        using CreateFn     = void (*)(uint32_t* handle);
        using WriteFn      = void (*)(const uint32_t* handle, const void* data);
        using UpdateFn     = void* (*)(void* object, void* update_data);
        using RecordFn     = void* (*)(void* graph, void* parent);
        using ListCtorFn   = void* (*)(void* list);

        uintptr_t             g_base{ 0 };
        bool                  g_installed{ false };
        safetyhook::MidHook   g_list_hook{};
        safetyhook::MidHook   g_upscale_hook{};
        safetyhook::MidHook   g_constants_hook{}; // (fix153)
        std::atomic<uint64_t> g_constants_let{ 0 };
        bool                  g_constants_logged{ false };
        std::atomic<bool>     g_failed{ false };
        std::atomic<void*>    g_camera{ nullptr };
        std::atomic<uint32_t> g_view{ kNoHandle };
        std::atomic<int64_t>  g_inserted_ms{ 0 };
        // (fix136) The right eye's own history list (null: not made, the views share the game's), the record maker's original.
        std::atomic<void*>    g_eye_list{ nullptr };
        uintptr_t             g_record_original{ 0 };
        bool                  g_list_tried{ false };
        std::atomic<uint64_t> g_own_records{ 0 };
        uint32_t              g_last_placed{ 0 };  // (the last frame the right eye's camera was placed for)
        std::atomic<uint64_t> g_inserted{ 0 }, g_skipped{ 0 }, g_upscale_let{ 0 };
        std::atomic<int>      g_skip_logs{ 0 };
        bool                  g_upscale_logged{ false };
        // The eyes' offset (right minus left) in the world camera's parent's space, and the frame it is of (the game's thread).
        std::mutex            g_delta_mtx{};
        glm::vec3             g_delta{ 0.0f };
        uint32_t              g_delta_frame{ 0 };
        bool                  g_delta_valid{ false };
        // Frames in a row a full stereo frame went without the right eye's view: full stereo let go for a while after many.
        std::atomic<int>      g_misses_in_row{ 0 };
        std::atomic<int64_t>  g_rest_until_ms{ 0 };
        int                   g_make_tries{ 0 };      // (the world's view's settings not readable yet: tried again, a while)
        int                   g_row_unread{ 0 };      // (frames in a row our view's row could not be read)
        std::atomic<bool>     g_row_ok{ false };      // (our view's row checked as ours this frame: only then added to the list)
        int                   g_not_possible_logs{ 0 };
        uint32_t              g_why_not_logged{ 0 };  // (each reason full stereo is not played, logged once)
        // The view's settings as last written (copies of the drawn view's: the world's or the space scene's): written again when
        // those change, or the view copied is the other one.
        std::array<uint8_t, kViewDataSize> g_written_data{};
        std::array<uint8_t, kFeaturesSize> g_written_features{};
        uint32_t                           g_written_space{ kNoHandle };
        std::array<uint8_t, kShadowSize>      g_written_shadow{};
        bool                               g_shadow_written{ false };
        std::atomic<uint32_t>              g_source_view{ kNoHandle }; // (the view copied last)
        int64_t                            g_source_changed_ms{ 0 };
        // (Each 30 s while it is wanted but not played: why, for the log.)
        int64_t               g_why_logged_ms{ 0 };
        // (In space, for the log: how much the galaxy camera turned against the world camera between frames, the worst in 10 s; the
        // two cameras' scales.)
        glm::mat3             g_last_relation{ 1.0f };
        bool                  g_relation_valid{ false };
        float                 g_relation_worst{ 0.0f };
        float                 g_scale_ratio{ 1.0f };
        // For the log: threads, and how the camera kept up.
        DWORD                 g_frame_thread{ 0 }, g_list_thread{ 0 };
        bool                  g_threads_logged{ false };
        std::chrono::steady_clock::time_point g_stats_at{};
        float                 g_worst_place{ 0.0f }; // (the world camera's place against its parent's and local's, the worst in 10 s)
        float                 g_eyes_apart{ 0.0f };
        RE::NiFrustum         g_right_lens{};  // (for the log: the right eye's frustum as last placed, fix138)
        RE::NiFrustum         g_source_lens{}; // (and the drawn camera's it was made from: the left lens, in space the galaxy camera's)
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        template <class T>
        T Fn(const Code& code)
        {
            const uintptr_t rva = code.rva; // (fix190) (each one checked at the install by its own constants)
            return reinterpret_cast<T>(At(rva));
        }

        bool ReadBytes(uintptr_t at, void* out, size_t size)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool WriteBytes(uintptr_t at, const void* in, size_t size)
        {
            __try {
                std::memcpy(reinterpret_cast<void*>(at), in, size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        uintptr_t Root()
        {
            auto* root = CreationEngineSingletonManager::GetSceneGraphRoot();
            return reinterpret_cast<uintptr_t>(root);
        }

        // The view the game draws the world with this frame, as the list builder picks it (0x140840A7B): the space scene's when +0x4e0
        // is set (aboard a ship in space), else the world's; when the world is drawn this frame.
        bool DrawnView(uint32_t& handle, bool& space)
        {
            const uintptr_t root = Root();
            uint8_t         in_space = 0, drawn = 0;
            if (root == 0 || !ReadBytes(root + kSpaceFlag, &in_space, 1) || !ReadBytes(root + kWorldDrawn, &drawn, 1) ||
                !ReadBytes(root + (in_space != 0 ? kSpaceView : kWorldView), &handle, 4)) {
                return false;
            }
            space = in_space != 0;
            return drawn != 0 && handle != kNoHandle;
        }

        // The camera that view is drawn with: the galaxy camera in space, else the world camera.
        RE::NiCamera* SourceCamera(bool space)
        {
            const uintptr_t root   = Root();
            uintptr_t       camera = 0;
            if (root == 0 || !ReadBytes(root + (space ? kGalaxyCameraAt : kWorldCameraAt), &camera, 8)) {
                return nullptr;
            }
            return reinterpret_cast<RE::NiCamera*>(camera);
        }

        // A view's data in the game's copy of one of the camera views' table's columns.
        // (or_default: a view with no row of its own in that column (slot 0) has the column's default record, items[0], as the game reads
        // it (0x140D6A7B6: no check of 0): that one.)
        bool ReadViewColumn(uint32_t handle, uintptr_t column_at, size_t size, void* out, bool or_default = false)
        {
            uintptr_t table = 0, column = 0, slots = 0, items = 0;
            if (!ReadBytes(At(kViewsTableRva), &table, 8) || table == 0 || !ReadBytes(table + column_at, &column, 8) || column == 0 ||
                !ReadBytes(column + kGameSlots, &slots, 8) || !ReadBytes(column + kSlotData, &items, 8) || slots == 0 || items == 0) {
                return false;
            }
            uint32_t slot = 0;
            if (!ReadBytes(slots + 4 * static_cast<uintptr_t>(handle & kNoHandle), &slot, 4) || (slot == 0 && !or_default) || slot >= kMaxSlot) {
                return false;
            }
            return ReadBytes(items + static_cast<uintptr_t>(slot) * size, out, size);
        }

        // The game's functions, each call guarded (an exception: full stereo off for the session).
        bool CallPoolInit()
        {
            __try {
                Fn<PoolInitFn>(kPoolInit)(reinterpret_cast<void*>(At(kPoolRva)), reinterpret_cast<void*>(At(kPoolStateRva)));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void* CallAlloc(uint32_t size)
        {
            __try {
                return Fn<AllocFn>(kAlloc)(reinterpret_cast<void*>(At(kPoolRva)), size, 0x10, true);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        void* CallSmallAlloc(uint64_t size)
        {
            __try {
                return Fn<SmallAllocFn>(kSmallAlloc)(size);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        void* CallCtor(void* memory)
        {
            __try {
                return Fn<CtorFn>(kCameraCtor)(memory);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        bool CallRegister(void* camera)
        {
            __try {
                Fn<RegisterFn>(kCameraReg)(camera);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallCreate(uint32_t* handle)
        {
            __try {
                Fn<CreateFn>(kViewCreate)(handle);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallWrite(const Code& writer, const uint32_t* handle, const void* data)
        {
            __try {
                Fn<WriteFn>(writer)(handle, data);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallUpdate(void* camera)
        {
            // (NiUpdateData: its stamp at +0x44 the one the game keeps for it, as NiCamera's constructor takes it, 0x142BE70CD; nothing else
            // asked of it.)
            alignas(16) uint8_t data[0x80]{};
            __try {
                std::memcpy(data + 0x44, reinterpret_cast<const void*>(At(kUpdateStampRva)), 4);
                const uintptr_t table = *reinterpret_cast<const uintptr_t*>(camera);
                const auto      fn    = *reinterpret_cast<const UpdateFn*>(table + kCameraUpdateSlot * sizeof(uintptr_t));
                fn(camera, data);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void Fail(const char* what)
        {
            g_failed = true;
            spdlog::error("[StereoView] {}: full stereo off until the game restarts", what);
            SetStatus(std::string{ "Full stereo: off (" } + what + ")");
        }

        // The right eye's camera and view, made once (the game's thread), as the menus make theirs: the camera from the cameras' pool,
        // registered; the view with the world's view's data (its scene, lens numbers, flags), its features and image space, and a
        // light reading of its own.
        void Make(uint32_t world, bool in_space)
        {
            auto* world_camera = SourceCamera(in_space); // (the drawn view's camera: the world camera, or the galaxy camera in space)
            if (world_camera == nullptr) {
                return;
            }
            std::array<uint8_t, kViewDataSize> data{};
            std::array<uint8_t, kFeaturesSize> features{};
            uint32_t                           space = kNoHandle;
            std::array<uintptr_t, 4>           world_readings{};
            if (!ReadViewColumn(world, kViewDataColumn, kViewDataSize, data.data()) ||
                !ReadViewColumn(world, kFeaturesColumn, kFeaturesSize, features.data()) ||
                !ReadViewColumn(world, kImageSpaceColumn, 4, &space)) {
                // (Not there yet, as a place loads: tried again, for about half a minute of frames.)
                if (++g_make_tries >= 2000) {
                    Fail("the drawn view's settings could not be read");
                }
                return;
            }
            // (Its light reading starts from the world's.)
            float reading = 1.0f;
            if (ReadViewColumn(world, kReadbacksColumn, kReadbacksSize, world_readings.data()) && world_readings[0] != 0) {
                float r = 0.0f;
                if (ReadBytes(world_readings[0], &r, 4) && std::isfinite(r) && r > 0.0f) {
                    reading = r;
                }
            }
            uint32_t pool_state = 0;
            if (!ReadBytes(At(kPoolStateRva), &pool_state, 4)) {
                Fail("the cameras' pool could not be read");
                return;
            }
            if (pool_state != 2 && !CallPoolInit()) {
                Fail("the cameras' pool could not be started");
                return;
            }
            void* memory = CallAlloc(0x220);
            void* camera = memory != nullptr ? CallCtor(memory) : nullptr;
            if (camera == nullptr) {
                Fail("the right eye's camera could not be made");
                return;
            }
            // (Its world update where NiCamera's is: called each frame through its type table.)
            uintptr_t table = 0, update = 0;
            if (!ReadBytes(reinterpret_cast<uintptr_t>(camera), &table, 8) || !ReadBytes(table + kCameraUpdateSlot * 8, &update, 8) ||
                update != At(kCameraUpdateRva)) {
                Fail("the right eye's camera is not as an NiCamera is expected to be");
                return;
            }
            // (Kept for the session: one reference of ours, never let go.)
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<uintptr_t>(camera) + 8));
            // (Its lens as the world camera's before it is registered: registering writes its viewport and frustum into the tables.)
            WriteBytes(reinterpret_cast<uintptr_t>(camera) + kOffsetMatrix, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(world_camera) + kOffsetMatrix), 64);
            WriteBytes(reinterpret_cast<uintptr_t>(camera) + kLensStart, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(world_camera) + kLensStart),
                kLensEnd - kLensStart);
            auto* cam    = static_cast<RE::NiCamera*>(camera);
            if (!ReadBytes(reinterpret_cast<uintptr_t>(world_camera) + kWorldTransform, &cam->local, sizeof(RE::NiTransform))) {
                Fail("the drawn view's camera could not be read");
                return;
            }
            if (!CallRegister(camera)) {
                Fail("the right eye's camera could not be registered");
                return;
            }
            uint32_t camera_handle = kNoHandle;
            if (!ReadBytes(reinterpret_cast<uintptr_t>(camera) + kCameraHandle, &camera_handle, 4) || camera_handle == kNoHandle) {
                Fail("the right eye's camera got no handle");
                return;
            }
            if (!CallUpdate(camera)) {
                Fail("the right eye's camera could not be updated");
                return;
            }
            uint32_t view = kNoHandle;
            if (!CallCreate(&view) || view == kNoHandle) {
                Fail("the right eye's view could not be made");
                return;
            }
            // Its light reading: a shared float of its own (the block the menus make: its type table, two counts, the value).
            auto* block = static_cast<uint8_t*>(CallSmallAlloc(0x18));
            if (block == nullptr) {
                Fail("the right eye's view's light reading could not be made");
                return;
            }
            const uintptr_t block_table = At(kReadbackTable);
            const uint32_t  one         = 1;
            std::memcpy(block, &block_table, 8);
            std::memcpy(block + 8, &one, 4);
            std::memcpy(block + 12, &one, 4);
            std::memcpy(block + 16, &reading, 4);
            // (Handed over as the menus do.)
            // (Moved into the table by its writer, which leaves these empty: the table holds the only reference.)
            std::array<uintptr_t, 4> readings{ reinterpret_cast<uintptr_t>(block + 16), reinterpret_cast<uintptr_t>(block), 0, 0 };
            std::memcpy(data.data() + 4, &camera_handle, 4);
            if (!CallWrite(kReadbacks, &view, readings.data()) || !CallWrite(kViewData, &view, data.data()) ||
                !CallWrite(kFeatures, &view, features.data()) || !CallWrite(kImageSpace, &view, &space)) {
                Fail("the right eye's view's settings could not be written");
                return;
            }
            // (Its shadow settings (DirectionalShadowData): the drawn view's, or the default record the game uses for a view with none;
            // the same as the drawn view's, so both eyes' shadows update on the same frames.)
            std::array<uint8_t, kShadowSize> shadow{};
            if (ReadViewColumn(world, kShadowColumn, kShadowSize, shadow.data(), true)) {
                if (CallWrite(kShadow, &view, shadow.data())) {
                    g_written_shadow = shadow;
                    g_shadow_written = true;
                }
            }
            g_source_view      = world;
            g_written_data     = data;
            g_written_features = features;
            g_written_space    = space;
            g_camera           = camera;
            g_view             = view;
            uint32_t scene = 0;
            std::memcpy(&scene, data.data(), 4);
            spdlog::info("[StereoView] the right eye's camera [{}] and view [{}] made (from the {} view [{}], its scene {:#x}, image space {:#x}, "
                         "light reading {:.3f})",
                camera_handle & kNoHandle, view & kNoHandle, in_space ? "space scene's" : "world's", world & kNoHandle, scene, space, reading);
            SetStatus("Full stereo: ready (both eyes drawn in each frame while it is on)");
        }

        // The drawn view's settings changed (the game sets its field of view, near and far, features, its shadows'), or the view copied is
        // the other one now (force: into space or out of it): the right eye's view given them too.
        void KeepAsWorld(uint32_t world, uint32_t view, bool force)
        {
            std::array<uint8_t, kViewDataSize> data{};
            std::array<uint8_t, kFeaturesSize> features{};
            std::array<uint8_t, kShadowSize>   shadow{};
            uint32_t                           space = kNoHandle;
            if (ReadViewColumn(world, kViewDataColumn, kViewDataSize, data.data())) {
                std::memcpy(data.data() + 4, g_written_data.data() + 4, 4); // (its own camera)
                if (force || data != g_written_data) {
                    if (CallWrite(kViewData, &view, data.data())) {
                        g_written_data = data;
                    } else {
                        Fail("the right eye's view's data could not be written");
                        return;
                    }
                }
            }
            if (ReadViewColumn(world, kFeaturesColumn, kFeaturesSize, features.data()) && (force || features != g_written_features)) {
                if (CallWrite(kFeatures, &view, features.data())) {
                    g_written_features = features;
                } else {
                    Fail("the right eye's view's features could not be written");
                    return;
                }
            }
            if (ReadViewColumn(world, kImageSpaceColumn, 4, &space) && (force || space != g_written_space)) {
                if (CallWrite(kImageSpace, &view, &space)) {
                    g_written_space = space;
                } else {
                    Fail("the right eye's view's image space could not be written");
                    return;
                }
            }
            // (Its shadow settings (DirectionalShadowData, 0x3c bytes: the space view has its own; the world's view may have none, then
            // the default record the game uses for it, else the right eye would keep the space view's after landing, fix135's review). Its
            // cascades' update mode as the drawn view's: both eyes' shadows updated on the same frames (fix136's review: the right eye's
            // time-sliced against the space view's all-every-frame would have left far shadows a frame or three behind in one eye).)
            if (ReadViewColumn(world, kShadowColumn, kShadowSize, shadow.data(), true)) {
                if (force || !g_shadow_written || shadow != g_written_shadow) {
                    if (CallWrite(kShadow, &view, shadow.data())) {
                        g_written_shadow = shadow;
                        g_shadow_written = true;
                    } else {
                        Fail("the right eye's view's shadow settings could not be written");
                    }
                }
            }
        }

        // The right eye's camera at the right eye, for the frame being listed: the drawn view's camera (the world camera, updated for
        // this frame: the left eye; in space the galaxy camera, which follows it) moved by the eyes' offset in that camera's own axes,
        // its lens that camera's; its records written by its own world update. (Both eyes from the one camera: a matched pair whatever
        // frame of reference the galaxy camera is in, and even were it a frame behind.)
        bool PlaceCamera(void* camera, uint32_t frame, bool space)
        {
            auto* world_camera  = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            auto* source_camera = SourceCamera(space);
            if (world_camera == nullptr || source_camera == nullptr) {
                return false;
            }
            glm::vec3 delta{ 0.0f };
            {
                std::scoped_lock _{ g_delta_mtx };
                // (This frame's offset only: the camera's update may not have run this frame.)
                if (!g_delta_valid || g_delta_frame != frame) {
                    return false;
                }
                delta = g_delta;
            }
            // The offset in the world (the world camera's parent's space turned and scaled into it), then in the world camera's own axes.
            glm::vec3 d = delta;
            if (world_camera->parent != nullptr) {
                const auto&     parent   = world_camera->parent->world;
                const glm::mat3 rotation = NiMath::to_glm(parent.rotate);
                d                        = rotation * delta * parent.scale;
                // (For the log: is the world camera's place this frame's, its parent's and its local's together? In the game's matrix
                // layout, as UpdateFirstPersonCamera composes them.)
                const glm::vec3 composed = NiMath::to_glm(parent.translate) + rotation * (NiMath::to_glm(world_camera->local.translate) * parent.scale);
                g_worst_place            = std::max(g_worst_place, glm::length(composed - NiMath::to_glm(world_camera->world.translate)));
            }
            const glm::mat3 world_axes = NiMath::to_glm(world_camera->world.rotate);
            const float     world_scale = world_camera->world.scale != 0.0f ? world_camera->world.scale : 1.0f;
            const glm::vec3 in_camera  = glm::transpose(world_axes) * d / world_scale;
            // Into the drawn view's camera: its place moved by that offset along its own axes, at its own scale.
            RE::NiTransform place{};
            if (!ReadBytes(reinterpret_cast<uintptr_t>(source_camera) + kWorldTransform, &place, sizeof(place))) {
                return false;
            }
            const glm::mat3 source_axes = NiMath::to_glm(place.rotate);
            const float     source_scale = place.scale != 0.0f ? place.scale : 1.0f;
            const glm::vec3 moved       = source_axes * in_camera * source_scale;
            g_eyes_apart = glm::length(moved);
            // (fix148) For the gun laser's beam drawn into the right eye's picture (its camera from the left eye's).
            VR::get()->m_stereo_eyes_apart.store(g_eyes_apart, std::memory_order_relaxed);
            if (space) {
                // (For the log: the galaxy camera against the world camera, how much that turned between frames (a frame behind shows as
                // turns while the head turns), and their scales.)
                const glm::mat3 relation = source_axes * glm::transpose(world_axes);
                if (g_relation_valid) {
                    const glm::mat3 change = relation * glm::transpose(g_last_relation);
                    const float     turn   = glm::degrees(std::acos(std::clamp((change[0][0] + change[1][1] + change[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
                    g_relation_worst       = std::max(g_relation_worst, turn);
                }
                g_last_relation  = relation;
                g_relation_valid = true;
                g_scale_ratio    = source_scale / world_scale;
            } else {
                g_relation_valid = false;
            }
            place.translate.x += moved.x;
            place.translate.y += moved.y;
            place.translate.z += moved.z;
            auto* cam  = static_cast<RE::NiCamera*>(camera);
            cam->local = place;
            WriteBytes(reinterpret_cast<uintptr_t>(camera) + kOffsetMatrix, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(source_camera) + kOffsetMatrix), 64);
            WriteBytes(reinterpret_cast<uintptr_t>(camera) + kLensStart, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(source_camera) + kLensStart),
                kLensEnd - kLensStart);
            // (fix138: each eye its own lens. The right eye's frustum: the drawn camera's (the left lens, as the game has it: aiming's
            // zoom with it), each edge turned by the angle between the left lens's edge and the right's, so the zoom carries over. The
            // same as the drawn camera's with the symmetric projection.)
            {
                static auto vr = VR::get();
                Vector4f    l{}, r{};
                {
                    std::shared_lock _{ vr->get_runtime()->eyes_mtx };
                    l = vr->get_runtime()->eye_frustums[0];
                    r = vr->get_runtime()->eye_frustums[1];
                }
                RE::NiFrustum lens{};
                if (!(l[1] > l[0] && l[2] > l[3] && r[1] > r[0] && r[2] > r[3]) ||
                    !ReadBytes(reinterpret_cast<uintptr_t>(camera) + kLensStart, &lens, sizeof(lens))) {
                    return false;
                }
                g_source_lens   = lens;
                const auto turn = [](float edge, float from, float to) { return std::tan(std::atan(edge) + std::atan(to) - std::atan(from)); };
                lens.left   = turn(lens.left, l[0], r[0]);
                lens.right  = turn(lens.right, l[1], r[1]);
                lens.top    = turn(lens.top, l[2], r[2]);
                lens.bottom = turn(lens.bottom, l[3], r[3]);
                if (!(lens.right > lens.left && lens.top > lens.bottom)) {
                    return false;
                }
                WriteBytes(reinterpret_cast<uintptr_t>(camera) + kLensStart, &lens, 16);
                g_right_lens = lens;
            }
            // (The first frame after one without the right eye's view: its camera marked as cut (NiCamera +0x210, read by its world
            // update at 0x142BE91AC, cleared at 0x142BE9207), so the game starts its histories over rather than reading pictures from
            // before the gap, fix136's review.)
            if (const uint32_t d = frame - g_last_placed; d != 1 && d != 0) { // (not a second placing in the same frame either)
                const uint8_t cut = 1;
                WriteBytes(reinterpret_cast<uintptr_t>(camera) + kCameraCut, &cut, 1);
            }
            g_last_placed = frame;
            return CallUpdate(camera);
        }

        void Skip(const char* why)
        {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            if (g_skip_logs.fetch_add(1) < 10) {
                spdlog::info("[StereoView] a full stereo frame without the right eye's view: {}", why);
            }
            // (Again and again: full stereo let go for 5 s, the eyes taking turns meanwhile, rather than one picture for both.)
            if (g_misses_in_row.fetch_add(1) + 1 >= 30) {
                g_misses_in_row.store(0);
                g_rest_until_ms.store(NowMs() + 5000);
                spdlog::info("[StereoView] 30 full stereo frames in a row without the right eye's view ({}): the eyes take turns for 5 s", why);
            }
        }

        // Just before the frame's view list is handed to the renderer (the game's thread): in a full stereo frame whose list is the
        // world's view alone, the right eye's added after it (its camera put at the right eye first).
        void OnList(safetyhook::Context& ctx)
        {
            if (g_failed.load(std::memory_order_relaxed)) {
                return;
            }
            void*          camera = g_camera.load(std::memory_order_acquire);
            const uint32_t view   = g_view.load(std::memory_order_acquire);
            auto&          vr     = VR::get();
            if (camera == nullptr || view == kNoHandle || vr == nullptr || !g_row_ok.load(std::memory_order_acquire)) {
                return;
            }
            const uint32_t frame = vr->m_engine_frame_count.load();
            if (!vr->is_native_frame(frame)) {
                return;
            }
            if (!g_threads_logged) {
                g_threads_logged = true;
                g_list_thread    = GetCurrentThreadId();
                spdlog::info("[StereoView] the frame's views are listed on thread {} (each frame's start on thread {})", g_list_thread, g_frame_thread);
            }
            // The list (the game's small list on the stack: its count, its room (the top bit: kept in place, in its own 4 entries), the
            // entries in place or elsewhere).
            const uintptr_t list = ctx.rsp + 0x50;
            uint32_t        count = 0;
            int32_t         room  = 0;
            if (!ReadBytes(list, &count, 4) || !ReadBytes(list + 8, &room, 4)) {
                Skip("its list could not be read");
                return;
            }
            uintptr_t entries = list + 0x10;
            if (room >= 0 && !ReadBytes(list + 0x10, &entries, 8)) {
                Skip("its list's entries could not be read");
                return;
            }
            uint32_t world = kNoHandle, first = kNoHandle;
            bool     space = false;
            if (count != 1 || (room & 0x7fffffff) < 2 || !DrawnView(world, space) || world != g_source_view || !ReadBytes(entries, &first, 4) || first != world) {
                Skip(count != 1 ? "the frame has other views (a menu's)" : ((room & 0x7fffffff) < 2 ? "no room in its list" : "its first view is not the world's"));
                return;
            }
            if (!PlaceCamera(camera, frame, space)) {
                Skip("the right eye's camera could not be placed (no eye offset for this frame)");
                return;
            }
            const uint32_t two = 2;
            if (!WriteBytes(entries + 4, &view, 4) || !WriteBytes(list, &two, 4)) {
                Skip("its list could not be written");
                return;
            }
            g_inserted.fetch_add(1, std::memory_order_relaxed);
            g_inserted_ms.store(NowMs(), std::memory_order_release);
            g_misses_in_row.store(0, std::memory_order_relaxed);
            // (The present shows the two pictures only for a frame with both.)
            vr->note_native_drawn(frame);
        }

        // The upscale's one-view check (as the frame's parts are built, after its list changed): with the eyes' two views, as for one
        // (each view upscaled by DLSS, not the picture copied up).
        void OnUpscaleCheck(safetyhook::Context& ctx)
        {
            const uint32_t view = g_view.load(std::memory_order_acquire);
            if (view == kNoHandle) {
                return;
            }
            const uintptr_t record = ctx.rax + ctx.rdx;
            uint32_t        count  = 0;
            int32_t         room   = 0;
            if (!ReadBytes(record, &count, 4) || count != 2 || !ReadBytes(record + 8, &room, 4)) {
                return;
            }
            uintptr_t entries = record + 0x10;
            if (room >= 0 && !ReadBytes(record + 0x10, &entries, 8)) {
                return;
            }
            uint32_t second = kNoHandle;
            if (!ReadBytes(entries + 4, &second, 4) || (second & kNoHandle) != (view & kNoHandle)) {
                return;
            }
            ctx.rflags |= 0x40; // (ZF: "one view or none" taken)
            g_upscale_let.fetch_add(1, std::memory_order_relaxed);
            if (!g_upscale_logged) {
                g_upscale_logged = true;
                spdlog::info("[StereoView] the frame's parts built with the eyes' two views: each upscaled by DLSS (the one-view check let through)");
            }
        }

        // (fix153) The scene setup's one-view check (each view's, every frame): with the eyes' two views, as for one, so each view hands
        // DLSS its own camera and jitter (the right eye's to its own instance: DlssViews marks its setup as view 1).
        void OnConstantsCheck(safetyhook::Context& ctx)
        {
            const uint32_t view = g_view.load(std::memory_order_acquire);
            if (view == kNoHandle) {
                return;
            }
            const uintptr_t record = ctx.rdx + ctx.rcx;
            uint32_t        count  = 0;
            int32_t         room   = 0;
            if (!ReadBytes(record, &count, 4) || count != 2 || !ReadBytes(record + 8, &room, 4)) {
                return;
            }
            uintptr_t entries = record + 0x10;
            if (room >= 0 && !ReadBytes(record + 0x10, &entries, 8)) {
                return;
            }
            uint32_t second = kNoHandle;
            if (!ReadBytes(entries + 4, &second, 4) || (second & kNoHandle) != (view & kNoHandle)) {
                return;
            }
            ctx.rflags |= 0x40; // (ZF: the jne "more than one view" not taken)
            g_constants_let.fetch_add(1, std::memory_order_relaxed);
            if (!g_constants_logged) {
                g_constants_logged = true;
                spdlog::info("[StereoView] (fix153) each eye's view hands DLSS its own camera and jitter (the scene setup's one-view check let through)");
            }
        }

        // A view's record made (RenderViewSubGraph's function 10, as its part of the frame starts): the right eye's given its own history
        // list, before anything made from it copies its parent's.
        void* OnViewRecord(void* graph, void* parent)
        {
            const auto original = reinterpret_cast<RecordFn>(g_record_original);
            void*      record   = original(graph, parent);
            void*      list     = g_eye_list.load(std::memory_order_acquire);
            if (record != nullptr && list != nullptr) {
                uint32_t handle = kNoHandle;
                if (ReadBytes(reinterpret_cast<uintptr_t>(graph) + kGraphView, &handle, 4) && IsEyeView(handle) &&
                    WriteBytes(reinterpret_cast<uintptr_t>(record) + kRecordList, &list, 8)) {
                    g_own_records.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return record;
        }

        bool CallListCtor(void* list)
        {
            __try {
                Fn<ListCtorFn>(kListCtor)(list);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The right eye's history list made, once (the game's thread, as the right eye's view is made): the game's own constructor
        // (its slots' picture holders, registered with the others). Kept for the session (its
        // pictures, about 33 MB at this size, are made by the game as the view first uses them). Not made: the views share the game's.
        void MakeEyeList()
        {
            if (g_list_tried || g_record_original == 0) {
                return;
            }
            g_list_tried = true;
            void* list   = _aligned_malloc(0x80, 16);
            if (list == nullptr) {
                return;
            }
            std::memset(list, 0, 0x80);
            if (!CallListCtor(list)) {
                spdlog::warn("[StereoView] the right eye's own history list could not be made (the game's constructor failed): the eyes share the game's");
                return; // (not freed: it may be half made)
            }
            // (Its slots only read: their holders are the game's, not all of one kind (slot 7, the near volumetric volume, is a smaller
            // object with its flag elsewhere: fix136's review); their pictures are made at first use, as the game's own list's.)
            int pairs = 0;
            for (size_t i = 0; i < kListSlots; ++i) {
                uintptr_t pair = 0;
                if (ReadBytes(reinterpret_cast<uintptr_t>(list) + i * 8, &pair, 8) && pair != 0) {
                    ++pairs;
                }
            }
            if (pairs == 0) {
                spdlog::warn("[StereoView] the right eye's own history list could not be made: the eyes share the game's (shimmer in fog and AO)");
                return; // (not freed: the constructor may have registered it)
            }
            g_eye_list.store(list, std::memory_order_release);
            spdlog::info("[StereoView] the right eye's view has a history list of its own ({} of its {} slots made): each eye's last frame its own",
                pairs, kListSlots);
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        // (fix190) Each one checked by its own rva and bytes (GameCode), in the list's order: the first not as expected is logged.
        const std::pair<const Code*, bool> checks[] = {
            { &kPoolInit, SameCode(kPoolInitRva, kPoolInitBytes, sizeof(kPoolInitBytes)) },
            { &kAlloc, SameCode(kAllocRva, kAllocBytes, sizeof(kAllocBytes)) },
            { &kSmallAlloc, SameCode(kSmallAllocRva, kSmallAllocBytes, sizeof(kSmallAllocBytes)) },
            { &kCameraCtor, SameCode(kCameraCtorRva, kCameraCtorBytes, sizeof(kCameraCtorBytes)) },
            { &kCameraReg, SameCode(kCameraRegRva, kCameraRegBytes, sizeof(kCameraRegBytes)) },
            { &kCameraUpdate, SameCode(kCameraUpdateRva, kCameraUpdateBytes, sizeof(kCameraUpdateBytes)) },
            { &kViewCreate, SameCode(kViewCreateRva, kViewCreateBytes, sizeof(kViewCreateBytes)) },
            { &kViewData, SameCode(kViewDataRva, kViewDataBytes, sizeof(kViewDataBytes)) },
            { &kFeatures, SameCode(kFeaturesRva, kFeaturesBytes, sizeof(kFeaturesBytes)) },
            { &kImageSpace, SameCode(kImageSpaceRva, kImageSpaceBytes, sizeof(kImageSpaceBytes)) },
            { &kReadbacks, SameCode(kReadbacksRva, kReadbacksBytes, sizeof(kReadbacksBytes)) },
            { &kShadow, SameCode(kShadowRva, kShadowBytes, sizeof(kShadowBytes)) },
            { &kListAt, SameCode(kListAtRva, kListAtBytes, sizeof(kListAtBytes)) },
            { &kUpscaleAt, SameCode(kUpscaleAtRva, kUpscaleAtBytes, sizeof(kUpscaleAtBytes)) },
        };
        for (const auto& [code, same] : checks) {
            if (g_base == 0 || !same) {
                spdlog::warn("[StereoView] {} is not at rva {:x} as in Starfield 1.16.236: full stereo off", code->what, code->rva);
                SetStatus(std::string{ "Full stereo: off (the game's code is not as in Starfield 1.16.236: " } + code->what + ")");
                return;
            }
        }
        // (fix190) The game's data it uses without a check of its own (the cameras' pool and its state, the camera views' table, the
        // update stamp, the light reading's type table): in a game version they are not known in, full stereo stays off.
        if (!GameCode::Known({ kPoolStateRva, kPoolRva, kViewsTableRva, kUpdateStampRva, kReadbackTable })) {
            spdlog::warn("[StereoView] (fix190) the game's data full stereo uses is not known in this game version (GameCode): full stereo off");
            SetStatus("Full stereo: off (the game's data is not known in this game version)");
            return;
        }
        g_list_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kListAtRva)), &OnList);
        g_upscale_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kUpscaleAtRva)), &OnUpscaleCheck);
        if (!g_list_hook || !g_upscale_hook) {
            g_list_hook    = {};
            g_upscale_hook = {};
            spdlog::warn("[StereoView] could not hook the frame's view list or the upscale's check: full stereo off");
            SetStatus("Full stereo: off (could not hook the game's code)");
            return;
        }
        g_installed = true;
        spdlog::info("[StereoView] the frame's view list (rva {:x}) and the upscale's one-view check (rva {:x}) hooked: full stereo can be "
                     "turned on (VR > Stereo)",
            kListAt.rva, kUpscaleAt.rva);
        // (fix153) The scene setup's check too, for each view's DLSS constants. Not: full stereo as before (DLSS on old constants there).
        if (SameCode(kConstantsAtRva, kConstantsAtBytes, sizeof(kConstantsAtBytes))) {
            g_constants_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kConstantsAtRva)), &OnConstantsCheck);
        }
        if (g_constants_hook) {
            spdlog::info("[StereoView] (fix153) the scene setup's DLSS one-view check hooked (rva {:x}): each eye's view sets its own DLSS camera "
                         "and jitter",
                kConstantsAt.rva);
        } else {
            spdlog::warn("[StereoView] (fix153) {} is not at rva {:x} as in Starfield 1.16.236 (or could not be hooked): in full stereo DLSS keeps "
                         "the constants of the frame before it",
                kConstantsAt.what, kConstantsAt.rva);
        }
        // (fix136) The views' record maker, for the right eye's own history list: its type table's entry replaced (only this kind's),
        // when it is the game's function as in 1.16.236. Not: the eyes share the game's list (as fix134/135).
        uintptr_t entry = 0;
        // (fix190) (its type table's entry not checked by its code: known in this game version, else not hooked)
        if (SameCode(kRecordFnRva, kRecordFnBytes, sizeof(kRecordFnBytes)) && SameCode(kListCtorRva, kListCtorBytes, sizeof(kListCtorBytes)) &&
            GameCode::Known({ kRecordEntryRva }) && ReadBytes(At(kRecordEntryRva), &entry, 8) && entry == At(kRecordFnRva)) {
            g_record_original = entry;
            auto* slot        = reinterpret_cast<uintptr_t*>(At(kRecordEntryRva));
            DWORD old         = 0;
            if (VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(reinterpret_cast<uintptr_t>(&OnViewRecord)));
                VirtualProtect(slot, sizeof(uintptr_t), old, &old);
                spdlog::info("[StereoView] the views' record maker hooked (rva {:x}): the right eye's view gets its own history list", kRecordFn.rva);
            } else {
                g_record_original = 0;
            }
        } else {
            spdlog::warn("[StereoView] the views' record maker is not as in Starfield 1.16.236: the eyes share the game's history list");
        }
        SetStatus("Full stereo: ready (turn it on in VR > Stereo)");
    }

    void OnFrameStart()
    {
        auto& vr = VR::get();
        if (vr == nullptr) {
            return;
        }
        if (g_frame_thread == 0) {
            g_frame_thread = GetCurrentThreadId();
        }
        // Wanted: the setting on, in stereo, in the headset (OpenXR, each eye's lens known: the right camera takes the drawn view's
        // camera's), in play (no menu, not near a loading screen, the world drawn: on a planet, inside, or aboard a ship in space), the
        // drawn view drawn with its own camera (not photo mode or a scene's camera), each eye able to have its own DLSS instance, the
        // body shown (the first-person arms and gun, drawn with a camera of their own at the left eye, hidden).
        uint32_t world = kNoHandle;
        bool     space = false;
        uint32_t why   = 0; // (what keeps it from being played, by bits)
        if (!g_installed || g_failed.load() || !vr->full_stereo_requested()) {
            why |= 1;
        } else {
            // (Each eye's lens known: the right camera's is the drawn view's camera's turned from the left lens to the right's,
            // PlaceCamera, fix138; before, both had to be the same, the symmetric projection.)
            Vector4f l{}, r{};
            {
                std::shared_lock _{vr->get_runtime()->eyes_mtx};
                l = vr->get_runtime()->eye_frustums[0];
                r = vr->get_runtime()->eye_frustums[1];
            }
            const bool lenses_known = l[1] > l[0] && l[2] > l[3] && r[1] > r[0] && r[2] > r[3];
            why |= !vr->is_hmd_active() || !vr->get_runtime()->is_openxr() ? 2 : 0;
            why |= !lenses_known ? 4 : 0;
            why |= ModSettings::g_internalSettings.showQuadDisplay ? 8 : 0;
            // (fix142) The hand scanner: its marks are drawn into the interface for the camera it is drawn with, and full stereo draws the
            // interface once for both eyes (one eye's marks off their objects): the eyes take turns while it is open, each with its own.
            why |= GameFlow::isScannerOpen() ? 1024 : 0;
            // (fix175) Build mode (no menu panel since fix175: the world drawn from your head as in play): the eyes take turns while it is
            // drawn, one view a frame (the mono-like frame rate chosen while building, ~36 fps, over full stereo's ~25), and its
            // marks and list are drawn for the camera they are seen from, as the scanner's. Not a brief stand-down (its pictures let go
            // after 3 s, as for any long stop: building lasts minutes), and logged as it starts (below).
            why |= GameFlow::isWorkshopShown() ? 2048 : 0;
            why |= CreationEngineSingletonManager::GetPlayerRef() == nullptr || NowMs() - GameFlow::lastLoadingScreen() < 3000 ? 16 : 0;
            why |= !DrawnView(world, space) ? 32 : 0;
            if ((why & 32) == 0) {
                std::array<uint8_t, kViewDataSize> data{};
                uint32_t                           drawn_with = kNoHandle, source_handle = kNoHandle;
                auto*                              source     = SourceCamera(space);
                if (source == nullptr || !ReadBytes(reinterpret_cast<uintptr_t>(source) + kCameraHandle, &source_handle, 4) ||
                    !ReadViewColumn(world, kViewDataColumn, kViewDataSize, data.data())) {
                    why |= 512;
                } else {
                    std::memcpy(&drawn_with, data.data() + 4, 4);
                    why |= (drawn_with & kNoHandle) != (source_handle & kNoHandle) ? 512 : 0;
                }
            }
            why |= !UpscalerAfrNvidiaModule::eyes_ready() ? 64 : 0;
            why |= !body::IsActive() ? 128 : 0;
            why |= NowMs() < g_rest_until_ms.load() ? 256 : 0;
            // (Logged as each reason first comes, and every 30 s while it is wanted but not played: why now.)
            const auto text = [](uint32_t bits) {
                std::string out{};
                const auto  add = [&](uint32_t bit, const char* what) {
                    if ((bits & bit) != 0) {
                        out += out.empty() ? "" : "; ";
                        out += what;
                    }
                };
                add(2, "not in the headset (OpenXR)");
                add(4, "the eyes' lenses not known yet");
                add(8, "a menu is open");
                add(1024, "the hand scanner is open");
                add(2048, "build mode is open (fix175: the eyes take turns while building)");
                add(16, "loading, or just after");
                add(32, "the world not drawn");
                add(512, "the view drawn with another camera (photo mode, a scene's camera)");
                add(64, "DLSS not in use, or the right eye's own DLSS instance not usable (per-eye DLSS, the DLSS/TAA fix)");
                add(128, "the body not shown (the first-person arms would be the left eye's in both)");
                add(256, "resting after frames without the right eye's view");
                return out;
            };
            const int64_t  now_ms = NowMs();
            if (why == 0) {
                g_why_logged_ms = now_ms; // (played: the 30 s count starts when it stops being played)
            }
            const uint32_t told   = why & ~g_why_not_logged & ~(8u | 16u | 1024u | 2048u); // (menus, loading, the scanner and build mode come and go: not each time)
            // (fix175) Build mode: said each time it makes full stereo stand down (otherwise it would be played).
            {
                static bool workshop_was = false;
                static int  workshop_logs = 0;
                const bool  workshop      = (why & 2048u) != 0;
                if (workshop && !workshop_was && (why & ~2048u) == 0 && workshop_logs < 20) {
                    ++workshop_logs;
                    spdlog::info("[StereoView] (fix175) build mode is open: full stereo stands down while building, the eyes take turns (one view "
                                 "drawn a frame, as in mono); back when it closes");
                }
                workshop_was = workshop;
            }
            if (told != 0 && g_not_possible_logs < 12) {
                ++g_not_possible_logs;
                g_why_not_logged |= told;
                g_why_logged_ms   = now_ms;
                spdlog::info("[StereoView] full stereo not played now: {}", text(why));
            } else if (why != 0 && now_ms - g_why_logged_ms >= 30000) {
                g_why_logged_ms = now_ms;
                spdlog::info("[StereoView] full stereo still not played (the last 30 s): {}", text(why));
            }
        }
        // (fix142: standing down only for the hand scanner: its pictures kept for when it closes, D3D12Component.)
        vr->m_native_brief_pause = why == 1024u;
        const bool wanted = why == 0;
        if (wanted && g_camera.load() == nullptr && !g_failed.load()) {
            Make(world, space);
        }
        // (The right eye's own history list, once its view is made.)
        if (wanted && g_view.load() != kNoHandle && !g_failed.load()) {
            MakeEyeList();
        }
        const uint32_t view   = g_view.load();
        bool           row_ok = false;
        bool           force  = false;
        // (Into space or out of it: the other view's settings copied at once, and full stereo let go half a second meanwhile.)
        if (wanted && view != kNoHandle && world != g_source_view.load()) {
            static int told = 0;
            if (told++ < 12) {
                spdlog::info("[StereoView] the drawn view is now the {} [{}]: the right eye's view copies it", space ? "space scene's" : "world's",
                    world & kNoHandle);
            }
            g_source_view       = world;
            g_source_changed_ms = NowMs();
            g_relation_valid    = false;
            force               = true;
        }
        if (wanted && view != kNoHandle && !g_failed.load()) {
            // (Still ours: its row holds our camera. Else something replaced it: given up rather than drawing another's view. Not
            // readable (its row let go): not added, and given up after a while.)
            std::array<uint8_t, kViewDataSize> ours{};
            if (!ReadViewColumn(view, kViewDataColumn, kViewDataSize, ours.data())) {
                if (++g_row_unread >= 120) {
                    Fail("the right eye's view's row is no longer there");
                }
            } else if (std::memcmp(ours.data() + 4, g_written_data.data() + 4, 4) != 0) {
                Fail("the right eye's view no longer has its camera");
            } else {
                g_row_unread = 0;
                row_ok       = true;
                KeepAsWorld(world, view, force);
            }
        }
        g_row_ok.store(row_ok, std::memory_order_release);
        // (Frames shown with one picture for both eyes again and again (the left eye's copy not made): the eyes take turns for 5 s.)
        if (vr->m_native_one_picture_run.load() >= 30) {
            vr->m_native_one_picture_run.store(0);
            g_rest_until_ms.store(NowMs() + 5000);
            spdlog::info("[StereoView] 30 full stereo frames in a row shown with one picture (the left eye's not copied): the eyes take turns for 5 s");
        }
        const bool possible = wanted && row_ok && !g_failed.load() && NowMs() >= g_rest_until_ms.load() && NowMs() - g_source_changed_ms >= 500;
        vr->m_native_possible.store(possible, std::memory_order_release);
        // (Every 10 s while it is played, for the log.)
        const auto now = std::chrono::steady_clock::now();
        if (vr->is_native() && now - g_stats_at >= std::chrono::seconds(10)) {
            g_stats_at = now;
            spdlog::info("[StereoView] the last 10 s ({}): the right eye's view added {} times, not {} times, with its own history list {} times; "
                         "the upscale let through {} times, each view's DLSS constants {} times (fix153); the "
                         "eyes {:.4f} apart (the drawn camera's units); the right eye's lens tan(l r t b) {:.3f} {:.3f} {:.3f} {:.3f} from the drawn camera's "
                         "{:.3f} {:.3f} {:.3f} {:.3f}; the world "
                         "camera's place off its parent's by up to {:.5f}{}",
                space ? "in space" : "on a planet or inside", g_inserted.exchange(0), g_skipped.exchange(0), g_own_records.exchange(0),
                g_upscale_let.exchange(0), g_constants_let.exchange(0), g_eyes_apart, g_right_lens.left, g_right_lens.right, g_right_lens.top, g_right_lens.bottom, g_source_lens.left,
                g_source_lens.right, g_source_lens.top, g_source_lens.bottom,
                g_worst_place,
                space ? std::format("; the galaxy camera turned against the world camera by up to {:.2f} deg between frames, its scale {:.4f} of "
                                    "the world camera's", g_relation_worst, g_scale_ratio)
                      : std::string{});
            g_worst_place    = 0.0f;
            g_relation_worst = 0.0f;
        }
    }

    void OnGraph(const char* name, bool before)
    {
        // (The frame drawn: the right eye's history list's camera-cut flag cleared, as the game clears its own list's: else a cut once
        // would start its histories over every frame after.)
        if (before || name == nullptr || std::strcmp(name, "Frame") != 0) {
            return;
        }
        if (void* list = g_eye_list.load(std::memory_order_acquire); list != nullptr) {
            const uint8_t zero = 0;
            WriteBytes(reinterpret_cast<uintptr_t>(list) + kListCut, &zero, 1);
        }
    }

    void SetEyeDelta(const glm::vec3& right_minus_left, uint32_t frame)
    {
        std::scoped_lock _{ g_delta_mtx };
        g_delta       = right_minus_left;
        g_delta_frame = frame;
        g_delta_valid = true;
    }

    bool IsEyeView(uint32_t handle)
    {
        const uint32_t view = g_view.load(std::memory_order_acquire);
        return view != kNoHandle && (handle & kNoHandle) == (view & kNoHandle);
    }

    float EyesApart()
    {
        return g_eyes_apart;
    }

    bool Active()
    {
        const int64_t at = g_inserted_ms.load(std::memory_order_acquire);
        return at != 0 && NowMs() - at < 500;
    }

    void* Camera()
    {
        return g_camera.load();
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
