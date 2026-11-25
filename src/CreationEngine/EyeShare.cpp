#include "EyeShare.h"

#include <windows.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "LiveWorld.h"
#include "RenderPassIo.h"
#include "StereoView.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"

namespace EyeShare
{
    namespace
    {
        // (fix192) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known: the step then stays run in both eyes).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // The steps' kinds (Starfield 1.16.236, a helper's research): each one's own type table, its function 7 and that function's
        // first bytes (the blurs are short jumps into 0x142B40140 with their direction).
        constexpr uintptr_t kTransmittanceTableRva = 0x4EE2EB8;
        constexpr uintptr_t kTransmittanceFnRva    = 0x2B316A0;
        constexpr uintptr_t kFogMapTableRva        = 0x4EE44E0;
        constexpr uintptr_t kFogMapFnRva           = 0x2B40750;
        constexpr uintptr_t kBlurHTableRva         = 0x4EE4528;
        constexpr uintptr_t kBlurHFnRva            = 0x2B40720;
        constexpr uintptr_t kBlurVTableRva         = 0x4EE4498;
        constexpr uintptr_t kBlurVFnRva            = 0x2B40730;
        constexpr uint8_t kTransmittanceBytes[] = { 0x40, 0x53, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x8B, 0x82, 0x40, 0x01, 0x00, 0x00, 0x48 };
        constexpr uint8_t kFogMapBytes[]        = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        constexpr uint8_t kBlurHBytes[]         = { 0x49, 0x8B, 0xC0, 0x33, 0xC9, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD0, 0xE9 };
        constexpr uint8_t kBlurVBytes[]         = { 0x49, 0x8B, 0xC0, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD0, 0xE9 };

        // A step's view: the graph data's +0x108 indexed by its +0x140, its handle at +0x24 (24 bits), as GpuTimer reads it.
        constexpr uintptr_t kDataRecords = 0x108, kDataIndex = 0x140, kRecordKey = 0x24;
        // (The review, the exe: the render graph places its textures in shared 128 MB pages (bUseCustomHeapAllocator:RenderGraph, 1) and
        // frees each one's range after its last use in the frame (0x142B19DF0), for later textures: the left eye's fog map can be
        // overwritten before the right eye reads it. A texture entry's +0xA0 is not 0 when it sits in that reused memory: only a step
        // whose every texture is outside it may be shared.)
        constexpr uintptr_t kEntryAliased = 0xA0;

        struct Kind
        {
            const char* name;
            // (At start.)
            uintptr_t table{ 0 };
            bool      ready{ false };
            // The left eye's step this frame: its textures, item by item (null: not a texture, e.g. a token).
            std::mutex      mtx{};
            bool            noted{ false };
            uint32_t        generation{ 0 };
            uint32_t        count{ 0 }, total{ 0 };
            ID3D12Resource* resources[RenderPassIo::kMaxItems]{};
            // Counts (the last 30 s).
            std::atomic<uint32_t> left{ 0 }, skipped{ 0 }, no_left{ 0 }, differs{ 0 }, unread{ 0 }, reused{ 0 };
            std::atomic<bool>     told{ false }; // (its textures' reused-memory marks logged once)
            std::atomic<uint64_t> skipped_all{ 0 };
        };
        // (fix192: in the order of the places given to check in Install.)
        Kind g_kinds[] = {
            { "PrecomputeAtmosphericScatteringTransmittance" },
            { "GenerateFogMap" },
            { "BlurTerrainHeightClipmapHorizontal" },
            { "BlurTerrainHeightClipmapVertical" },
        };

        std::atomic<bool> g_on{ false };
        std::atomic<bool> g_any{ false };
        std::chrono::steady_clock::time_point g_logged_at{};

