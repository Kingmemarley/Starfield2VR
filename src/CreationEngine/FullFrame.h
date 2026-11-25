#pragma once

#include <string>

// The world drawn at its full size behind a see-through menu.
//
// While a menu with the 16:9 interface box is open (most full-screen menus), the game makes its frame picture the size of that box
// (1996x1123 against the 1996x2150 eye picture: it sends the box as the render window's rectangle, 0x142499D80 -> 0x142990090, and
// the window's frame texture is made again at that size): every view of the frame, the world's too, is drawn at about half the
// height, and the eye images stretch it (the world behind the menu blocky). With the world kept drawn behind a see-through menu
// (LiveWorld) this sends the whole window's rectangle instead, as the game itself does when no box is wanted: the frame stays full
// size. The menus' interface keeps its box (its movies' viewports, the cursor's area: from the game's settings, not changed); the
// menu's 3D views keep their 16:9 lens and are drawn over the whole picture (stretched in height), and the menu panel lays each one
// in the box (D3D12Component::compose_menu_layer: the whole picture into the box). The interface layer then has the menu in its top
// rows (its movies draw at their viewport, from the top left): the panel takes the box from there.
namespace FullFrame
{
    // Finds the game's render-window rectangle call and the box's (by their bytes): hooked. Once, at start.
    void Install();
    // The game's main thread, each frame a full-screen menu is shown (LiveWorld's check): the rectangle sent again when what is
    // wanted changed (the world kept drawn behind the menu, or no longer).
    void OnMenuFrame();
    // The frame is kept full size behind the open menu now (the last rectangle sent was the whole window's, with the box on).
    bool Active();
    // (fix173) The game's 16:9 interface box is on (a menu with the box is shown: S+0x41): true, with the size its function last worked out
    // (S+0x30 wide, S+0x2C high; 0 when they could not be read). False: no box (the game draws its frame over the whole picture) or not
    // known (not installed). Any thread (read as the game keeps them).
    bool GameBox(int& width, int& height);
    // Nothing about to change the frame's size: what is wanted is what the window has, and steady (or the frame left to the game).
    bool Settled();
    std::string Status();
}
