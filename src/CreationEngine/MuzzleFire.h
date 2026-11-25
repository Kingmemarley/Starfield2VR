#pragma once

// Makes the player's shots leave from the shown body's gun (its projectile node, along the barrel)
// while aiming with the hand. Ported from SFVR (AetherianArtificer's fork of starfield2vr).
namespace MuzzleFire
{
    void Install();
    // The player's last shot (while aiming with the hand) went along the barrel as the shown body holds the gun; false: it
    // went the game's own way (along the aim). True until a shot says otherwise.
    bool ShotsFollowBarrel();
    // Another weapon: until it fires, assume it follows the barrel.
    void ResetShotsFollowBarrel();
}
