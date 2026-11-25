#include "GameFlow.h"
#include "ModSettingsStore.h"
#include "RE/P/PlayerCamera.h"
#include <CreationEngine/CreationEngineSingletonManager.h>
#include <CreationEngine/WorkbenchCamera.h>
#include <mods/VR.hpp>
#include <ModSettings.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <CreationEngine/memory/GameCode.h>
#include <excpt.h>
#include <Xinput.h>

namespace GameFlow
{
    State gState{};
    auto vr = VR::get();

    const std::unordered_map<uint32_t, MenuSettings> menu_settings = {
            {         "Interface/ScopeMenu.swf"_DJB,   { 1.0f, 400 } },
            {         "Interface/ScopeMenu_LRG.swf"_DJB,   { 1.0f, 400 } },
            // (fix175) Build mode (WorkshopMenu: ship decorating, outpost building), drawn over the world as the HUD now (no menu panel): its
            // item list, categories and button hints at three quarters of the picture, centred (the 1920x1080 stage shown whole in it), not at
            // the HUD's own size (a HUDScale of 0.4 made its text a third of the game's); the HUD's own depth (perspective) kept.
            {      "Interface/WorkshopMenu.swf"_DJB,   { 0.75f, 0 } },
            {      "Interface/WorkshopMenu_LRG.swf"_DJB,   { 0.75f, 0 } }
            // (The hand scanner, MonocleMenu: not shrunk nor moved at all since fix142, CreationEngineCameraManager.)
    };

    namespace {
        // (fix181) The game's frames (each frame's start, resetGameState), from 1: a menu's movie set up in the last two of them counts as
        // shown, whatever their length (viewportShown).
        std::atomic<uint32_t> g_ui_frames{ 1 };
        // (fix189, the review) The debug list of this frame's interface movies (gStore.debugData.ui_parts) is added to on the game's
        // interface threads, emptied on the render thread and read by the F11 window on the present thread: all under this lock. It holds
        // copies of the movies' names (the game frees a movie's name when its menu closes).
        std::mutex g_ui_parts_mtx{};
    }

    std::vector<std::string> uiPartsDrawn() {
        std::scoped_lock _{ g_ui_parts_mtx };
        return gStore.debugData.ui_parts;
    }

    void resetGameState() {
        g_ui_frames.fetch_add(1, std::memory_order_relaxed); // (fix181)
        {
            std::scoped_lock _{ g_ui_parts_mtx }; // (fix189)
            gStore.debugData.ui_parts.clear();
        }
        gState.uiData.modulino++;
        gState.uiData.rendered_menus_count[gState.uiData.modulino % 2] = 0;
        gState.uiData.dialogue_drawn[gState.uiData.modulino % 2]       = 0;
        gState.uiData.fullscreen_drawn[gState.uiData.modulino % 2]     = 0;
    }

    namespace {
        std::atomic<bool>    g_recenter_pending{false};
        std::atomic<int64_t> g_last_loading_ms{0};
        std::atomic<int64_t> g_last_main_menu_ms{-100000};
        std::atomic<int64_t> g_last_ship_hud_ms{-100000};
        std::atomic<int64_t> g_last_dialogue_ms{-100000};
        std::atomic<int64_t> g_last_scanner_ms{-100000};
        std::atomic<int64_t> g_last_favorites_ms{-100000};
        std::atomic<int64_t> g_fullscreen_last_ms{-100000}; // (fix159) a full-screen menu last drawn
        std::atomic<int64_t> g_fullscreen_run_ms{-100000};  // (fix159) since when they have been drawn without a gap
        std::atomic<int64_t> g_last_slate_ms{-100000};      // (fix173) the slate's own movie last set up (each frame while it is open)
        std::atomic<int64_t> g_last_workshop_ms{-100000};   // (fix175) build mode's movie last set up (each frame while it is open)

        int64_t now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    void noteLoadingScreen() {
        g_last_loading_ms = now_ms();
        g_recenter_pending = true;
    }

    int64_t lastLoadingScreen() {
        return g_last_loading_ms.load();
    }

    bool isLoadingScreenShown() {
        return now_ms() - g_last_loading_ms.load() < 300;
    }

    bool takeRecenterAfterLoading() {
        if (!g_recenter_pending.load()) {
            return false;
        }
        // Wait until the loading screen has been gone for a moment and the game is playable.
        if (now_ms() - g_last_loading_ms.load() < 1500 || CreationEngineSingletonManager::GetPlayerRef() == nullptr || isShowingMenu()) {
            return false;
        }
        return g_recenter_pending.exchange(false);
    }

    bool isMainMenuShowing() {
        return now_ms() - g_last_main_menu_ms.load() < 1000;
    }

    bool worldBehindMenu() {
        return CreationEngineSingletonManager::GetPlayerRef() != nullptr && !isMainMenuShowing() && now_ms() - g_last_loading_ms.load() > 1500;
    }

    bool isPilotingShip() {
        return now_ms() - g_last_ship_hud_ms.load() < 300;
    }

    bool isInDialogue() {
        return now_ms() - g_last_dialogue_ms.load() < 300;
    }

    bool isScannerOpen() {
        return now_ms() - g_last_scanner_ms.load() < 300;
    }

    bool isFavoritesShown() {
        return now_ms() - g_last_favorites_ms.load() < 300;
    }

    bool isWorkshopShown(int withinMs) {
        return now_ms() - g_last_workshop_ms.load() < withinMs;
    }

    bool isFavoritesDrawnNow() {
        return now_ms() - g_last_favorites_ms.load() < 80;
    }

    bool isDialogueDrawnNow() {
        // (In this frame or the one before, whatever their length: a slow frame is no close. Within a second as well, in case the
        // frames stop.)
        const auto& drawn = gState.uiData.dialogue_drawn;
        return (drawn[0] > 0 || drawn[1] > 0) && now_ms() - g_last_dialogue_ms.load() < 1000;
    }

