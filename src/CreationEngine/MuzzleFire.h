#pragma once

// Makes the player's shots leave from the shown body's gun (its projectile node, along the barrel)
// while aiming with the hand. Ported from SFVR (AetherianArtificer's fork of starfield2vr).
// (fix195) A continuous beam (the cutter), which the game aims again every frame, is kept along the barrel in every frame, and what it
// hits is tested along the beam as drawn.
// (fix204) The beam's own node, where the game draws it and starts the test of what it hits, is checked to be the gun in your hand's
// muzzle (the game's launch copies it from the launch data) and set to it if a beam has none; logged with the game's camera FOVs.
namespace MuzzleFire
{
    void Install();
    // The player's last shot (while aiming with the hand) went along the barrel as the shown body holds the gun; false: it
    // went the game's own way (along the aim). True until a shot says otherwise.
    bool ShotsFollowBarrel();
    // Another weapon: until it fires, assume it follows the barrel.
    void ResetShotsFollowBarrel();
    // (fix195) Once a frame (GunSight::Update): writes the log's summary of a continuous beam's trigger pull (the cutter) once it has
    // ended (0.3 s without its frames).
    void Update();
}
