#include "ModConfig.h"
#include "Mods.hpp"
#include <mods/VR.hpp>
#include <mods/VRConfig.hpp>
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include <nvidia/UpscalerAfrNvidiaModule.h>

#include "ModSettings.h"
#include "CreationEngine/CreationEngineEntry.h"
#include "CreationEngine/GameSettingsComponent.h"
#include "CreationEngine/models/ModSettingsStore.h"
#include "CreationEngine/models/GameFlow.h"
#include "CreationEngine/ImpactMelee.h"
#include "CreationEngine/HandActions.h"
#include "CreationEngine/TempleGestures.h"
#include "CreationEngine/HandGrab.h"
#include "CreationEngine/body/BodyIK.h"
#include "CreationEngine/LeftWeapon.h"
#include "CreationEngine/FavoritesWheel.h"
#include "CreationEngine/MeleeThrow.h"
#include "CreationEngine/MenuPointer.h"
#include "CreationEngine/LadderClimb.h"
#include "CreationEngine/Climbing.h"
#include "CreationEngine/Physical.h"
#include "CreationEngine/Haptics.h"

namespace ModSettings {
    // HudScale g_hudScale;
    // CrosshairTranslate g_crosshairTranslate;
    // AW2InternalSettings g_aw2_settings;
    // AW2GameFlow g_game_state;
    // DebugAndCalibration g_debugAndCalibration;
}

namespace {
    // (fix173) LB and RB from the thumbrests and from the grips' taps in the hand scanner and the star map. The game takes LB and RB down
    // together as your power (powers could fire while using the scanner: the right thumbrest's RB was sent whatever else was,
    // and a left thumb touching its rest then made LB + RB), a tap of LB as the scanner on or off, LB held over 0.4 s as the flashlight
    // (the left temple's now), RB in the scanner as its surface map (a star map opened 0.65 s after a scanner tap in a test's log: the
    // right thumb resting there, its RB back as the temple's LB ended). So: never both at once from these (the one that comes second waits
    // until the other is let go, and a moment more; at most a second, else dropped); the left thumbrest's LB only a tap (kThumbTap as the
    // thumb touches, let go then even if it rests on); the right thumbrest's RB never in the scanner, and once taken away (the scanner, a
    // temple's LB, a grenade's hold) not back until the thumb leaves its rest.
    struct Shoulders {
        using Clock = std::chrono::steady_clock;
        Clock::time_point lb_sent{}, rb_sent{}; // the last reports that handed LB / RB to the game
        bool              lb_in_last{ false }, rb_in_last{ false };
        // The left thumbrest's tap.
        bool              lb_touch_was{ false };
        bool              lb_pending{ false }; // touched, waiting for RB to be let go
        Clock::time_point lb_pending_at{};
        bool              lb_on{ false };
        Clock::time_point lb_until{};
        bool              lb_menu{ false }; // (fix192, the review) this tap was on while a menu was shown: it ends as the menu does
        // The right thumbrest's hold.
        bool              rb_touch_was{ false };
        bool              rb_pending{ false }; // touched, waiting for LB to be let go
        Clock::time_point rb_pending_at{};
        bool              rb_on{ false };
        bool              rb_cut{ false }; // taken away: not back until the thumb leaves
        bool              rb_menu{ false }; // (fix192) this RB was on while a menu was shown: it ends as the menu does
        Clock::time_point rb_touched_at{};  // (fix192, the review) when the thumb was last on the rest (a cut ends only once it has been off a while)
        // (fix194) The touches that start just after a menu closed (kAfterMenu): the thumb coming back from the button that closed it.
        // (The review: a "menu" here is any place the face buttons close: a menu, a conversation, build mode, the hand scanner, the mod's own.)
        bool              menu_was{ false };      // such a place was shown at the last poll
        Clock::time_point menu_closed_at{};       // when the last one closed
        bool              lb_after_menu{ false }; // the left thumb's touch began just after a menu closed: no LB until it leaves a while
        Clock::time_point lb_touched_at{};        // when the left thumb was last on its rest
        int               after_menu_logs{ 0 };
        bool              late_logged{ false };   // (the review) the first fresh RB touch 0.5-1.5 s after a close logged (for tuning kAfterMenu)
        int               late_logs{ 0 };
        // The grips' taps (0 the left grip's LB, 1 the right grip's RB).
        bool              grip_was[2]{ false, false };
        bool              grip_queued[2]{ false, false };
        Clock::time_point grip_queued_at[2]{};
        Clock::time_point grip_until[2]{};
        bool              left_waits{ false }; // (fix175) the left grip's LB queued in the star map or build mode: it waits for a combo first
        int               combo_logs{ 0 };
        int               tap_logs{ 0 };
        int               wait_logs{ 0 };
        int               guard_logs{ 0 };
        int               scanner_logs{ 0 };
        int               menu_cut_logs{ 0 };   // (fix192)
        int               filter_cut_logs{ 0 }; // (fix192)
        int               grip_cut_logs{ 0 };   // (fix192)
        int               lb_cut_logs{ 0 };     // (fix192, the review)
    };
    Shoulders g_shoulders{};

    // (fix179) The Y press held back while only the left hand's weapon is out (below): its state, reset while the mod's menu is drawn (the
    // review: a press held back as the menu opened gave a stray X tap, your weapon drawn, as it closed).
    // (fix194) Named for the old layout, where Touch Y was the reload button: it is whichever button sends the game's X (reload; held, your
    // weapons put away or drawn), Touch X with the face buttons as printed.
    struct YHold {
        bool                                  was_down{ false };
        bool                                  held_back{ false };
        bool                                  done{ false };
        std::chrono::steady_clock::time_point since{};
        std::chrono::steady_clock::time_point tap_until{};
    };
    YHold g_y_hold{};
    constexpr auto kShoulderGap = std::chrono::milliseconds(50);   // the other button out of the reports this long before a new press
    constexpr auto kRbAfterLb   = std::chrono::milliseconds(400);  // ...the right thumbrest's RB after an LB (the scanner shows by then)
    constexpr auto kThumbTap    = std::chrono::milliseconds(200);  // the left thumbrest's LB this long (a tap, never the flashlight's hold)
    constexpr auto kGripTap     = std::chrono::milliseconds(90);   // a grip's LB or RB tap in the scanner and the star map
    constexpr auto kLeftGripWait = std::chrono::milliseconds(150); // (fix175) the left grip's LB in the star map and build mode: a combo may join first
    constexpr auto kWaitAtMost  = std::chrono::milliseconds(1000); // a press held back longer than this is dropped
    constexpr auto kThumbLeft   = std::chrono::milliseconds(100);  // (fix192, the review) the right thumb off its rest this long: a cut RB ends
    constexpr int  kPlaceDrawnMs = 100; // (fix192, the review) the scanner or build mode drawn this lately: the right grip's RB tap may run
    constexpr auto kAfterMenu   = std::chrono::milliseconds(500);  // (fix194) a thumbrest touch begun this soon after a menu closed is no press

    // (fix189) The flat screen on or off (the headset's picture off: the game on the monitor), at most every half second, logged.
    void ToggleFlatScreen(const char* by)
    {
        static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto  now  = std::chrono::steady_clock::now();
        if (now - last <= std::chrono::milliseconds(500)) {
            return;
        }
        last             = now;
        auto& flatScreen = ModSettings::g_internalSettings.forceFlatScreen;
        flatScreen       = !flatScreen;
        spdlog::info("[VR] (fix189) the flat screen turned {} by {}", flatScreen ? "on (the headset's picture off)" : "off (the headset's picture back)", by);
    }

    // (fix189) F12: Steam's screenshot key too, and Windows reports it whatever window has the keyboard, so it counts only while the game's
    // window is in front. A press is the key found down when it was up at the last look (not Windows' "pressed since the last call" bit,
    // which kept a press made in another window, or while this mod's menu was open, for later). Looked at before the controllers' idle test
    // (the review: with the headset off, no buttons for 30 s, the mod stopped before it, and F12 is just what one presses then to play on
    // the monitor).
    void FlatScreenKey()
    {
        static bool was  = false;
        const bool  down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        const bool  hit  = down && !was;
        was              = down;
        if (!hit) {
            return;
        }
        const HWND window = g_framework != nullptr ? g_framework->get_window() : nullptr;
        const HWND front  = GetForegroundWindow();
        if (window != nullptr && front != nullptr && (front == window || GetAncestor(front, GA_ROOT) == GetAncestor(window, GA_ROOT))) {
            ToggleFlatScreen("F12 (the game's window in front)");
        }
    }
}

Mods::Mods() {
    m_mods.emplace_back(VRConfig::get());
    m_mods.emplace_back(VR::get());
    m_mods.emplace_back(UpscalerAfrNvidiaModule::Get());
#ifdef _DEBUG
    m_mods.emplace_back(ShaderDebugOverlay::Get());
#endif
    m_mods.emplace_back(CreationEngineEntry::Get());
    m_mods.emplace_back(GameSettingsComponent::Get());
}




void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
//    ZoneScopedN(__FUNCTION__);
    auto pXinputGamepad = (XINPUT_GAMEPAD*)state;

    const auto now = std::chrono::steady_clock::now();

    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }

        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }

    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;

    if (is_using_controllers_within(std::chrono::minutes(5)))
        if (is_using_controllers_within(std::chrono::minutes(5)))
        {
            *retval = ERROR_SUCCESS;

        }

    // The menu pointer (a game menu on its panel): the hand pointing at it clicks with its trigger, which is then not the
    // gamepad's (bit 0: the left controller's, bit 1: the right one's).
    const int pointer_triggers = MenuPointer::Update(is_action_active(m_action_trigger, get_left_joystick()),
        is_action_active(m_action_trigger, get_right_joystick()), is_using_controllers() && !g_framework->is_drawing_ui());

    FlatScreenKey(); // (fix189: before the idle test)

    if (!is_using_controllers()) {
        return;
    }

