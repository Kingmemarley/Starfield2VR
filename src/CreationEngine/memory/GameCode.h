#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

// (fix188) The game's code and data by their place in Starfield 1.16.236, for this build's game version.
//
// A separate version of the mod (this version kept) for the latest version of Starfield, as "Two DLLs, one code": the same
// code builds the mod for Starfield 1.16.236 and, with the build setting
// SFVR_GAME_1_16_244 (preset release-no-debug-msvc-1.16.244), for 1.16.244 (Steam's latest, June 2026). Every address in the mod's code
// stays the one it has in 1.16.236; the code asks here for it:
// - At(rva): the address of that place in this build's game (in 1.16.236 the game's base plus rva, as before);
// - Code(rva, bytes): the same for a run of the game's code the mod checks before it uses it (hooks, calls, reads): in 1.16.236 the bytes
//   must be the ones given (as before); in 1.16.244 the run's own bytes there (its calls and data references are other numbers) are in
//   the port table.
// For 1.16.244 the places come from a table (GameCode_1_16_244.inc) made by tools/port/bake.py from the two versions' Starfield.exe
// (docs/PORTING.md): a place not in it gives 0, and the feature that needs it stays off and says so in the log. Either build also gives
// 0 for everything when the game it runs in is another version than the one it was built for (read from the game's own version
// resource): nothing is hooked or called at another version's places.
namespace GameCode
{
#if defined(SFVR_GAME_1_16_244)
    inline constexpr const char* kTargetVersion = "1.16.244";
    inline constexpr bool        kPorted        = true; // (places from the port table)
#else
    inline constexpr const char* kTargetVersion = "1.16.236";
    inline constexpr bool        kPorted        = false; // (places as they are in the mod's code)
#endif

    // The game's image base (Starfield.exe as loaded).
    uintptr_t Base();
    // The running game's version, read from its version resource ("1.16.236.0"; "unknown" when it cannot be read).
    const std::string& GameVersion();
    // Whether the running game is the version this build is for (its version's first three numbers).
    bool VersionMatches();

    // The address of a place given by its 1.16.236 rva, in this build's game; 0 when not known (not ported, or another game version).
    uintptr_t At(uintptr_t rva236);
    // The same as an rva in this build's game (0 when not known).
    uintptr_t Rva(uintptr_t rva236);
    // Whether every one of these 1.16.236 places is known in this build's game.
    bool Known(std::initializer_list<uintptr_t> rvas236);

    // A run of the game's code checked: given its 1.16.236 rva and bytes, its address in this build's game when the game's bytes there
    // are the expected ones (1.16.236: these; a ported build: the port table's for it); else 0.
    uintptr_t Code(uintptr_t rva236, const uint8_t* bytes236, size_t n);
    template <size_t N>
    uintptr_t Code(uintptr_t rva236, const uint8_t (&bytes236)[N])
    {
        return Code(rva236, bytes236, N);
    }
    // Code(...) != 0.
    inline bool Same(uintptr_t rva236, const uint8_t* bytes236, size_t n) { return Code(rva236, bytes236, n) != 0; }
    template <size_t N>
    bool Same(uintptr_t rva236, const uint8_t (&bytes236)[N])
    {
        return Code(rva236, bytes236, N) != 0;
    }

    // One line for the log and the F11 window: the build's game version, the running game's, the port table's size and what was asked
    // for and not known.
    std::string Status();
    // Logs Status() once (at the mod's start).
    void LogOnce();
}
