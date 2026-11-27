#pragma once

#include <cstdint>

// Controller vibration: a kick in the gun hand when you fire, a pulse when you reload, melee swings and
// strikes in the swinging hand, and the game's own rumble (getting hit, explosions, hard landings) passed
// on to both controllers.
namespace haptics
{
    // The game set the virtual Xbox controller's motors (0-255). Called on the ViGEm thread, or (fix193) from the game's own rumble
    // call on the virtual pad in its gamepad list (VirtualGamepad).
    void SetGameRumble(std::uint8_t large_motor, std::uint8_t small_motor);
    // The player launched a projectile (from the launch hook; several per shot for shotguns).
    void PlayerFired();
    // Once per game controller poll, with the Xbox buttons sent to the game this poll.
    void Update(std::uint16_t xbox_buttons);
    // Melee: a pulse in one controller (left or right as the game sees them), e.g. a light tick when a
    // swing starts an attack and a hard kick when it strikes something. Any thread.
    void MeleePulse(bool left_controller, float amplitude, float seconds);
    // (fix195) When reload (the game's X) was last tapped with the weapon out, and when you last fired: steady-clock microseconds
    // (far in the past before the first). Any thread. The body keeps the gun's grip live meanwhile (BodyIK's grip lock).
    std::int64_t LastReloadTapUs();
    std::int64_t LastFiredUs();
}