//    // Clear button state for VR controllers
//    if (is_using_controllers_within(std::chrono::seconds(5))) {
//        state->Gamepad.wButtons = 0;
//        state->Gamepad.bLeftTrigger = 0;
//        state->Gamepad.bRightTrigger = 0;
//        state->Gamepad.sThumbLX = 0;
//        state->Gamepad.sThumbLY = 0;
//        state->Gamepad.sThumbRX = 0;
//        state->Gamepad.sThumbRY = 0;
//    }


    const auto wants_swap = m_swap_controllers->value();
    const auto left_joystick = !wants_swap ? get_left_joystick():get_right_joystick();
    const auto right_joystick = !wants_swap? get_right_joystick():get_left_joystick();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    // The stick clicks the game gets: the left one's is sprint, the right one's melee. (fix198) This menu opens and closes with F11
    // only (its window's close button closes it too): both sticks clicked together no longer toggle it, now that the everyday settings
    // are in the game's own Settings > VR tab. So each click goes to the game as it is, also both together, with no wait to see whether
    // the other stick joins it (before, a click was held back 100 ms for that, and one down together with the other's went nowhere).
    bool send_stick_click[2]{ is_left_joystick_click_down, is_right_joystick_click_down }; // left, right
    bool temple_start_tap       = false; // (fix205, below) Start tapped for the game now: the Data menu at your right temple
    bool temple_click_taken     = false; // ...the right controller's click is that gesture's until let go
    bool temple_click_game_left = false; // ...and it is the game's left stick click (Swap Controller Inputs)

    // (fix205) The Data menu at your right temple: the right controller in your hands held beside the right side of your head (the same box as
    // the scanner's trigger gesture there, TempleGestures) and its thumbstick clicked gives the game a tap of Start (the Data menu) instead of
    // the click. For headsets whose menu button never reaches the game (SteamVR keeps the Touch left menu button for its own dashboard, and the
    // PlayStation VR2 Sense's Options button gets nothing by itself there), and it works on every headset. Only a click that begins with the
    // controller there is taken, judged as it goes down: a click anywhere else (melee, sprint) reaches the game at once, as before, with no
    // wait. A taken click is kept from the game until it is let go. In the Data menu itself (no other menu over it) it gives Start again, the
    // pause menu, as the menu button does there. Not taken in any other menu (the click is the menu's), the mod's menu, a conversation, build mode,
    // the favorites menu or wheel, the hand scanner, on a loading screen; nor with the left grip held (left grip + right stick click
    // is the eye-mode switch; with Swap Controller Inputs the right controller's grip with its click is the flat screen's), the right grip held,
    // the temple's own trigger gesture under way (its trigger held back, or its LB being sent), or that hand holding something, a person, a
    // wall or a ladder, a grenade, a power just pushed out, or its thrown melee weapon away. The
    // controller is the one in your right hand whatever Swap Controller Inputs says (the temples do not swap): with it on, its click is the
    // game's left stick click (sprint), and that is the one kept. A short buzz in that controller.
    {
        struct TempleMenu {
            bool                                  was_down{ false };
            bool                                  taken{ false };
            int                                   game_click{ 1 }; // (the game's stick click it took: kept while held)
            std::chrono::steady_clock::time_point start_until{};
            int                                   logs{ 0 };
            int                                   refused_logs{ 0 };
        };
        static TempleMenu tm{};
        constexpr auto    kStartTap = std::chrono::milliseconds(90);
        // The right controller in your hands: its click and grip; which of the game's stick clicks its click is.
        const bool  down       = is_action_active(m_action_joystick_click, get_right_joystick());
        const int   click_now  = wants_swap ? 0 : 1; // (send_stick_click's index: 0 the game's left, 1 its right)
        const bool  pressed    = down && !tm.was_down;
        tm.was_down            = down;
        if (!down) {
            tm.taken = false;
        }
        float side = 0.0f, up = 0.0f, ahead = 0.0f;
        if (pressed && TempleGestures::AtOwnTemple(false, &side, &up, &ahead)) {
            // (The game's hand this controller is: its left with Swap Controller Inputs.)
            const bool  hand_left = wants_swap;
            const char* refused   = g_framework->is_drawing_ui()                                    ? "the mod's menu is open"
                                    : GameFlow::isShowingMenu() && !GameFlow::isOnlyDataMenuShown() ? "a menu other than the Data menu is shown (the click is the menu's)"
                                    : GameFlow::isInDialogue() || GameFlow::isDialogueDrawnNow()     ? "in a conversation"
                                    : GameFlow::isWorkshopShown()                                    ? "in build mode"
                                    : GameFlow::isFavoritesShown() || FavoritesWheel::IsOpen()       ? "the favorites are open"
                                    : GameFlow::isScannerOpen()                                      ? "the hand scanner is open"
                                    : GameFlow::isLoadingScreenShown()                               ? "a loading screen"
                                    // (The review: the left controller's grip, without Swap Controller Inputs: left grip + right stick click is the
                                    // eye-mode switch. With it on this click is the game's left one, whose combo is the right controller's own grip.)
                                    : !wants_swap && is_action_active(m_action_grip, get_left_joystick()) ? "the left grip is held (left grip + right stick click: the eye mode)"
                                    : is_action_active(m_action_grip, get_right_joystick())
                                        ? (wants_swap ? "the right grip is held (with Swap Controller Inputs, its grip + this click: the flat screen)"
                                                      : "the right grip is held")
                                    : TempleGestures::HoldsTrigger(hand_left) || TempleGestures::PressLeftShoulder() ? "the temple's trigger gesture is under way"
                                    : HandGrab::HoldingWith(hand_left) || HandGrab::Deciding(hand_left) ? "the right hand holds something"
                                    : Physical::HoldingPerson(hand_left)                             ? "the right hand holds a person"
                                    : Climbing::Active() || Climbing::HoldsGrip(hand_left) || GameFlow::isOnLadder() ? "climbing"
                                    : HandActions::GrenadeInHand() || HandActions::OwnsGrenadeButton() ? "a grenade in hand"
                                    : HandActions::PowerAimActive()                                  ? "a power just pushed out"
                                    : MeleeThrow::Away(hand_left)                                    ? "its thrown melee weapon is away"
                                                                                                     : nullptr;
            if (refused == nullptr) {
                tm.taken       = true;
                tm.game_click  = click_now;
                tm.start_until = now + kStartTap;
                haptics::MeleePulse(hand_left, 0.4f, 0.06f); // (MeleePulse takes the game's hand: this controller)
                if (tm.logs < 20) {
                    ++tm.logs;
                    spdlog::info("[Temple] (fix205) the right stick clicked with the right controller at your right temple ({:.2f} out, {:.2f} up, {:.2f} "
                                 "ahead): Start tapped for the game ({}), the click ({}) kept from it until let go",
                        side, up, ahead, GameFlow::isShowingMenu() ? "in the Data menu: the pause menu" : "the Data menu", click_now == 1 ? "melee" : "sprint, the game's left stick with Swap Controller Inputs");
                }
            } else if (tm.refused_logs < 20) {
                ++tm.refused_logs;
                spdlog::info("[Temple] (fix205) the right stick clicked at your right temple ({:.2f} out, {:.2f} up, {:.2f} ahead), not taken for the Data "
                             "menu: {}; the click goes to the game as it is",
                    side, up, ahead, refused);
            }
        }
        if (tm.taken) {
            send_stick_click[tm.game_click] = false;
        }
        // The tap itself: added after the menu button's Start and Back below (which set both each poll).
        temple_start_tap = now < tm.start_until;
        temple_click_taken = tm.taken;
        temple_click_game_left = tm.game_click == 0;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    // (fix194) The face buttons. As printed (setting FaceButtonsAsPrinted, on by default): each Touch button sends the game's button of
    // the same letter (right A = A, right B = B, left X = X, left Y = Y), so the game's prompts name the button to press, and back (B, the
    // right hand) is no longer next to drop (X, the left hand): in the old layout both were under the left thumb, one above the other, and
    // a slip off back dropped the item. The letters are printed on the controllers, so Swap Controller Inputs does not move them. Off: the
    // old layout exactly, the hands swapped with Swap Controller Inputs as before (right A = A, left X = B, right B = Y, left Y = X).
    // Starfield's own actions: A use, take, talk (menus: select); B sneak (menus: back); X reload, held: your weapons put away or drawn
    // (menus: drop, transfer; build mode: delete); Y jump, in the air the boost pack (menus: favorite; Settings: defaults).
    const bool as_printed = GameFlow::gStore.internalSettings.faceButtonsAsPrinted;
    const bool face_a = as_printed ? is_action_active_any_joystick(m_action_a_button_right) : is_right_a_button_down; // the game's A
    const bool face_b = as_printed ? is_action_active_any_joystick(m_action_b_button_right) : is_left_a_button_down;  // the game's B
    const bool face_x = as_printed ? is_action_active_any_joystick(m_action_a_button_left) : is_left_b_button_down;   // the game's X (reload)
    const bool face_y = as_printed ? is_action_active_any_joystick(m_action_b_button_left) : is_right_b_button_down;  // the game's Y

    if (g_framework->is_drawing_ui()) {
        // (fix179, the review) A Y press held back below ends here: the mod's menu has the buttons now; no tap of X once it closes, and a
        // press held through the menu reaches the game in no way until it is let go.
        g_y_hold.held_back = face_x;
        g_y_hold.done      = true;
        g_y_hold.tap_until = {};
        g_y_hold.was_down  = face_x;
        // (fix192, the review) The thumbrests and the grips' taps likewise, their state taken as it stands now: closing the mod's menu with a
        // thumb resting is no fresh touch (the right one's RB in play readied a grenade, thrown as the thumb left), an RB held into the menu does
        // not come back as a fresh press, and a resting right thumb gives no RB until it leaves its rest and touches again; no grip's tap waits
        // or runs on.
        {
            auto& sh        = g_shoulders;
            sh.rb_touch_was = is_action_active_any_joystick(m_action_thumbrest_touch_right);
            sh.rb_pending = sh.rb_on = sh.rb_menu = false;
            sh.rb_cut       = sh.rb_touch_was;
            if (sh.rb_touch_was) {
                sh.rb_touched_at = now;
            }
            sh.lb_touch_was = is_action_active_any_joystick(m_action_thumbrest_touch_left);
            sh.lb_pending = sh.lb_on = sh.lb_menu = false;
            sh.grip_was[0]  = is_action_active(m_action_grip, left_joystick);
            sh.grip_was[1]  = is_action_active(m_action_grip, right_joystick);
            sh.grip_queued[0] = sh.grip_queued[1] = false;
            sh.grip_until[0] = sh.grip_until[1] = {};
            sh.menu_was     = true; // (fix194, the review) the mod's menu counts as a place: closing it starts the touch guard's window
        }
        return;
    }

    // (fix179) The game's A (use: take, open, talk) held back while a hand holds you on a wall (Climbing), a press begun then until it is
    // let go. (fix194: this comment called A jump before; Starfield's jump is Y, which is not held back here.)
    {
        static bool a_held_back = false;
        if (Climbing::Active() && face_a) {
            a_held_back = true;
        } else if (!face_a) {
            a_held_back = false;
        }
        if (face_a && !a_held_back) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_A;
        }
    }

    if (face_b) { // (fix194: Touch B as printed, Touch X in the old layout)
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_B;
    }

    if (face_y) { // (fix194: Touch Y as printed, Touch B in the old layout)
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_Y;
    }

    // (fix179) The reload button (the game's X; fix194: Touch X as printed, Touch Y in the old layout) held with only the left hand's weapon
    // out (your own put away): the game would take it as "draw your weapons" (with the right melee weapon put away and the left one out, the
    // button held should sheath the left one). Such a press (judged as it starts, in play) is held back from the game: held 0.35 s, the left
    // one is put away alone; let go sooner, it reaches the game as a tap then (60 ms).
    // (The review: its state is g_y_hold, reset while the mod's menu is drawn, above.)
    {
        auto&      y     = g_y_hold;
        const auto y_now = std::chrono::steady_clock::now();
        if (face_x && !y.was_down) {
            y.held_back = !GameFlow::isShowingMenu() && !GameFlow::isInDialogue() && !GameFlow::isWorkshopShown() && !GameFlow::isPilotingShip() &&
                          GameFlow::isInFirstPerson() && LeftWeapon::OnlyLeftOut();
            y.since = y_now;
            y.done  = false;
        }
        bool send = face_x;
        if (y.held_back) {
            if (face_x) {
                send = false;
                if (!y.done && y_now - y.since >= std::chrono::milliseconds(350)) {
                    y.done = true;
                    LeftWeapon::PutAwayAlone();
                }
            } else {
                if (!y.done) {
                    y.tap_until = y_now + std::chrono::milliseconds(60);
                }
                y.held_back = false;
            }
        }
        y.was_down = face_x;
        if (send || y_now < y.tap_until) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_X;
        }
    }

    // (fix157) Two quick presses of the reload button (fix194: Touch X as printed, Touch Y in the old layout): the left hand's weapon put
    // away, or drawn again (the left hand's melee weapon sheathed with two quick presses, both sheathed with the button held as before).
    // The presses still reach the game (X: a tap reloads; held, the game puts your weapons away, the left one with them).
    {
        static bool was_down = false;
        static auto first_at = std::chrono::steady_clock::time_point{};
        const bool  pressed  = face_x && !was_down;
        was_down             = face_x;
        if (pressed && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown()) { // (fix175: nor in build mode, where it is X: delete)
            if (first_at != std::chrono::steady_clock::time_point{} && now - first_at < std::chrono::milliseconds(400)) {
                LeftWeapon::ToggleSheathed();
                first_at = {};
            } else {
                first_at = now;
            }
        }
    }


    // (fix198) The left grip holding the gun in both hands (body::GunHeldWithBothHands: squeezed at the gun's support grip): the grip is the
    // gun's, so its combos stand down while it is held (the eye mode below, the flat screen, the system button's back, the alternative
    // layout's LB/RB and D-pad); a squeeze begun with the left hand already drawn on the gun counts so from its start (the body decides a
    // frame after this poll), until let go.
    bool gun_grip = false;
    {
        static bool left_grip_was  = false;
        static bool gun_grip_latch = false;
        const bool  left_grip_now  = is_action_active(m_action_grip, left_joystick);
        if (left_grip_now && !left_grip_was && body::IsSupportHandOnGun()) {
            gun_grip_latch = true;
        }
        if (!left_grip_now) {
            gun_grip_latch = false;
        }
        left_grip_was = left_grip_now;
        gun_grip      = left_grip_now && (gun_grip_latch || body::GunHeldWithBothHands());
    }

    // Mono / stereo: with the left grip already held, click the right stick.
    // That click does not also melee (held, or as the short press a quick click is sent as), until the next click.
    // A short buzz in both hands.
    // (fix173) Whatever the left hand holds: the click was skipped while the left hand counted as holding something (HandGrab::HeldCurl), and
    // since fix170 a hand also counts while it holds an object on its own or as the second hand on one, so the switch often did nothing.
    // The left grip held and then a click of the right stick is a deliberate combo.
    bool mono_click = false;
    {
        static bool right_click_was_down = false;
        static bool mono_press           = false;
        const bool  clicked              = is_right_joystick_click_down && !right_click_was_down;
        right_click_was_down             = is_right_joystick_click_down;
        if (clicked) {
            mono_press = false;
        }
        if (clicked && !is_left_joystick_click_down && is_action_active(m_action_grip, left_joystick) && !gun_grip) {
            // (Stereo, mono, AER in turn: a short buzz for mono, a long one for AER, a medium one back to stereo. fix196: with AER out of the
            // button, the default ("Include AER in the eye-mode button" off), a press from mono goes back to stereo: the long buzz means that;
            // from AER chosen in the menu, the medium one.)
            const float buzz = m_mono->value() ? 0.25f : (m_aer->value() ? 0.15f : 0.08f);
            toggle_mono();
            trigger_haptic_vibration(0.0f, buzz, 0.0f, 0.5f, left_joystick);
            trigger_haptic_vibration(0.0f, buzz, 0.0f, 0.5f, right_joystick);
            mono_press = true;
        }
        mono_click = mono_press;
    }

    {
        if (send_stick_click[0]) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }

        // Melee: the right stick click, or a hard swing with the weapon out (impact melee). (Not while your thrown melee weapon
        // is away from your hand.)
        if (((send_stick_click[1] && !mono_click) || ImpactMelee::WantsPress()) && !MeleeThrow::Away(false)) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }
    }


    // (A trigger that is the menu pointer's is not pulled for the game.)
    const bool left_trigger_pointing = (pointer_triggers & (wants_swap ? 2 : 1)) != 0;
    const bool right_trigger_pointing = (pointer_triggers & (wants_swap ? 1 : 2)) != 0;
    const bool left_trigger_raw = is_action_active(m_action_trigger, left_joystick) && !left_trigger_pointing;
    const bool right_trigger_raw = is_action_active(m_action_trigger, right_joystick) && !right_trigger_pointing;
    const bool left_grip_raw = is_action_active(m_action_grip, left_joystick);
    const bool right_grip_raw = is_action_active(m_action_grip, right_joystick);
    // (fix171) The favorites wheel (a Spell Wheel VR-style wheel in place of the game's): the left grip held, then the left
    // trigger squeezed, the left stick in the middle, opens it in front of the left hand. While it is open its triggers and grips are its own (no
    // shot, aim, grab, LB, RB or D-pad for the game); its opening pull is held back from the game until let go (it never draws the left weapon,
    // FavoritesWheel); a trigger still held as it closes likewise, and LB/RB from the thumbrests until the thumb leaves them.
    const auto wheel_stick = get_joystick_axis(m_left_joystick); // (the stick the d-pad combo reads)
    // (fix179) A grip holding you on a wall (Climbing) is not the wheel's: its trigger pulled meanwhile opens nothing (you would drop).
    const bool left_grip_climbs = Climbing::HoldsGrip(true);
    const bool right_grip_climbs = Climbing::HoldsGrip(false);
    const auto wheel = FavoritesWheel::Poll(FavoritesWheel::Input{ left_grip_raw && !left_grip_climbs, left_trigger_raw, right_grip_raw && !right_grip_climbs,
        right_trigger_raw, wheel_stick.x, wheel_stick.y, is_action_active_any_joystick(m_action_thumbrest_touch_left),
        is_action_active_any_joystick(m_action_thumbrest_touch_right) });
    const auto is_left_trigger_down = left_trigger_raw && !wheel.left_trigger;
    const auto is_right_trigger_down = right_trigger_raw && !wheel.right_trigger;
    const auto is_left_grip_down = left_grip_raw && !wheel.open;
    const auto is_right_grip_down = right_grip_raw && !wheel.open;

    // (fix158) The left trigger draws the left hand's weapon when it is put away (if it is
    // equipped): a fresh pull, not with the left grip (the d-pad), at the temple (the flashlight), on a menu or the favorites wheel; and the
    // reload button held is noted (your weapons drawn with it held draw both; fix194: Touch X as printed, Touch Y in the old layout).
    {
        static bool left_trigger_was = false;
        const bool  fresh            = is_left_trigger_down && !left_trigger_was;
        left_trigger_was             = is_left_trigger_down;
        if (fresh && !is_left_grip_down && !TempleGestures::HoldsTrigger(true) && !GameFlow::isShowingMenu() && !GameFlow::isFavoritesShown() &&
            !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && // (not in a conversation or the pilot's seat either; the review)
            !GameFlow::isWorkshopShown() && // (fix175: nor in build mode, where the left trigger is LT: rotate)
            !Climbing::Active()) {          // (fix179: nor while a hand holds you on a wall: a weapon drawn would drop you)
            LeftWeapon::LeftTriggerPulled();
        }
        // (fix179, the review) A reload-button press held back from the game (only the left weapon out, above) is no hold for it.
        LeftWeapon::NoteYHeld(face_x && !g_y_hold.held_back);
    }



    // The flat screen on or off: the left grip held with the left stick clicked, or F12.
    // (fix189, the review) The left grip is held for many of the mod's own things now (an object in the hand, the second hand on one, the
    // gravity gloves' aim, a grip still deciding what to grab, a person held, a wall climbed, LB in the scanner, the left weapon's throw), and
    // the left stick's click is the game's sprint: sprinting with something held switched the headset's picture off. The combo counts only
    // while the left grip is none of those (the same test that keeps the left grip's D-pad from the game, below, and the climb's). F12: see
    // FlatScreenKey (looked at above, before the idle test).
    {
        const bool left_grip_busy = HandGrab::SuppressLeftGripDpad() || HandGrab::HoldingWith(true) || Physical::HoldingPerson(true) ||
                                    left_grip_climbs || Climbing::Active() || HandActions::GripIsShoulder(true) || MeleeThrow::Available(true) ||
                                    MeleeThrow::Away(true) || gun_grip; // (fix198: sprinting with the gun in both hands)
        // (fix205: not the right temple's click for the Data menu, the game's left stick click with Swap Controller Inputs, its grip squeezed
        // after it began.)
        if (is_left_joystick_click_down && is_left_grip_down && !left_grip_busy && !(temple_click_taken && temple_click_game_left)) {
            ToggleFlatScreen("the left grip held with the left stick clicked");
        }
    }

    const auto system_button_down = is_action_active_any_joystick(m_action_system_button);
    if(system_button_down) {
        if(is_left_grip_down && !gun_grip) { // (fix198: the gun's grip is no back modifier)
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_BACK;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        } else {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_START;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
        }

    } else {
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
    }
    // (fix205) The Steam Frame's View button: the game's View (Back) by itself, as on a gamepad (first / third person; the star map's
    // missions). (Its Menu button is the menu button above: Start.)
    if (is_action_active_any_joystick(m_action_back_button)) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_BACK;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
    }
    // (fix205) The Data menu at your right temple (above): its tap of Start (never with Back).
    if (temple_start_tap) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_START;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
    }

    // (fix173) The hand scanner and the star map (its planet view too): the grips are LB and RB there (setting ScannerGrips: the left
    // and right grips work as LB and RB in the scanner and on the planet map; in the scanner LB closes it and RB opens the surface map, in a
    // planet's view LB shows its resources and RB its surface map). A fresh squeeze taps its button (kGripTap, below, after the menu pointer's
    // filter, which keeps shoulder buttons from the game while you point); a grip squeezed there is that button's until let go
    // (HandActions::GripIsShoulder: no aim, grab, gloves' pull, throw, power push or grenade with it).
    // (fix175) Build mode too: LB and RB are its categories (the right grip's LT aim means nothing there; its LT is the left trigger's, below).
    const bool grip_shoulders = HandActions::GripsAreShoulders();

    // The right grip aims, except while it grabs (hand grab, weapon put away).
    // (fix173: nor while it is RB, in the scanner and the star map, and until it is let go after.)
    // (fix175, the review: never in build mode, ScannerGrips off or the alternative layout too: its LT rotates there.)
    // (fix179: nor while it holds you on a wall: Climbing.)
    if (is_right_grip_down && !HandGrab::SuppressRightGripTrigger() && !grip_shoulders && !HandActions::GripIsShoulder(false) &&
        !(GameFlow::isWorkshopShown() && !GameFlow::isShowingMenu()) && !right_grip_climbs) {
        pXinputGamepad->bLeftTrigger = 255;
    }

    // (fix175) Build mode: the left trigger is LT for the game (rotate left; the right trigger is RT, rotate right), as the game's own build
    // controls are (a helper's research: the exe's Workshop control map, eip.gg's outpost controls). In play the left trigger is the mod's
    // (the left weapon, the favorites wheel, a temple) and LT is the right grip's; in build mode the right grip is RB. Only a pull begun with
    // the left grip let go (the grip with the trigger is the D-pad's combo there: its list and variants), until it is let go, and not while
    // the left grip is held.
    {
        static bool build_lt       = false;
        static bool build_lt_was   = false;
        const bool  building       = GameFlow::isWorkshopShown(150) && !GameFlow::isShowingMenu();
        const bool  fresh          = is_left_trigger_down && !build_lt_was;
        build_lt_was               = is_left_trigger_down;
        if (!is_left_trigger_down || !building) {
            build_lt = false;
        } else if (fresh && !left_grip_raw) {
            build_lt = true;
        }
        if (build_lt && !left_grip_raw) {
            pXinputGamepad->bLeftTrigger = 255;
        }
    }

    // (fix157) The favorites menu (the quick-select wheel, drawn in the HUD layer): a fresh pull of a trigger chooses the selected favorite
    // as A does (the right trigger for the right weapon and the left trigger for the left weapon): A sent for a moment, only while
    // the menu is drawn (never into play: A is use there; fix194: this said A jumps), and the trigger noted for LeftWeapon (a melee
    // weapon chosen with the left one goes to the left hand). That trigger is kept from the game until it is let go (else it would fire
    // what was just equipped as the menu closes); a pull while the left stick is pushed (moving through the menu with the d-pad) does not
    // choose.
    static bool fav_held[2]{ false, false };
    {
        static bool fav_was[2]{ false, false };
        static bool fav_a_was = false;
        static auto fav_a_until = std::chrono::steady_clock::time_point{};
        static auto fav_last    = std::chrono::steady_clock::time_point{};
        static bool fav_deferred = false; // (a left pull with the grip held: chosen as it is let go, if the stick stayed still)
        static bool fav_moved    = false;
        // (Only while the menu is drawn now: a pull in its last moments, closing, is play's; the review.)
        const bool  favorites   = GameFlow::isFavoritesDrawnNow();
        const auto  stick       = get_joystick_axis(m_left_joystick);
        const bool  pushed      = std::abs(stick.x) > 0.5f || std::abs(stick.y) > 0.5f;
        const bool  pulled[2]{ is_left_trigger_down, is_right_trigger_down };
        const auto  choose = [&](int h, bool hold) {
            fav_held[h] = hold;
            fav_a_until = now + std::chrono::milliseconds(80);
            fav_last    = now;
            LeftWeapon::NoteFavoritesPress(h == 0);
        };
        for (int h = 0; h < 2; ++h) {
            const bool fresh    = pulled[h] && !fav_was[h];
            const bool released = !pulled[h] && fav_was[h];
            fav_was[h]          = pulled[h];
            if (!pulled[h]) {
                fav_held[h] = false;
            }
            const bool may = favorites && now - fav_last > std::chrono::milliseconds(300);
            // (The left grip held (the default layout): the left trigger with it moves through the wheel with the stick (the d-pad); so
            // such a pull chooses as it is let go, and only if the stick stayed still meanwhile. The review.)
            if (h == 0 && fresh && is_left_grip_down && !GameFlow::gStore.internalSettings.alternativeJoyLayout) {
                fav_deferred = favorites;
                fav_moved    = pushed;
                continue;
            }
            if (h == 0 && fav_deferred) {
                fav_moved = fav_moved || pushed;
                if (released) {
                    fav_deferred = false;
                    if (may && !fav_moved) {
                        choose(0, false);
                    }
                }
                continue;
            }
            if (fresh && may && !(h == 0 && pushed)) {
                choose(h, true);
            }
        }
        // (The game's own A there: the right hand's. fix194: the button that sends the game's A.)
        if (face_a && !fav_a_was && GameFlow::isFavoritesShown()) {
            LeftWeapon::NoteFavoritesPress(false);
        }
        fav_a_was = face_a;
        if (now < fav_a_until && GameFlow::isFavoritesDrawnNow()) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_A;
        }
    }

    // (Not while a weapon is in the right grip: its trigger equips that weapon (HandGrab); then not until this trigger is let go.)
    static bool right_trigger_held_back = false;
    // (Nor while your thrown melee weapon is away: the trigger guides it.)
    // (Nor while it is at your right temple: the scanner's gesture, TempleGestures.)
    // (fix157: nor the pull that chose a favorite.)
    // (fix179: nor while a hand holds you on a wall, Climbing: with your weapon put away it would draw it, and you would drop.)
    if (HandGrab::SuppressRightTrigger() || MeleeThrow::BlocksRightTrigger() || TempleGestures::HoldsTrigger(false) || fav_held[1] ||
        Climbing::Active()) {
        right_trigger_held_back = true;
    } else if (!is_right_trigger_down) {
        right_trigger_held_back = false;
    }
    // (fix159) Not with a melee weapon out in your right hand (no right trigger melee while a melee weapon is out: your
    // swings strike, impact melee; put away, the trigger still draws it, and after a throw it guides it, MeleeThrow).
    // (In menus as always, and only with impact melee on: without it the trigger is your only melee; the review.)
    // (fix175: nor in build mode, where the right trigger is RT: rotate.)
    const bool melee_trigger_off = GameFlow::gStore.internalSettings.impactMelee && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown() &&
                                   HandGrab::MeleeWeaponOut() != 0;
    if (is_right_trigger_down && !right_trigger_held_back && !melee_trigger_off) {
        pXinputGamepad->bRightTrigger = 255;
    }
    // The trigger that equipped a picked-up weapon, still held once the weapon is out: it fires (a fresh press for the game).
    // (Only while the trigger is still down, as the game would fire; not the alternative layout's left grip + trigger, which is RB.)
    if (HandGrab::FireOverride() == 1 && is_right_trigger_down &&
        !(is_left_grip_down && !gun_grip && GameFlow::gStore.internalSettings.alternativeJoyLayout)) {
        pXinputGamepad->bRightTrigger = 255;
    }
    // A picked-up weapon put in the bag with no weapon before it: put away (reload held); one just equipped: drawn (reload tapped).
    if (HandGrab::PressHolster() || HandGrab::PressReload()) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_X;
    }

    // (fix205) The thumbrests are LB and RB only with "Thumbrests as LB/RB" on (off by default): off, a touch counts as no touch here (no LB
    // tap, no RB hold), and an LB or RB they were sending ends at once. The Steam Frame's bumpers are bound to the same
    // actions (real presses, its shoulder buttons): with its controllers they stay LB and RB whatever the setting. Turned on with a thumb
    // already resting (in Settings > VR, a menu), that touch is no fresh one: nothing until the thumb leaves and touches again (a resting right
    // thumb would have switched the menu's tab, or readied a grenade in play). The favorites wheel still reads the touches as before.
    // (fix205, the review) The Steam Frame's bumpers are buttons, not resting thumbs: below they are not held back with the left grip
    // squeezed (a thumb rests on its thumbrest as the hand holds something; a bumper is pressed on purpose), nor taken for a thumb coming
    // back as a menu closes, and they stay LB and RB in the alternative layout too.
    // (fix207) The Valve Index controllers' trackpad presses likewise (vrcore gives each one as its hand's thumbrest action, the Index having
    // no thumbrests): buttons, whatever the setting, as the Frame's bumpers. "frame_now" below means either.
    const bool frame_now            = is_openxr_loaded() && (m_openxr->frame_controllers_now.load() || m_openxr->index_pads_now.load());
    const bool thumbrests_shoulders = GameFlow::gStore.internalSettings.thumbrestsAsShoulders || frame_now;
    const bool raw_rest_right       = is_action_active_any_joystick(m_action_thumbrest_touch_right);
    const bool raw_rest_left        = is_action_active_any_joystick(m_action_thumbrest_touch_left);
    {
        static bool thumbrests_were = thumbrests_shoulders;
        if (thumbrests_shoulders != thumbrests_were) {
            auto& s = g_shoulders;
            if (thumbrests_shoulders) {
                s.lb_touch_was = raw_rest_left;
                s.rb_touch_was = raw_rest_right;
                s.rb_cut       = raw_rest_right;
                if (raw_rest_right) {
                    s.rb_touched_at = now;
                }
                if (raw_rest_left) {
                    s.lb_touched_at = now;
                }
            } else {
                s.lb_pending = s.lb_on = s.lb_menu = false;
                s.rb_pending = s.rb_on = s.rb_menu = false;
            }
            static int thumbrest_logs = 0; // (fix207: at most 20 lines, the controllers being looked at each sync now)
            if (thumbrest_logs < 20) {
                ++thumbrest_logs;
                spdlog::info("[Buttons] (fix205) the thumbrests {} (a thumb resting now: left {}, right {}){}",
                    thumbrests_shoulders ? "are LB and RB now" : "send nothing now", raw_rest_left, raw_rest_right,
                    frame_now ? " (fix207: the Steam Frame's bumpers or the Index trackpads' presses: buttons)" : "");
            }
            thumbrests_were = thumbrests_shoulders;
        }
    }
    const auto thumbrest_touch_right_down = thumbrests_shoulders && raw_rest_right;
    const auto thumbrest_touch_left_down  = thumbrests_shoulders && raw_rest_left;


    // (A grenade taken from behind the shoulder has the grenade button to itself until the game has thrown it.)
    const bool grenade_button_owned = HandActions::OwnsGrenadeButton();
    auto&      sh                   = g_shoulders;
    // (fix194) When the last menu closed (looked at each poll, whatever the layout). A thumb that pressed the button closing a menu comes back
    // to its rest a moment later: with the face buttons as printed back is Touch B, under the right thumb, and that fresh touch in play was
    // RB, a grenade readied (thrown as the thumb lifts); in the old layout back is Touch X, under the left thumb, and its LB tap toggled the
    // scanner. So a touch that starts within kAfterMenu of a menu closing is no press, on either side and in both layouts, until the thumb
    // leaves its rest and touches again (below). A thumb resting through the menu's end is not new: fix192's rules for it stand.
    // (The review) "A menu" is any place the face buttons close: a menu, a conversation, build mode, the hand scanner (and the mod's own menu,
    // above); and a menu still counted as shown while no full-screen menu has been drawn for 100 ms (nor a conversation) is closing: a touch
    // then is the same thumb coming back. The menu is read once a poll (in_menu below is this one).
    const bool menu_now = GameFlow::isShowingMenu();
    {
        const bool place_now = menu_now || GameFlow::isDialogueDrawnNow() || GameFlow::isWorkshopShown(kPlaceDrawnMs) ||
                               GameFlow::isScannerDrawnWithin(kPlaceDrawnMs);
        if (sh.menu_was && !place_now) {
            sh.menu_closed_at = now;
            sh.late_logged    = false;
        }
        sh.menu_was = place_now;
    }
    const bool closed_once     = sh.menu_closed_at != Shoulders::Clock::time_point{};
    const auto since_close_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - sh.menu_closed_at).count();
    const bool just_after_menu = !sh.menu_was && closed_once && now - sh.menu_closed_at < kAfterMenu;
    const bool menu_closing    = menu_now && !GameFlow::isFullscreenMenuDrawnWithin(100) && !GameFlow::isDialogueDrawnNow();
    const bool touch_guard     = just_after_menu || menu_closing;
    if (GameFlow::gStore.internalSettings.alternativeJoyLayout) {
        // (The alternative layout: the left grip with a trigger is LB or RB, as before; the thumbrests are not shoulder buttons. fix205: the
        // Steam Frame's bumpers still are, below.)
        if (!frame_now) {
            sh.lb_touch_was = sh.rb_touch_was = false;
            sh.lb_pending = sh.lb_on = sh.lb_menu = sh.rb_pending = sh.rb_on = sh.rb_cut = sh.rb_menu = false;
            sh.lb_after_menu = false; // (fix194)
        }
        // (fix198: not the gun's grip, held in both hands: the right trigger fires then.)
        if (is_left_grip_down && !gun_grip && is_right_trigger_down && !right_trigger_held_back) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            pXinputGamepad->bRightTrigger = 0;
        }
        if (is_left_grip_down && !gun_grip && is_left_trigger_down && !fav_held[0] && !TempleGestures::HoldsTrigger(true)) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }
    }
    if (!GameFlow::gStore.internalSettings.alternativeJoyLayout || frame_now) { // (fix205: the Frame's bumpers in either layout)
        // (fix173) The thumbrests' LB and RB, never both at once outside menus (Shoulders, above; in a menu they are its tabs, no power: each
        // as it comes, the review). The other button "out" means: not sent by the other thumbrest now, not in the last reports handed over
        // for kShoulderGap, no grip's tap of it running.
        const bool in_menu  = menu_now; // (fix194, the review: read once, above)
        // (fix203) Character creation, its tabs on the grips: no thumbrest LB or RB there. It switches tabs as the button is let go, so a resting
        // right thumb's held RB switched tabs as it lifted (to press A), as the pointer's filter or the stick's navigation took it away, or
        // twice with a grip's tap (the review).
        const bool chargen_tabs = grip_shoulders && GameFlow::isChargenShown();
        const bool rb_clear = in_menu || (!sh.rb_on && now - sh.rb_sent >= kShoulderGap && now >= sh.grip_until[1]);
        // The left thumbrest: LB tapped as the thumb touches it (fix158: not with the left grip squeezed: the thumb rests there as you hold or
        // throw the left weapon, and LB opened the scanner: the scanner was opening with the left grip; fix171: nor while the favorites
        // wheel is open, nor until the thumb leaves after). A tap of kThumbTap whatever the thumb does meanwhile (a brief touch still a press
        // the game sees whole); a touch that could not be sent is done until the thumb leaves and touches again (before, the grip let go with
        // the thumb resting sent LB, held: the scanner, or over 0.4 s the flashlight).
        {
            const bool touch = thumbrest_touch_left_down;
            const bool fresh = touch && !sh.lb_touch_was;
            sh.lb_touch_was  = touch;
            if (touch) {
                sh.lb_touched_at = now;
            }
            // (fix194) A touch begun just after a menu closed (above) gives no LB, nor does any touch until the thumb has been off its rest
            // kThumbLeft (a sensor dropout of a poll or two is not a new touch).
            if (fresh && touch_guard && !frame_now) { // (fix205: not the Frame's bumper, a press)
                if (!sh.lb_after_menu && sh.after_menu_logs < 20) {
                    ++sh.after_menu_logs;
                    spdlog::info("[Buttons] (fix194) the left thumb came back to its rest {}: no LB (the scanner) until it leaves the rest and "
                                 "touches again",
                        menu_closing ? std::string{ "as a menu closed" } : fmt::format("{} ms after a menu closed", since_close_ms));
                }
                sh.lb_after_menu = true;
            } else if (!touch && now - sh.lb_touched_at >= kThumbLeft) {
                sh.lb_after_menu = false;
            }
            // (fix175, the review: not in build mode: LB is its category there, the grips give it; a thumb resting there changed categories.)
            // (fix198) The gun's grip, held in both hands, counts as the grip here too: the thumb rests on the thumbrest as the hand holds the
            // gun, as it did holding the left weapon (fix158), and its touch would open the scanner while aiming.
            const bool may   = (frame_now || !is_left_grip_down) && !wheel.left_shoulder && !(GameFlow::isWorkshopShown() && !in_menu) && !chargen_tabs; // (fix203)
            if (fresh) {
                sh.lb_pending    = may && !sh.lb_after_menu;
                sh.lb_pending_at = now;
                sh.lb_on         = false;
            }
            if (!may || (sh.lb_pending && !touch)) { // (waiting, the thumb gone: no press)
                sh.lb_pending = false;
                sh.lb_on      = false;
            }
            if (sh.lb_pending && now - sh.lb_pending_at > kWaitAtMost) {
                sh.lb_pending = false;
                if (sh.wait_logs < 20) {
                    ++sh.wait_logs;
                    spdlog::info("[Buttons] (fix173) the left thumbrest's LB waited a second for RB to be let go: dropped (LB and RB together are your power)");
                }
            }
            if (sh.lb_pending && rb_clear) {
                sh.lb_pending = false;
                sh.lb_on      = true;
                sh.lb_until   = now + kThumbTap;
            }
            if (sh.lb_on && now >= sh.lb_until) {
                sh.lb_on = false; // (the thumb still resting: no more LB until it leaves and touches again)
            }
            // (fix192, the review) A tap begun or running while a menu was shown (there LB is its tab before) ends as the menu does: the rest of
            // it never reaches play, where LB is the scanner. (No more LB from this touch, as after any tap.)
            if (!sh.lb_on) {
                sh.lb_menu = false;
            } else if (in_menu) {
                sh.lb_menu = true;
            } else if (sh.lb_menu) {
                sh.lb_menu = false;
                sh.lb_on   = false;
                if (sh.lb_cut_logs < 20) {
                    ++sh.lb_cut_logs;
                    spdlog::info("[Buttons] (fix192) the menu closed during the left thumbrest's LB tap: the rest of it not carried into play (the scanner)");
                }
            }
            if (sh.lb_on) {
                pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            }
        }
        // The right thumbrest: RB held while the thumb rests. Not in the hand scanner (its surface map), nor while a grenade has the button,
        // nor while the favorites wheel is open (fix171): taken away so, not back until the thumb leaves (back with the thumb still resting, it
        // was a fresh press of RB: the surface map as the scanner opened, another grenade readied).
        {
            const bool touch = thumbrest_touch_right_down;
            const bool fresh = touch && !sh.rb_touch_was;
            sh.rb_touch_was  = touch;
            if (touch) {
                sh.rb_touched_at = now;
            }
            if (!touch) {
                sh.rb_pending = sh.rb_on = false;
                // (fix192, the review) A cut ends only once the thumb has been off its rest kThumbLeft: the sensor missing the touch for a poll or
                // two is no new touch (it would have been a fresh RB in play: a grenade).
                if (now - sh.rb_touched_at >= kThumbLeft) {
                    sh.rb_cut = false;
                }
            } else {
                // (fix194) A touch begun just after a menu closed (above): taken away as the scanner's is, no RB until the thumb has been off
                // its rest kThumbLeft and touches again.
                if (fresh && touch_guard && !frame_now) { // (fix205: not the Frame's bumper, a press)
                    if (!sh.rb_cut && sh.after_menu_logs < 20) {
                        ++sh.after_menu_logs;
                        spdlog::info("[Buttons] (fix194) the right thumb came back to its rest {}: no RB (a grenade readied) until it leaves the "
                                     "rest and touches again",
                            menu_closing ? std::string{ "as a menu closed" } : fmt::format("{} ms after a menu closed", since_close_ms));
                    }
                    sh.rb_cut = true;
                } else if (fresh && !sh.menu_was && closed_once && !sh.late_logged && since_close_ms < 1500 && sh.late_logs < 20) {
                    // (The review, for tuning kAfterMenu) The first fresh touch after the window, within 1.5 s of the close: it is RB as usual.
                    sh.late_logged = true;
                    ++sh.late_logs;
                    spdlog::info("[Buttons] (fix194) the right thumb touched its rest {} ms after a menu closed (after the guard's {} ms): RB as usual",
                        since_close_ms, kAfterMenu.count());
                }
                if (GameFlow::isScannerOpen() && !sh.rb_cut && sh.scanner_logs < 20) {
                    ++sh.scanner_logs;
                    spdlog::info("[Buttons] (fix173) the right thumbrest touched in the hand scanner: no RB (its surface map) until the thumb leaves it");
                }
                if (GameFlow::isScannerOpen() || grenade_button_owned || wheel.right_shoulder || (GameFlow::isWorkshopShown() && !in_menu) ||
                    chargen_tabs) { // (fix175: nor in build mode; fix203: nor in character creation, its tabs on the grips)
                    sh.rb_cut = true;
                }
                if (sh.rb_cut) {
                    sh.rb_pending = sh.rb_on = false;
                } else if (fresh) {
                    sh.rb_pending    = true;
                    sh.rb_pending_at = now;
                }
                if (sh.rb_pending && now - sh.rb_pending_at > kWaitAtMost) {
                    sh.rb_pending = false;
                    sh.rb_cut     = true;
                    if (sh.wait_logs < 20) {
                        ++sh.wait_logs;
                        spdlog::info("[Buttons] (fix173) the right thumbrest's RB waited a second for LB to be let go: dropped until the thumb leaves");
                    }
                }
                // (LB out: not from the left thumbrest now, not in the last reports for kRbAfterLb, no left grip's tap. Longer than the other
                // way: an LB tap may be opening the scanner, shown some 0.2 s later in a test's log; RB right after it is its surface map.)
                if (sh.rb_pending && (in_menu || (!sh.lb_on && now - sh.lb_sent >= kRbAfterLb && now >= sh.grip_until[0]))) {
                    sh.rb_pending = false;
                    sh.rb_on      = true;
                }
            }
            // (fix192) An RB that began or was held while a menu was shown (there it is the menu's next tab) ends as the menu does: on the first
            // poll with no menu shown it is taken away as the scanner's is, not back until the thumb leaves its rest. Kept on, the game got it in
            // play as a fresh press (the menu pointer's filter had kept it from the game until the panel went): with a grenade equipped it
            // readied one, thrown as the thumb left (a grenade equipped from a menu released one by itself on unpausing; the
            // right thumb rests there as the right trigger equips, and the left hand closes the menu: the old layout's back, Touch X; fix194: as
            // printed back is Touch B, and the right thumb's touch as it comes back from it is the guard's above). Taken here, before the menu
            // pointer's filter, so this very report has no RB. A touch made in play is RB as before (fix194: but not one begun just after a menu
            // closed).
            if (!sh.rb_on) {
                sh.rb_menu = false;
            } else if (in_menu) {
                sh.rb_menu = true;
            } else if (sh.rb_menu) {
                sh.rb_menu = false;
                sh.rb_on   = false;
                sh.rb_cut  = true;
                if (sh.menu_cut_logs < 20) {
                    ++sh.menu_cut_logs;
                    spdlog::info("[Buttons] (fix192) the menu closed with the right thumb on its rest: its RB (the menu's) not carried into play, no RB "
                                 "until the thumb leaves the rest (in play it would have been a fresh press: the grenade readied, thrown as the thumb left)");
                }
            }
            if (sh.rb_on) {
                pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            }
        }
    }

    // Hand actions: a grenade taken from behind the shoulder holds the grenade button (RB) until it is thrown; a power pushed
    // out presses both shoulder buttons together (the game's power).
    if (grenade_button_owned) {
        // Nothing else presses RB, nor LB (the left thumbrest: with the grenade's RB the game would take LB + RB for a power).
        pXinputGamepad->wButtons &= ~(XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER);
    }
    // (fix171: none of these hand gestures' buttons while the favorites wheel is open: its hands move over it.)
    if (HandActions::PressGrenade() && !wheel.open) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }
    // A trigger at a temple: LB held (the flashlight) or tapped (the scanner), TempleGestures. Alone: RB let go meanwhile (LB and
    // RB together are the game's power: the right thumbrest's RB, fix132's review); not while a grenade has the buttons.
    if (TempleGestures::PressLeftShoulder() && !grenade_button_owned && !HandActions::PressGrenade() && !wheel.open) {
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_RIGHT_SHOULDER;
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }
    if (HandActions::PressPower() && !wheel.open) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }
    // (fix173) The right thumbrest's RB taken away just above (a temple's LB, a grenade's hold): not back until the thumb leaves its rest.
    if (sh.rb_on && (pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) == 0) {
        sh.rb_on  = false;
        sh.rb_cut = true;
    }

    // (fix171) In play while the favorites wheel can be used (and while it is open), no D-pad at all: the game's favorites menu opens only
    // from it (the inventory's own "favorite" still opens it to assign a slot, and there its D-pad works as before).
    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up) && !wheel.dpad;

    if (is_dpad_up_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right) && !wheel.dpad;

    if (is_dpad_right_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down) && !wheel.dpad;

    if (is_dpad_down_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left) && !wheel.dpad;

    if (is_dpad_left_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = get_joystick_axis(left_joystick);
    const auto right_joystick_axis = get_joystick_axis(right_joystick);

    const auto true_left_joystick_axis = get_joystick_axis(m_left_joystick);
    const auto true_right_joystick_axis = get_joystick_axis(m_right_joystick);

    pXinputGamepad->sThumbLX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbLY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbRY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    // Turning with the right stick, on foot in first person: snap in fixed steps, or smooth at a set
    // speed. Menus, the pilot's seat and third person keep the stick as it is.
    {
        const auto& turn = GameFlow::gStore.internalSettings;
        const bool on_foot = GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() && !GameFlow::isPilotingShip() &&
                             !GameFlow::isImmovable() && !GameFlow::isControlledByAI() && !ModSettings::showFlatScreenDisplay() && !GameFlow::isOnLadder();
        // (fix173) In a conversation (smooth turning there too): the right stick's left and right turn the room
        // ourselves, smoothly at the smooth turn's speed (120 degrees a second at full speed, SmoothTurnSpeed), whatever the turn mode, as the
        // ladder's view does (GameFlow::pendingDialogueYaw, the camera hook). Through the game it did nothing: the camera hook holds the game's
        // own heading changes back in a conversation (fix163, so the menu pointer's mouse does not shake the view), and while a hand points at
        // the conversation the menu pointer keeps both sticks from the game. Read from the stick itself, so pointing does not stop it; only a
        // push mostly sideways (up and down scroll the answers while you point: MenuPointer). The game gets no left and right meanwhile.
        // (Only with the room's turn held by the camera hook, PawnControl; else as on foot.)
        const bool dialogue_turn = on_foot && turn.dialogueTurn && turn.pawnControl && GameFlow::isInDialogue();
        static std::chrono::steady_clock::time_point dialogue_turn_at{};
        static bool snap_armed = true; // (a snap turn: the stick must come back before the next one)
        if (dialogue_turn) {
            const float dt = dialogue_turn_at == std::chrono::steady_clock::time_point{}
                                 ? 0.0f
                                 : std::clamp(std::chrono::duration<float>(now - dialogue_turn_at).count(), 0.0f, 0.1f);
            dialogue_turn_at = now;
            const float x    = right_joystick_axis.x;
            if (std::abs(x) > 0.15f && std::abs(x) > std::abs(right_joystick_axis.y)) {
                GameFlow::pendingDialogueYaw.fetch_add(glm::radians(120.0f) * std::clamp(turn.smoothTurnSpeed, 0.1f, 1.0f) * x * dt);
            }
            pXinputGamepad->sThumbRX = 0;
            snap_armed               = false; // (the stick still pushed as the conversation ends makes no snap: it must come back first)
        } else {
            dialogue_turn_at = {};
        }
        if (dialogue_turn) {
            // (Turned above.)
        } else if (on_foot && turn.turnMode == 0) {
            constexpr float kSnapTrigger = 0.7f;  // stick pushed this far turns once
            constexpr float kSnapRearm   = 0.3f;  // and must come back this far before the next turn
            static std::chrono::steady_clock::time_point last_snap{};

            const float x = right_joystick_axis.x;
            if (std::abs(x) < kSnapRearm) {
                snap_armed = true;
            } else if (snap_armed && std::abs(x) >= kSnapTrigger && now - last_snap >= std::chrono::milliseconds(200)) {
                snap_armed = false;
                last_snap = now;
                GameFlow::pendingSnapYaw.fetch_add(glm::radians(std::clamp(turn.snapTurnDegrees, 5.0f, 180.0f)) * (x > 0.0f ? 1.0f : -1.0f));
            }
            pXinputGamepad->sThumbRX = 0;
        } else if (on_foot) {
            const float speed = std::clamp(turn.smoothTurnSpeed, 0.1f, 1.0f);
            pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>((float)pXinputGamepad->sThumbRX * speed, -32767.0f, 32767.0f);
        }
        // (fix150) On a ladder with the view at the body's head: the stick turns the view (the room's turn, held by LadderClimb), snap or
        // smooth as on foot. It went to the game's third-person camera before, whose turn the ladder's view does not use: it did nothing
        // (whichever way you looked while climbing, the view stayed stuck that way).
        if (LadderClimb::HeadWanted() && !GameFlow::isShowingMenu() && !ModSettings::showFlatScreenDisplay()) {
            const float x = right_joystick_axis.x;
            if (turn.turnMode == 0) {
                static bool ladder_armed = true;
                static std::chrono::steady_clock::time_point ladder_last{};
                if (std::abs(x) < 0.3f) {
                    ladder_armed = true;
                } else if (ladder_armed && std::abs(x) >= 0.7f && now - ladder_last >= std::chrono::milliseconds(200)) {
                    ladder_armed = false;
                    ladder_last = now;
                    GameFlow::pendingLadderYaw.fetch_add(glm::radians(std::clamp(turn.snapTurnDegrees, 5.0f, 180.0f)) * (x > 0.0f ? 1.0f : -1.0f));
                }
            } else {
                // (Smooth: 120 degrees a second at full speed, the stick's push past a little.)
                static std::chrono::steady_clock::time_point ladder_smooth_at{};
                const float dt = ladder_smooth_at == std::chrono::steady_clock::time_point{}
                                     ? 0.0f
                                     : std::clamp(std::chrono::duration<float>(now - ladder_smooth_at).count(), 0.0f, 0.1f);
                ladder_smooth_at = now;
                if (std::abs(x) > 0.15f) {
                    GameFlow::pendingLadderYaw.fetch_add(glm::radians(120.0f) * std::clamp(turn.smoothTurnSpeed, 0.1f, 1.0f) * x * dt);
                }
            }
            pXinputGamepad->sThumbRX = 0;
        }
    }


    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    // (Not while the left grip reaches for or holds an object: the left stick keeps walking. fix143: nor on a ladder: the grips climb it.)
    const bool on_ladder = GameFlow::isOnLadder();
    // (fix156) The left grip and the left trigger held together (the favorites opened by holding the left grip and
    // trigger while using the left stick, not by the grip alone): the grip alone keeps the stick walking (it throws the left hand's weapon, and
    // the hand grab's squeeze). The alternative layout keeps the grip alone (its grip and trigger together are LB).
    const bool alternative_layout = GameFlow::gStore.internalSettings.alternativeJoyLayout;
    // (Not while the left hand's weapon is thrown: its trigger guides it and its grip calls it back; nor, in the alternative layout, the
    // grip alone while a left weapon is held: that grip throws it. The review.)
    const bool dpad_combo = is_left_grip_down && !gun_grip && !MeleeThrow::Away(true) && // (fix198: not the gun's grip, held in both hands)
                            (alternative_layout ? !LeftWeapon::Held() : is_left_trigger_down && !fav_held[0]);
    // (fix166) While the favorites wheel is open the left stick alone moves through its slots (its D-pad), as the combo does, and does not walk
    // (the left stick is needed to move through every menu screen; the wheel's slots are the D-pad's: a helper's research).
    const bool favorites_open = GameFlow::isFavoritesDrawnNow(); // (drawn now: not the 0.3 s after it closes, when the stick walks again; the review)
    // (fix171) The combo's D-pad not in play while the favorites wheel can be used there (its gesture opens the wheel instead; the stick walks).
    // (fix175) The left grip taken as LB (the star map, build mode, the scanner: HandActions::GripIsShoulder) leaves the hand grab, which then
    // counted it as another action's grip and kept the combo's D-pad from the game (SuppressLeftGripDpad); it grabs nothing there: the combo
    // stays (the star map's pan, build mode's list and variants).
    if (((dpad_combo && !wheel.dpad) || favorites_open) && (!HandGrab::SuppressLeftGripDpad() || HandActions::GripIsShoulder(true)) && !on_ladder) {

        float ty{0.0f};
        float tx{0.0f};
        //SHORT ThumbY{0};
        //SHORT ThumbX{0};
        // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.

        ty = true_left_joystick_axis.y; // ? wants_swap
        tx = true_left_joystick_axis.x;

        if (ty >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if (ty <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (tx >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if (tx <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        if (wants_swap) {
            pXinputGamepad->sThumbRY = 0;
            pXinputGamepad->sThumbRX = 0;
        } else {
            pXinputGamepad->sThumbLY = 0;
            pXinputGamepad->sThumbLX = 0;
        }
    }

    // (fix143) Climbing a ladder with the hands: a squeezed grip pulled down is the stick pushed up for the game (the climb reads the
    // left stick past half way), pushed up the stick back; the stick itself still climbs when the hands do not.
    if (on_ladder) {
        if (const int climb = LadderClimb::StickY(); climb != 0) {
            pXinputGamepad->sThumbLY = climb > 0 ? 32767 : -32767;
        }
    }

    // (fix179) Holding on to a wall with a hand (Climbing): the hands move you, the game's walking stick does nothing meanwhile (its air
    // control would push you off where the hands hold you).
    if (Climbing::Active()) {
        pXinputGamepad->sThumbLX = 0;
        pXinputGamepad->sThumbLY = 0;
    }

    // (fix173) The grips' taps in the hand scanner and the star map (grip_shoulders, above): a fresh squeeze of the left grip taps LB, of the
    // right grip RB, kGripTap each, put in the report after the menu pointer's filter below so the star map gets them while you point. Never
    // both at once: a tap waits while the other button is in the report or was in the last ones (kShoulderGap), at most a second. A squeeze
    // with the mode off (in play) is no tap, and a tap under way ends whole after the mode ends (the left grip's LB closes the scanner).
    // Each squeeze told to HandActions (its grip is that button's until let go), and to the menu pointer before its filter (as a face button:
    // the game goes to gamepad mode while it is held, so no mode key meanwhile; the review).
    constexpr WORD kLane[2]{ XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER };
    {
        const bool grips[2]{ left_grip_raw, right_grip_raw };
        for (int h = 0; h < 2; ++h) {
            const bool fresh = grips[h] && !sh.grip_was[h];
            sh.grip_was[h]   = grips[h];
            if (fresh && grip_shoulders) {
                sh.grip_queued[h]    = true;
                sh.grip_queued_at[h] = now;
                HandActions::NoteGripTap(h == 0);
                if (h == 0) {
                    sh.left_waits = HandActions::LeftGripWaits(); // (fix175)
                }
            }
            if (!grip_shoulders) {
                sh.grip_queued[h] = false;
            }
            if (sh.grip_queued[h] && now - sh.grip_queued_at[h] > kWaitAtMost) {
                sh.grip_queued[h] = false;
                if (sh.wait_logs < 20) {
                    ++sh.wait_logs;
                    spdlog::info("[Buttons] (fix173) the {} grip's {} waited a second for the other to be let go: dropped", h == 0 ? "left" : "right",
                        h == 0 ? "LB" : "RB");
                }
            }
        }
        // (fix175) The left grip's LB in the star map and build mode waits kLeftGripWait from the squeeze: the left trigger, the menu button or a
        // stick click joining it meanwhile (or already held) makes it one of the left grip's combos (the trigger and the stick: the D-pad, the
        // star map's pan and build mode's list and variants; the menu button: View, the star map's missions and build mode's view; the right
        // stick's click: the eyes' mode switch; the left's: the flat screen), and no LB goes to the game. Before (fix173's trade-off), each
        // combo tapped LB first: the planet's resources view, a change of build category. The right grip's RB stays at once, and the scanner's
        // LB too (it closes the scanner; no combo means anything there).
        if (sh.grip_queued[0] && sh.left_waits) {
            const bool  trigger = is_action_active(m_action_trigger, left_joystick); // (pointing with it or not)
            const char* joined  = trigger ? "the left trigger" : system_button_down ? "the menu button"
                                : is_left_joystick_click_down || is_right_joystick_click_down ? "a stick click" : nullptr;
            if (joined != nullptr) {
                sh.grip_queued[0] = false;
                if (sh.combo_logs < 40) {
                    ++sh.combo_logs;
                    spdlog::info("[Buttons] (fix175) the left grip in the {}: {} joined it within {} ms: a combo, no LB", HandActions::GripsPlace(), joined,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - sh.grip_queued_at[0]).count());
                }
            }
        }
        if (sh.grip_queued[0] || sh.grip_queued[1] || now < sh.grip_until[0] || now < sh.grip_until[1]) {
            MenuPointer::HoldModeKey();
        }
    }

    // While the menu pointer is in use: no sticks, triggers or shoulder buttons for the game (they would switch it to gamepad mode,
    // which hides its menu cursor); a stick's up and down scrolls instead.
    const bool rb_before_filter = (pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0; // (fix192)
    const bool lb_before_filter = (pXinputGamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;  // (fix192, the review)
    MenuPointer::FilterGamepad(pXinputGamepad, true_left_joystick_axis.y, true_right_joystick_axis.y);
    // (fix192) The right thumbrest's RB kept from the game by that filter (you point at a menu, or the left stick navigates it): that touch is
    // done, taken away as the scanner's is, not back until the thumb leaves its rest and touches again. Before, it reached the game later with
    // the thumb still resting: the menu's next tab once the hand had been still 2 s, or the grenade as a fresh press in play once the panel
    // went. A touch made while you do not point still reaches the menu (its next tab) as before.
    // (The review: only while a menu or a conversation is shown. The filter runs on a moment after they go (the pointer's last 150 ms): a
    // touch made in play then is play's RB, as before; one carried from the menu is taken away as the menu closes, above.)
    const bool pointer_place = GameFlow::isShowingMenu() || GameFlow::isDialogueDrawnNow();
    if (pointer_place && sh.rb_on && rb_before_filter && (pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) == 0) {
        sh.rb_on   = false;
        sh.rb_cut  = true;
        sh.rb_menu = false;
        if (sh.filter_cut_logs < 20) {
            ++sh.filter_cut_logs;
            spdlog::info("[Buttons] (fix192) the right thumbrest's RB kept from the game by the menu pointer: this touch gives no RB later (not until "
                         "the thumb leaves its rest and touches again)");
        }
    }
    // (fix192, the review) The left thumbrest's LB tap likewise: kept from the game by the filter, it ends there (the rest of it would have
    // reached the game once the filter stopped: in play a fresh LB, the scanner). A tap made while you do not point is the menu's as before.
    if (pointer_place && sh.lb_on && lb_before_filter && (pXinputGamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) == 0) {
        sh.lb_on   = false;
        sh.lb_menu = false;
        if (sh.lb_cut_logs < 20) {
            ++sh.lb_cut_logs;
            spdlog::info("[Buttons] (fix192) the left thumbrest's LB tap kept from the game by the menu pointer: ended there (none of it later)");
        }
    }
    // (fix147, fix169) The menus that move sideways only on the D-pad or LB and RB: the left stick's left and right make those moves.
    MenuPointer::StickSideways(pXinputGamepad, true_left_joystick_axis.x);
    // (fix175) The star map: the right stick's up and down zoom there whether you point or not (pointing: the mouse wheel, above; else RT/LT).
    MenuPointer::StarMapZoom(pXinputGamepad);

    {
        // (A squeeze is meant; a thumb resting on the right thumbrest often is not: its RB, reaching the star map while you neither point nor
        // move the stick, gives way to the left grip's LB, not back until the thumb leaves its rest. In the scanner it is never sent.)
        if (sh.grip_queued[0] && sh.rb_on) {
            sh.rb_on  = false;
            sh.rb_cut = true;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        // (fix192, the review) The right grip's RB tap only while its place is drawn now: a menu shown (the star map is one), the scanner or build
        // mode drawn within kPlaceDrawnMs. The grips stay LB and RB 150 ms after the place was last drawn, longer than a tap (90 ms): a tap begun
        // or running then reached play, where RB is the grenade. (A tap waiting then starts if the place is drawn again; else it is dropped as
        // the mode ends, as before.)
        const bool rb_place_drawn = GameFlow::isShowingMenu() || GameFlow::isScannerDrawnWithin(kPlaceDrawnMs) || GameFlow::isWorkshopShown(kPlaceDrawnMs);
        for (int h = 0; h < 2; ++h) {
            const auto lane_free = [&](int b) {
                return (pXinputGamepad->wButtons & kLane[b]) == 0 && now - (b == 0 ? sh.lb_sent : sh.rb_sent) >= kShoulderGap && now >= sh.grip_until[b];
            };
            // (fix175: the left grip's LB in the star map and build mode only once kLeftGripWait has passed with no combo, above.)
            const bool waiting = (h == 0 && sh.left_waits && now - sh.grip_queued_at[0] < kLeftGripWait) || (h == 1 && !rb_place_drawn);
            if (sh.grip_queued[h] && !waiting && lane_free(h) && lane_free(1 - h)) {
                sh.grip_queued[h] = false;
                sh.grip_until[h]  = now + kGripTap;
                if (sh.tap_logs < 40) {
                    ++sh.tap_logs;
                    spdlog::info("[Buttons] (fix173) the {} grip squeezed in the {}: {} tapped{}", h == 0 ? "left" : "right", HandActions::GripsPlace(),
                        h == 0 ? "LB" : "RB", h == 0 && sh.left_waits ? " ((fix175) after 0.15 s, no combo joined it)" : "");
                }
            }
            // (fix192) The right grip's RB tap ends at once when the grips stop being LB and RB, or its place is no longer drawn (above): in play
            // RB is the grenade. (The left grip's LB tap still runs whole: it is what closes the scanner, which ends the mode.)
            if (h == 1 && now < sh.grip_until[1] && (!grip_shoulders || !rb_place_drawn)) {
                sh.grip_until[1] = now;
                if (sh.grip_cut_logs < 20) {
                    ++sh.grip_cut_logs;
                    spdlog::info("[Buttons] (fix192) the right grip's RB tap ended early: the grips are no longer LB and RB (no RB into play)");
                }
            }
            if (now < sh.grip_until[h]) {
                pXinputGamepad->wButtons |= kLane[h];
            }
        }
    }
    // (fix173) Last, LB and RB never handed over together outside menus (the game's power) but for the power push (HandActions: both at once,
    // on purpose) and the alternative layout's own LB + RB: the button already in the last report kept, the newcomer held back while both
    // are asked for (LB kept if both are new). (In a menu LB and RB are its tabs, no power: left as they come; the review.)
    {
        constexpr WORD kBoth = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if ((pXinputGamepad->wButtons & kBoth) == kBoth && !HandActions::PressPower() && !GameFlow::gStore.internalSettings.alternativeJoyLayout &&
            !GameFlow::isShowingMenu()) {
            const bool keep_rb = sh.rb_in_last && !sh.lb_in_last;
            pXinputGamepad->wButtons &= ~(keep_rb ? XINPUT_GAMEPAD_LEFT_SHOULDER : XINPUT_GAMEPAD_RIGHT_SHOULDER);
            if (sh.guard_logs < 20) {
                ++sh.guard_logs;
                spdlog::info("[Buttons] (fix173) LB and RB asked for together (the game's power): {} held back until {} is let go", keep_rb ? "LB" : "RB",
                    keep_rb ? "RB" : "LB");
            }
        }
        const bool lb_now = (pXinputGamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        if (lb_now && !sh.lb_in_last) {
            HandActions::NoteScannerLb(); // (a new LB press: in the scanner, it closes it; the grips are the hand grab's again)
        }
        sh.lb_in_last = lb_now;
        sh.rb_in_last = (pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        if (sh.lb_in_last) {
            sh.lb_sent = now;
        }
        if (sh.rb_in_last) {
            sh.rb_sent = now;
        }
    }
    // (fix165) What the game gets, counted for the seat log.
    GameFlow::noteButtonsSent(pXinputGamepad->wButtons);
    // (fix175) And for build mode's log.
    GameFlow::noteWorkshopPad(pXinputGamepad->wButtons, pXinputGamepad->bLeftTrigger, pXinputGamepad->bRightTrigger);
    // (fix203) And for character creation's.
    GameFlow::noteChargenPad(pXinputGamepad->wButtons, pXinputGamepad->bLeftTrigger, pXinputGamepad->bRightTrigger, pXinputGamepad->sThumbLX,
        pXinputGamepad->sThumbLY, pXinputGamepad->sThumbRX, pXinputGamepad->sThumbRY);
}

