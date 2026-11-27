#include "CreationEngineEntry.h"
#include "CreationEngineConstants.h"
#include "imgui.h"
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <imgui_internal.h>
#include <mods/VR.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <ModSettings.h>
#include <spdlog/spdlog.h>

#include "CreationEngineCameraManager.h"
#include "CreationEngineInputManager.h"
#include "CreationEngineRendererModule.h"
#include "MotionWeapon.h"
#include "MuzzleFire.h"
#include "NodeTreeDump.h"
#include "PointInteract.h"
#include "ImpactMelee.h"
#include "MeleeThrow.h"
#include "GrenadeHand.h"
#include "MenuPointer.h"
#include "InputMode.h"
#include "LiveWorld.h"
#include "MenuCamera.h"
#include "FullFrame.h"
#include "DlssViews.h"
#include "MenuTaa.h"
#include "StereoView.h"
#include "LensFit.h"
#include "EyeShare.h"
#include "Foveation.h"
#include "GpuTimer.h"
#include "ShadowShare.h"
#include "GloveHighlight.h"
#include "WorkbenchCamera.h"
#include "CreationEngineSingletonManager.h"
#include "LadderClimb.h"
#include "LeftWeapon.h"
#include "FavoritesWheel.h"
#include "ConversationPanel.h"
#include "BuildConsole.h"
#include "MessagesBlock.h"
#include "Climbing.h"
#include "VrSettingsTab.h"
#include "memory/GameCode.h"
#include "MenuClean.h"
#include "RenderSteps.h"
#include "MenuLayer.h"
#include <mods/VRConfig.hpp>
#include <chrono>
#include <thread>
#include "HandActions.h"
#include "HandGrab.h"
#include "Physical.h"
#include "Consume.h"
#include "PhysicsProbe.h"
#include "body/BodyIK.h"
#include <nvidia/UpscalerAfrNvidiaModule.h>

namespace {
    // (fix189, the review) The physics probe's scan of the game's code (PhysicsProbe.h), a test that ran at every start: now only with
    // the developer setting PhysicsCodeScan on (the settings are read after on_initialize, so it starts from the settings' load or from
    // its checkbox), once a session.
    std::atomic<bool> g_physics_scan_started{ false };

    void StartPhysicsCodeScan(const char* why)
    {
        if (!g_physics_scan_started.exchange(true)) {
            spdlog::info("[Physics] (fix189) the scan of the game's code starts in 5 s ({})", why);
            PhysicsProbe::Install();
        }
    }
}