        bool Read(uintptr_t at, void* out, size_t n)
        {
            __try {
                std::memcpy(out, reinterpret_cast<const void*>(at), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        Kind* KindOf(uintptr_t table)
        {
            for (auto& k : g_kinds) {
                if (k.ready && k.table == table) {
                    return &k;
                }
            }
            return nullptr;
        }

        // The step's view's handle (24 bits); false: not read.
        bool ViewKey(const void* data, uint32_t& key)
        {
            uint32_t  index  = 0;
            uintptr_t record = 0;
            const auto d     = reinterpret_cast<uintptr_t>(data);
            if (d == 0 || !Read(d + kDataIndex, &index, sizeof(index)) || index >= 64 || !Read(d + kDataRecords + 8 * static_cast<uintptr_t>(index), &record, sizeof(record)) ||
                record == 0 || !Read(record + kRecordKey, &key, sizeof(key))) {
                return false;
            }
            key &= 0xFFFFFF;
            return true;
        }

        // A texture in the graph's reused memory (its entry's +0xA0 set, or not read).
        bool Reused(const RenderPassIo::PassIo::Item& item)
        {
            uint64_t mark = 1;
            return item.entry == 0 || !Read(item.entry + kEntryAliased, &mark, sizeof(mark)) || mark != 0;
        }

        bool IsLeftEye(uint32_t key)
        {
            uint32_t world = 0;
            return LiveWorld::WorldViewHandle(world) && (world & 0xFFFFFF) == key;
        }
    }

    void Install()
    {
        if (MemoryScan::mod == nullptr) {
            return;
        }
        std::string ready{}, not_ready{};
        // (fix192) Each kind's type table, its function 7's place and that function's first bytes, by their 1.16.236 rvas through
        // GameCode (written out per kind so the port tool sees each place).
        const auto check = [&](Kind& k, uintptr_t table, uintptr_t function, bool code) {
            uintptr_t fn = 0;
            // (Its function 7: the game's own, or StepHook already (another of the mod's walks hooked it first: it calls the game's).)
            const bool fn_ok = table != 0 && function != 0 && Read(table + 7 * sizeof(uintptr_t), &fn, sizeof(fn)) &&
                               (fn == function || fn == LiveWorld::HookAddress());
            k.table = table;
            k.ready = fn_ok && code && LiveWorld::HookKind(table);
            (k.ready ? ready : not_ready) += std::format("{}{}", (k.ready ? ready : not_ready).empty() ? "" : ", ", k.name);
        };
        check(g_kinds[0], At(kTransmittanceTableRva), At(kTransmittanceFnRva), SameCode(kTransmittanceFnRva, kTransmittanceBytes, sizeof(kTransmittanceBytes)));
        check(g_kinds[1], At(kFogMapTableRva), At(kFogMapFnRva), SameCode(kFogMapFnRva, kFogMapBytes, sizeof(kFogMapBytes)));
        check(g_kinds[2], At(kBlurHTableRva), At(kBlurHFnRva), SameCode(kBlurHFnRva, kBlurHBytes, sizeof(kBlurHBytes)));
        check(g_kinds[3], At(kBlurVTableRva), At(kBlurVFnRva), SameCode(kBlurVFnRva, kBlurVBytes, sizeof(kBlurVBytes)));
        g_any = !ready.empty();
        spdlog::info("[EyeShare] (fix156) full stereo's right eye given the left eye's results of: {}{}", ready.empty() ? std::string{ "none" } : ready,
            not_ready.empty() ? std::string{} : std::format(" (not as in Starfield 1.16.236, so run in both eyes: {})", not_ready));
    }

    void SetOn(bool on) { g_on.store(on, std::memory_order_relaxed); }

    bool Skip(uintptr_t table, void* data, const void* pass_data)
    {
        if (!g_any.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) {
            return false;
        }
        Kind* k = KindOf(table);
        if (k == nullptr) {
            return false;
        }
        uint32_t key = 0;
        if (!ViewKey(data, key) || !StereoView::IsEyeView(key)) {
            return false; // (not full stereo's right eye)
        }
        RenderPassIo::PassIo io{};
        if (!RenderPassIo::ReadPassIo(pass_data, io) || io.count != io.total) {
            k->unread.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::scoped_lock _{ k->mtx };
        if (!k->noted || k->generation != RenderPassIo::g_generation.load(std::memory_order_relaxed)) {
            k->no_left.fetch_add(1, std::memory_order_relaxed);
            return false; // (the left eye's not seen this frame, or not yet: recorded on another thread)
        }
        bool same = io.count == k->count && io.total == k->total;
        for (uint32_t i = 0; same && i < io.count; ++i) {
            const auto& item = io.items[i];
            if (item.kind == RenderPassIo::kToken) {
                continue;
            }
            same = item.kind == RenderPassIo::kTexture && item.found == RenderPassIo::kFound && item.resource != nullptr && item.resource == k->resources[i] &&
                   !Reused(item);
        }
        if (!same) {
            k->differs.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        k->skipped.fetch_add(1, std::memory_order_relaxed);
        k->skipped_all.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void After(uintptr_t table, void* data, const void* pass_data)
    {
        if (!g_any.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) {
            return; // (off: nothing read on the recording threads)
        }
        Kind* k = KindOf(table);
        uint32_t key = 0;
        if (k == nullptr || !ViewKey(data, key) || !IsLeftEye(key)) {
            return;
        }
        RenderPassIo::PassIo io{};
        const bool read = RenderPassIo::ReadPassIo(pass_data, io) && io.count == io.total;
        std::scoped_lock _{ k->mtx };
        k->noted = false;
        if (!read) {
            return;
        }
        // (Only a step whose every item is a texture found, or a token: the right eye's then compared item by item.)
        for (uint32_t i = 0; i < io.count; ++i) {
            const auto& item = io.items[i];
            if (item.kind == RenderPassIo::kToken) {
                k->resources[i] = nullptr;
                continue;
            }
            if (item.kind != RenderPassIo::kTexture || item.found != RenderPassIo::kFound || item.resource == nullptr) {
                return;
            }
            // (In the graph's reused memory: the right eye may find other bytes there: not shared; logged once a kind.)
            if (Reused(item)) {
                k->reused.fetch_add(1, std::memory_order_relaxed);
                if (!k->told.exchange(true)) {
                    spdlog::info("[EyeShare] {}: its texture {} is in the render graph's reused memory: run in both eyes", k->name, i);
                }
                return;
            }
            k->resources[i] = item.resource;
        }
        k->count      = io.count;
        k->total      = io.total;
        k->generation = RenderPassIo::g_generation.load(std::memory_order_relaxed);
        k->noted      = true;
        k->left.fetch_add(1, std::memory_order_relaxed);
    }

    void Log()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!g_any.load(std::memory_order_relaxed) || now - g_logged_at < std::chrono::seconds(30)) {
            return;
        }
        g_logged_at = now;
        std::string text{};
        bool        any = false;
        for (auto& k : g_kinds) {
            if (!k.ready) {
                continue;
            }
            const uint32_t left = k.left.exchange(0), skipped = k.skipped.exchange(0), no_left = k.no_left.exchange(0), differs = k.differs.exchange(0),
                           unread = k.unread.exchange(0), reused = k.reused.exchange(0);
            any = any || left + skipped + no_left + differs + unread + reused > 0;
            text += std::format("{}{}: left eye noted {}, right eye skipped {} (run: the left's not noted {}, other textures {}, not read {}; the left's in "
                                "reused memory {})",
                text.empty() ? "" : "; ", k.name, left, skipped, no_left, differs, unread, reused);
        }
        if (any) {
            spdlog::info("[EyeShare] the last 30 s{}: {}", g_on.load() ? "" : " (off)", text);
        }
    }

    std::string Status()
    {
        if (!g_any.load()) {
            return "Sharing eye-identical steps: not available (the game's steps are not as in Starfield 1.16.236)";
        }
        uint64_t all = 0;
        for (const auto& k : g_kinds) {
            all += k.skipped_all.load();
        }
        return std::format("Sharing eye-identical steps: {}; right-eye steps skipped this session: {}", g_on.load() ? "on" : "off", all);
    }
}
