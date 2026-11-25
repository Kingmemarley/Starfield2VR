#pragma once
#include <Mod.hpp>

class CreationEngineEntry : public Mod
{
public:
    inline static std::shared_ptr<CreationEngineEntry>& Get()
    {
        static std::shared_ptr<CreationEngineEntry> instance{ std::make_shared<CreationEngineEntry>() };
        return instance;
    }

    [[nodiscard]] inline std::string_view get_name() const override { return "CreationEngine"; }

    std::optional<std::string> on_initialize() override;
    void                       on_frame() override;
    void                       on_present() override;
    void                       on_draw_ui() override;
    void                       on_draw_quick_ui() override;
    void                       on_config_load(const utility::Config& cfg, bool ) override;
    void                       on_config_save(utility::Config& cfg) override;

private:
    inline static const std::vector<std::string> s_dominant_eye{
        "Right",
        "Left",
    };

    inline static const std::vector<std::string> s_aiming_mode{
      "HeadGun",
      "Free",
      "HeadGunAlwaysOn",
      "RightHand",
  };
    inline static const std::vector<std::string> s_gun_head_reference{
        "Rendered camera (default)",
        "Skeleton camera",
    };
    const ModCombo::Ptr m_dominant_eye{ ModCombo::create(generate_name("DominantEye"), s_dominant_eye) };
    const ModSlider::Ptr m_head_tracking_multiplier{ ModSlider::create(generate_name("HeadTrackingSensitivity"), 0.5, 2.0, 1.0) };
    const ModCombo::Ptr m_head_tracking_type{ ModCombo::create(generate_name("AimingMode"), s_aiming_mode) };
    const ModToggle::Ptr m_disable_zoom{ ModToggle::create(generate_name("DisableZoom"), false) };
    // New config name so everyone starts with it off once: with it on the gun didn't follow the view
    // while shots did.
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitchV2"), false) };
    const ModToggle::Ptr m_pawn_control_rotation{ ModToggle::create(generate_name("PawnControlRotation"), true) };
    const ModToggle::Ptr m_taa_anf_nvidia_fix{ ModToggle::create(generate_name("NvidiaDlssAndTaaFix"), true) };
    const ModSlider::Ptr m_hud_scale{ModSlider::create(generate_name("HUDScale"), 0.1, 1.0, 0.4) };
    const ModSlider::Ptr m_hud_perspective{ ModSlider::create(generate_name("HUDPerspective"), 0, 600, 150) };
    const ModToggle::Ptr m_sights_on_dominant_eye{ ModToggle::create(generate_name("SightsOnDominantEye"), true) };
    const ModToggle::Ptr m_hud_on_optical_center{ ModToggle::create(generate_name("HudOnOpticalCenter"), true) };
    // New config name: the old value was never applied at load (0.3 was always used), and some
    // configs hold 100 there, which would have switched the reduction off.
    const ModSlider::Ptr m_weapon_stereo_depth{ ModSlider::create(generate_name("WeaponStereoDepthV2"), 0.0, 1.0, 0.3) };
    const ModToggle::Ptr m_gun_follows_controller{ ModToggle::create(generate_name("GunFollowsController"), false) };
    const ModSlider::Ptr m_gun_pitch_offset{ ModSlider::create(generate_name("GunPitchOffset"), -90.0, 90.0, 0.0) };
    const ModSlider::Ptr m_gun_yaw_offset{ ModSlider::create(generate_name("GunYawOffset"), -45.0, 45.0, 0.0) };
    const ModSlider::Ptr m_gun_forward_offset{ ModSlider::create(generate_name("GunForwardOffset"), -0.2, 0.2, 0.0) };
    const ModSlider::Ptr m_gun_up_offset{ ModSlider::create(generate_name("GunUpOffset"), -0.2, 0.2, 0.0) };
    const ModCombo::Ptr m_gun_head_reference{ ModCombo::create(generate_name("GunHeadReference"), s_gun_head_reference) };
    const ModToggle::Ptr m_recenter_after_loading{ ModToggle::create(generate_name("RecenterAfterLoading"), true) };
    // V2: the first version (2.5) did nothing, so start again from the game's 1.
    const ModSlider::Ptr m_detail_boost{ ModSlider::create(generate_name("DetailBoostV2"), 1.0, 5.0, 1.0) };
    // V2: forcing level 0 didn't change the gun (the GPU picks it), so it starts off again.
    const ModToggle::Ptr m_force_full_detail{ ModToggle::create(generate_name("ForceFullDetailV2"), false) };
    const ModToggle::Ptr m_keep_gun_transform{ ModToggle::create(generate_name("KeepGunTransform"), true) };
    const ModToggle::Ptr m_gun_reapply_before_animation{ ModToggle::create(generate_name("GunReapplyBeforeAnimation"), true) };
    const ModToggle::Ptr m_first_person_eye_lens{ ModToggle::create(generate_name("FirstPersonEyeLens"), true) };
    const ModToggle::Ptr m_alternative_joy_layout{ ModToggle::create(generate_name("JoyAlternativeLayout"), false) };
    // Shown body (ported from SFVR): third-person body in first person, arms on the controllers, gun in the hand.
    const ModToggle::Ptr m_show_body{ ModToggle::create(generate_name("ShowBody"), true) };
    const ModToggle::Ptr m_aim_with_gun_hand{ ModToggle::create(generate_name("AimWithGunHand"), true) };
    const ModToggle::Ptr m_shoot_from_muzzle{ ModToggle::create(generate_name("ShootFromMuzzle"), true) };
    const ModToggle::Ptr m_match_body_height{ ModToggle::create(generate_name("MatchBodyHeight"), true) };
    const ModToggle::Ptr m_body_lean{ ModToggle::create(generate_name("BodyLean"), true) };
    const ModToggle::Ptr m_body_facing{ ModToggle::create(generate_name("BodyFacing"), true) };
    const ModToggle::Ptr m_support_hand{ ModToggle::create(generate_name("SupportHand"), true) };
    const ModToggle::Ptr m_finger_poses{ ModToggle::create(generate_name("FingerPoses"), true) };
    const ModToggle::Ptr m_walking_legs{ ModToggle::create(generate_name("WalkingLegs"), true) };
    const ModToggle::Ptr m_body_tracking{ ModToggle::create(generate_name("BodyTracking"), true) };
    const ModToggle::Ptr m_hand_tracking{ ModToggle::create(generate_name("HandTracking"), true) };
    const ModToggle::Ptr m_desktop_window_on_main_menu{ ModToggle::create(generate_name("DesktopWindowOnMainMenu"), true) };
    const ModToggle::Ptr m_real_crouch{ ModToggle::create(generate_name("RealCrouch"), false) };
    const ModToggle::Ptr m_crouch_view{ ModToggle::create(generate_name("CrouchLowersView"), true) };