std::optional<std::string> CreationEngineEntry::on_initialize()
{
    GameCode::LogOnce(); // (fix188) the game version against the build's, first
    CreationEngineCameraManager::Get()->InstallHooks();
    CreationEngineRendererModule::Get()->InstallHooks();
    CreationEngineInputManager::Get()->Init();
    MuzzleFire::Install();
    PointInteract::Install();
    ImpactMelee::Install();
    MeleeThrow::Install();
    GrenadeHand::Install();
    LeftWeapon::Install(); // (fix154)
    FavoritesWheel::Install(); // (fix171: after LeftWeapon, whose favorites hook it uses for the left hand)
    HandGrab::Install();
    Physical::Install(); // (fix172: people grabbed, shoved, knocked down; blocks; the probe)
    Consume::Install();
    // Test build: what hand and arm collision needs from the game's physics (see PhysicsProbe.h). Reads only.
    // (fix189) Not here any more: only with the developer setting PhysicsCodeScan on (on_config_load, its checkbox).
    // The menu pointer's line in the menu panel's settings (vrcore's "Menus and Virtual Screen").
    VRConfig::s_pointer_status = [] {
        return MenuPointer::Status() + "\n" + MenuLayer::Status() + "\n" + MenuClean::Status() + "\n" + LiveWorld::Status() + "\n" + MenuCamera::Status() + "\n" +
               FullFrame::Status() + "\n" + DlssViews::Status() + "\n" + MenuTaa::Status() + "\n" + StereoView::Status() + "\n" + GloveHighlight::Status() + "\n" + RenderSteps::Status();
    };
    // The game's input mode, for the menu pointer (a scan of the game's code, away from its threads; the game makes its input
    // manager early, so this is found within the first seconds).
    std::thread([] {
        for (int i = 0; i < 40 && !InputMode::Find(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }).detach();
    // See-through menus: the game's interface layer, copied for the menu panel (MenuLayer.h).
    MenuLayer::Install();
    // ...and without the backgrounds some menus paint themselves (MenuClean.h).
    MenuClean::Install();
    // (The edges each eye's lens does not show, or blurs, shaded coarsely by the game's own variable rate shading: Foveation.h. Hooked
    // before anything else may hook the same step, which then calls it.)
    Foveation::Install();
    EyeShare::Install(); // (fix156)
    // ...and with the world kept drawn behind them (the game stops drawing it for these menus: LiveWorld.h).
    LiveWorld::Install();
    // ...from where your head is now (the renderer's copy of the camera is left as the menu opened: MenuCamera.h).
    MenuCamera::Install();
    // ...at its full size (the game makes its frame the menu's 16:9 box: FullFrame.h), with DLSS kept on, each view its own (DlssViews.h).
    FullFrame::Install();
    DlssViews::Install();
    // ...and every menu with a 3D view of its own with the game's anti-aliasing at full size, as the Data menu (MenuTaa.h).
    MenuTaa::Install();
    StereoView::Install();
    // Each eye drawn with its own off-centre lens: the game's few calculations that take a view as centred given its whole frustum
    // (the sun's shadows, ambient occlusion, the sun's disk: LensFit.h).
    LensFit::Install();
    // In full stereo, the right eye lit by the left eye's sun shadows instead of drawing its own (ShadowShare.h).
    ShadowShare::Install();
    // The gravity gloves' target highlighted on the object itself, by the game's own highlight (GloveHighlight.h).
    GloveHighlight::Install();
    // (fix143) Workbenches in first person: the game's turn to its third-person camera skipped (WorkbenchCamera.h).
    WorkbenchCamera::Install();
    // (fix143) Ladders in first person, climbed with the hands (LadderClimb.h).
    LadderClimb::Install();
    // (fix145) Conversations on a panel of their own in front of the person speaking (ConversationPanel.h).
    ConversationPanel::Install();
    // (fix177) Build mode as a console of panels low in front of your seat (BuildConsole.h).
    BuildConsole::Install();
    // (fix178) The game's messages in one place below your view's middle, bigger (MessagesBlock.h).
    MessagesBlock::Install();
    // (fix179) Climbing with the hands: the game's ray pick and queued move checked, the move's heading hooked (Climbing.h).
    Climbing::Install();
    // (fix186) A VR tab in the game's own Settings (VrSettingsTab.h): a test with a few of the settings, one of each kind of row.
    set_vr_tab_rows();
    VrSettingsTab::Install();
    // (fix198) With it working, the main VR settings are there; the F11 menu's Main tab keeps only its own rows (else it shows them as before).
    VRConfig::s_game_vr_tab = VrSettingsTab::Working();
    return Mod::on_initialize();
}

void CreationEngineEntry::on_frame()
{
    MenuPointer::Watchdog();
    // (fix198, the review) This thread kept (the VR tab's changes are applied on it), and what that tab's handlers left for it run, in order.
    m_frame_thread = GetCurrentThreadId();
    {
        std::vector<std::function<void()>> queued;
        {
            std::scoped_lock _{ m_vr_tab_mtx };
            queued.swap(m_vr_tab_queue);
        }
        for (auto& fn : queued) {
            fn();
        }
    }
    // ...and "Recenter now" unticked once more, its tab's rows asked for again (the tab's handlers run on this thread: VrSettingsTab).
    if (m_vr_tab_refresh.exchange(false)) {
        m_vr_tab_recenter->value() = false;
        VrSettingsTab::RequestRowsAgain();
    }
    // (fix154) The left hand's weapon chosen anew: kept in the settings (saved), (fix166) with its mods.
    {
        uint32_t    id = 0;
        std::string mods;
        if (LeftWeapon::TakeChanged(id, mods)) {
            m_left_weapon_form->value() = static_cast<int32_t>(id);
            m_left_weapon_mods->value() = mods;
            g_framework->request_save_config();
        }
    }
    if (std::string strays; GrenadeHand::TakeStraysChanged(strays)) {
        m_stray_copies->value() = strays;
        g_framework->request_save_config();
    }
    // (fix165) The seat log (in the pilot's seat during a grav jump's hold the game got stuck: nothing could be done, not even a pause;
    // a helper's research: the mod kept handing the buttons over, the game did nothing with them): once a second while out of first person,
    // in the pilot's seat, or with the game holding the player (immovable, AI-controlled): the game's camera state, the player's raw state
    // words (+0xF8, +0xFC: the sit state is in them), the menus, video memory and the buttons handed over since the last line. And always: a
    // START handed over with no menu shown a second later.
    {
        static auto seat_logged = std::chrono::steady_clock::now();
        static GameFlow::ButtonsSent seat_last{};
        static int seat_lines = 0, start_lines = 0, stretch_lines = 0;
        static auto seat_outside = std::chrono::steady_clock::now();
        static int64_t start_checked = 0;
        const auto now     = std::chrono::steady_clock::now();
        const auto buttons = GameFlow::buttonsSent();
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (buttons.last_start_ms != 0 && buttons.last_start_ms != start_checked && now_ms - buttons.last_start_ms >= 1000) {
            start_checked = buttons.last_start_ms;
            if (!buttons.start_with_menu && !GameFlow::isShowingMenu() && start_lines < 20) { // (not a START that closed a menu; the review)
                ++start_lines;
                spdlog::info("[Seat] (fix165) START handed to the game {:.1f} s ago and no menu shown (first person {}, the pilot's seat {}, camera state {})",
                    (now_ms - buttons.last_start_ms) / 1000.0, GameFlow::isInFirstPerson(), GameFlow::isPilotingShip(), WorkbenchCamera::CameraStateIndex());
            }
        }
        // (In the game only: not the main menu or a loading screen, where the player is missing or not in first person; at most 60 lines a
        // stretch, a new stretch after 10 s outside it; the review.)
        const bool held  = GameFlow::isImmovable() || GameFlow::isControlledByAI();
        const bool wants = GameFlow::worldBehindMenu() && (!GameFlow::isInFirstPerson() || GameFlow::isPilotingShip() || held);
        if (!wants) {
            if (now - seat_outside >= std::chrono::seconds(10)) {
                stretch_lines = 0;
            }
        } else {
            seat_outside = now;
        }
        if (now - seat_logged >= std::chrono::seconds(1) && wants && stretch_lines < 60 && seat_lines < 600) {
            seat_logged = now;
            ++seat_lines;
            ++stretch_lines;
            const auto* player = CreationEngineSingletonManager::GetPlayerRef();
            static auto vr     = VR::get();
            spdlog::info("[Seat] (fix165) camera state {}, first person {}, the pilot's seat {}, immovable {}, AI-controlled {}, player state {:08x} "
                         "{:08x}, a menu {}, video memory {} of {} MB; handed to the game since: A {} B {} X {} Y {} START {} BACK {}",
                WorkbenchCamera::CameraStateIndex(), GameFlow::isInFirstPerson(), GameFlow::isPilotingShip(), GameFlow::isImmovable(),
                GameFlow::isControlledByAI(), player != nullptr ? player->actorState : 0u, player != nullptr ? player->actorState2 : 0u,
                GameFlow::isShowingMenu(), vr->m_d3d12.m_mem_used_mb.load(), vr->m_d3d12.m_mem_budget_mb.load(), buttons.a - seat_last.a,
                buttons.b - seat_last.b, buttons.x - seat_last.x, buttons.y - seat_last.y, buttons.start - seat_last.start, buttons.back - seat_last.back);
            seat_last = buttons;
        } else if (now - seat_logged >= std::chrono::seconds(1)) {
            seat_last = buttons; // (counted from the start of the next stretch)
        }
    }
    // (Every 30 s, for the log: how often the off-centre lens fixes were used.)
    static auto lens_logged = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - lens_logged >= std::chrono::seconds(30)) {
        lens_logged = now;
        spdlog::info("[LensFit] {}", LensFit::Status());
        spdlog::info("[Foveation] {}", Foveation::Status());
        spdlog::info("[ShadowShare] {}", ShadowShare::Status());
    }
}

void CreationEngineEntry::on_present()
{
    // (fix140) The timed frames' GPU times resolved and read back, logged every 10 s (GpuTimer.h).
    GpuTimer::OnPresent();
}

void CreationEngineEntry::apply_body_settings()
{
    auto& s = GameFlow::gStore.internalSettings;
    s.showBody = m_show_body->value();
    s.matchBodyHeight = m_match_body_height->value();
    s.bodyLean = m_body_lean->value();
    s.bodyFacing = m_body_facing->value();
    s.supportHand = m_support_hand->value();
    s.fingerPoses = m_finger_poses->value();
    s.walkingLegs = m_walking_legs->value();
    s.bodyTracking = m_body_tracking->value();
    s.handTracking = m_hand_tracking->value();
    s.realCrouch = m_real_crouch->value();
    s.crouchView = m_crouch_view->value();
    s.tieViewToBody = m_tie_view_to_body->value(); // (fix195)
    s.lockGunGrip   = m_lock_gun_grip->value();
    s.virtualStock  = m_virtual_stock->value();
    s.seatViewAtEyes = m_seat_view_at_eyes->value(); // (fix199)
    ModConstants::aimWithGunHand = m_aim_with_gun_hand->value();
    ModConstants::shootFromMuzzle = m_shoot_from_muzzle->value();
}

void CreationEngineEntry::apply_comfort_settings()
{
    auto& s = GameFlow::gStore.internalSettings;
    s.turnMode = std::clamp(m_turn_mode->value(), 0, 1);
    s.snapTurnDegrees = m_snap_turn_degrees->value();
    s.smoothTurnSpeed = m_smooth_turn_speed->value();
    s.levelView = m_level_view->value();
    s.haptics = m_haptics->value();
    s.hapticStrength = m_haptic_strength->value();
    s.hapticGameRumble = m_haptic_game_rumble->value();
    s.handInteract = m_hand_interact->value();
    s.handReach = m_hand_reach->value();
    s.impactMelee = m_impact_melee->value();
    s.meleeSwingSpeed = m_melee_swing_speed->value();
    s.meleeInstantHits = m_melee_instant_hits->value();
    s.meleeThrow = m_melee_throw->value();
    s.handGrab = m_hand_grab->value();
    s.gripGrab = m_grip_grab->value();
    s.grabStiffness = std::clamp(m_grab_stiffness->value(), 0.0f, 1.0f);
    s.grabAlign = m_grab_align->value();
    s.twoHandHold = m_two_hand_hold->value();
    s.oneInEachHand = m_one_in_each_hand->value();
    s.grabInHand = m_grab_in_hand->value();
    s.grabIgnoresYou = m_grab_ignores_you->value();
    s.gravityGloves = m_gravity_gloves->value();
    s.pullRange = std::clamp(m_pull_range->value(), 1.0f, 10.0f);
    s.gloveMarker = m_glove_marker->value();
    s.gloveHighlight = m_glove_highlight->value();
    s.gloveFrozen = m_glove_frozen->value();
    s.gloveHighlightFill = std::clamp(m_glove_highlight_fill->value(), 0.0f, 1.0f);
    s.gloveAimLine = std::clamp(m_glove_aim_line->value(), 0, 2);
    s.mouthEat = m_mouth_eat->value();
    s.bagStash = m_bag_stash->value();
    s.autoEquipWeapons = m_auto_equip_weapons->value();
    s.gunSight = m_gun_sight->value();
    s.gunSightDistance = std::clamp(m_gun_sight_distance->value(), 3.0f, 60.0f);
    s.gunLaser = m_gun_laser->value();
    s.gunLaserBeamLength = std::clamp(m_gun_laser_beam->value(), 0.05f, 2.0f);
    s.templeGestures = m_temple_gestures->value();
    s.scannerPlain = m_scanner_plain->value();
    s.levelViewMenus = m_level_view_menus->value();
    s.workbenchFirstPerson = m_workbench_first_person->value();
    s.ladderFirstPerson = m_ladder_first_person->value();
    s.climbing = m_climbing->value();                              // (fix179)
    s.climbPull = std::clamp(m_climb_pull->value(), 1.0f, 3.0f);  // (fix179)
    s.dialoguePointer = m_dialogue_pointer->value();
    s.dialogueTouch = m_dialogue_touch->value();
    s.dialoguePanel = m_dialogue_panel->value();
    s.dialogueTouchPress = m_dialogue_touch_press->value(); // (fix173)
    s.dialogueTurn = m_dialogue_turn->value();
    s.scannerGrips = m_scanner_grips->value();
    s.buildConsole = m_build_console->value(); // (fix177)
    s.messagesBlock = m_messages_block->value(); // (fix178)
    s.menuStickNavigate = m_menu_stick_navigate->value();
    s.conversationDistance = std::clamp(m_conversation_distance->value(), 0.35f, 1.0f);
    s.tradeStickTabs = m_trade_stick_tabs->value();
    s.dualWield = m_dual_wield->value();
    s.favoritesWheel = m_favorites_wheel->value();
    s.pushPowers = m_push_powers->value();
    s.pushPowerSpeed = std::clamp(m_push_power_speed->value(), 0.8f, 4.0f);
    s.shoulderGrenade = m_shoulder_grenade->value();
    s.grenadeInHand = m_grenade_in_hand->value();
    // (fix172) Physical interaction with people.
    s.physicalProbe    = m_physical_probe->value();
    s.grabBodies       = m_grab_bodies->value();
    s.bodyGrabStrength = std::clamp(m_body_grab_strength->value(), 0.0f, 1.0f);
    s.keepHeldDown     = m_keep_held_down->value();
    s.grabKnocksDown   = m_grab_knocks_down->value();
    s.holdPeople       = m_hold_people->value();
    s.shovePeople      = m_shove_people->value();
    s.punchKnockdown   = m_punch_knockdown->value();
    s.knockdownSpeed   = std::clamp(m_knockdown_speed->value(), 3.0f, 10.0f);
    s.saberBlock       = m_saber_block->value();
    s.shoveAnger       = m_shove_anger->value();
    s.spareFollowers   = m_spare_followers->value();
}

// (fix198, the review) The game's Settings menu runs the VR tab's handlers on the frame step's thread in testing (the log's "[VrTab] ...
// first reached on thread" and this function's line): then each change is applied at once, as before. On another thread it is kept for
// the next frame step (on_frame), so the mod's settings are set on one thread.
void CreationEngineEntry::on_frame_thread(std::function<void()> fn)
{
    const uint32_t frame = m_frame_thread.load(), here = GetCurrentThreadId();
    static std::atomic<bool> s_told{ false };
    if (!s_told.exchange(true)) {
        spdlog::info("[VrTab] (fix198) the VR tab's changes come on thread {}, the frame step runs on thread {}: {}", here, frame,
            frame == 0 || frame == here ? "applied at once" : "applied at the next frame step");
    }
    if (frame == 0 || frame == here) {
        fn();
        return;
    }
    std::scoped_lock _{ m_vr_tab_mtx };
    if (m_vr_tab_queue.size() < 256) { // (a frame step at least every few seconds even at the main menu: never this many)
        m_vr_tab_queue.push_back(std::move(fn));
    }
}

void CreationEngineEntry::set_vr_tab_rows()
{
    // (fix186) The test's rows: a heading, a stepper (the turn mode; the snap angle, a slider's setting in steps of 5), sliders and
    // checkboxes, rows usable only with another's choice. Each change is applied as the F11 window's (the same settings, saved).
    // (fix198) The main VR settings, all here, out of the F11 menu (its Advanced tab shows them too only when asked for): recentring,
    // the eye mode and the sharpness, the body, turning, the controllers, vibration, the HUD's size, climbing. Every row is the same
    // setting as the F11 menu's.
    using VrSettingsTab::Kind;
    using VrSettingsTab::Row;
    // (fix198, the review) Each row's apply on the frame step's thread (on_frame_thread), and its log lines capped (the tab's Defaults
    // runs every row's apply at once).
    const auto later = [this](std::function<void()> fn) {
        return std::function<void()>{ [this, fn = std::move(fn)] { on_frame_thread(fn); } };
    };
    const auto comfort = later([this] {
        apply_comfort_settings();
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1) < 30) {
            log_comfort_settings();
        }
    });
    const auto body = later([this] {
        apply_body_settings();
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1) < 30) {
            log_body_settings();
        }
    });
    const auto percent = [](float v) { return std::format("{:.0f}%", v * 100.0f); };
    auto& vr = VR::get();
    std::vector<Row> rows;
    const auto header = [&rows](const char* text, const char* description) {
        Row r{};
        r.kind        = Kind::Header;
        r.text        = text;
        r.description = description != nullptr ? description : "";
        rows.push_back(std::move(r));
    };

    header("Recentring", "The VR mod's main settings (the rest are in its F11 menu's Advanced tab). Defaults puts every row here back to the "
                         "mod's default: the eye mode to Full stereo and the sharpness to 1.0 too.");
    {
        // (A checkbox that acts once: ticked, the view is recentred and it is unticked at once, as the tab is built anew, and once more a
        // frame step on (m_vr_tab_refresh). Its setting is not saved: it is not in m_options.)
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Recenter now";
        r.description = "Tick it to put the view and your body where you are now (it unticks itself). Sit or stand as you will play and "
                        "look straight ahead first.";
        r.toggle      = m_vr_tab_recenter.get();
        r.apply       = [this] {
            if (m_vr_tab_recenter->value()) {
                m_vr_tab_recenter->value() = false;
                m_vr_tab_refresh           = true;
                on_frame_thread([] { CreationEngineCameraManager::recenter_head("the game's Settings > VR tab"); });
            }
        };
        r.rebuild     = true;
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Recenter after loading screens";
        r.description = "After every loading screen the view is put where you are, as with Recenter now.";
        r.toggle      = m_recenter_after_loading.get();
        r.apply       = later([this] { ModConstants::recenterAfterLoading = m_recenter_after_loading->value(); });
        rows.push_back(std::move(r));
    }

    header("View", nullptr);
    {
        // (fix198, the review) The refusal checked at once (atomics; its reason kept under its lock, its buzz asked for), the three switches
        // set on the frame step's thread; the choice shown by the tab meanwhile (m_eye_mode_pending).
        const auto choose = [this](int choice, const char* where) {
            if (VR::get()->eye_mode_refused(choice, where)) {
                return false;
            }
            m_eye_mode_pending = choice;
            on_frame_thread([this, choice] {
                VR::get()->choose_eye_mode(choice);
                int expected = choice;
                m_eye_mode_pending.compare_exchange_strong(expected, -1);
            });
            return true;
        };
        Row r{};
        r.kind      = Kind::Stepper;
        r.text      = "Eye mode";
        r.options   = { "Full stereo", "Stereo, eyes take turns", "Mono", "AER" };
        r.get_index = [this] {
            const int pending = m_eye_mode_pending.load();
            return static_cast<uint32_t>(std::clamp(pending >= 0 ? pending : VR::get()->eye_mode_choice(), 0, 3));
        };
        r.set_index = [choose](uint32_t index) { return choose(static_cast<int>(index), "the game's Settings > VR tab"); };
        r.reset     = [choose] { choose(0, "the game's Settings > VR tab (Defaults)"); };
        r.describe  = [] {
            std::string text = "Full stereo: real depth, the heaviest (the world drawn twice a frame). Eyes take turns: depth, cheaper, each eye "
                               "a new picture every other frame. Mono: no depth, the most fps. AER: one eye drawn a frame, light and smooth. "
                               "Shown once you close the menu (the game's menus are shown in mono). The left grip held + a right stick click "
                               "switches between stereo and mono too.";
            // (Its last refusal, while recent: worded as past, since the tab is built anew only as it is opened or changed.)
            if (const std::string refusal = VR::get()->eye_mode_refusal(); !refusal.empty()) {
                text += " " + refusal;
            }
            return text;
        };
        r.rebuild   = true;
        rows.push_back(std::move(r));
    }
    {
        // A stepper over the resolution scale in tenths (each step one remaking of the headset's images, never one per hundredth of a
        // slider), (fix198, the review) applied once it has not changed for 750 ms and no game menu is shown (VR::apply_resolution_scale,
        // on_begin_rendering): as the Settings menu closes; at the main menu, once a save is loaded.
        Row r{};
        r.kind        = Kind::Stepper;
        r.text        = "Sharpness (resolution)";
        r.description = "1.0, the headset's own size, by default. Higher: sharper, but fewer fps and more video memory. Lower: more fps, but "
                        "blurrier (0.8 has about 64% of the pixels of 1.0). Applied as you close the menu (at the main menu: seen once a "
                        "save is loaded).";
        r.slider      = vr->m_resolution_scale.get();
        for (int tenths = 6; tenths <= 15; ++tenths) {
            const float value = tenths / 10.0f;
            r.options.push_back(tenths == 10 ? std::string{"1.0 (the headset's own)"} : std::format("{:.1f}", value));
            r.values.push_back(value);
        }
        r.enabled     = [] {
            const auto runtime = VR::get()->get_runtime();
            return runtime != nullptr && runtime->is_openxr();
        };
        r.apply       = [] { VR::get()->apply_resolution_scale(); }; // (any thread: it only notes the value and the time)
        rows.push_back(std::move(r));
    }

    header("Body", nullptr);
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Body and motion controls";
        r.description = "Your body shown, its arms following the controllers, the gun in your hand: you aim with it. Off: no body, the gun "
                        "follows your head. More body settings are in the F11 menu (Advanced > Body).";
        r.toggle      = m_show_body.get();
        r.apply       = body;
        rows.push_back(std::move(r));
    }
    {
        // (fix199) Seated: the view at the seated body's eyes (SeatView).
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Seat: view at the body's eyes";
        r.description = "Sitting in a chair or the pilot's seat (landed), your view starts at your seated body's eyes. Leaning more than "
                        "15 cm from where you sat is held back softly, never past 30 cm. Off: the game's own seat camera.";
        r.toggle      = m_seat_view_at_eyes.get();
        r.apply       = body;
        rows.push_back(std::move(r));
    }

    header("Turning", nullptr);
    {
        Row r{};
        r.kind        = Kind::Stepper;
        r.text        = "Turn mode";
        r.description = "How the right thumbstick turns you: in steps (snap) or smoothly.";
        r.combo       = m_turn_mode.get();
        r.options     = { "Snap", "Smooth" };
        r.apply       = comfort;
        r.rebuild     = true;
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Stepper;
        r.text        = "Snap angle";
        r.description = "How far one push of the right thumbstick turns you with snap turning.";
        r.slider      = m_snap_turn_degrees.get();
        for (int degrees = 15; degrees <= 90; degrees += 5) {
            r.options.push_back(std::format("{} degrees", degrees));
            r.values.push_back(static_cast<float>(degrees));
        }
        r.enabled = [this] { return m_turn_mode->value() == 0; };
        r.apply   = comfort;
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Slider;
        r.text        = "Smooth turn speed";
        r.description = "How fast smooth turning turns you (100% is the game's own look speed).";
        r.slider      = m_smooth_turn_speed.get();
        r.show        = percent;
        r.enabled     = [this] { return m_turn_mode->value() == 1; };
        r.apply       = comfort;
        rows.push_back(std::move(r));
    }

    header("Controllers", nullptr);
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Left-handed";
        r.description = "The controllers swap jobs: the left one fires, aims and turns, the right one walks (the face buttons keep their letters).";
        r.toggle      = vr->m_swap_controllers.get();
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Face buttons as printed";
        r.description = "On: A, B, X and Y each send the game's button of the same letter, so the game's prompts match (B is back, X drop). "
                        "Off: the old layout (left X = B, left Y = X, right B = Y).";
        r.toggle      = m_face_buttons_as_printed.get();
        r.apply       = later([this] {
            GameFlow::gStore.internalSettings.faceButtonsAsPrinted = m_face_buttons_as_printed->value();
            spdlog::info("[Buttons] (fix194) face buttons as printed: {}", m_face_buttons_as_printed->value() ? "on" : "off (the old layout)");
        });
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Controller vibration";
        r.description = "The controllers vibrate when you fire, get hit or reload (the game's own rumble passed on: keep the game's Controller "
                        "Vibrations on).";
        r.toggle      = m_haptics.get();
        r.apply       = comfort;
        r.rebuild     = true;
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Slider;
        r.text        = "Vibration strength";
        r.description = "How strongly the controllers vibrate.";
        r.slider      = m_haptic_strength.get();
        r.show        = percent;
        r.enabled     = [this] { return m_haptics->value(); };
        r.apply       = comfort;
        rows.push_back(std::move(r));
    }

    header("HUD", nullptr);
    {
        Row r{};
        r.kind        = Kind::Slider;
        r.text        = "HUD size";
        r.description = "The game's HUD (health, ammo, compass) in your view: smaller leaves more of the view free.";
        r.slider      = m_hud_scale.get();
        r.show        = percent;
        r.apply       = later([this] { GameFlow::gStore.hudSettings.hudScale = m_hud_scale->value(); });
        rows.push_back(std::move(r));
    }

    header("Climbing", nullptr);
    {
        Row r{};
        r.kind        = Kind::Checkbox;
        r.text        = "Climb with your hands";
        r.description = "Put a hand against a wall, rock or ledge, squeeze its grip and pull yourself along; let go of every grip to drop. "
                        "With your weapon put away or fists only.";
        r.toggle      = m_climbing.get();
        r.apply       = comfort;
        r.rebuild     = true;
        rows.push_back(std::move(r));
    }
    {
        Row r{};
        r.kind        = Kind::Slider;
        r.text        = "Climbing pull";
        r.description = "Your body moves this many times your hand's pull (1.0x: exactly with your hand; more reaches further each pull, seated).";
        r.slider      = m_climb_pull.get();
        r.show        = [](float v) { return std::format("{:.1f}x", v); };
        r.enabled     = [this] { return m_climbing->value(); };
        r.apply       = comfort;
        rows.push_back(std::move(r));
    }
    VrSettingsTab::SetRows(std::move(rows));
}

