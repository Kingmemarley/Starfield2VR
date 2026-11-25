#include "DlssViews.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "CreationEngineSettings.h"
#include "FullFrame.h"
#include "StereoView.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include <nvidia/UpscalerAfrNvidiaModule.h>

namespace DlssViews
{
    namespace
    {
        constexpr const char* kSceneSetupCoded = ".?AVSceneSetupRenderPass@CreationRendererPrivate@@";
        constexpr const char* kDlssStepCoded   = ".?AVDLSSUpscaleRenderPass@CreationRendererPrivate@@";
        constexpr size_t      kSetupSlot       = 6; // (pass, the view's record): sets DLSS's constants for the view
        constexpr size_t      kStepSlot        = 7; // (pass, graph data, pass data): tags DLSS's pictures and evaluates
        // (Their functions there in 1.16.236: a table of the kind whose function is another (one of its other bases'), or another
        // version's, is left alone.)
        constexpr uintptr_t   kSetupFnRva      = 0x2923D40;
        constexpr uintptr_t   kStepFnRva       = 0x2AB6C30;
        constexpr const char* kSettingName     = "bTempMenusCanDisableUpscalers:Display";
        constexpr uintptr_t   kSettingRva      = 0x5E99928; // (1.16.236: the setting's object, checked by its name)
        constexpr int         kMaxViews        = 4;         // the world and three of the menu's (vrcore's ids)

        using SetupFn = uintptr_t (*)(void*, void*, void*, void*);
        using StepFn  = uintptr_t (*)(void*, void*, void*);

        // Each hooked kind's table and what was in its function (several tables may share a function: each its own entry).
        struct Hooked
        {
            uintptr_t table{ 0 };
            uintptr_t original{ 0 };
        };
        std::array<Hooked, 8> g_setups{};
        std::array<Hooked, 8> g_steps{};
        std::atomic<int>      g_setup_count{ 0 };
        std::atomic<int>      g_step_count{ 0 };

        // The frame's views' handles (the low 24 bits, as the game indexes its tables), by their place: the world's at 0.
        std::array<std::atomic<uint32_t>, kMaxViews> g_keys{};
        std::atomic<int>                             g_key_count{ 0 };
        std::atomic<bool>                            g_too_many{ false };

