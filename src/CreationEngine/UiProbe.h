#pragma once

#include <cstdint>
#include <string>

// Test build: finds where Starfield (1.16.236) draws its interface, for menus you can see the world through and HUD pieces placed
// in the world (the next step after the menu panel). Nothing is changed; it only logs (prefix [UI-PROBE]):
// - every render graph the game runs (by name, the first time each is seen), and the whole order of one frame's steps during
//   play and in each full-screen menu (its fifth frame open): the render graphs with the interface movies' viewport settings in
//   between (where in the frame each movie is set up, and on which thread), and whether the game still draws its world behind
//   the menu (its "Frame" render graph);
// - each interface movie's viewport as the game sets it (the size of what it draws into, its place, and whether it draws into a
//   texture of its own: Scaleform's "render texture" flag), its stage size, scale mode and the part of the stage shown;
// - while the menu pointer is in use: the game's own mouse place in its menus (each movie's mouse state, read with the movie's
//   own function after checking its code), so the log shows whether the menu cursor follows the pointer.
namespace UiProbe
{
    // Render thread, each render graph, before and after it runs (CreationEngineRendererModule::RenderGraphStart).
    void OnRenderGraph(const char* name, bool before);
    // Each interface movie's viewport as the game sets it, before the mod changes it (the Scaleform SetViewport hook).
    void OnMovieViewport(void* movie, const char* url, const void* viewport);

    // The game's mouse place in a menu movie (Scaleform's own coordinates: pixels of the area the movie draws in, buffer_w x
    // buffer_h, the menu's box), read each frame while wanted.
    struct GameMouse
    {
        bool        read{ false }; // (false: not read yet, or the movie's function is not the one expected)
        float       x{ 0.0f };
        float       y{ 0.0f };
        uint32_t    buttons{ 0 };
        std::string movie{};
        int64_t     at_ms{ 0 };      // when it was read (steady clock, ms)
        int64_t     changed_ms{ 0 }; // when it last moved (0: not since it was first read)
        uint64_t    seq{ 0 };        // counts the readings of this movie
        int         buffer_w{ 0 };
        int         buffer_h{ 0 };
    };
    // The menu showing now (read in the last moments) whose mouse moved last, or `prefer` while it shows unless another's moved
    // clearly later; read == false when there is none.
    GameMouse LastGameMouse(const std::string& prefer = {});
    // Read the game's mouse place from now on (while the menu pointer is in use), or stop.
    void WantGameMouse(bool on);
}