// (fix196) The settings' log lines after a change in the F11 menu (as before, now one place for every tab).
void CreationEngineEntry::log_body_settings()
{
    spdlog::info("[QuickTest] (fix195) weapon steadiness: tie the view to the body {}, lock the gun's grip {}, virtual stock {}",
        m_tie_view_to_body->value(), m_lock_gun_grip->value(), m_virtual_stock->value());
    spdlog::info("[QuickTest] (fix199) seated: the view at the body's eyes {}", m_seat_view_at_eyes->value());
    spdlog::info("[QuickTest] body: shown {}, aim with gun {}, shots from muzzle {}, two-handed {}, fingers {}, lean {}, facing {}, legs {}, height {}, body tracking {}, hand tracking {}, crouch lowers view {}, real crouch {}",
        m_show_body->value(), m_aim_with_gun_hand->value(), m_shoot_from_muzzle->value(), m_support_hand->value(), m_finger_poses->value(),
        m_body_lean->value(), m_body_facing->value(), m_walking_legs->value(), m_match_body_height->value(), m_body_tracking->value(),
        m_hand_tracking->value(), m_crouch_view->value(), m_real_crouch->value());
}

void CreationEngineEntry::log_comfort_settings()
{
    spdlog::info("[QuickTest] turning {} ({:.0f} deg, smooth speed {:.2f}), vibration {} (strength {:.2f}, game rumble {}), hand interaction {} (reach {:.2f} m), impact melee {} (swing {:.1f} m/s, instant hits {}), melee throw {}, hand grab {} (grip {}, stiffness {:.2f}, square in hand {}, drawn in hand {}, ignores you {}, gravity gloves {} reaching {:.1f} m, marker {}, pointing with {}, eat and drink at your mouth {}, bag {}, picked-up weapons {})",
        m_turn_mode->value() == 0 ? "snap" : "smooth", m_snap_turn_degrees->value(), m_smooth_turn_speed->value(),
        m_haptics->value(), m_haptic_strength->value(), m_haptic_game_rumble->value(), m_hand_interact->value(), m_hand_reach->value(),
        m_impact_melee->value(), m_melee_swing_speed->value(), m_melee_instant_hits->value(), m_melee_throw->value(), m_hand_grab->value(), m_grip_grab->value(),
        m_grab_stiffness->value(), m_grab_align->value(), m_grab_in_hand->value(), m_grab_ignores_you->value(), m_gravity_gloves->value(),
        m_pull_range->value(), m_glove_marker->value(), m_glove_aim_line->value() == 1 ? "your palm" : (m_glove_aim_line->value() == 2 ? "sighting along your hand" : "your hand"),
        m_mouth_eat->value(), m_bag_stash->value(), m_auto_equip_weapons->value());
    spdlog::info("[QuickTest] (fix172) people: grab bodies {} (strength {:.2f}, kept down {}, a grip holds standing people {} (fix173), else knocks them down {}), shoves {}, punch knockdowns {} ({:.1f} m/s), "
                 "blade blocks {}, anger {}, followers spared {}, probe {}",
        m_grab_bodies->value(), m_body_grab_strength->value(), m_keep_held_down->value(), m_hold_people->value(), m_grab_knocks_down->value(), m_shove_people->value(),
        m_punch_knockdown->value(), m_knockdown_speed->value(), m_saber_block->value(), m_shove_anger->value(), m_spare_followers->value(),
        m_physical_probe->value());
    spdlog::info("[QuickTest] gun sight {} ({:.0f} m), push powers {} ({:.1f} m/s), shoulder grenades {} (in your hand {})", m_gun_sight->value(),
        m_gun_sight_distance->value(), m_push_powers->value(), m_push_power_speed->value(), m_shoulder_grenade->value(), m_grenade_in_hand->value());
    spdlog::info("[QuickTest] (fix173) conversations: answers pressed with a finger {}, the right stick turns {}; the grips as LB and RB in the "
                 "scanner and the star map {}",
        m_dialogue_touch_press->value(), m_dialogue_turn->value(), m_scanner_grips->value());
    spdlog::info("[QuickTest] (fix177) build mode as a console {}; (fix178) messages below the middle {}", m_build_console->value(),
        m_messages_block->value());
    spdlog::info("[QuickTest] (fix179) climbing {} (pull x{:.1f})", m_climbing->value(), m_climb_pull->value());
}

