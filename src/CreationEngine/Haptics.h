#pragma once

#include <cstdint>

// Controller vibration: a kick in the gun hand when you fire, a pulse when you reload, melee swings and
// strikes in the swinging hand, and the game's own rumble (getting hit, explosions, hard landings) passed
// on to both controllers.
namespace haptics
{
    // The game set the virtual Xbox controller's motors (0-255). Called on the ViGEm thread.
    void SetGameRumble(std::uint8_t large_motor, std::uint8_t small_motor);
    // The player launched a projectile (from the launch hook; several per shot for shotguns).
    void PlayerFired();
    // Once per game controller poll, with the Xbox buttons sent to the game this poll.
    void Update(std::uint16_t xbox_buttons);
    // Melee: a pulse in one controller (left or right as the game sees them), e.g. a light tick when a
    // swing starts an attack and a hard kick when it strikes something. Any thread.
    void MeleePulse(bool left_controller, float amplitude, float seconds);
}