        // What the hooks saw in the frame (exchanged at each frame's start): the views' scene setups found (bits by place), those
        // not found, and the DLSS steps not found.
        std::atomic<uint32_t> g_setup_seen{ 0 };
        std::atomic<uint32_t> g_setup_unknown{ 0 };
        std::atomic<uint32_t> g_step_unknown{ 0 };
        std::atomic<uint32_t> g_step_seen{ 0 };
        int                   g_steady{ 0 };     // frames in a row with every view's scene setup found and none not found
        int                   g_unsteady{ 0 };   // ...and without
        int                   g_settled{ 0 };    // frames in a row with the frame's size settled (FullFrame)
        bool                  g_active{ false };
        int                   g_linger{ 0 };     // (frames the views keep their own ids after DLSS was turned off behind the menu)
        bool                  g_left_this_menu{ false }; // (a DLSS step of a view not found while active: not again until the world is let go)
        RE::Setting*          g_setting{ nullptr };
        bool                  g_setting_tried{ false };
        bool                  g_original_value{ true };
        bool                  g_have_original{ false };
        std::atomic<int>      g_key_logs{ 0 };     // (the scene setups' handles logged: those found, the DLSS steps', those not found)
        std::atomic<int>      g_step_logs{ 0 };
        std::atomic<int>      g_unknown_logs{ 0 };
        int                   g_switch_logs{ 0 };
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        // (fix190) A game place by its 1.16.236 rva, for this build's game version (GameCode.h); 0 when not known for it.
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool ReadU32(uintptr_t at, uint32_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ReadPointer(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        uintptr_t OriginalOf(const std::array<Hooked, 8>& hooked, int count, void* object)
        {
            uintptr_t table = 0;
            if (!ReadPointer(reinterpret_cast<uintptr_t>(object), table)) {
                return 0;
            }
            for (int i = 0; i < count && i < static_cast<int>(hooked.size()); ++i) {
                if (hooked[i].table == table) {
                    return hooked[i].original;
                }
            }
            return 0;
        }

        // A view's place among the frame's views by its handle (-1: not one of them).
        int PlaceOf(uint32_t key)
        {
            const int count = g_key_count.load(std::memory_order_acquire);
            for (int i = 0; i < count && i < kMaxViews; ++i) {
                if (g_keys[i].load(std::memory_order_relaxed) == key) {
                    return i;
                }
            }
            return -1;
        }

        // The view's scene setup: its DLSS constants set for its own id.
        uintptr_t OnSceneSetup(void* pass, void* record, void* a3, void* a4)
        {
            // (Its table not among those hooked, as cannot be: the game's own function, the one every hooked table had.)
            auto original = reinterpret_cast<SetupFn>(OriginalOf(g_setups, g_setup_count.load(std::memory_order_acquire), pass));
            if (original == nullptr) {
                original = reinterpret_cast<SetupFn>(At(kSetupFnRva));
            }
            uint32_t   key      = 0;
            int        place    = -1;
            if (record != nullptr && ReadU32(reinterpret_cast<uintptr_t>(record) + 0x24, key)) {
                key &= 0xFFFFFF;
                place = PlaceOf(key);
                // (Full stereo's right eye: always the frame's second view, also before the frame's views are known.)
                if (StereoView::IsEyeView(key)) {
                    place = 1;
                }
            }
            if (place >= 0) {
                g_setup_seen.fetch_or(1u << place);
            } else {
                g_setup_unknown.fetch_add(1);
            }
            if ((place >= 0 ? g_key_logs.fetch_add(1) < 8 : g_unknown_logs.fetch_add(1) < 8)) {
                spdlog::info("[DlssViews] a view's scene setup: its handle [{}] {}", key, place >= 0 ? fmt::format("is the frame's view {}", place)
                                                                                                : std::string{ "is not one of the frame's views" });
            }
            const int       before = UpscalerAfrNvidiaModule::set_view(place);
            const uintptr_t result = original(pass, record, a3, a4);
            UpscalerAfrNvidiaModule::set_view(before);
            return result;
        }

        // The view's DLSS step: its pictures tagged, and evaluated, with its own id.
        uintptr_t OnDlssStep(void* pass, void* data, void* pass_data)
        {
            auto original = reinterpret_cast<StepFn>(OriginalOf(g_steps, g_step_count.load(std::memory_order_acquire), pass));
            if (original == nullptr) {
                original = reinterpret_cast<StepFn>(At(kStepFnRva));
            }
            // (Its view's record, as the game finds it: the graph data's +0x108, indexed by its +0x140.)
            uint32_t  index = 0, key = 0;
            uintptr_t record = 0;
            int       place  = -1;
            if (data != nullptr && ReadU32(reinterpret_cast<uintptr_t>(data) + 0x140, index) && index < 64 &&
                ReadPointer(reinterpret_cast<uintptr_t>(data) + 0x108 + 8 * static_cast<uintptr_t>(index), record) && record != 0 &&
                ReadU32(record + 0x24, key)) {
                key &= 0xFFFFFF;
                place = PlaceOf(key);
                // (Full stereo's right eye: always the frame's second view, also before the frame's views are known.)
                if (StereoView::IsEyeView(key)) {
                    place = 1;
                }
            }
            if (place >= 0) {
                g_step_seen.fetch_or(1u << place);
            } else {
                g_step_unknown.fetch_add(1);
            }
            if ((place >= 0 ? g_step_logs.fetch_add(1) < 8 : g_unknown_logs.fetch_add(1) < 8)) {
                spdlog::info("[DlssViews] a view's DLSS step: its handle [{}] {}", key, place >= 0 ? fmt::format("is the frame's view {}", place)
                                                                                            : std::string{ "is not one of the frame's views" });
            }
            const int       before = UpscalerAfrNvidiaModule::set_view(place);
            const uintptr_t result = original(pass, data, pass_data);
            UpscalerAfrNvidiaModule::set_view(before);
            return result;
        }

        // Each table of the kind: its function changed to the detour (whatever was there called after). (fix190) The function expected
        // there is given by its address in this game version (GameCode): 0 when not known for it, and then nothing is hooked.
        int HookKind(const char* coded, size_t slot, uintptr_t function, void* detour, std::array<Hooked, 8>& hooked, std::atomic<int>& count)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            int        made = 0;
            if (function == 0) {
                return 0;
            }
            for (const uintptr_t table : utility::rtti::find_vtables(MemoryScan::mod, coded)) {
                const int at = count.load();
                if (at >= static_cast<int>(hooked.size())) {
                    break;
                }
                const uintptr_t entry    = table + slot * sizeof(uintptr_t);
                uintptr_t       original = 0;
                if (!ReadPointer(entry, original) || original == reinterpret_cast<uintptr_t>(detour)) {
                    continue;
                }
                if (original != function) {
                    spdlog::info("[DlssViews] {}: a table (rva {:x}) whose function {} is not the one expected (rva {:x}, not {:x}): left alone", coded,
                        table - base, slot, original - base, function - base);
                    continue;
                }
                hooked[at] = Hooked{ table, original };
                count.store(at + 1, std::memory_order_release); // (before the function is changed: it may be called at once)
                auto* p   = reinterpret_cast<uintptr_t*>(entry);
                DWORD old = 0;
                if (!VirtualProtect(p, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                    continue;
                }
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p), static_cast<LONG64>(reinterpret_cast<uintptr_t>(detour)));
                VirtualProtect(p, sizeof(uintptr_t), old, &old);
                ++made;
                spdlog::info("[DlssViews] {} function {} hooked (table rva {:x}, function rva {:x})", coded, slot, table - base, original - base);
            }
            return made;
        }