    bool isScannerMovieHash(uint32_t hash) {
        return hash == "Interface/MonocleMenu.swf"_DJB || hash == "Interface/MonocleMenu_LRG.swf"_DJB;
    }

    namespace {
        // (fix173) The slate's buttons drawn without the slate's own movie in the last second (never seen; the review: a sound slate still
        // playing after it closed, say): not a menu then, so the panel never comes up in play for it. (The slate's movie is set up each frame
        // while it is open, its 1024x1024 picture too, before its buttons.)
        bool isSlateButtonsAlone(uint32_t hash) {
            return (hash == "Interface/DataSlateButtons.swf"_DJB || hash == "Interface/DataSlateButtons_LRG.swf"_DJB) &&
                   now_ms() - g_last_slate_ms.load() > 1000;
        }
    }

    // The movies that lower the menu count (the HUD and HUD-like screens) and those that raise it (full-screen menus).
    bool isHudMovieHash(uint32_t hash) {
        switch (hash) {
        case "Interface/HUDMenu.gfx"_DJB:
        case "Interface/HUDMenu_LRG.gfx"_DJB:
        case "Interface/SpaceshipHudMenu.swf"_DJB:
        case "Interface/SpaceshipHudMenu_LRG.swf"_DJB:
        case "Interface/ScopeMenu.swf"_DJB:
        case "Interface/ScopeMenu_LRG.swf"_DJB:
        case "Interface/FavoritesMenu.swf"_DJB:
        case "Interface/FavoritesMenu_LRG.swf"_DJB:
        case "Interface/MonocleMenu.swf"_DJB:
        case "Interface/MonocleMenu_LRG.swf"_DJB:
        case "Interface/DialogueMenu.swf"_DJB:
        case "Interface/DialogueMenu_LRG.swf"_DJB:
        // (fix175) Build mode (ship decorating, outpost building; taken at first for a crafting menu, its category was Crafting): a HUD-like screen as
        // the hand scanner. The game keeps drawing the world there from its own camera (no 0x100 in its flags, a helper's research of the
        // exe), and places what you build where that camera looks, while you walk with the left stick. On the panel (a full-screen menu
        // before) the world behind it was the last frame of play, frozen and dimmed, and each push of the left stick (walking) turned on the
        // menu pointer's stick navigation, which kept LB, RB, LT and RT from the game and made the right trigger A (build). Now no panel: the
        // world drawn from your head as in play, the build menu in the HUD layer, play's hand features held off (isWorkshopShown).
        case "Interface/WorkshopMenu.swf"_DJB:
        case "Interface/WorkshopMenu_LRG.swf"_DJB:
            return true;
        default:
            return false;
        }
    }

    bool isFullscreenMenuHash(uint32_t hash) {
        switch (hash) {
        case "Interface/ResearchMenu.swf"_DJB:
        case "Interface/ResearchMenu_LRG.swf"_DJB:
        case "Interface/WeaponCraftingMenu.swf"_DJB:
        case "Interface/WeaponCraftingMenu_LRG.swf"_DJB:
        case "Interface/WeaponsCraftingMenu.swf"_DJB:
        case "Interface/WeaponsCraftingMenu_LRG.swf"_DJB:
        case "Interface/ArmorCraftingMenu.swf"_DJB:
        case "Interface/ArmorCraftingMenu_LRG.swf"_DJB:
        case "Interface/IndustrialCraftingMenu.swf"_DJB:
        case "Interface/IndustrialCraftingMenu_LRG.swf"_DJB:
        case "Interface/DrugsCraftingMenu.swf"_DJB:
        case "Interface/DrugsCraftingMenu.swf_LRG"_DJB: // (a typo kept: the large file is the next one)
        case "Interface/DrugsCraftingMenu_LRG.swf"_DJB:
        case "Interface/FoodCraftingMenu.swf"_DJB:
        case "Interface/FoodCraftingMenu_LRG.swf"_DJB:
        // (The container and barter menus: full-screen ones of the game (its flag 0x100: the world not drawn), fix133.)
        case "Interface/ContainerMenu.swf"_DJB:
        case "Interface/ContainerMenu_LRG.swf"_DJB:
        case "Interface/BarterMenu.swf"_DJB:
        case "Interface/BarterMenu_LRG.swf"_DJB:
        case "Interface/SpaceShipEditorMenu.swf"_DJB:
        case "Interface/SpaceShipEditorMenu_LRG.swf"_DJB:
        case "Interface/ShipCrewMenu.swf"_DJB:
        case "Interface/ShipCrewMenu_LRG.swf"_DJB:
        case "Interface/SkillsMenu.swf"_DJB:
        case "Interface/SkillsMenu_LRG.swf"_DJB:
        case "Interface/MainMenu.swf"_DJB:
        case "Interface/MainMenu_LRG.swf"_DJB:
        case "Interface/CursorMenu.swf"_DJB:
        case "Interface/CursorMenu_LRG.swf"_DJB:
        case "Interface/DataMenu.swf"_DJB:
        case "Interface/DataMenu_LRG.swf"_DJB:
        case "Interface/InventoryMenu.swf"_DJB:
        case "Interface/InventoryMenu_LRG.swf"_DJB:
        case "Interface/LoadingMenu.swf"_DJB:
        case "Interface/LoadingMenu_LRG.swf"_DJB:
        case "Interface/PauseMenu.swf"_DJB:
        case "Interface/PauseMenu_LRG.swf"_DJB:
        case "Interface/GalaxyStarMapMenu.swf"_DJB:
        case "Interface/GalaxyStarMapMenu_LRG.swf"_DJB:
        case "Interface/StarMapMenu.swf"_DJB:
        case "Interface/StarMapMenu_LRG.swf"_DJB:
        // (The Missions and Powers menus, opened from the Data menu: full-screen menus of their own.)
        case "Interface/MissionMenu.swf"_DJB:
        case "Interface/MissionMenu_LRG.swf"_DJB:
        case "Interface/PowersMenu.swf"_DJB:
        case "Interface/PowersMenu_LRG.swf"_DJB:
        // (fix163) Terminals (the game's GenesisTerminalMenu: the view was not updated while using computers; drawn as a
        // HUD piece before, the world in its 16:9 box squeezed into a band and full stereo refusing it), mission boards, notes and data
        // slates, the Data menu's Status screen; books (no 16:9 box: on the panel, not see-through). Their flags from
        // their constructors (a helper's research): terminal 0x08000203, mission board 0x08000213, slate 0x08031323, status 0x28000393,
        // security 0x00020203, book 0x00022293.
        case "Interface/GenesisTerminalMenu.swf"_DJB:
        case "Interface/GenesisTerminalMenu_LRG.swf"_DJB:
        case "Interface/MissionBoard.swf"_DJB:
        case "Interface/MissionBoard_LRG.swf"_DJB:
        case "Interface/DataSlateMenu.swf"_DJB:
        case "Interface/DataSlateMenu_LRG.swf"_DJB:
        // (fix173) The slate's buttons (Stop, Exit...): a movie of its own the slate shows with it, drawn every frame over the picture's
        // width in the 16:9 box (1996x1123). The slate's own movie sets that box only in its first frames (21:09:40.136 and .158 in the
        // test's log), then draws only its 1024x1024 slate picture (not counted): 0.3 s on no menu of the lists was set up any more, the
        // world was left to the game and the panel showed the whole picture with the game's black bars above and below its box (the
        // inspected music box, "showing rows 0 to 2150"). Counted with the slate, it keeps the box and the see-through look.
        case "Interface/DataSlateButtons.swf"_DJB:
        case "Interface/DataSlateButtons_LRG.swf"_DJB:
        // (fix175) The game's message box (a question with buttons): with build mode no longer a menu (fix175) it was drawn small in the HUD
        // layer over it, out of the pointer's reach (the review); a menu on the panel, see-through, wherever it comes.
        case "Interface/MessageBoxMenu.swf"_DJB:
        case "Interface/MessageBoxMenu_LRG.swf"_DJB:
        case "Interface/StatusMenu.swf"_DJB:
        case "Interface/StatusMenu_LRG.swf"_DJB:
        // (Not the digipick, SecurityMenu, for now: while the pointer is on a panel the sticks, triggers and shoulder buttons it uses are
        // held back from the game; the review.)
        case "Interface/BookMenu.swf"_DJB:
        case "Interface/BookMenu_LRG.swf"_DJB:
            return true;
        default:
            return false;
        }
    }

