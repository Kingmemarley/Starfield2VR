#pragma once
#include "glm/glm.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameFlow {
    struct MenuSettings
    {
        float hud_scale;
        int   perspective;
    };
    struct State
    {

        struct DebugWeaponData {
            int deepLevel;
            bool type;
            float yaw{0.f}, pitch{0.f}, roll {0.f};
        } debugWeaponData{};
        struct UiData {
            int rendered_menus_count[2]{};
            int dialogue_drawn[2]{};   // the dialogue screens drawn in the frame (fix133)
            int fullscreen_drawn[2]{}; // the full-screen menus drawn in it
            int modulino{0};
        } uiData{};

        struct RayCastData {
            glm::vec3 weaponRayCastOrigin{};
            glm::vec3 weaponRayCastDestination{};
        } rayCastData{};
    };

    extern State gState;

    void renderMenu(std::string_view menuNameHash);
    // (fix189) The interface movies set up this frame so far, by their files (the F11 window's debug list): a copy, taken under the list's
    // lock (the game's interface threads add to it, the render thread empties it each frame, the F11 window reads it as it draws).
    std::vector<std::string> uiPartsDrawn();
    bool isHudMovieHash(uint32_t hash);
    bool isFullscreenMenuHash(uint32_t hash);
    // Each full-screen menu movie's viewport as the game sets it (the Scaleform hook): the area it draws in.
    void noteMenuViewport(const char* url, int bufferWidth, int bufferHeight);
    // The height of the box the open menus draw in (the game draws menus in a 16:9 box, centred, and nothing around it), when
    // it is less than the picture's: 0 otherwise.
    int menuBoxRows(int backbufferWidth, int backbufferHeight);
    // (fix173) A full-screen menu movie set up in the last moments over the whole picture (the star map: 1996x2150): menuBoxRows' 0 then
    // means "no box", not "no box known" (the game's own box is not taken for it).
    bool menuMovieFillsPicture(int backbufferWidth, int backbufferHeight);
    // The full-screen menus set up in the last moments, by their files (for the log).
    std::string shownMenus();
    // Counts the full-screen menus shown anew (opened, or another tab of one).
    unsigned menuShownSerial();
    // The full-screen menus set up in the last moments are all ones drawn by the interface alone (see-through menus: the Data
    // menu, the inventory, the pause menu, skills, crew, research), and there is one. Not the star map, ship building, crafting,
    // the main menu or a loading screen (mostly 3D, or no world behind them). (Build mode is no menu since fix175: isWorkshopShown.)
    bool menusAllowSeeThrough();
    // (fix143) The full-screen menus set up in the last moments are all ones opened from what you use in the world (the container,
    // barter, the workbenches), and there is one: the game's look kept level meanwhile (LevelView), as in play.
    bool menusKeepLevelView();
    // (fix147) The trading menu (BarterMenu) drawn in the last moments; (fix148) or the container menu (ContainerMenu: a container, your
    // ship's cargo, a companion's things), whose LB and RB switch between the two sides too.
    bool isBarterShown();
    // (fix154) The inventory (InventoryMenu) drawn in the last moments.
    bool isInventoryShown();
    // (fix169) The menus whose sideways moves the left stick makes (MenuPointer::StickSideways; a helper's research of each menu's own
    // code): their tabs on the D-pad's left and right (missions, ship crew), on LB and RB (skills, research), the almanac (pages on LB
    // and RB, categories on the D-pad), trading, a container, the ship menu (ships on LB and RB, power on the stick).
    enum SidewaysMenu : int {
        kSidewaysNone = 0,
        kSidewaysDpad,
        kSidewaysShoulders,
        kSidewaysAlmanac,
        kSidewaysTrading,
        kSidewaysContainer,
        kSidewaysShip,
        kSidewaysKinds
    };
    // Which of them is drawn now (in the last 300 ms; the latest if two are), by its movie's file name: kSidewaysNone if none.
    int stickSidewaysMenu();
    // A menu whose buttons are drawn faint, for the dark backdrop of the flat game, is open (the Inventory: its item rows are a
    // light box at a tenth of its strength): see-through, a dark backing is laid under them.
    bool menuWantsButtonBacking();
    MenuSettings getMenuSettings(std::string_view menuNameHash);
    bool isShowingMenu();
    bool isAimingDownSights();
    bool isWeaponDrawn();
    // Your weapon's state (the low 3 bits of the player's actor state 2): 0 put away, 1 about to draw, 2 drawing, 3 out, 4 about to
    // put away, 5 putting away; -1 no player.
    int weaponState();
    bool isImmovable();
    bool isControlledByAI();
    bool isInFirstPerson();
    // (fix143) Climbing a ladder now (the game's climb, PlayerTraversalState, on the player: LadderClimb).
    bool isOnLadder();
    void resetGameState();

    // A loading screen was drawn; once it has gone and gameplay is back, take...() returns true
    // once (used to put the view back at the player's head after every load).
    void noteLoadingScreen();
    bool takeRecenterAfterLoading();
    // (fix201) Diagnostics, called once a game frame: notes each loading screen's end (for the 90 s of diagnostics after it, VR's
    // m_diag_load_end_ms) and logs the movies the game drew in each second (always in those 90 s, else when the set changes).
    void diagnosticsTick();
    // When a loading screen was last drawn (an opaque time; changes with every loading screen).
    int64_t lastLoadingScreen();
    // (fix164) A loading screen (Interface/LoadingMenu.swf) was drawn within the last 0.3 s.
    bool isLoadingScreenShown();

    // The game's main menu (Interface/MainMenu.swf) was drawn within the last second.
    bool isMainMenuShowing();
    // The world behind a menu is the game's world: you are in the game (not the main menu, not loading).
    bool worldBehindMenu();

    // The ship's piloting HUD (Interface/SpaceshipHudMenu.swf) was drawn within the last 0.3 s.
    bool isPilotingShip();
    // A conversation (Interface/DialogueMenu.swf) was drawn within the last 0.3 s.
    bool isInDialogue();
    // (fix142) A conversation drawn in this frame or the one before: for what must stop as it closes (MenuPointer: the
    // next moments are play, where a mouse move turns you and a press attacks).
    bool isDialogueDrawnNow();
    // (fix142) The hand scanner (Interface/MonocleMenu.swf) was drawn within the last 0.3 s.
    bool isScannerOpen();
    // (fix173) The star map (Interface/GalaxyStarMapMenu.swf: the galaxy, a system, a planet's view) set up in the last 0.3 s (or this long).
    bool isStarMapShown(int withinMs = 300);
    // (fix173) The hand scanner drawn within this long (isScannerOpen: 0.3 s; shorter for what must end as soon as it closes).
    bool isScannerDrawnWithin(int withinMs);
    // (fix194) A full-screen menu (not the cursor) drawn within this long: a menu still counted as shown with none drawn lately is closing.
    bool isFullscreenMenuDrawnWithin(int withinMs);
    // (fix203) Character creation (Interface/ChargenMenu.swf) set up within this long: a full-screen menu shown opaque on the panel (its own
    // 16:9 box), the grips LB and RB there (HandActions::GripsAreShoulders), the soft light round the panel instead of the world as it was.
    bool isChargenShown(int withinMs = 300);
    // (fix157) The favorites menu (Interface/FavoritesMenu.swf, drawn in the HUD layer: not a menu by the count) drawn within the last 0.3 s.
    bool isFavoritesShown();
    // (fix175) Build mode (Interface/WorkshopMenu.swf: ship decorating, outpost building; a HUD-like screen, not a menu by the count, the world
    // drawn from your head as in play) drawn within the last 0.3 s (or this long). Play's hand features are held off meanwhile (each checks it
    // beside isShowingMenu), the grips are LB and RB (HandActions::GripsAreShoulders), the left trigger LT, and full stereo stands down.
    bool isWorkshopShown(int withinMs = 300);
    // (fix175) Build mode's log (each gamepad poll, with what the game gets): "[Workshop] build mode open/closed" with the buttons sent.
    void noteWorkshopPad(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger);
    // (fix203) Character creation's log (each gamepad poll, with what the game gets): "[Chargen] character creation open/closed" with the buttons
    // and stick pushes sent while it was open (did LB and RB, the D-pad and the sticks reach it).
    void noteChargenPad(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger, int16_t leftX, int16_t leftY, int16_t rightX, int16_t rightY);
    // (fix165) The buttons the mod hands to the game (each press counted, START's last time), for the seat log: the pilot's seat where
    // nothing could be done, not even a pause.
    struct ButtonsSent {
        uint32_t a{ 0 }, b{ 0 }, x{ 0 }, y{ 0 }, start{ 0 }, back{ 0 };
        int64_t  last_start_ms{ 0 };
        bool     start_with_menu{ false }; // (a menu shown as START was pressed: it closes it, no pause menu wanted)
    };
    void        noteButtonsSent(uint16_t buttons);
    ButtonsSent buttonsSent();
    // (fix157) ...drawn within the last 80 ms (this frame or the one before): an A sent for its choice only then, never into play.
    bool isFavoritesDrawnNow();
    // (fix142) The scanner's movie: drawn over the whole picture as the game laid it out (its marks are where the world camera sees
    // their objects), not shrunk or moved as the HUD is.
    bool isScannerMovieHash(uint32_t hash);

    // (fix142) A hand points at a conversation's responses now (MenuPointer): with the eyes taking turns, its movie is drawn in both
    // eyes' frames at the same place meanwhile (the game's mouse is placed for one; moved in the other eye's frames, the hover would
    // jump between two places).
    inline std::atomic<bool> dialoguePointing{false};
    // (fix144) How far the conversation fixed in the room is in front of the eye the game's mouse is placed for (metres; 0 none yet:
    // CreationEngineCameraManager, with DialogueTouch): the pointer's dot and the touch take the answers' depth from it.
    inline std::atomic<float> dialogueDepthM{0.0f};

    // Snap turn queued by the controller (radians, positive turns right); the camera hook applies it.
    inline std::atomic<float> pendingSnapYaw{0.0f};
    // (fix150) The same on a ladder, the view at the body's head (radians, positive turns right): LadderClimb turns the room's turn by it.
    inline std::atomic<float> pendingLadderYaw{0.0f};
    // (fix173) The same in a conversation (radians, positive turns right; smooth, from the right stick): the camera hook turns the room by it.
    inline std::atomic<float> pendingDialogueYaw{0.0f};
}

