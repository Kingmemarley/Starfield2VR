#include "ShadowShare.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "LiveWorld.h"
#include "StereoView.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <mods/VR.hpp>

namespace ShadowShare
{
    namespace
    {
        // (fix190) Each place by its 1.16.236 rva and its bytes as constants of their own, for GameCode (GameCode.h).
        // Starfield 1.16.236, in the sun's shadow setup (0x1429A92B0; its stack fixed after sub rsp, 0x1390). Right after the cascades'
        // fit returns: xor sil, sil; test r14b, r14b (r14b: the cascade count; 0 skips the per-cascade loop to the end below).
        constexpr uintptr_t kLoopStartRva = 0x29AA65A;
        constexpr uint8_t   kLoopStart[]  = { 0x40, 0x32, 0xF6, 0x45, 0x84, 0xF6, 0x0F, 0x84 }; // (the cascades' loop)
        // After the loop (also the skip's target): mov ecx, [rsp+0x13E8] (the records to copy into the view's constants, from its row).
        constexpr uintptr_t kLoopEndRva = 0x29AAD36;
        constexpr uint8_t   kLoopEnd[]  = { 0x8B, 0x8C, 0x24, 0xE8, 0x13, 0x00, 0x00, 0x41 }; // (the cascades' records copied)
        // Its stack: the view's index (the view's handle's low 24 bits), its CascadeShadowMapCache row, the local-light shadow budget
        // the cascades left, and the records' count.
        constexpr uintptr_t kStackIndex  = 0xC0;
        constexpr uintptr_t kStackRow    = 0x98;
        constexpr uintptr_t kStackBudget = 0x54;
        constexpr uintptr_t kStackCount  = 0x13E8;
        // The row: its light (+0), its record count (+0xB8), its storage's capacity (+0xC0; the top bit: the records in place at +0xC8,
        // else +0xC8 points at them), the records (0x90 bytes each: a matrix, the cascade's eye, its map's bindless index, its split).
        constexpr uintptr_t kRowLight    = 0x00;
        constexpr uintptr_t kRowCount    = 0xB8;
        constexpr uintptr_t kRowCapacity = 0xC0;
        constexpr uintptr_t kRowRecords  = 0xC8;
        constexpr size_t    kRecordSize  = 0x90;
        constexpr uint32_t  kMaxRecords  = 4; // (the constants and the row's storage hold four)
        // The setup's own copy of the renderer's frame stamp (written at 0x1429AA581, valid at both hooks: fix140's review).
        constexpr uintptr_t kStackStamp = 0x88;

        uintptr_t           g_base{ 0 };
        safetyhook::MidHook g_start_hook{}, g_end_hook{};
        std::atomic<bool>   g_installed{ false };
        std::atomic<bool>   g_on{ true };
        std::atomic<uint32_t> g_kept{ 0 }, g_shared{ 0 }, g_own{ 0 }, g_failed{ 0 };
        std::atomic<int>    g_logs{ 0 };

        // The left eye's records this frame (the setup runs the views one after the other on one thread; a lock anyway).
        struct Kept
        {
            bool     valid{ false };
            uint32_t stamp{ 0 };
            uint32_t light{ 0 };
            uint32_t row_count{ 0 };
            uint32_t count{ 0 };
            uint32_t budget{ 0 };
            uint8_t  records[kMaxRecords * kRecordSize]{};
        };
        std::mutex g_mtx{};
        Kept       g_kept_records{};

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

