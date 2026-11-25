#pragma once

#include <string>

// See-through menus without the menus' own painted backgrounds. Some of Starfield's menus paint a background or a dark edge
// (a vignette) themselves, in their Flash file (Bethesda's published interface sources: github.com/Creation-Hub/Interface): the
// Missions menu a dark-blue picture over the whole screen, the Powers menu a dark-blue "hologram" picture and a black edge, the
// Data, Inventory and Crew menus a black edge, the Research menu a picture, the Data and Inventory menus a dark-blue block while they
// open. Those are part of the interface layer that see-through menus show (MenuLayer), so they hid the world behind the menu.
//
// While a menu is see-through, they are taken out of the menu as it runs: through the menu's Flash player (Scaleform; the calls of
// its ActionScript root: GetVariable, SetVariable, IsAvailable, InvokeArgs, functions 0x32, 0x31, 0x37 and 0x3a of its type table,
// as CommonLibSF and CommonLibF4 have them), right after the game advances that menu's movie (its Advance, the movie's type table's
// function 0x18), on that movie's own thread. A named part is hidden (its "visible" set to false); a part with no name (the
// backgrounds and edges at the bottom of the menu's clip, Menu_mc) is removed by its place in the clip (removeChildAt), only when
// the clip has exactly the parts the published files show (their number, and named parts next to them), so a menu changed by an
// update or another mod is left as it is (and the log says so). Nothing else of the menu is touched.
//
// (fix143) The hand scanner too, in the headset (setting ScannerPlain): its dark edges and the Sci Fi HUD mod's blue tint and visor
// frame taken out, its circles, marks and text left (MenuClean.cpp, kScannerFile).
namespace MenuClean
{
    void Install();
    // One line for the settings panel and the log.
    std::string Status();
}