// (fix196) The F11 menu's Main tab: recentring (part 0), the body and motion controls (2), turning (3), the face buttons and vibration (4), the
// HUD's size (5). (The eye mode, the sharpness and left-handed are the VR module's; this menu's own size the framework's.)
void CreationEngineEntry::on_draw_main_ui(int part)
{
    using menu_ui::hint;
    // (fix198) With the game's Settings > VR tab working, these rows are there (the same settings); here only a line saying so. (fix198,
    // the review) Drawn also in the Advanced tab's "Main VR settings" group when asked for (VRConfig::s_main_rows_here).
    const bool in_game_tab = VrSettingsTab::Working() && !VRConfig::s_main_rows_here.load();
    if (part == 0) {
        // (fix196, the review) Another game version than this build's: said at the very top, as the install notes promise (on Main).
        if (!VRConfig::s_main_rows_here.load() && !GameCode::VersionMatches()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", GameCode::Status().c_str());
            ImGui::PopStyleColor();
        }
        if (in_game_tab) {
            hint("The main VR settings are in the game's Settings > VR tab (Pause > Settings > VR, or Settings on the main menu): the eye "
                 "mode, sharpness, recentring, the body, turning, left-handed, the face buttons, vibration and the HUD's size. Everything else is "
                 "in this menu's Advanced tab. If that tab is not shown (another mod may replace the Settings menu), turn on \"Show the main "
                 "VR settings here too\" at the top of Advanced.");
            return;
        }
        if (ImGui::Button("Recenter now")) {
            CreationEngineCameraManager::recenter_head("button");
        }
        ImGui::SameLine();
        if (m_recenter_after_loading->draw("After loading screens too")) {
            ModConstants::recenterAfterLoading = m_recenter_after_loading->value();
            spdlog::info("[QuickTest] Recenter after loading screens -> {}", m_recenter_after_loading->value() ? "on" : "off");
        }
        hint("Sit or stand as you will play and look straight ahead: the view and your body are put where you are.");
    } else if (in_game_tab) {
        return; // (fix198: the rows below are in the game's Settings > VR tab)
    } else if (part == 2) {
        if (m_show_body->draw("Body and motion controls")) {
            apply_body_settings();
            log_body_settings();
        }
        hint(m_show_body->value() ? "Your body is shown, its arms follow the controllers and the gun is in your hand: you aim with it (more in "
                                    "Advanced > Body)."
                                  : "Off: no body; the gun follows your head (the older way of aiming).");
    } else if (part == 3) {
        bool changed = m_turn_mode->draw("Turning (right stick)");
        if (m_turn_mode->value() == 0) {
            if (m_snap_turn_degrees->draw("Snap angle (degrees)")) {
                m_snap_turn_degrees->value() = std::round(m_snap_turn_degrees->value() / 5.0f) * 5.0f;
                changed = true;
            }
        } else {
            changed |= m_smooth_turn_speed->draw("Smooth turn speed");
            hint("1 is the game's own look speed.");
        }
        if (changed) {
            apply_comfort_settings();
            log_comfort_settings();
        }
    } else if (part == 4) {
        if (m_face_buttons_as_printed->draw("Face buttons as printed")) {
            GameFlow::gStore.internalSettings.faceButtonsAsPrinted = m_face_buttons_as_printed->value();
            spdlog::info("[Buttons] (fix194) face buttons as printed: {}", m_face_buttons_as_printed->value() ? "on" : "off (the old layout)");
        }
        hint("On: A, B, X and Y each send the game's button of the same letter, so the game's prompts match (B is back, X drop). Off: the old "
             "layout (left X = B, left Y = X, right B = Y).");
        bool changed = m_haptics->draw("Controller vibration");
        if (m_haptics->value()) {
            changed |= m_haptic_strength->draw("Vibration strength");
        }
        hint("When you fire, get hit or reload (the game's own rumble passed on: keep Starfield's Controller Vibrations setting on).");
        if (changed) {
            apply_comfort_settings();
            log_comfort_settings();
        }
    } else if (part == 5) {
        if (m_hud_scale->draw("HUD size")) {
            GameFlow::gStore.hudSettings.hudScale = m_hud_scale->value();
        }
        hint("The game's HUD (health, ammo, compass) in your view: lower is smaller, more of the view free.");
    }
}