        template <class T>
        bool Read(uintptr_t at, T& out)
        {
            return ReadBytes(at, &out, sizeof(T));
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // The row's records' storage (in place, or where +0xC8 points) and its capacity.
        bool Storage(uintptr_t row, uintptr_t& records, uint32_t& capacity)
        {
            int32_t cap = 0;
            if (!Read(row + kRowCapacity, cap)) {
                return false;
            }
            capacity = static_cast<uint32_t>(cap) & 0x7FFFFFFFu;
            if (cap < 0) {
                records = row + kRowRecords;
                return true;
            }
            return Read(row + kRowRecords, records) && records != 0;
        }

        // A full stereo frame being set up, and this view's place in it: 0 the world's (the left eye), 1 the right eye's, -1 neither.
        int ViewOf(uint32_t index)
        {
            auto& vr = VR::get();
            if (vr == nullptr || !vr->is_native_frame(vr->m_render_frame_count.load())) {
                return -1;
            }
            if (StereoView::IsEyeView(index)) {
                return 1;
            }
            uint32_t world = 0;
            return LiveWorld::WorldViewHandle(world) && (world & 0xFFFFFF) == index ? 0 : -1;
        }

        // After the loop: the world's view keeps its records (all the cascades', the ones not drawn this frame still those of their maps).
        void OnLoopEnd(safetyhook::Context& ctx)
        {
            if (!g_on.load(std::memory_order_relaxed)) {
                return;
            }
            uint32_t  index = 0, count = 0, budget = 0, stamp = 0, light = 0, row_count = 0, capacity = 0;
            uintptr_t row = 0, records = 0;
            if (!Read(ctx.rsp + kStackIndex, index) || ViewOf(index & 0xFFFFFF) != 0 || !Read(ctx.rsp + kStackRow, row) || row == 0 ||
                !Read(ctx.rsp + kStackCount, count) || !Read(ctx.rsp + kStackBudget, budget) || !Read(ctx.rsp + kStackStamp, stamp) ||
                !Read(row + kRowLight, light) || !Read(row + kRowCount, row_count) || !Storage(row, records, capacity) || count > kMaxRecords ||
                count > capacity || row_count > capacity) {
                return;
            }
            std::scoped_lock _{ g_mtx };
            // (As many as the row holds or the constants take: the row's are read as the right eye's steps are recorded too.)
            const uint32_t n     = std::max(count, row_count);
            g_kept_records.valid = n <= kMaxRecords && ReadBytes(records, g_kept_records.records, static_cast<size_t>(n) * kRecordSize);
            g_kept_records.stamp     = stamp;
            g_kept_records.light     = light;
            g_kept_records.row_count = row_count;
            g_kept_records.count     = count;
            g_kept_records.budget    = budget;
            if (g_kept_records.valid) {
                g_kept.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Before the loop: the right eye's view gets the left's records this frame, and its loop is skipped.
        void OnLoopStart(safetyhook::Context& ctx)
        {
            if (!g_on.load(std::memory_order_relaxed)) {
                return;
            }
            uint32_t  index = 0, stamp = 0, light = 0, capacity = 0;
            uintptr_t row = 0, records = 0;
            if (!Read(ctx.rsp + kStackIndex, index) || ViewOf(index & 0xFFFFFF) != 1) {
                return;
            }
            if (!Read(ctx.rsp + kStackRow, row) || row == 0 || !Read(ctx.rsp + kStackStamp, stamp) || !Read(row + kRowLight, light) ||
                !Storage(row, records, capacity)) {
                g_failed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::scoped_lock _{ g_mtx };
            const auto& k = g_kept_records;
            if (!k.valid || k.stamp != stamp || k.light != light || k.count > capacity || k.row_count > capacity) {
                g_own.fetch_add(1, std::memory_order_relaxed);
                if (g_logs.fetch_add(1) < 6) {
                    spdlog::info("[ShadowShare] the right eye draws its own sun shadows this frame ({})",
                        !k.valid ? "the left eye's records not kept" : k.stamp != stamp ? "the left eye's are from another frame"
                                                                       : k.light != light ? "another light" : "its row has no room");
                }
                return;
            }
            const uint32_t count = k.count;
            const uint32_t n     = std::max(k.count, k.row_count);
            if (!WriteBytes(records, k.records, static_cast<size_t>(n) * kRecordSize) || !WriteBytes(row + kRowCount, &k.row_count, 4) ||
                !WriteBytes(ctx.rsp + kStackCount, &count, 4) || !WriteBytes(ctx.rsp + kStackBudget, &k.budget, 4)) {
                g_failed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ctx.r14 &= ~static_cast<uintptr_t>(0xFF); // (no cascades drawn: the loop skipped to the records' copy)
            g_shared.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        if (g_base == 0 || !SameCode(kLoopStartRva, kLoopStart, sizeof(kLoopStart)) || !SameCode(kLoopEndRva, kLoopEnd, sizeof(kLoopEnd))) {
            spdlog::warn("[ShadowShare] the sun's shadow setup is not as in Starfield 1.16.236: each eye draws its own shadows");
            return;
        }
        g_end_hook   = safetyhook::create_mid(reinterpret_cast<void*>(At(kLoopEndRva)), &OnLoopEnd);
        g_start_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kLoopStartRva)), &OnLoopStart);
        if (!g_end_hook || !g_start_hook) {
            g_end_hook   = {};
            g_start_hook = {};
            spdlog::warn("[ShadowShare] could not hook the sun's shadow setup: each eye draws its own shadows");
            return;
        }
        g_installed = true;
        spdlog::info("[ShadowShare] the sun's shadow setup hooked: in full stereo the right eye uses the left eye's shadow maps");
    }

    void SetOn(bool on)
    {
        g_on.store(on, std::memory_order_relaxed);
    }

    bool On()
    {
        return g_installed.load() && g_on.load(std::memory_order_relaxed);
    }

    std::string Status()
    {
        if (!g_installed.load()) {
            return "not installed";
        }
        return std::format("{}; since last: the left eye's kept {}, the right eye given them {}, drew its own {}, failed {}", g_on.load() ? "on" : "off",
            g_kept.exchange(0), g_shared.exchange(0), g_own.exchange(0), g_failed.exchange(0));
    }
}