    // Turning, vibration, pointing and melee.
    inline static const std::vector<std::string> s_turn_mode{
        "Snap (fixed steps)",
        "Smooth",
    };
    const ModCombo::Ptr m_turn_mode{ ModCombo::create(generate_name("TurnMode"), s_turn_mode) };
    const ModSlider::Ptr m_snap_turn_degrees{ ModSlider::create(generate_name("SnapTurnDegrees"), 15.0, 90.0, 45.0) };
    const ModSlider::Ptr m_smooth_turn_speed{ ModSlider::create(generate_name("SmoothTurnSpeed"), 0.1, 1.0, 1.0) };
    const ModToggle::Ptr m_level_view{ ModToggle::create(generate_name("LevelView"), true) };
    const ModToggle::Ptr m_haptics{ ModToggle::create(generate_name("Haptics"), true) };
    const ModSlider::Ptr m_haptic_strength{ ModSlider::create(generate_name("HapticStrength"), 0.1, 1.0, 0.8) };
    const ModToggle::Ptr m_haptic_game_rumble{ ModToggle::create(generate_name("HapticGameRumble"), true) };
    const ModToggle::Ptr m_hand_interact{ ModToggle::create(generate_name("HandInteract"), true) };
    const ModSlider::Ptr m_hand_reach{ ModSlider::create(generate_name("HandReach"), 0.1, 1.5, 0.35) };
    const ModToggle::Ptr m_impact_melee{ ModToggle::create(generate_name("ImpactMelee"), true) };
    const ModSlider::Ptr m_melee_swing_speed{ ModSlider::create(generate_name("MeleeSwingSpeed"), 1.0, 5.0, 2.2) };
    const ModToggle::Ptr m_melee_instant_hits{ ModToggle::create(generate_name("MeleeInstantHits"), true) };
    const ModToggle::Ptr m_melee_throw{ ModToggle::create(generate_name("MeleeThrow"), true) };
    const ModToggle::Ptr m_hand_grab{ ModToggle::create(generate_name("HandGrab"), true) };
    const ModToggle::Ptr m_grip_grab{ ModToggle::create(generate_name("GripGrab"), true) };
    const ModSlider::Ptr m_grab_stiffness{ ModSlider::create(generate_name("GrabStiffness"), 0.0, 1.0, 1.0) };
    const ModToggle::Ptr m_grab_align{ ModToggle::create(generate_name("GrabAlign"), true) };
    const ModToggle::Ptr m_two_hand_hold{ ModToggle::create(generate_name("TwoHandHold"), true) };    // (fix170)
    const ModToggle::Ptr m_one_in_each_hand{ ModToggle::create(generate_name("OneInEachHand"), true) }; // (fix170)
    const ModToggle::Ptr m_grab_in_hand{ ModToggle::create(generate_name("GrabInHand"), true) };
    const ModToggle::Ptr m_grab_ignores_you{ ModToggle::create(generate_name("GrabIgnoresYou"), true) };
    const ModToggle::Ptr m_gravity_gloves{ ModToggle::create(generate_name("GravityGloves"), true) };
    const ModSlider::Ptr m_pull_range{ ModSlider::create(generate_name("PullRange"), 1.0, 10.0, 5.0) };
    const ModToggle::Ptr m_glove_marker{ ModToggle::create(generate_name("GloveMarker"), true) };
    const ModToggle::Ptr m_glove_highlight{ ModToggle::create(generate_name("GloveHighlight"), true) };
    const ModToggle::Ptr m_glove_frozen{ ModToggle::create(generate_name("GloveFrozen"), true) };
    const ModSlider::Ptr m_glove_highlight_fill{ ModSlider::create(generate_name("GloveHighlightGlow"), 0.0, 1.0, 0.4) };
    inline static const std::vector<std::string> s_glove_aim_line{
        "Your hand (fingers' direction, as HIGGS)",
        "Your palm (out of the palm, as ROCK and Half-Life: Alyx)",
        "Sighting along your hand (from your eyes through the controller)",
    };
    const ModCombo::Ptr m_glove_aim_line{ ModCombo::create(generate_name("GloveAimLine"), s_glove_aim_line) };
    const ModToggle::Ptr m_mouth_eat{ ModToggle::create(generate_name("MouthEat"), true) };
    const ModToggle::Ptr m_bag_stash{ ModToggle::create(generate_name("BagStash"), true) };
    const ModToggle::Ptr m_auto_equip_weapons{ ModToggle::create(generate_name("AutoEquipWeapons"), true) };
    // Aiming, powers and grenades with the hands.
    const ModToggle::Ptr m_gun_sight{ ModToggle::create(generate_name("GunSight"), true) };
    const ModSlider::Ptr m_gun_sight_distance{ ModSlider::create(generate_name("GunSightDistance"), 3.0, 60.0, 15.0) };
    const ModToggle::Ptr m_gun_laser{ ModToggle::create(generate_name("GunLaser"), true) };
    const ModSlider::Ptr m_gun_laser_beam{ ModSlider::create(generate_name("GunLaserBeamLength"), 0.05, 2.0, 0.30) };
    const ModToggle::Ptr m_temple_gestures{ ModToggle::create(generate_name("TempleGestures"), true) };
    const ModToggle::Ptr m_scanner_plain{ ModToggle::create(generate_name("ScannerPlain"), true) };
    const ModToggle::Ptr m_level_view_menus{ ModToggle::create(generate_name("LevelViewInWorldMenus"), true) };
    const ModToggle::Ptr m_workbench_first_person{ ModToggle::create(generate_name("WorkbenchFirstPerson"), true) };
    const ModToggle::Ptr m_ladder_first_person{ ModToggle::create(generate_name("LadderFirstPerson"), true) };
    // (fix179) Climbing with the hands (Climbing.h): a grip squeezed against a wall, rock or ledge holds you on; the pull's multiplier.
    const ModToggle::Ptr m_climbing{ ModToggle::create(generate_name("Climbing"), true) };
    const ModSlider::Ptr m_climb_pull{ ModSlider::create(generate_name("ClimbPull"), 1.0, 3.0, 1.0) };
    const ModToggle::Ptr m_dialogue_pointer{ ModToggle::create(generate_name("DialoguePointer"), true) };
    const ModToggle::Ptr m_dialogue_touch{ ModToggle::create(generate_name("DialogueTouch"), true) };
    const ModToggle::Ptr m_dialogue_panel{ ModToggle::create(generate_name("DialoguePanel"), true) };
    // (fix173) The answers pressed with a finger (off by default: pointed at and the trigger pulled); the right stick's smooth turn in a
    // conversation; the grips as LB and RB in the hand scanner and the star map.
    const ModToggle::Ptr m_dialogue_touch_press{ ModToggle::create(generate_name("DialogueTouchPress"), false) };
    const ModToggle::Ptr m_dialogue_turn{ ModToggle::create(generate_name("DialogueTurn"), true) };
    const ModToggle::Ptr m_scanner_grips{ ModToggle::create(generate_name("ScannerGrips"), true) };
    // (fix177) Build mode as a console of panels low in front of your seat (off: in your view as the HUD, fix175's way).
    const ModToggle::Ptr m_build_console{ ModToggle::create(generate_name("BuildConsole"), true) };
    // (fix178) The game's messages in one place below your view's middle, bigger (off: where the HUD has them).
    const ModToggle::Ptr m_messages_block{ ModToggle::create(generate_name("MessagesBlock"), true) };
    const ModToggle::Ptr m_menu_stick_navigate{ ModToggle::create(generate_name("MenuStickNavigate"), true) };
    const ModSlider::Ptr m_conversation_distance{ ModSlider::create(generate_name("ConversationDistance"), 0.35, 1.0, 0.60) };
    const ModToggle::Ptr m_trade_stick_tabs{ ModToggle::create(generate_name("TradeStickTabs"), true) };
    const ModToggle::Ptr m_dual_wield{ ModToggle::create(generate_name("DualWield"), true) };
    // (fix171) The favorites as a Spell Wheel VR-style wheel at the left hand (off: the game's own favorites menu).
    const ModToggle::Ptr m_favorites_wheel{ ModToggle::create(generate_name("FavoritesWheel"), true) };
    // (fix154) The left hand's weapon's form id, kept across sessions (not drawn: chosen in the inventory).
    const ModInt32::Ptr  m_left_weapon_form{ ModInt32::create(generate_name("LeftWeaponForm"), 0) };
    // (fix166) Its mods as chosen (its inventory item's own: "mod id,mod id,..."), so its copy is made with them.
    const ModValue<std::string>::Ptr m_left_weapon_mods{ ModValue<std::string>::create(generate_name("LeftWeaponMods"), std::string{}) };
    // (fix154) The copies in your hands that may be in a save (noted as the game saved; deleted if a load brings one back): "ref:base,...".
    const ModValue<std::string>::Ptr m_stray_copies{ ModValue<std::string>::create(generate_name("StrayCopies"), std::string{}) };
    const ModToggle::Ptr m_push_powers{ ModToggle::create(generate_name("PushPowers"), true) };
    const ModSlider::Ptr m_push_power_speed{ ModSlider::create(generate_name("PushPowerSpeed"), 0.8, 4.0, 1.5) };
    const ModToggle::Ptr m_shoulder_grenade{ ModToggle::create(generate_name("ShoulderGrenade"), true) };
    const ModToggle::Ptr m_grenade_in_hand{ ModToggle::create(generate_name("GrenadeInHand"), true) };
    // (fix172) Physical interaction with people, as PLANCK (Physical.h).
    // (fix189) V2: the census of the bodies and people near you (only for the log; 1-3 ms spikes every 10 s for its first 10 minutes)
    // starts off again; the old name kept everyone's saved "on".
    const ModToggle::Ptr m_physical_probe{ ModToggle::create(generate_name("PhysicalProbeV2"), false) };
    // (fix189) The physics probe's scan of the game's code at start (PhysicsProbe.h: 3-4 s of reading the exe 5 s after start, ~270 log
    // lines, sfvr/physics_dump.bin rewritten): only for developers, nothing uses it any more, so off unless turned on.
    const ModToggle::Ptr m_physics_code_scan{ ModToggle::create(generate_name("PhysicsCodeScan"), false) };
    const ModToggle::Ptr m_grab_bodies{ ModToggle::create(generate_name("GrabBodies"), true) };
    const ModSlider::Ptr m_body_grab_strength{ ModSlider::create(generate_name("BodyGrabStrength"), 0.0, 1.0, 0.5) };
    const ModToggle::Ptr m_keep_held_down{ ModToggle::create(generate_name("KeepHeldDown"), true) };
    const ModToggle::Ptr m_grab_knocks_down{ ModToggle::create(generate_name("GrabKnocksDown"), true) };
    const ModToggle::Ptr m_hold_people{ ModToggle::create(generate_name("HoldPeople"), true) }; // (fix173)
    const ModToggle::Ptr m_shove_people{ ModToggle::create(generate_name("ShovePeople"), true) };
    const ModToggle::Ptr m_punch_knockdown{ ModToggle::create(generate_name("PunchKnockdown"), true) };
    const ModSlider::Ptr m_knockdown_speed{ ModSlider::create(generate_name("KnockdownSpeed"), 3.0, 10.0, 6.0) };
    const ModToggle::Ptr m_saber_block{ ModToggle::create(generate_name("SaberBlock"), true) };
    const ModToggle::Ptr m_shove_anger{ ModToggle::create(generate_name("ShoveAnger"), true) };
    const ModToggle::Ptr m_spare_followers{ ModToggle::create(generate_name("SpareFollowers"), true) };

