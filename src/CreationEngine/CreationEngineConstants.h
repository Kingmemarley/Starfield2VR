#pragma once

namespace ModConstants
{
    inline float headTrackingMultiplier = 1.0f;

    inline constexpr float DEG_TO_RAD = std::numbers::pi / 180;
    inline float lodAdjustFov = 0.0f;
    inline float fixedCameraRoll = 0.0f;
    inline bool preventCameraRoll{false};
    inline bool cameraShake{false};

    static constexpr int NUM_STAGES = 12;
    inline std::array<bool, NUM_STAGES> enabledResourcesToCopy{false, true, true, true, false, false, false, false, false, false, false, false};
    inline std::array<int, NUM_STAGES> rgResourceCount{2,2,2,2,2,2,2,1,2,2,2,2};
    inline int dominantEye{0};
    inline bool sightsOnDominantEye{true};
    inline bool hudOnOpticalCenter{true};
    inline float weaponStereoDepth{0.3f};
    // Motion controls: gun on the right controller (MotionWeapon).
    inline bool  gunFollowsController{false};
    inline float gunPitchOffset{0.0f};   // degrees, tilts the gun in the hand
    inline float gunYawOffset{0.0f};     // degrees
    inline float gunForwardOffset{0.0f}; // metres, along the gun
    inline float gunUpOffset{0.0f};      // metres
    inline int   gunHeadReference{0};
    inline bool  gunReapplyBeforeAnimation{true}; // put the gun transform back before the game's animation step (reference mod method)
    // Draw the gun and arms with the headset eye's lens. The game draws them with its own
    // first-person camera, whose lens it sets itself (about 73x76 deg against the eye's 108x110),
    // so the gun was drawn ~1.9x too big in each eye and looked squashed to half its length.
    inline bool  firstPersonEyeLens{true};
    inline bool  keepGunTransform{true};   // leave the controller-driven gun transform in place after the update
    inline bool  recenterAfterLoading{true}; // put the view at your head after every loading screen
    inline float detailBoost{1.0f};
    inline bool  forceFullDetail{false};      // iForcedMeshLODIndex 0: every mesh uses its detailed model          // game's level-of-detail screen-size multiplier (1 = game default)    // 0 = turn with the rendered (game) camera, 1 = with the skeleton's camera
    inline int headTrackingType{0};
    // Aiming mode 3: the game aims along the right controller (SFVR's "aim with right hand").
    inline constexpr int kAimWithRightHand = 3;
    // While the body holds a drawn weapon, aim along the right controller (the gun is in your hand).
    inline bool aimWithGunHand{true};
    // Make shots leave the shown body's gun muzzle (along the barrel) while aiming with the hand.
    inline bool shootFromMuzzle{true};
    // Keep the game window on the desktop screen while the main menu is shown (so the mouse can reach
    // the whole menu); it goes to the headset's size again once the game is loaded.
    inline bool desktopWindowOnMainMenu{true};
}