    namespace {
        // The latest viewport of each full-screen menu movie (its drawing area: the menu's box), and when it was set.
        struct MenuViewport {
            int      width{ 0 };
            int      height{ 0 };
            int64_t  ms{ 0 };
            uint32_t frame{ 0 }; // (fix181) the game's frame it was set in (g_ui_frames then; 0: none)
        };
        std::mutex                                g_menu_viewport_mtx{};
        std::unordered_map<uint32_t, MenuViewport> g_menu_viewports{};
        std::unordered_map<uint32_t, std::string>  g_menu_names{}; // (each one's file, for the log)
        std::atomic<unsigned>                      g_menu_shown_serial{ 0 }; // (a menu shown anew: menuShownSerial)
        std::atomic<int>      g_stall_kept{ 0 };      // (fix181) menus kept as shown through a long frame (checks: for the log)
        std::atomic<int64_t>  g_stall_logged_ms{ 0 };

        // (fix181) A menu's movie set up "now": in the last 300 ms, or in the game's last two frames. A long frame of the game's stops
        // every movie's set-up for its length (the Data menu's frames of 300-500 ms as its 3D pictures were made, fix180's test): by time
        // alone every menu aged out for a frame, and the panel showed the game's whole picture, the whole height, with no world behind it
        // (the Data menu still showed half a second of glitch). A menu that closes stops being set up: out after 300 ms and three frames.
        // (Under g_menu_viewport_mtx.)
        bool viewportShown(const MenuViewport& v, int64_t now)
        {
            if (now - v.ms <= 300) {
                return true;
            }
            if (v.frame == 0 || g_ui_frames.load(std::memory_order_relaxed) - v.frame > 2) {
                return false;
            }
            // (Kept by its frames only: logged at most once a second, 40 times a session.)
            const int n = g_stall_kept.fetch_add(1, std::memory_order_relaxed);
            if (n < 2000 && now - g_stall_logged_ms.load(std::memory_order_relaxed) >= 1000) {
                static std::atomic<int> lines{ 0 };
                if (lines.fetch_add(1, std::memory_order_relaxed) < 40) {
                    g_stall_logged_ms.store(now, std::memory_order_relaxed);
                    spdlog::info("[UI] (fix181) a menu's movie kept as shown through a long frame of the game's: set up {} ms ago, {} frame(s) ago "
                                 "(by time alone it would have counted as closed: the panel's see-through picture, its box and the world behind "
                                 "it kept; {} checks so far)",
                        now - v.ms, g_ui_frames.load(std::memory_order_relaxed) - v.frame, n + 1);
                }
            }
            return true;
        }
    }

    namespace {
        // (fix169) The menus whose sideways moves the left stick makes (MenuPointer::StickSideways): when a movie of each kind was last
        // drawn. Known by the file's own name, whatever its letters' case (modded copies keep the game's names).
        std::atomic<int64_t> g_sideways_ms[kSidewaysKinds]{};

