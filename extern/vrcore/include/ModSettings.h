// vrcore - display mode switches shared between the framework and the game integration.
#pragma once

namespace ModSettings {
struct InternalSettings {
    // Toggled by the player (left grip + left stick click, or F12): show the game as a flat screen.
    bool forceFlatScreen{false};
    // Set by the game integration while a fullscreen menu is open.
    bool showQuadDisplay{false};
    // Set by the game integration with it: the world behind that menu is the game's world (you are in the game: not the main
    // menu or a loading screen), so the world as it last was may be shown around the menu's panel.
    bool menuWorldBehind{false};
    // Set by the game integration with it: the height of the box the open menus draw in (the game draws menus in a 16:9 box,
    // centred, nothing around it) when it is less than the picture's; 0 otherwise.
    int menuBoxRows{0};
    // Set by the game integration with it: the open menus are drawn by the interface alone (none of them mostly 3D, as the star
    // map), so they may be shown see-through (the game's interface layer over the world kept behind the panel).
    bool menuSeeThrough{false};
    // Set by the game integration with it: the game keeps drawing the world behind the open see-through menu (LiveWorld), so
    // the camera stays the headset's (head turned, each eye's projection) as in play; what is used in play (hands on objects,
    // the gun sight...) stays off as in any menu.
    bool menuLiveWorld{false};
    // Set by the game integration with it: the open menu's buttons are drawn faint, made for a dark backdrop (the Inventory's
    // item rows): a dark backing is laid under the see-through parts of its interface.
    bool menuButtonBacking{false};
    // Set by the game integration: counts the full-screen menus shown anew (a menu opened, or another of its tabs), for the log's
    // pictures of each (fix128: the Ships tab had none).
    unsigned menuShownSerial{0};
    // (fix164) Set by the game integration with showQuadDisplay: a loading screen (LoadingMenu) is drawn now (its picture saved once for the
    // log, with the soft light round the panel).
    bool menuLoadingScreen{false};
    // (fix183) Set by the game integration with showQuadDisplay: the star map (its surface map too) is open: instead of the world as it was
    // behind its panel (opened in space: mostly black), the soft light round the panel as on loading screens.
    bool menuGlowWhenEmpty{false};
};

extern InternalSettings g_internalSettings;

// Troubleshooting switches, toggled live from the F11 menu (VRConfig > Troubleshooting).
// Each one turns off one of the game-side camera/render changes.
struct DebugSwitches {
    bool disableHeadTracking{false};   // world camera position/rotation from the headset
    bool disableFrustumOverride{false}; // per-eye asymmetric projection
    bool disableHudViewport{false};    // HUD/menu viewport scaling
    bool disableWindowResize{false};   // resizing the game window to the headset resolution
    bool swapHudEyes{false};           // apply the HUD depth offset to the other eye
    bool forceFrustumEveryFrame{true}; // re-apply the eye's projection every frame, not only when the game sets it
    bool originalMenuDetection{true};  // treat "no HUD seen this frame" as a menu (the original rule; safe while loading)
    bool neverFlatScreenMenus{false};  // never switch to the flat virtual screen for menus
    bool taaHistoryInvert{false};      // test: use the other history slot in the per-eye TAA fix
    bool taaHistoryReset{false};       // test: tell the game to reset TAA history every frame (no ghosting, more shimmer)
    // Move the game's first-person reference camera with each eye. Off: neither Mutar's mod nor the
    // reference motion-control mod (which shows the whole gun) touches this camera, and the old
    // code here placed it about 20 cm and 60 degrees off (wrong matrix order).
    bool eyeOffsetFirstPersonCamera{false};
    // Give other screen-shaped cameras the eye's projection too (old test, keep off: it would also
    // catch menu preview cameras). The first-person gun camera has its own switch now
    // (ModConstants::firstPersonEyeLens, on by default).
    bool eyeProjectionOtherCameras{false};
    // Mutar's code turns the first-person arms/weapon with the headset so the gun follows your head.
    bool disableFirstPersonMeshRotation{false};
    // Tests for the low-detail (blocky) gun model:
    bool gameWeaponFov{false};      // leave the game's own first-person weapon FOV instead of the headset's
    bool gameWorldFov{false};       // with gameWeaponFov: the game's own world FOV too, so the gun keeps its size
    bool gpuLodEvaluation{true};    // the game's GPU mesh-detail selection (bGPULodEvaluation:Display)
    // Missing parts of the hand-held gun: the game culls pieces of meshes (meshlets) and whole
    // meshes on the GPU, using depth from the previous frame, which in VR is the other eye.
    bool meshletCulling{true};      // bUseMeshletCulling:Display (game default on; off hides most objects)
    bool gpuCulling{true};          // bUseGPUCulling:Display (game default on)
    bool firstPersonAtOrigin{true}; // bMove1stPersonToOrigin:Gameplay (game default on)
};

extern DebugSwitches g_debug;

// True when the game should be shown on a virtual screen instead of in stereo.
inline bool showFlatScreenDisplay() {
    return g_internalSettings.forceFlatScreen || g_internalSettings.showQuadDisplay;
}

// True when the camera is the game's own (not the headset's): the flat screen, or a menu, unless the world is kept drawn behind
// that menu (menuLiveWorld): then the camera, and what is drawn in the world with it (the body, the gun in the hand), stays as in
// play.
inline bool cameraFlat() {
    return g_internalSettings.forceFlatScreen || (g_internalSettings.showQuadDisplay && !g_internalSettings.menuLiveWorld);
}
} // namespace ModSettings