    // Copies the body toggles above into the settings the body module reads.
    void apply_body_settings();
    // Copies the turning, vibration, pointing and melee settings above into the game settings.
    void apply_comfort_settings();
    // (fix186) The rows of the VR tab in the game's Settings (VrSettingsTab.h), bound to the settings above.
    void set_vr_tab_rows();


    ValueList m_options{*m_dominant_eye, *m_head_tracking_multiplier, *m_head_tracking_type, *m_taa_anf_nvidia_fix, *m_disable_zoom, *m_hud_scale, *m_hud_perspective, *m_alternative_joy_layout, *m_decoupled_pitch, *m_pawn_control_rotation, *m_sights_on_dominant_eye, *m_hud_on_optical_center, *m_weapon_stereo_depth, *m_gun_follows_controller, *m_gun_pitch_offset, *m_gun_yaw_offset, *m_gun_forward_offset, *m_gun_up_offset, *m_gun_head_reference, *m_recenter_after_loading, *m_detail_boost, *m_keep_gun_transform, *m_gun_reapply_before_animation, *m_force_full_detail, *m_first_person_eye_lens,
        *m_show_body, *m_aim_with_gun_hand, *m_shoot_from_muzzle, *m_match_body_height, *m_body_lean, *m_body_facing, *m_support_hand, *m_finger_poses,
        *m_walking_legs, *m_body_tracking, *m_hand_tracking, *m_desktop_window_on_main_menu,
        *m_real_crouch, *m_crouch_view,
        *m_turn_mode, *m_snap_turn_degrees, *m_smooth_turn_speed, *m_level_view, *m_haptics, *m_haptic_strength,
        *m_haptic_game_rumble, *m_hand_interact, *m_hand_reach, *m_impact_melee, *m_melee_swing_speed, *m_melee_instant_hits, *m_melee_throw, *m_hand_grab,
        *m_grip_grab, *m_grab_stiffness, *m_grab_align, *m_two_hand_hold, *m_one_in_each_hand, *m_grab_in_hand, *m_grab_ignores_you, *m_gravity_gloves, *m_pull_range,
        *m_glove_marker, *m_glove_highlight, *m_glove_frozen, *m_glove_highlight_fill, *m_glove_aim_line, *m_mouth_eat, *m_bag_stash, *m_auto_equip_weapons,
        *m_gun_sight, *m_gun_sight_distance, *m_gun_laser, *m_gun_laser_beam, *m_temple_gestures, *m_scanner_plain, *m_level_view_menus, *m_workbench_first_person, *m_ladder_first_person, *m_climbing, *m_climb_pull, *m_dialogue_pointer, *m_dialogue_touch, *m_dialogue_panel, *m_dialogue_touch_press, *m_dialogue_turn, *m_scanner_grips, *m_build_console, *m_messages_block, *m_menu_stick_navigate, *m_conversation_distance, *m_trade_stick_tabs, *m_dual_wield, *m_favorites_wheel, *m_left_weapon_form, *m_left_weapon_mods, *m_stray_copies, *m_push_powers, *m_push_power_speed,
        *m_shoulder_grenade,
        *m_physical_probe, *m_grab_bodies, *m_body_grab_strength, *m_keep_held_down, *m_grab_knocks_down, *m_hold_people, *m_shove_people, *m_punch_knockdown,
        *m_knockdown_speed, *m_saber_block, *m_shove_anger, *m_spare_followers, *m_physics_code_scan };
};