        int sidewaysKindOf(const char* url) {
            std::string_view name{ url };
            if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
                name.remove_prefix(slash + 1);
            }
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.ends_with(".swf")) {
                lower.resize(lower.size() - 4);
            }
            if (lower.ends_with("_lrg")) {
                lower.resize(lower.size() - 4);
            }
            static constexpr std::pair<std::string_view, int> kKinds[] = {
                { "missionmenu", kSidewaysDpad },       { "shipcrewmenu", kSidewaysDpad },       { "skillsmenu", kSidewaysShoulders },
                { "researchmenu", kSidewaysShoulders }, { "almanacmenu", kSidewaysAlmanac },     { "bartermenu", kSidewaysTrading },
                { "containermenu", kSidewaysContainer }, { "spaceshipinfomenu", kSidewaysShip },
            };
            for (const auto& [file, kind] : kKinds) {
                if (lower == file) {
                    return kind;
                }
            }
            return kSidewaysNone;
        }
    }

    int stickSidewaysMenu() {
        const auto now  = now_ms();
        int        kind = kSidewaysNone;
        int64_t    last = 0;
        for (int k = 1; k < kSidewaysKinds; ++k) {
            const int64_t ms = g_sideways_ms[k].load(std::memory_order_relaxed);
            if (ms != 0 && now - ms <= 300 && ms > last) {
                kind = k; // (the latest drawn, if two are)
                last = ms;
            }
        }
        return kind;
    }

    void noteMenuViewport(const char* url, int bufferWidth, int bufferHeight) {
        if (url == nullptr) {
            return;
        }
        if (const int kind = sidewaysKindOf(url); kind != kSidewaysNone) {
            g_sideways_ms[kind].store(now_ms(), std::memory_order_relaxed);
        }
        const auto hash = djb2Hash(url);
        // (The ship menu, the Data menu's Ships tab: the game's file is "SpaceshipEditorMenu", not the list's "SpaceShipEditorMenu",
        // so it is not counted as a full-screen menu (counting it would put the shipyard's ship builder on the panel too; not
        // changed). Only noted as shown anew, for the log's pictures (fix128).)
        if (hash == "Interface/SpaceshipEditorMenu.swf"_DJB || hash == "Interface/SpaceshipEditorMenu_LRG.swf"_DJB) {
            static std::atomic<int64_t> ship_seen_ms{ 0 };
            if (now_ms() - ship_seen_ms.exchange(now_ms()) > 1000) {
                g_menu_shown_serial.fetch_add(1);
            }
            return;
        }
        // (Not the mouse cursor's movie: it is drawn over everything, whatever its size.)
        if (!isFullscreenMenuHash(hash) || hash == "Interface/CursorMenu.swf"_DJB || hash == "Interface/CursorMenu_LRG.swf"_DJB) {
            return;
        }
        if (isSlateButtonsAlone(hash)) { // (fix173)
            return;
        }
        // (fix163) Nor a menu's square offscreen picture (the slate draws one of 1024x1024 each frame besides its own: its box would flip
        // between the two; a helper's research).
        if (bufferWidth == bufferHeight && bufferWidth < 1280) { // (a small square only: never an eye image; the review)
            return;
        }
        std::scoped_lock _{ g_menu_viewport_mtx };
        auto& viewport = g_menu_viewports[hash];
        // (Shown anew: not set up for a while, as a menu opens or one of its tabs is chosen.)
        if (now_ms() - viewport.ms > 1000) {
            g_menu_shown_serial.fetch_add(1);
        }
        viewport = MenuViewport{ bufferWidth, bufferHeight, now_ms(), g_ui_frames.load(std::memory_order_relaxed) }; // (fix181: its frame too)
        if (g_menu_names.size() < 64) {
            g_menu_names.try_emplace(hash, url);
        }
    }

    namespace {
        bool isSeeThroughMenuHash(uint32_t hash); // (below)
    }

    int menuBoxRows(int backbufferWidth, int backbufferHeight) {
        if (backbufferWidth <= 0 || backbufferHeight <= 0) {
            return 0;
        }
        const auto now = now_ms();
        int        rows = 0;
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            // (Movies set up in the last moments, as wide as the picture: the menus showing now.)
            if (!viewportShown(v, now) || v.height <= 0) {
                continue;
            }
            // (fix164) A loading screen: its movie is set up over the whole picture, but its stage (the game's 1920x1080, shown whole) fills
            // only the picture's width, so its photo (bForcePhotoModeLoadScreen) is the 16:9 band in the middle, black above and below. The
            // panel showed the whole 1996x2150 picture ("rows 0 to 2150" in the log); now that band only (the loading
            // screens' panel trimmed to the photo band), as a menu's box.
            if (hash == "Interface/LoadingMenu.swf"_DJB || hash == "Interface/LoadingMenu_LRG.swf"_DJB) {
                rows = std::max(rows, static_cast<int>(std::lround(backbufferWidth * 9.0 / 16.0)));
                continue;
            }
            if (std::abs(v.width - backbufferWidth) > 2) {
                // (fix162) A see-through menu not yet set up at the picture's width (its first frames: its movie at the game's 1920x1080 stage
                // first, at the picture's width 70-140 ms later): its 16:9 box at once, as it will be (1996 wide: 1123 rows). The panel showed the
                // whole picture until then, the menu 0.46 m too high, then jumped down (a helper's research). Not for the others (the star map
                // fills the picture).
                // (fix163: not the Powers menu, see-through but without the box: a helper's research.)
                if (isSeeThroughMenuHash(hash) && hash != "Interface/PowersMenu.swf"_DJB && hash != "Interface/PowersMenu_LRG.swf"_DJB) {
                    rows = std::max(rows, static_cast<int>(std::lround(backbufferWidth * 9.0 / 16.0)));
                }
                continue;
            }
            rows = std::max(rows, v.height);
        }
        return rows >= backbufferHeight - 2 ? 0 : rows;
    }

    bool menuMovieFillsPicture(int backbufferWidth, int backbufferHeight) {
        const auto now = now_ms();
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (viewportShown(v, now) && std::abs(v.width - backbufferWidth) <= 2 && v.height >= backbufferHeight - 2) {
                return true;
            }
        }
        return false;
    }

    namespace {
        bool isSeeThroughMenuHash(uint32_t hash) {
            switch (hash) {
            case "Interface/DataMenu.swf"_DJB:
            case "Interface/DataMenu_LRG.swf"_DJB:
            case "Interface/InventoryMenu.swf"_DJB:
            case "Interface/InventoryMenu_LRG.swf"_DJB:
            case "Interface/PauseMenu.swf"_DJB:
            case "Interface/PauseMenu_LRG.swf"_DJB:
            case "Interface/SkillsMenu.swf"_DJB:
            case "Interface/SkillsMenu_LRG.swf"_DJB:
            case "Interface/ShipCrewMenu.swf"_DJB:
            case "Interface/ShipCrewMenu_LRG.swf"_DJB:
            case "Interface/ResearchMenu.swf"_DJB:
            case "Interface/ResearchMenu_LRG.swf"_DJB:
            // (Their own painted backgrounds are taken out while they are see-through: MenuClean.)
            case "Interface/MissionMenu.swf"_DJB:
            case "Interface/MissionMenu_LRG.swf"_DJB:
            case "Interface/PowersMenu.swf"_DJB:
            case "Interface/PowersMenu_LRG.swf"_DJB:
            // (fix133: the container, barter and the workbenches, as the Inventory; their painted backgrounds taken out: MenuClean.)
            case "Interface/ContainerMenu.swf"_DJB:
            case "Interface/ContainerMenu_LRG.swf"_DJB:
            case "Interface/BarterMenu.swf"_DJB:
            case "Interface/BarterMenu_LRG.swf"_DJB:
            case "Interface/WeaponsCraftingMenu.swf"_DJB:
            case "Interface/WeaponsCraftingMenu_LRG.swf"_DJB:
            case "Interface/ArmorCraftingMenu.swf"_DJB:
            case "Interface/ArmorCraftingMenu_LRG.swf"_DJB:
            case "Interface/DrugsCraftingMenu.swf"_DJB:
            case "Interface/DrugsCraftingMenu_LRG.swf"_DJB:
            case "Interface/IndustrialCraftingMenu.swf"_DJB:
            case "Interface/IndustrialCraftingMenu_LRG.swf"_DJB:
            case "Interface/FoodCraftingMenu.swf"_DJB:
            case "Interface/FoodCraftingMenu_LRG.swf"_DJB:
            // (fix163) Terminals, mission boards, notes and slates (drawn by the interface alone, in the 16:9 box), the Status screen (its 3D
            // view of you cut out, as the inventory's).
            case "Interface/GenesisTerminalMenu.swf"_DJB:
            case "Interface/GenesisTerminalMenu_LRG.swf"_DJB:
            case "Interface/MissionBoard.swf"_DJB:
            case "Interface/MissionBoard_LRG.swf"_DJB:
            case "Interface/DataSlateMenu.swf"_DJB:
            case "Interface/DataSlateMenu_LRG.swf"_DJB:
            case "Interface/DataSlateButtons.swf"_DJB: // (fix173: the slate's buttons, with it)
            case "Interface/DataSlateButtons_LRG.swf"_DJB:
            case "Interface/MessageBoxMenu.swf"_DJB: // (fix175)
            case "Interface/MessageBoxMenu_LRG.swf"_DJB:
            case "Interface/StatusMenu.swf"_DJB:
            case "Interface/StatusMenu_LRG.swf"_DJB:
                return true;
            default:
                return false;
            }
        }
    }

    namespace {
        // (fix143) The menus opened from what you use in the world, with the view kept level while they are open (menusKeepLevelView): the
        // container, barter and the workbenches (the research station, the weapon, armour, chemistry and industrial workbenches, the
        // cooking station). (fix142 showed them in stereo, the panel in front of the head: only the view was meant not to move.)
        bool isLevelViewMenuHash(uint32_t hash) {
            switch (hash) {
            case "Interface/ContainerMenu.swf"_DJB:
            case "Interface/ContainerMenu_LRG.swf"_DJB:
            case "Interface/BarterMenu.swf"_DJB:
            case "Interface/BarterMenu_LRG.swf"_DJB:
            case "Interface/ResearchMenu.swf"_DJB:
            case "Interface/ResearchMenu_LRG.swf"_DJB:
            case "Interface/WeaponsCraftingMenu.swf"_DJB:
            case "Interface/WeaponsCraftingMenu_LRG.swf"_DJB:
            case "Interface/ArmorCraftingMenu.swf"_DJB:
            case "Interface/ArmorCraftingMenu_LRG.swf"_DJB:
            case "Interface/DrugsCraftingMenu.swf"_DJB:
            case "Interface/DrugsCraftingMenu_LRG.swf"_DJB:
            case "Interface/IndustrialCraftingMenu.swf"_DJB:
            case "Interface/IndustrialCraftingMenu_LRG.swf"_DJB:
            case "Interface/FoodCraftingMenu.swf"_DJB:
            case "Interface/FoodCraftingMenu_LRG.swf"_DJB:
            // (fix163) Terminals, mission boards, notes and slates, and books: used in the world too.
            case "Interface/GenesisTerminalMenu.swf"_DJB:
            case "Interface/GenesisTerminalMenu_LRG.swf"_DJB:
            case "Interface/MissionBoard.swf"_DJB:
            case "Interface/MissionBoard_LRG.swf"_DJB:
            case "Interface/DataSlateMenu.swf"_DJB:
            case "Interface/DataSlateMenu_LRG.swf"_DJB:
            case "Interface/DataSlateButtons.swf"_DJB: // (fix173: the slate's buttons, with it)
            case "Interface/DataSlateButtons_LRG.swf"_DJB:
            case "Interface/MessageBoxMenu.swf"_DJB: // (fix175)
            case "Interface/MessageBoxMenu_LRG.swf"_DJB:
            case "Interface/BookMenu.swf"_DJB:
            case "Interface/BookMenu_LRG.swf"_DJB:
                return true;
            default:
                return false;
            }
        }
    }

    bool menusKeepLevelView() {
        const auto now   = now_ms();
        int        shown = 0;
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (!viewportShown(v, now)) {
                continue;
            }
            if (!isLevelViewMenuHash(hash)) {
                return false;
            }
            ++shown;
        }
        return shown > 0;
    }

    bool isBarterShown() {
        const auto now = now_ms();
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (viewportShown(v, now) && (hash == "Interface/BarterMenu.swf"_DJB || hash == "Interface/BarterMenu_LRG.swf"_DJB ||
                                      hash == "Interface/ContainerMenu.swf"_DJB || hash == "Interface/ContainerMenu_LRG.swf"_DJB)) {
                return true;
            }
        }
        return false;
    }

    bool isInventoryShown() {
        const auto now = now_ms();
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (viewportShown(v, now) && (hash == "Interface/InventoryMenu.swf"_DJB || hash == "Interface/InventoryMenu_LRG.swf"_DJB)) {
                return true;
            }
        }
        return false;
    }

    bool menusAllowSeeThrough() {
        const auto now   = now_ms();
        int        shown = 0;
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (!viewportShown(v, now)) {
                continue;
            }
            if (!isSeeThroughMenuHash(hash)) {
                return false;
            }
            ++shown;
        }
        return shown > 0;
    }

    bool menuWantsButtonBacking() {
        const auto now = now_ms();
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            // (The container's and barter's item rows are the Inventory's: faint, made for a dark backdrop, fix133.)
            if (viewportShown(v, now) && (hash == "Interface/InventoryMenu.swf"_DJB || hash == "Interface/InventoryMenu_LRG.swf"_DJB ||
                                      hash == "Interface/ContainerMenu.swf"_DJB || hash == "Interface/ContainerMenu_LRG.swf"_DJB ||
                                      hash == "Interface/BarterMenu.swf"_DJB || hash == "Interface/BarterMenu_LRG.swf"_DJB)) {
                return true;
            }
        }
        return false;
    }

    void renderMenu(std::string_view menuNameHash) {
        const auto hash = djb2Hash(menuNameHash.data());
        if (hash == "Interface/LoadingMenu.swf"_DJB || hash == "Interface/LoadingMenu_LRG.swf"_DJB) {
            noteLoadingScreen();
        }
        if (hash == "Interface/MainMenu.swf"_DJB || hash == "Interface/MainMenu_LRG.swf"_DJB) {
            g_last_main_menu_ms = now_ms();
        }
        if (hash == "Interface/SpaceshipHudMenu.swf"_DJB || hash == "Interface/SpaceshipHudMenu_LRG.swf"_DJB) {
            g_last_ship_hud_ms = now_ms();
        }
        if (hash == "Interface/DialogueMenu.swf"_DJB || hash == "Interface/DialogueMenu_LRG.swf"_DJB) {
            g_last_dialogue_ms = now_ms();
        }
        if (isScannerMovieHash(hash)) {
            g_last_scanner_ms = now_ms();
        }
        if (hash == "Interface/FavoritesMenu.swf"_DJB || hash == "Interface/FavoritesMenu_LRG.swf"_DJB) {
            g_last_favorites_ms = now_ms(); // (fix157)
        }
        if (hash == "Interface/DataSlateMenu.swf"_DJB || hash == "Interface/DataSlateMenu_LRG.swf"_DJB) {
            g_last_slate_ms = now_ms(); // (fix173)
        }
        if (hash == "Interface/WorkshopMenu.swf"_DJB || hash == "Interface/WorkshopMenu_LRG.swf"_DJB) {
            g_last_workshop_ms = now_ms(); // (fix175)
        }
        if (isSlateButtonsAlone(hash)) {
            // (fix173: the slate's buttons without the slate: counted neither way.)
        } else if (isHudMovieHash(hash)) {
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]--;
            if (hash == "Interface/DialogueMenu.swf"_DJB || hash == "Interface/DialogueMenu_LRG.swf"_DJB) {
                gState.uiData.dialogue_drawn[gState.uiData.modulino % 2]++;
            }
        } else if (isFullscreenMenuHash(hash)) {
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]++;
            // (Not the cursor: the dialogue has it too. A full-screen menu drawn is a menu even with the HUD still drawn: lastMenuCount. Build
            // mode, kept out of this since fix158, is a HUD-like screen since fix175: not counted here at all.)
            if (hash != "Interface/CursorMenu.swf"_DJB && hash != "Interface/CursorMenu_LRG.swf"_DJB) {
                gState.uiData.fullscreen_drawn[gState.uiData.modulino % 2]++;
                // (fix159) When full-screen menus were last drawn, and since when they have been (a run: no gap over 0.3 s).
                const int64_t at = now_ms();
                if (at - g_fullscreen_last_ms.load() > 300) {
                    g_fullscreen_run_ms.store(at);
                }
                g_fullscreen_last_ms.store(at);
                // (fix179) Its name kept a few seconds, for the menu panel's lines as a menu opens and closes (the log names the menu).
                VR::get()->m_d3d12.note_menu_movie(hash, menuNameHash);
            }
        }
        {
            std::scoped_lock _{ g_ui_parts_mtx }; // (fix189: a copy of its name, under the list's lock)
            gStore.debugData.ui_parts.emplace_back(menuNameHash);
        }

        // Log each interface movie the first time it is drawn, so menu detection can be checked.
        static std::mutex seen_mtx{};
        static std::unordered_set<uint32_t> seen{};
        {
            std::scoped_lock _{seen_mtx};
            if (seen.insert(djb2Hash(menuNameHash.data())).second) {
                spdlog::info("[UI] first drawn: {} (menu count this frame {})", menuNameHash,
                    gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]);
            }
        }
    }

    namespace {
        // The last whole frame's menu count. A dialogue screen still drawn under a full-screen menu opened from it (barter, a
        // companion's things in the container menu) does not lower it: the menu counts (fix133).
        int lastMenuCount() {
            const int slot  = (gState.uiData.modulino + 1) % 2;
            const int count = gState.uiData.rendered_menus_count[slot];
            // (fix158) Nor the HUD's last fade-out frames under a full-screen menu just opened (the pause menu: the HUD still drawn its
            // first few frames, the count 0, the panel down and up again and the world kept behind it lost: a brief
            // glitch of half a second when pausing; a helper's research): a full-screen menu drawn counts as a menu.
            if (gState.uiData.fullscreen_drawn[slot] > 0) {
                return std::max(count + gState.uiData.dialogue_drawn[slot], 1);
            }
            // (fix159) Nor a frame the game skips drawing a menu it has just opened (it sets the menu's movie up, then draws nothing of it for
            // a frame or more, the star map while it loads: the panel went down a frame and came back dark, a menu still
            // glitching for a second as it opened; a helper's research: gaps of 52-231 ms): a full-screen menu drawn in the last 0.3 s, its run begun under
            // half a second ago, still counts. (Tighter than first planned: a menu closed quickly is not held over play; the review.)
            if (const int64_t now = now_ms(); count < 1 && now - g_fullscreen_last_ms.load() < 300 && now - g_fullscreen_run_ms.load() < 500) {
                return 1;
            }
            return count;
        }
    }

    MenuSettings getMenuSettings(std::string_view menuUrl) {
        auto hash = djb2Hash(menuUrl.data());
        MenuSettings settings = {gStore.hudSettings.hudScale, gStore.hudSettings.perspective };
        auto         it       = menu_settings.find(hash);
        if (it != menu_settings.end()) {
            settings.hud_scale = it->second.hud_scale;
            settings.perspective += it->second.perspective;
        }
        auto isMenu = lastMenuCount() >= 0;
        if(isMenu) {
            settings.perspective = 0;
        }
        return settings;
    }

    bool isShowingMenu() {
        if (ModSettings::g_debug.neverFlatScreenMenus) {
            return false;
        }
        const auto count = lastMenuCount();
        // The HUD lowers the count and full-screen menus raise it. The original rule treated 0
        // (no HUD drawn) as a menu, but in this game version that was true during normal play,
        // which turned off head tracking and the per-eye projection. Only a menu counts now.
        if (ModSettings::g_debug.originalMenuDetection) {
            return count >= 0;
        }
        if (count > 0) {
            return true;
        }
        // Nothing drawn and no player yet (main menu, loading a save): keep the flat screen.
        return count == 0 && CreationEngineSingletonManager::GetPlayerRef() == nullptr;
    }

    bool isAimingDownSights() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        return p_player && p_player->IsInIronSights();
    }
    bool isWeaponDrawn() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        return p_player && p_player->IsWeaponDrawn();
    }
    int weaponState() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        return p_player ? static_cast<int>(p_player->actorState2 & 7) : -1;
    }
    bool isImmovable() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        if(!p_player) {
            return true;
        }
        return p_player->Immovable();
    }
    bool isControlledByAI() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        if(!p_player) {
            return false;
        }
        return p_player->ControlledByAI();
    }
    bool isInFirstPerson() {
        auto p_camera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
        return p_camera && p_camera->IsInFirstPerson();
    }

    namespace {
        // (fix143) The player's climb: PlayerTraversalState at player+0x10A8 (its type table at 0x144CC2180), set while climbing a ladder
        // (Starfield 1.16.236: read by the climb's update, 0x141A33A17; the climb's start, 0x141A32870, sets it and tells a climb by it
        // alone). (fix145) Any one there counts with the camera in its third-person state (as for a climb: its type table noted for the
        // log, a ladder's never seen yet, fix144's test), else only PlayerTraversalState's.
        constexpr uintptr_t kTraversalTableRva = 0x4CC2180; // (fix190) PlayerTraversalState's type table, by its 1.16.236 rva (GameCode)
        bool ReadTraversal(uintptr_t player, uintptr_t& table) {
            __try {
                const uintptr_t state = *reinterpret_cast<const uintptr_t*>(player + 0x10A8);
                table = state != 0 ? *reinterpret_cast<const uintptr_t*>(state) : 0;
                return state != 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    bool isOnLadder() {
        auto* player = CreationEngineSingletonManager::GetPlayerRef();
        static const uintptr_t base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        uintptr_t table = 0;
        const bool there = player != nullptr && base != 0 && ReadTraversal(reinterpret_cast<uintptr_t>(player), table);
        const bool expected = table != 0 && table == GameCode::At(kTraversalTableRva); // (fix190: 0 when not known for this game version)
        const bool on = there && (expected || WorkbenchCamera::CameraStateIndex() == 20);
        static std::atomic<uintptr_t> told{ 0 };
        if (there && table != 0 && told.exchange(table) != table) {
            spdlog::info("[Ladder] the player's climb (player+0x10A8): its type table rva {:x} ({}; the camera in state {})", table - base,
                expected ? "PlayerTraversalState, as expected" : "not PlayerTraversalState's", WorkbenchCamera::CameraStateIndex());
        }
        return on;
    }
}

namespace GameFlow {
    unsigned menuShownSerial() { return g_menu_shown_serial.load(); }

    std::string shownMenus() {
        const auto  now = now_ms();
        std::string names{};
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (!viewportShown(v, now)) {
                continue;
            }
            const auto it = g_menu_names.find(hash);
            std::string name = it != g_menu_names.end() ? it->second : std::string{ "?" };
            if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) {
                name = name.substr(slash + 1);
            }
            names += (names.empty() ? "" : ", ") + name;
        }
        return names.empty() ? std::string{ "none" } : names;
    }
}