        // The game's setting (by its name; else at its place in 1.16.236, checked by its name).
        RE::Setting* Setting()
        {
            if (g_setting_tried) {
                return g_setting;
            }
            g_setting_tried = true;
            using T         = CreationEngineSettings::SettingType;
            for (const T type : { T::kINISetting, T::kINIPrefSetting, T::kRendererQualitySetting, T::kRendererPerfSetting }) {
                if (auto* s = CreationEngineSettings::get_setting(kSettingName, type); s != nullptr) {
                    g_setting = s;
                    break;
                }
            }
            if (g_setting == nullptr && At(kSettingRva) != 0) { // (fix190: its place not known for this game version, by its name only)
                auto* at = reinterpret_cast<RE::Setting*>(At(kSettingRva));
                __try {
                    if (at->GetKey() == kSettingName) {
                        g_setting = at;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            spdlog::info("[DlssViews] the game's setting {} {}", kSettingName, g_setting != nullptr ? "found" : "NOT found: DLSS stays off behind menus");
            return g_setting;
        }

        void SetSetting(bool menus_turn_dlss_off)
        {
            RE::Setting* s = Setting();
            if (s == nullptr) {
                return;
            }
            if (!g_have_original) {
                g_have_original  = true;
                g_original_value = s->GetValue<bool>(true);
            }
            const bool value = menus_turn_dlss_off ? g_original_value : false;
            if (s->GetValue<bool>(value) != value) {
                s->SetValue(value);
            }
        }
    }

    void Install()
    {
        const int setups = HookKind(kSceneSetupCoded, kSetupSlot, At(kSetupFnRva), reinterpret_cast<void*>(&OnSceneSetup), g_setups, g_setup_count);
        const int steps  = HookKind(kDlssStepCoded, kStepSlot, At(kStepFnRva), reinterpret_cast<void*>(&OnDlssStep), g_steps, g_step_count);
        if (setups == 0 || steps == 0) {
            spdlog::warn("[DlssViews] the views' scene setup ({}) or DLSS step ({}) could not be hooked: DLSS stays off behind menus", setups, steps);
            SetStatus("DLSS behind menus: off (the game's steps not found)");
            return;
        }
        SetStatus("DLSS behind menus: ready (each view its own, while the world is kept drawn)");
    }

    void SetViews(const std::vector<std::pair<void*, uint32_t>>& views, uint32_t world)
    {
        // (The world's first, then the menu's in their order.)
        int count = 0;
        for (const auto& [graph, handle] : views) {
            if (handle == world && count < kMaxViews) {
                g_keys[count++].store(handle & 0xFFFFFF, std::memory_order_relaxed);
            }
        }
        if (count == 0) {
            g_key_count.store(0, std::memory_order_release);
            return;
        }
        for (const auto& [graph, handle] : views) {
            if (handle != world && count < kMaxViews) {
                g_keys[count++].store(handle & 0xFFFFFF, std::memory_order_relaxed);
            }
        }
        g_too_many.store(views.size() > static_cast<size_t>(kMaxViews), std::memory_order_relaxed);
        g_key_count.store(count, std::memory_order_release);
    }

    void OnFrame(bool world_kept)
    {
        if (g_setup_count.load() == 0 || g_step_count.load() == 0) {
            return;
        }
        const uint32_t seen         = g_setup_seen.exchange(0);
        const uint32_t unknown      = g_setup_unknown.exchange(0);
        const uint32_t step_unknown = g_step_unknown.exchange(0);
        g_step_seen.exchange(0);
        const int  count = g_key_count.load();
        const bool all   = count >= 2 && !g_too_many.load() && seen == (1u << count) - 1 && unknown == 0;
        g_steady         = world_kept && all ? std::min(g_steady + 1, 1000) : 0;
        g_unsteady       = world_kept && !all ? std::min(g_unsteady + 1, 1000) : 0;
        // (The frame's size settled first (FullFrame): each change makes the game replace its DLSS object, its histories started
        // over.)
        g_settled = world_kept && FullFrame::Settled() ? std::min(g_settled + 1, 1000) : 0;
        if (!world_kept) {
            g_left_this_menu = false;
        }
        // (A DLSS step whose view was not found while each has its own: its picture would be built on the world's id. Left to the
        // game's way until the world is let go.)
        // (Likewise a scene setup whose view is not one of the frame's: its DLSS constants would go to the world's id.)
        if (g_active && (step_unknown > 0 || unknown > 0) && !g_left_this_menu) {
            g_left_this_menu = true;
            spdlog::warn("[DlssViews] {} DLSS steps and {} scene setups of a view not among the frame's views: DLSS off behind this menu again",
                step_unknown, unknown);
        }
        // On after a few steady frames; once on, kept through a short unsteady spell (a view's scene setup missing for a frame or two,
        // as when the Inventory's item changes: each turn on or off replaces the game's DLSS object); off at once when the world is let
        // go or a DLSS step's view is not known.
        const bool usable = world_kept && !g_left_this_menu && UpscalerAfrNvidiaModule::views_ready();
        const bool want   = usable && (g_active ? g_unsteady < 30 : g_steady >= 3 && g_settled >= 10);
        if (want != g_active) {
            g_active = want;
            if (g_switch_logs++ < 12) {
                spdlog::info("[DlssViews] DLSS {} behind the see-through menu ({} views)", want ? "kept on, each view its own," : "left to the game", count);
            }
            SetStatus(want ? "DLSS behind menus: on, each view its own" : "DLSS behind menus: ready");
        }
        // (Turned off: the game's setting first; the views keep their own ids a few more frames, as the game may still evaluate DLSS
        // for them in the frame now beginning, which would otherwise go to the world's id.)
        g_linger = want ? 3 : std::max(g_linger - 1, 0);
        UpscalerAfrNvidiaModule::set_views_active(want || g_linger > 0);
        SetSetting(!want);
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
