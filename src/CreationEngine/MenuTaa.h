#pragma once

#include <string>

// Every menu with a 3D view of its own drawn as the Data menu is: the world at full size with the game's anti-aliasing (TAA).
//
// Each frame the game picks how it scales its picture up (0x140840A10). With DLSS chosen it switches to TAA at full size when a
// shown menu with 3D views asks for it: 0x142541B40 (its only caller) lists the frame's views (the world's first, then each shown
// menu's active 3D views) and answers yes only if one of those menus has flag bit 29 (0x20000000, at IMenu+0xC0), and the setting
// bTempMenusCanDisableUpscalers:Display is on. The Data and Inventory menus have it; the Missions menu (0x08000791), Powers,
// Skills and Ship Crew do not. Without it DLSS stays chosen, the frame is drawn at DLSS's size, and the upscale step, which cannot
// run DLSS for more than one view (0x14298CC08: two views and DLSS), copies the picture up instead ("Upscale CopyWithShader"): the
// world behind the Missions menu small and without any anti-aliasing (the shimmer seen; fix127's log).
//
// This hooks the answer (where it is stored, 0x140840B6C): yes whenever a game menu is shown and the frame's list has more than one
// view (the world's and at least one menu view). That is
// exactly the case the copy would be made in; the frame then goes as in the Data menu.
namespace MenuTaa
{
    // Checks the game's code there (Starfield 1.16.236) and hooks it. Once, at start.
    void Install();
    std::string Status();
}