namespace GameFlow {
    namespace {
        std::mutex  g_buttons_mtx{};
        ButtonsSent g_buttons{};
        uint16_t    g_buttons_was{ 0 };
    }

    // (fix165) Each press (the button down now, not the call before) counted; called once per XInput poll with what the game gets.
    void noteButtonsSent(uint16_t buttons) {
        std::scoped_lock _{ g_buttons_mtx };
        const uint16_t pressed = static_cast<uint16_t>(buttons & ~g_buttons_was);
        g_buttons_was          = buttons;
        g_buttons.a += (pressed & XINPUT_GAMEPAD_A) != 0;
        g_buttons.b += (pressed & XINPUT_GAMEPAD_B) != 0;
        g_buttons.x += (pressed & XINPUT_GAMEPAD_X) != 0;
        g_buttons.y += (pressed & XINPUT_GAMEPAD_Y) != 0;
        g_buttons.back += (pressed & XINPUT_GAMEPAD_BACK) != 0;
        if ((pressed & XINPUT_GAMEPAD_START) != 0) {
            ++g_buttons.start;
            g_buttons.last_start_ms   = now_ms();
            g_buttons.start_with_menu = isShowingMenu();
        }
    }

    ButtonsSent buttonsSent() {
        std::scoped_lock _{ g_buttons_mtx };
        return g_buttons;
    }
}