// (fix196) The Advanced tab: every other gameplay setting, by feature (part 0), the HUD's and the aim's look (1), conversations, the scanner,
// menus and the controllers' layout (2).
void CreationEngineEntry::on_draw_advanced_ui(int part)
{
    using menu_ui::group;
    using menu_ui::hint;
    auto log_change = [](const char* what, bool value) { spdlog::info("[QuickTest] {} -> {}", what, value ? "on" : "off"); };
    if (part == 0) {
        if (group("Body")) {
            bool changed = false;
            // (fix199) The game's own seated body: with the mod's body on or off.
            changed |= m_seat_view_at_eyes->draw("Seat: view at the body's eyes");
            hint("Sitting (chairs, the pilot's seat when landed): the view at your seated body's eyes; leaning past 15 cm held softly, at "
                 "most 30 cm. Works with the body below on or off.");
            if (!m_show_body->value()) {
                hint("The body is off (Settings > VR: \"Body and motion controls\"): the settings below take effect with it on.");
            } else {
                ImGui::TextWrapped("Status: %s", body::IsActive() ? "body shown" : "waiting (first person, headset on, controllers in use)");
            }
            changed |= m_aim_with_gun_hand->draw("Aim with the gun");
            hint("Shots follow your gun hand while the weapon is out.");
            changed |= m_shoot_from_muzzle->draw("Shots leave from the gun's muzzle");
            changed |= m_support_hand->draw("Two-handed aim (squeeze the left grip at the gun's front)"); // (fix198)
            hint("The gun points along the line between your hands while the left grip is squeezed there. A left hand only resting near the "
                 "gun no longer steers it.");
            changed |= m_virtual_stock->draw("Virtual stock");
            hint("Two-handed, the gun hand near your shoulder: the aim steadied by the shoulder.");
            changed |= m_lock_gun_grip->draw("Lock the gun's grip");
            hint("The hand holds each gun the same way (live while reloading).");
            changed |= m_tie_view_to_body->draw("Tie the view to the body");
            hint("The gun does not shake against the view while you move.");
            changed |= m_finger_poses->draw("Fingers follow trigger, grip and thumb");
            changed |= m_body_lean->draw("Lean");
            hint("The spine bends toward your head as you lean.");
            changed |= m_body_facing->draw("Torso turns toward your hands");
            changed |= m_walking_legs->draw("Stepping legs while moving");
            changed |= m_crouch_view->draw("Crouch button lowers your view");
            hint("To the crouched body's eyes.");
            changed |= m_real_crouch->draw("Real crouch");
            hint("The body bends when you crouch in real life (standing play; leave off when seated).");
            changed |= m_match_body_height->draw("Match your height to the character's");
            changed |= m_body_tracking->draw("Headset body tracking (Quest)");
            hint("Used when the runtime offers it.");
            changed |= m_hand_tracking->draw("Hand tracking for the fingers");
            hint("Used when the runtime offers it.");
            if (changed) {
                apply_body_settings();
                log_body_settings();
            }
        }
        if (group("Hands: interaction, grabbing and gravity gloves")) {
            bool changed = false;
            changed |= m_hand_interact->draw("Hand interaction");
            hint("Weapon put away: the prompt is for whatever is near your right hand.");
            if (m_hand_interact->value()) {
                changed |= m_hand_reach->draw("Hand reach (m)");
                hint("How far in front of the controller counts as near your hand.");
            }
            changed |= m_hand_grab->draw("Hand grab");
            hint("An object you pick up is held firmly in your hand and turns with it.");
            if (m_hand_grab->value()) {
                changed |= m_grip_grab->draw("Grab with the grip");
                hint("Weapon put away: squeeze a grip next to an object; let go to drop or throw it.");
                changed |= m_grab_stiffness->draw("Hold stiffness");
                hint("1: locked to your hand; 0: the game's floaty hold.");
                changed |= m_grab_align->draw("Held objects sit square in your hand");
                hint("Round ones lie across the palm.");
                changed |= m_two_hand_hold->draw("Both hands on one object");
                hint("Grip a held object with your other hand to hold and turn it with both.");
                changed |= m_one_in_each_hand->draw("One object in each hand");
                hint("Grip with your other hand away from what you hold to grab something else.");
                changed |= m_grab_in_hand->draw("Draw held objects exactly in your hand");
                hint("No sliding while you move; their physics shown while they touch something.");
                changed |= m_grab_ignores_you->draw("Held objects don't bump into your body");
                if (m_grip_grab->value()) {
                    changed |= m_gravity_gloves->draw("Gravity gloves");
                    hint("Grip with nothing near: point at an object, flick your hand back, catch it.");
                    if (m_gravity_gloves->value()) {
                        changed |= m_pull_range->draw("Gravity gloves reach (m)");
                        if (m_glove_marker->draw("Show what the gloves aim at")) {
                            changed = true;
                            if (m_glove_marker->value()) {
                                VR::get()->retry_reticle(); // (it shares the gun sight's picture)
                            }
                        }
                        hint("Blue on it, white once locked on; a blue dot shows your line before.");
                        if (m_glove_marker->value()) {
                            changed |= m_glove_highlight->draw("...on the object itself");
                            hint("Its own shape, as the hand scanner outlines items: blue, white once locked on.");
                            if (m_glove_highlight->value()) {
                                changed |= m_glove_frozen->draw("...glowing instead of the outline");
                                hint("The game's pulsing signal glow.");
                                if (!m_glove_frozen->value()) {
                                    changed |= m_glove_highlight_fill->draw("...its glow over the object");
                                    hint("0: the outline only; the object is always seen through it.");
                                }
                            }
                        }
                        changed |= m_glove_aim_line->draw("Gloves point with");
                        if (m_glove_aim_line->value() == 1) {
                            hint("With your fists out, a grip held palm out is a power push (Powers), not the gloves: put your weapon away to pull "
                                 "with your palm, or turn the powers off.");
                        }
                        if (m_glove_marker->value() && VR::get()->reticle_blocked()) {
                            ImGui::TextWrapped("The marker is off: the game stopped while the gun sight's picture was being set up last time. "
                                               "Turn this off and on to try again.");
                        }
                    }
                    changed |= m_mouth_eat->draw("Eat and drink at your mouth");
                    hint("Hold food or a drink at your mouth (a steady buzz) and let go there.");
                    changed |= m_bag_stash->draw("Bag behind a shoulder or at a hip");
                    hint("Let go of something there (a light buzz) and it goes in your inventory.");
                    changed |= m_auto_equip_weapons->draw("Weapons you pick up");
                    hint("Pull the trigger to equip it (swing and let go to throw it); dropping it, or putting it in the bag (gun hand behind a "
                         "shoulder: squeeze and let go of the grip), brings back your weapon.");
                }
            }
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("Melee and throwing")) {
            bool changed = false;
            changed |= m_impact_melee->draw("Impact melee");
            hint("Weapon out: swing hard to attack; it hits what your hand or weapon swept through.");
            if (m_impact_melee->value()) {
                changed |= m_melee_swing_speed->draw("Swing speed that attacks (m/s)");
                hint("Higher: only hard swings.");
                changed |= m_melee_instant_hits->draw("Hit the moment a swing connects");
                hint("Every swing counts (no attack animation or oxygen cost); off: swings press melee.");
            }
            changed |= m_melee_throw->draw("Throw your melee weapon");
            hint("Squeeze the grip, swing and let go; squeeze again (or pull the trigger) to call it back.");
            changed |= m_dual_wield->draw("Dual wielding");
            hint("The left trigger on a melee weapon in the inventory puts it in your left hand (again: takes it out).");
            changed |= m_saber_block->draw("Block with your blade");
            hint("An attack at you that meets your blade misses, and the attacker recoils.");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("People (grab, shove, knock down)")) {
            bool changed = false;
            changed |= m_grab_bodies->draw("Grab and drag bodies");
            hint("Corpses and people knocked down, by the limb your hand touches (hand grab).");
            if (m_grab_bodies->value()) {
                changed |= m_body_grab_strength->draw("Body hold strength");
                hint("0: the game's floaty drag; 1: firm (never strong enough to stretch them).");
                changed |= m_keep_held_down->draw("A person you hold stays down");
                hint("While you hold them, at most 30 s.");
                changed |= m_hold_people->draw("Gripping a standing person holds them");
                hint("Weapons put away: their arm bent to your hand, dragged where you pull.");
                changed |= m_grab_knocks_down->draw("...else gripping knocks them down");
                hint("With the row above off: gripping a standing person knocks them down, then you hold them.");
            }
            changed |= m_shove_people->draw("Shove");
            hint("Weapons put away, push someone with an open hand (light: they are bumped; firm: they stagger).");
            changed |= m_punch_knockdown->draw("Hard punches knock down");
            hint("Fists out; the game gets them up.");
            if (m_punch_knockdown->value()) {
                changed |= m_knockdown_speed->draw("Punch speed that knocks down (m/s)");
            }
            changed |= m_shove_anger->draw("People get angry");
            hint("Shoving or knocking someone down again and again raises the game's assault alarm.");
            changed |= m_spare_followers->draw("Spare your followers");
            hint("Your hand's hits and thrown weapons never hurt them.");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("Aiming, powers and grenades")) {
            bool changed = false;
            if (m_gun_sight->draw("Gun sight")) {
                changed = true;
                if (m_gun_sight->value()) {
                    VR::get()->retry_reticle(); // turned back on: try setting it up again
                }
            }
            hint("A red dot where your gun's shots go (turn the game's crosshair off: Settings > Interface).");
            if (m_gun_sight->value()) {
                changed |= m_gun_laser->draw("Laser");
                hint("A red beam from the gun, its dot on what it points at.");
                if (m_gun_laser->value()) {
                    changed |= m_gun_laser_beam->draw("Beam length (m)");
                    hint("It fades to nothing at its end, hidden by the gun and what is in front of it.");
                }
                changed |= m_gun_sight_distance->draw(m_gun_laser->value() ? "Distance when nothing is found (m)" : "Gun sight distance (m)");
                if (!m_gun_laser->value()) {
                    hint("Exact at this range, a little off nearer.");
                }
                if (VR::get()->reticle_blocked()) {
                    ImGui::TextWrapped("The gun sight is off: the game stopped while it was being set up last time. Turn it off and on to try again.");
                }
            }
            changed |= m_push_powers->draw("Powers by pushing your palm");
            hint("Hold a grip, turn your palm out and push it toward the target (not the gun hand).");
            if (m_push_powers->value()) {
                changed |= m_push_power_speed->draw("Push speed that uses your power (m/s)");
                hint("Higher: only hard pushes.");
            }
            changed |= m_shoulder_grenade->draw("Grenades from your shoulder");
            hint("Right grip behind your right shoulder takes one; let go as you throw.");
            if (m_shoulder_grenade->value()) {
                changed |= m_grenade_in_hand->draw("The grenade shown in your hand");
                hint("A copy that cannot be taken, gone when you throw.");
            }
            changed |= m_favorites_wheel->draw("Favorites wheel");
            hint("Hold the left grip, then squeeze the left trigger; each hand picks a slice, let go of the trigger to choose (off: the game's own "
                 "favorites menu).");
            changed |= m_temple_gestures->draw("Flashlight and scanner at your temples");
            hint("The left trigger by your left temple: the flashlight; the right trigger by your right: the hand scanner.");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("Climbing, ladders and workbenches")) {
            bool changed = false;
            changed |= m_climbing->draw("Climbing with your hands");
            hint("Put a hand against a wall, rock or ledge, squeeze its grip and pull yourself along; let go of every grip to drop (a plain fall). "
                 "Weapon put away or fists only; an object or person next to the hand is grabbed instead.");
            if (m_climbing->value()) {
                changed |= m_climb_pull->draw("Climbing pull");
                hint("Your body moves this many times your hand's pull (1: exactly with your hand; more reaches further each pull, seated).");
            }
            changed |= m_ladder_first_person->draw("Ladders in first person");
            hint("Squeeze a grip and pull down to climb up, push up to climb down.");
            changed |= m_workbench_first_person->draw("Workbenches in first person");
            hint("The game would turn to a view of your body from behind.");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("View on foot")) {
            bool changed = false;
            changed |= m_level_view->draw("Level view");
            hint("Look up and down with your head only (the right stick's up and down no longer tilt the view).");
            changed |= m_level_view_menus->draw("Level view in containers, trading and workbenches");
            hint("The view kept level as they open (not turned toward what you used).");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
    } else if (part == 1) {
        // (fix198, the review) Recentring is in the game's Settings > VR tab (and in "Main VR settings" at the top, when asked for).
        if (group("HUD and the aim's look")) {
            if (m_hud_perspective->draw("HUD depth")) {
                GameFlow::gStore.hudSettings.perspective = (int)m_hud_perspective->value();
            }
            hint("How far away the HUD looks (each eye's picture of it moved apart).");
            if (m_dominant_eye->draw("Dominant eye")) {
                ModConstants::dominantEye = m_dominant_eye->value();
            }
            if (m_sights_on_dominant_eye->draw("Line up sights with the dominant eye when aiming")) {
                ModConstants::sightsOnDominantEye = m_sights_on_dominant_eye->value();
            }
            if (m_hud_on_optical_center->draw("Centre the HUD and crosshair where you look")) {
                ModConstants::hudOnOpticalCenter = m_hud_on_optical_center->value();
            }
            if (m_weapon_stereo_depth->draw("Weapon and arms stereo depth")) {
                ModConstants::weaponStereoDepth = m_weapon_stereo_depth->value();
            }
            hint("Lower it if the gun or arms look doubled.");
            if (m_disable_zoom->draw("Prevent the game's zooming")) {
                GameFlow::gStore.internalSettings.preventZoom = m_disable_zoom->value();
            }
            hint("The game's own zoom while aiming down the sights, left out.");
        }
    } else if (part == 2) {
        if (group("Conversations, the scanner and menus")) {
            bool changed = false;
            changed |= m_dialogue_pointer->draw("Conversations: point and pull the trigger");
            hint("Point at a response with a hand and pull its trigger.");
            changed |= m_dialogue_touch->draw("Conversations: answers within reach");
            hint("The answers bigger, at arm's length.");
            if (m_dialogue_touch->value()) {
                changed |= m_dialogue_touch_press->draw("Conversations: press answers with a finger");
                hint("Push your index finger into an answer (off: point at it and pull the trigger).");
            }
            changed |= m_dialogue_panel->draw("Conversations on a panel of their own");
            hint("Fixed in the room in front of the person speaking, with the answers above.");
            changed |= m_conversation_distance->draw("Conversation panel distance (m)");
            hint("It looks as large at any distance.");
            changed |= m_dialogue_turn->draw("Conversations: the right stick turns smoothly");
            hint("Whatever the turn mode; the panel stays in front of the person.");
            changed |= m_menu_stick_navigate->draw("Menus: the left stick moves through them");
            hint("As on a gamepad; the hand pointer steps aside until you move your hand or pull a trigger.");
            changed |= m_trade_stick_tabs->draw("Trading: the left stick switches sides");
            hint("Left and right switch between the trader, you and your ship (as LB and RB).");
            changed |= m_scanner_plain->draw("A plain hand scanner");
            hint("Without the blue tint, the visor frame and the dark edges (only its circles and text).");
            changed |= m_scanner_grips->draw("The grips as LB and RB in the scanner");
            hint("The hand scanner, the star map and build mode: the left grip taps LB, the right RB (no grab or throw there).");
            changed |= m_build_console->draw("Build mode as a console");
            hint("The item card, the list, the button hints and the outpost's footer on panels low in front of you; the action cards and "
                 "warnings near the middle of your view (off: all in your view as before).");
            changed |= m_messages_block->draw("Messages below the middle of your view");
            hint("Notifications, mission updates, skill challenges and level-ups together, bigger, a little below where you look (off: where the "
                 "HUD has them).");
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("Controller layout and vibration")) {
            if (m_alternative_joy_layout->draw("Alternative joy layout")) {
                GameFlow::gStore.internalSettings.alternativeJoyLayout = m_alternative_joy_layout->value();
            }
            hint("For controllers without thumbrest sensors: LB and RB from the left grip + a trigger instead of the thumbrests (the favorites wheel and the scanner grips off).");
            bool changed = false;
            if (m_haptics->value()) {
                changed |= m_haptic_game_rumble->draw("Pass on the game's own rumble");
                hint("Hits, explosions, landing.");
                if (ImGui::Button("Test vibration")) {
                    auto vr = VR::get();
                    for (int hand = 0; hand < 2; ++hand) {
                        vr->trigger_haptic_vibration(0.0f, 0.3f, 0.0f, m_haptic_strength->value(), hand == 0 ? vr->get_left_joystick() : vr->get_right_joystick());
                    }
                    spdlog::info("[QuickTest] Test vibration (strength {:.2f})", m_haptic_strength->value());
                }
            } else {
                hint("Vibration is off (Settings > VR: \"Controller vibration\").");
            }
            if (changed) {
                apply_comfort_settings();
                log_comfort_settings();
            }
        }
        if (group("Game window")) {
            if (m_desktop_window_on_main_menu->draw("Fit the game window to the desktop on the main menu")) {
                ModConstants::desktopWindowOnMainMenu = m_desktop_window_on_main_menu->value();
                log_change("Fit the game window to the desktop screen on the main menu", m_desktop_window_on_main_menu->value());
            }
            hint("So the mouse can be used there; in play the window is the headset's size.");
        }
    }
}

// (fix196) The Debug tab: the status lines, the first-person gun's settings (the body off), the tests and the older aiming settings.
void CreationEngineEntry::on_draw_debug_ui()
{
    auto log_change = [](const char* what, bool value) { spdlog::info("[QuickTest] {} -> {}", what, value ? "on" : "off"); };
    auto& d = ModSettings::g_debug;

    if (menu_ui::group("Status")) {
        ImGui::TextWrapped("%s", VrSettingsTab::Status().c_str()); // (fix186)
        ImGui::TextWrapped("%s", GameCode::Status().c_str()); // (fix188)
        ImGui::TextWrapped("Body: %s", !m_show_body->value() ? "off" : body::IsActive() ? "shown" : "waiting (first person, headset on, controllers in use)");
        ImGui::TextWrapped("%s", ImpactMelee::Status().c_str());
        ImGui::TextWrapped("%s", MeleeThrow::Status().c_str());
        ImGui::TextWrapped("%s", HandGrab::Status().c_str());
        ImGui::TextWrapped("%s", Physical::Status().c_str());
        ImGui::TextWrapped("%s", Climbing::Status().c_str());
        ImGui::TextWrapped("%s", BuildConsole::Status().c_str());
        ImGui::TextWrapped("%s", MessagesBlock::Status().c_str());
        ImGui::TextWrapped("%s", LeftWeapon::Status().c_str());
        ImGui::TextWrapped("%s", FavoritesWheel::Status().c_str());
        ImGui::TextWrapped("%s", HandActions::Status().c_str());
        if (m_shoulder_grenade->value() && m_grenade_in_hand->value()) {
            ImGui::TextWrapped("%s", GrenadeHand::Status().c_str());
        }
    }

    if (menu_ui::group("Logs for testing")) {
        if (m_physical_probe->draw("Log the bodies and people near you and the physics time every 10 s (for the next builds)")) {
            apply_comfort_settings();
            log_comfort_settings();
        }
        if (ImGui::Button("Log gun parts (also done after each stereo screenshot)")) {
            MotionWeapon::request_piece_report(2);
        }
        if (ImGui::Button("Log First-Person Nodes (gun out, first person)")) {
            NodeTreeDump::request();
        }
        // (fix189) The physics probe's scan of the game's code (developers only: 3-4 s of reading the game's code, ~270 log lines and
        // sfvr/physics_dump.bin rewritten). Turned on, it runs once now and at each start; nothing in the mod needs it.
        if (m_physics_code_scan->draw("Scan the game's physics code at start (developers only: writes sfvr/physics_dump.bin; leave off)")) {
            log_change("Scan the game's physics code at start", m_physics_code_scan->value());
            if (m_physics_code_scan->value()) {
                StartPhysicsCodeScan("turned on in the menu");
            }
        }
    }

    if (menu_ui::group("First-person gun (used when the body is off)")) {
        if (m_gun_follows_controller->draw("Gun follows right controller (motion controls, experimental)")) {
            ModConstants::gunFollowsController = m_gun_follows_controller->value();
            log_change("Gun follows right controller", m_gun_follows_controller->value());
        }
        if (m_first_person_eye_lens->draw("Draw the gun and arms with the headset's lens (fixes the short, too-close gun)")) {
            ModConstants::firstPersonEyeLens = m_first_person_eye_lens->value();
            log_change("Draw the gun and arms with the headset's lens", m_first_person_eye_lens->value());
        }
        if (m_gun_follows_controller->value()) {
            if (m_gun_pitch_offset->draw("Gun tilt in hand (deg)")) ModConstants::gunPitchOffset = m_gun_pitch_offset->value();
            if (m_gun_yaw_offset->draw("Gun turn in hand (deg)")) ModConstants::gunYawOffset = m_gun_yaw_offset->value();
            if (m_gun_forward_offset->draw("Grip forward/back (m)")) ModConstants::gunForwardOffset = m_gun_forward_offset->value();
            if (m_gun_up_offset->draw("Grip up/down (m)")) ModConstants::gunUpOffset = m_gun_up_offset->value();
            if (ImGui::Button("Reset gun tilt/turn/grip")) {
                m_gun_pitch_offset->value() = 0.0f;
                m_gun_yaw_offset->value() = 0.0f;
                m_gun_forward_offset->value() = 0.0f;
                m_gun_up_offset->value() = 0.0f;
                ModConstants::gunPitchOffset = ModConstants::gunYawOffset = ModConstants::gunForwardOffset = ModConstants::gunUpOffset = 0.0f;
            }
            if (m_keep_gun_transform->draw("Keep the whole gun in your hand (fixes missing gun parts)")) {
                ModConstants::keepGunTransform = m_keep_gun_transform->value();
                ModConstants::forceFullDetail = m_force_full_detail->value();
                log_change("Keep the whole gun in your hand", m_keep_gun_transform->value());
            }
            if (m_gun_reapply_before_animation->draw("Put the gun back before the game's animation step (method from the mod that shows the whole gun)")) {
                ModConstants::gunReapplyBeforeAnimation = m_gun_reapply_before_animation->value();
                log_change("Put the gun back before the animation step", m_gun_reapply_before_animation->value());
            }
            if (m_gun_head_reference->draw("Gun turns with")) {
                ModConstants::gunHeadReference = m_gun_head_reference->value();
                spdlog::info("[QuickTest] Gun turns with -> {}", s_gun_head_reference[m_gun_head_reference->value()]);
            }
            if (ImGui::Button("Log gun placement (gun out, first person)")) {
                MotionWeapon::request_report();
            }
        }
    }

    if (menu_ui::group("Aiming and the camera (older settings)")) {
        if (m_head_tracking_type->draw("Head Tracking Type")) {
            ModConstants::headTrackingType = m_head_tracking_type->value();
        }
        if (m_pawn_control_rotation->draw("Pawn Control Rotation (body turns with your head)")) {
            GameFlow::gStore.internalSettings.pawnControl = m_pawn_control_rotation->value();
            log_change("Pawn Control Rotation", m_pawn_control_rotation->value());
        }
        if (m_decoupled_pitch->draw("Decoupled Pitch (keep off: shots follow your view)")) {
            GameFlow::gStore.internalSettings.decoupledPitch = m_decoupled_pitch->value();
            log_change("Decoupled Pitch", m_decoupled_pitch->value());
        }
        if (m_head_tracking_multiplier->draw("Head Tracking Sensitivity")) {
            ModConstants::headTrackingMultiplier = m_head_tracking_multiplier->value();
        }
        if (m_taa_anf_nvidia_fix->draw("Nvidia DLSS and TAA Fix")) {
            GameFlow::gStore.internalSettings.nvidiaAndTAAfix = m_taa_anf_nvidia_fix->value();
            UpscalerAfrNvidiaModule::set_history_fix(m_taa_anf_nvidia_fix->value()); // per-eye DLSS needs each eye's own motion
        }
        bool turn_mesh = !d.disableFirstPersonMeshRotation;
        if (ImGui::Checkbox("Turn first-person arms/weapon with the headset (gun follows your head)", &turn_mesh)) {
            d.disableFirstPersonMeshRotation = !turn_mesh;
            log_change("Turn first-person arms/weapon with the headset", turn_mesh);
        }
    }

    if (menu_ui::group("Gun detail and rendering tests (leave as they are)")) {
        // The game's weapon FOV made the gun detailed but drew it closer. With the headset's lens
        // on the gun (above) its size should no longer change, only the detail.
        if (ImGui::Checkbox("Use the game's own weapon FOV (detail test)", &d.gameWeaponFov)) {
            if (!d.gameWeaponFov) d.gameWorldFov = false;
            log_change("Use the game's own weapon FOV", d.gameWeaponFov);
        }
        if (ImGui::Checkbox("Game's meshlet culling (keep on: off hides objects)", &d.meshletCulling)) {
            log_change("Game's meshlet culling", d.meshletCulling);
        }
        if (ImGui::Checkbox("Game's GPU culling (keep on)", &d.gpuCulling)) {
            log_change("Game's GPU culling", d.gpuCulling);
        }
        if (ImGui::Checkbox("Game moves first person to origin (keep on)", &d.firstPersonAtOrigin)) {
            log_change("Game moves first person to origin", d.firstPersonAtOrigin);
        }
        if (m_force_full_detail->draw("Force full-detail models (had no effect on the gun)")) {
            ModConstants::forceFullDetail = m_force_full_detail->value();
            log_change("Force full-detail models", m_force_full_detail->value());
        }
        if (m_detail_boost->draw("Detail boost (had no effect; leave at 1)")) {
            ModConstants::detailBoost = m_detail_boost->value();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            spdlog::info("[QuickTest] Detail boost -> {:.2f}", m_detail_boost->value());
        }
        ImGui::TextWrapped("Older tests (leave off):");
        if (d.gameWeaponFov) {
            if (ImGui::Checkbox("...and the game's own world FOV with the weapon FOV test", &d.gameWorldFov)) {
                log_change("Use the game's own world FOV too", d.gameWorldFov);
            }
        }
        if (ImGui::Checkbox("Game's GPU detail selection (keep on: off hides buildings)", &d.gpuLodEvaluation)) {
            log_change("Game's GPU detail selection", d.gpuLodEvaluation);
        }
        if (ImGui::Checkbox("Eye offset for the first-person weapon camera (normally off)", &d.eyeOffsetFirstPersonCamera)) {
            log_change("Eye offset for first-person camera", d.eyeOffsetFirstPersonCamera);
        }
    }

    if (menu_ui::group("The game's interface parts drawn")) {
        draw_ui_parts_list();
    }
}

// (fix196) The interface parts the game drew lately (the Debug tab's last group; and the original mod's old test code, kept switched off).
void CreationEngineEntry::draw_ui_parts_list()
{
    for(const auto& ui_part : GameFlow::uiPartsDrawn()) // (fix189: a copy, taken under the list's lock)
    {
        ImGui::Text("UI: %s", ui_part.c_str());
    }
#if 0
    if(0){
        auto player = CreationEngineSingletonManager::GetPlayerRef();
        GameFlow::State::DebugWeaponData& debugWeaponData = GameFlow::gState.debugWeaponData;

        if(player){
            ImGui::Checkbox("Debug Weapon [X] - LaserNode [ ] - Projectile", &debugWeaponData.type);
            ImGui::InputInt("Deep Level", &debugWeaponData.deepLevel, 0, 10);

            RE::NiNode* obj_int{};
            if(debugWeaponData.type) {
                obj_int = player->getLaserNode(debugWeaponData.deepLevel);
            } else {
                obj_int = player->getProjectileNode(debugWeaponData.deepLevel);
            }

            ImGui::InputFloat3("yaw,roll,pitch", &debugWeaponData.yaw);

            if(obj_int)
            {
                ImGui::Text("Current Node [%s] %s", debugWeaponData.type? "Laser": "Projectile" ,obj_int->name.data());
                auto local = glm::mat4(*(glm::mat3x4*) &obj_int->local.rotate);
                local = glm::rowMajor4(local);
                auto euler = glm::eulerAngles(glm::quat{local});
                ImGui::Text("Current Euler [%.2f,%.2f,%.2f]", euler.x, euler.y, euler.z);
                auto qua = glm::quat{local};
                ImGui::Text("Current quaternion [x=%.2f,y=%.2f,z=%.2f,w=%.2f]", qua.x, qua.y, qua.z, qua.w);
            }

            {
                RE::NiNode* obj = player->getLaserNode(0);
                std::string laser_hierarchy = "$";
                while(obj)
                {
                    laser_hierarchy = obj->name.data() + std::string(":") + laser_hierarchy;
                    obj = obj->parent;
                }
                obj = player->getProjectileNode(0);
                std::string projectile_hierarchy = "$";
                while(obj)
                {
                    projectile_hierarchy = obj->name.data() + std::string(":") + projectile_hierarchy;
                    obj = obj->parent;
                }
                ImGui::Text("[Laser] %s", laser_hierarchy.data());
                ImGui::Text("[Projectile] %s", projectile_hierarchy.data());
            }
        }
    }


    {
        static auto camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
        auto rt_w    = (float)g_framework->get_rendertarget_width_d3d12();
        auto rt_h    = (float)g_framework->get_rendertarget_height_d3d12();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(rt_w, rt_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("Circle", nullptr,
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);

        auto* draw_list = ImGui::GetWindowDrawList();
        int index = 0;
        std::vector<ImRect> drawnRects;

        {

            for (auto& point : GameFlow::gStore.debugData.points) {

                ImVec2 center{};
                auto wts = camera->WorldToScreenNormalized({ point.x, point.y, point.z });

                // Compute center position in render target space
                center.x = wts.x * rt_w;
                center.y = wts.y * rt_h;

                // Draw the point
                draw_list->AddCircleFilled(center, 5.0f, IM_COL32(255, 0, 0, 255));

                // Create the label string for this point
                char label[64];
                sprintf_s(label, "%d:[%.1f,%.1f,%.1f]", (int)(index / 2), point.x, point.y, point.z);

                // Calculate text size
                ImVec2 labelSize = ImGui::CalcTextSize(label);
                // Start with a zero offset in Y
                float offsetY = 0.0f;
                ImRect currRect(center.x, center.y + 5 + offsetY, center.x + labelSize.x, center.y + 5 + offsetY + labelSize.y);

                // Set a maximum offset value so that it eventually exits if offsets go too high
                const float maxOffset = rt_h;

                while (true) {
                    bool hasOverlap = false;
                    for (const auto& r : drawnRects) {
                        if (currRect.Overlaps(r)) {
                            hasOverlap = true;
                            break;
                        }
                    }
                    if (!hasOverlap || offsetY >= maxOffset) {
                        break;
                    }
                    offsetY += labelSize.y;
                    currRect = ImRect(center.x, center.y + 5 + offsetY, center.x + labelSize.x, center.y + 5 + offsetY + labelSize.y);
                }
                
                // Draw the label and save its rectangle for future overlap checks
                draw_list->AddText(ImVec2(center.x, center.y + 5 + offsetY), IM_COL32(255, 255, 255, 255), label);
                drawnRects.push_back(currRect);

                index++;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        GameFlow::gStore.debugData.points.clear();
    }
#endif
}

void CreationEngineEntry::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
    // (fix154) The left hand's weapon as last chosen.
    LeftWeapon::SetFormId(static_cast<uint32_t>(m_left_weapon_form->value()), "kept from the last session");
    LeftWeapon::SetMods(m_left_weapon_mods->value()); // (fix166: its mods as chosen)
    GrenadeHand::LoadStrays(m_stray_copies->value()); // (fix154: copies that may be in a save)
    {
        uint32_t    ignored = 0;
        std::string ignored_mods;
        LeftWeapon::TakeChanged(ignored, ignored_mods); // (as loaded: nothing to save)
    }
    ModConstants::dominantEye = m_dominant_eye->value();
    ModConstants::sightsOnDominantEye = m_sights_on_dominant_eye->value();
    ModConstants::headTrackingMultiplier = m_head_tracking_multiplier->value();
    ModConstants::headTrackingType = m_head_tracking_type->value();
    GameFlow::gStore.internalSettings.nvidiaAndTAAfix = m_taa_anf_nvidia_fix->value();
    UpscalerAfrNvidiaModule::set_history_fix(m_taa_anf_nvidia_fix->value());
    GameFlow::gStore.internalSettings.pawnControl = m_pawn_control_rotation->value();
    GameFlow::gStore.internalSettings.preventZoom = m_disable_zoom->value();
    GameFlow::gStore.hudSettings.hudScale = m_hud_scale->value();
    GameFlow::gStore.hudSettings.perspective = (int) m_hud_perspective->value();
    GameFlow::gStore.internalSettings.alternativeJoyLayout = m_alternative_joy_layout->value();
    GameFlow::gStore.internalSettings.faceButtonsAsPrinted = m_face_buttons_as_printed->value(); // (fix194)
    GameFlow::gStore.internalSettings.decoupledPitch = m_decoupled_pitch->value();
    // These were only applied when a control was changed in the menu, so after a restart the
    // checkboxes showed the saved value while the mod used the default.
    ModConstants::hudOnOpticalCenter = m_hud_on_optical_center->value();
    ModConstants::weaponStereoDepth = m_weapon_stereo_depth->value();
    ModConstants::gunFollowsController = m_gun_follows_controller->value();
    ModConstants::gunPitchOffset = m_gun_pitch_offset->value();
    ModConstants::gunYawOffset = m_gun_yaw_offset->value();
    ModConstants::gunForwardOffset = m_gun_forward_offset->value();
    ModConstants::gunUpOffset = m_gun_up_offset->value();
    ModConstants::gunHeadReference = m_gun_head_reference->value();
    ModConstants::recenterAfterLoading = m_recenter_after_loading->value();
    ModConstants::detailBoost = m_detail_boost->value();
    ModConstants::keepGunTransform = m_keep_gun_transform->value();
    ModConstants::gunReapplyBeforeAnimation = m_gun_reapply_before_animation->value();
    ModConstants::forceFullDetail = m_force_full_detail->value();
    ModConstants::firstPersonEyeLens = m_first_person_eye_lens->value();
    ModConstants::desktopWindowOnMainMenu = m_desktop_window_on_main_menu->value();
    apply_body_settings();
    apply_comfort_settings();
    spdlog::info("[Settings] turning {} ({:.0f} deg, smooth speed {:.2f}), level view {}, vibration {} (strength {:.2f}, game rumble {}), hand interaction {} (reach {:.2f} m)",
        m_turn_mode->value() == 0 ? "snap" : "smooth", m_snap_turn_degrees->value(), m_smooth_turn_speed->value(), m_level_view->value(),
        m_haptics->value(), m_haptic_strength->value(), m_haptic_game_rumble->value(), m_hand_interact->value(), m_hand_reach->value());
    spdlog::info("[Settings] body: shown {}, aim with gun {}, shots from muzzle {}, two-handed {}, fingers {}, lean {}, facing {}, legs {}, height {}, body tracking {}, hand tracking {}, crouch lowers view {}, real crouch {}",
        m_show_body->value(), m_aim_with_gun_hand->value(), m_shoot_from_muzzle->value(), m_support_hand->value(), m_finger_poses->value(),
        m_body_lean->value(), m_body_facing->value(), m_walking_legs->value(), m_match_body_height->value(), m_body_tracking->value(),
        m_hand_tracking->value(), m_crouch_view->value(), m_real_crouch->value());
    spdlog::info("[Settings] detail boost {:.2f}, keep whole gun in hand {}, gun put back before the animation step {}, force full detail {}, gun and arms drawn with the headset's lens {}",
        m_detail_boost->value(), m_keep_gun_transform->value(), m_gun_reapply_before_animation->value(), m_force_full_detail->value(),
        m_first_person_eye_lens->value());
    spdlog::info("[Settings] mod: dominant eye {}, aiming mode {}, decoupled pitch {}, pawn control {}, prevent zoom {}, weapon stereo depth {:.2f}, "
                 "HUD on optical centre {}, gun follows controller {} (tilt {:.1f}, turn {:.1f}, forward {:.3f}, up {:.3f}, reference {}), "
                 "face buttons {} (fix194)",
        m_dominant_eye->value() == 1 ? "Left" : "Right", m_head_tracking_type->value(), m_decoupled_pitch->value(),
        m_pawn_control_rotation->value(), m_disable_zoom->value(), m_weapon_stereo_depth->value(), m_hud_on_optical_center->value(),
        m_gun_follows_controller->value(), m_gun_pitch_offset->value(), m_gun_yaw_offset->value(), m_gun_forward_offset->value(),
        m_gun_up_offset->value(), m_gun_head_reference->value(),
        m_face_buttons_as_printed->value() ? "as printed (A B X Y)" : "the old layout (left X = B, left Y = X, right B = Y)");
    spdlog::info("[Settings] (fix172) people: grab bodies {} (strength {:.2f}, kept down while held {}, a grip holds standing people {} (fix173), else knocks them down {}), shoves {}, punch knockdowns {} "
                 "({:.1f} m/s), blade blocks {}, anger {}, followers spared {}, probe {}",
        m_grab_bodies->value(), m_body_grab_strength->value(), m_keep_held_down->value(), m_hold_people->value(), m_grab_knocks_down->value(), m_shove_people->value(),
        m_punch_knockdown->value(), m_knockdown_speed->value(), m_saber_block->value(), m_shove_anger->value(), m_spare_followers->value(),
        m_physical_probe->value());
    // (fix189) The physics probe's scan of the game's code, a developer's test: only when its setting is on.
    spdlog::info("[Settings] (fix189) the physics probe's scan of the game's code at start {}", m_physics_code_scan->value() ? "on" : "off (PhysicsCodeScan)");
    if (m_physics_code_scan->value()) {
        StartPhysicsCodeScan("the setting PhysicsCodeScan is on");
    }
}

void CreationEngineEntry::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
