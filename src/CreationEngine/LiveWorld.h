#pragma once

#include <cstdint>
#include <string>

// The world kept drawn behind a see-through menu.
//
// Starfield stops drawing the world while a "full-screen" menu is shown: each frame (in its frame code, before rendering) it looks
// at the open menus, and if one shown has that flag (0x100 in the menu's flags; the Data menu, the Inventory, looting, trading,
// skills, missions, the pause menu have it; the Favorites menu does not), the world's view is not among the frame's views and only
// the menus' own 3D views are drawn (you, the planet, the item, the menu's blue room: a hidden cell). Its one exception: during a
// loading screen, once the place is ready, the world is drawn behind it (the screen fades into the game).
//
// While a see-through menu is open in VR this module watches that check (a hook at it, on the game's main thread): with such a
// menu shown and no loading screen up, it lets the world be drawn as the loading screen's exception does, so the game keeps drawing
// the world as it does behind the Favorites menu; the camera stays the headset's (ModSettings::cameraFlat), from a few frames before
// the world is drawn. Each frame the world's view's picture (the last steps of its part of the frame: the game's RenderViewSubGraph
// whose view is the world's, by its handle at +0xd28) is copied for the eye images, and the menu's own views' pictures for the panel
// (D3D12Component::record_live_picture). Left to the game as soon as no see-through menu is open, a loading screen is up or near,
// or with the setting off.
namespace LiveWorld
{
    // Finds the check (by its bytes) and the game's views (the world's view's handle). Once, at start.
    void Install();
    // Each of the frame's render graphs as it starts (the render thread): "CRBeginFrame" switches it on or off, "Frame" finds this
    // frame's views and their last steps (hooked: their pictures copied).
    void OnGraph(void* graph, const char* name, bool before);
    // The world is kept drawn behind the open see-through menu.
    bool Active();
    // (fix158) ...or wanted now (a see-through menu just opened: kept drawn from its third frame): FullFrame sends the whole window at once.
    bool Coming();
    // The world's view's handle now (false: the game's views not there yet).
    bool WorldViewHandle(uint32_t& handle);
    // (fix140) Every kind of step in the graph watched (through StepHook: GpuTimer times them).
    int HookAllSteps(const void* graph); // (how many kinds were newly hooked)
    // The function the watched kinds of step go through (RenderSteps' test may go through it too).
    uintptr_t HookAddress();
    // (fix156) A kind of step (its type table) through StepHook from now on (EyeShare's, hooked at start). True when it is.
    bool HookKind(uintptr_t table);
    std::string Status();
}