namespace GameFlow {
    // (fix173) The star map's movie set up in the last moments (as isInventoryShown: its viewport noted each frame it is drawn). Its planet
    // view is inside the same movie (the log shows no other while it is open). The grips are LB and RB there (ModConfig, HandActions).
    bool isStarMapShown(int withinMs) {
        const auto now = now_ms();
        std::scoped_lock _{ g_menu_viewport_mtx };
        for (const auto& [hash, v] : g_menu_viewports) {
            if (now - v.ms <= withinMs && (hash == "Interface/GalaxyStarMapMenu.swf"_DJB || hash == "Interface/GalaxyStarMapMenu_LRG.swf"_DJB)) {
                return true;
            }
        }
        return false;
    }

    bool isScannerDrawnWithin(int withinMs) {
        return now_ms() - g_last_scanner_ms.load() < withinMs;
    }
}

namespace GameFlow {
    namespace {
        // (fix175) Build mode's own log: what the game got while it was open (each press counted, the triggers past the game's 30), and the
        // eyes' mode while building.
        struct WorkshopPad {
            bool        open{ false };
            int64_t     since_ms{ 0 };
            uint16_t    was{ 0 };
            bool        lt_was{ false }, rt_was{ false };
            uint32_t    a{ 0 }, b{ 0 }, x{ 0 }, y{ 0 }, lb{ 0 }, rb{ 0 }, lt{ 0 }, rt{ 0 }, up{ 0 }, down{ 0 }, left{ 0 }, right{ 0 }, view{ 0 }, rs{ 0 }, ls{ 0 };
            const char* eyes{ "" };
            int         logs{ 0 };
        };
        WorkshopPad g_workshop_pad{}; // (the gamepad poll's own)

