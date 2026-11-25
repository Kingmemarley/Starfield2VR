#pragma once

// Gun sight: a small red dot where your gun's shots go, drawn by the headset over the game image (an OpenXR quad layer),
// for aiming with the hand. The game's own crosshair stays in the middle of the view (where your head looks), so turn it off
// in the game (Settings > Interface > Crosshair) to aim by the dot alone.
// The dot sits a set distance along the ray the shots take: from the shown body's muzzle along the gun's barrel as it holds
// it (two-handed too), or, when the shots are the game's own, from your head along the controller's aim. From the muzzle it
// is exact at that distance and a little off nearer than that (more so with the gun held low, away from your eyes).
namespace GunSight
{
    // Once per engine frame (main thread, tracking::Snapshot): shows, moves or hides the dot. `aiming_with_hand`: this
    // frame's aim follows the right controller.
    void Update(bool aiming_with_hand);
}
