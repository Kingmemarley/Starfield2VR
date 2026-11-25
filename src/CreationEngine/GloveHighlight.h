#pragma once

#include <cstdint>
#include <string>

// The gravity gloves' target highlighted on the object itself (fix130: exactly over the target instead of a circle):
// a light blue glow and outline over what they aim at, white once locked on, drawn by the game's own highlight (the one the hand
// scanner outlines items with, and the ship and outpost builders use), which follows the object's shape.
//
// Starfield 1.16.236 (a helper's research, checked against the scanner's own code, 0x1417D7630): a HighlightManager (0x48 bytes,
// vtable 0x144B339D8, made by 0x140653FC0(memory, style)) holds a key of the renderer's highlights (+0x40) and the geometry it has
// marked. An object is marked by walking its 3D (0x14241A660(&visitor, root)) with the visitor whose type table is 0x144B33A48 and
// whose captures are the manager and its key: each geometry gets the key. 0x140654160(manager) unmarks all it marked (safe after
// the object's 3D is gone). The style (0x20 bytes: fill colour, outline colour (0xAARRGGBB), outline width in pixels, hardness)
// can be set again at any time with 0x140654520(&key, style) (the scanner pulses its own so). Two managers are made once and kept:
// one blue (aimed at), one white (locked on). The game's main thread only (the scanner's; the gloves' aim runs there too).
namespace GloveHighlight
{
    // Checks the game's code (Starfield 1.16.236). Once, at start.
    void Install();
    // (The game's main thread, each frame the gloves aim.) What they aim at: 0 nothing, 1 aimed at, 2 locked on; its reference's
    // handle. Marked again only when it changes.
    void Show(int kind, uint32_t handle);
    // The highlight is drawn on the target now (the gloves' ring and glow pictures are then left out).
    bool Showing();
    std::string Status();
}