        const char* EyeModeNow() {
            const auto& v = VR::get();
            if (v == nullptr) {
                return "not known";
            }
            return v->is_native() ? "full stereo" : v->is_mono() ? "mono" : v->is_aer() ? "AER" : "stereo, the eyes taking turns";
        }
    }

    void noteWorkshopPad(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger) {
        auto&      w    = g_workshop_pad;
        const bool open = isWorkshopShown();
        const auto now  = now_ms();
        if (open && !w.open) {
            const auto&  v      = VR::get();
            const bool   stands = v != nullptr && v->full_stereo_requested();
            const char*  eyes   = EyeModeNow();
            const int    logs   = w.logs;
            w                   = WorkshopPad{};
            w.logs              = logs;
            w.open              = true;
            w.since_ms          = now;
            w.was               = buttons; // (what is already held as it opens is no press in it)
            w.lt_was            = leftTrigger > 30;
            w.rt_was            = rightTrigger > 30;
            w.eyes              = eyes;
            if (w.logs < 40) {
                ++w.logs;
                spdlog::info("[Workshop] (fix175) build mode open: no menu panel, the world drawn from your head as in play, its menu in the HUD "
                             "layer (three quarters of the picture); the eyes in play: {}{}; the grips tap LB and RB{}, the left trigger is LT, the "
                             "right RT; play's hand features held off",
                    eyes, stands ? " (full stereo stands down while building: the eyes take turns, one view drawn a frame as in mono)" : "",
                    gStore.internalSettings.scannerGrips && !gStore.internalSettings.alternativeJoyLayout ? "" : " (not: ScannerGrips off or the alternative layout)");
            }
            return;
        }
        if (!open && w.open) {
            w.open = false;
            if (w.logs < 40) {
                ++w.logs;
                spdlog::info("[Workshop] (fix175) build mode closed ({:.1f} s; the eyes while building: {}; buttons sent: A {}, B {}, X {}, Y {}, LB {}, "
                             "RB {}, LT {}, RT {}, D-pad up {} down {} left {} right {}, View {}, right stick click {}, left stick click {}; play's hand "
                             "features held off meanwhile: grabs and the gloves, palm powers, grenades, melee strikes and throws, the temples, the left "
                             "weapon, shoves and holds, the hand's pick, the favorites wheel)",
                    (now - w.since_ms) / 1000.0, w.eyes, w.a, w.b, w.x, w.y, w.lb, w.rb, w.lt, w.rt, w.up, w.down, w.left, w.right, w.view, w.rs, w.ls);
            }
            return;
        }
        if (!open) {
            return;
        }
        // (The eyes' mode after the first second: the switch for building made by then.)
        if (now - w.since_ms > 1000) {
            w.eyes = EyeModeNow();
        }
        const uint16_t pressed = static_cast<uint16_t>(buttons & ~w.was);
        w.was                  = buttons;
        w.a += (pressed & XINPUT_GAMEPAD_A) != 0;
        w.b += (pressed & XINPUT_GAMEPAD_B) != 0;
        w.x += (pressed & XINPUT_GAMEPAD_X) != 0;
        w.y += (pressed & XINPUT_GAMEPAD_Y) != 0;
        w.lb += (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        w.rb += (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        w.up += (pressed & XINPUT_GAMEPAD_DPAD_UP) != 0;
        w.down += (pressed & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        w.left += (pressed & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        w.right += (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        w.view += (pressed & XINPUT_GAMEPAD_BACK) != 0;
        w.rs += (pressed & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        w.ls += (pressed & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        const bool lt = leftTrigger > 30, rt = rightTrigger > 30;
        w.lt += lt && !w.lt_was;
        w.rt += rt && !w.rt_was;
        w.lt_was = lt;
        w.rt_was = rt;
    }
}
