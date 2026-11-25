#pragma once

#include <cstdint>
#include <string>

namespace Scaleform::Gfx
{
    struct Viewport;
}

// (fix178) The game's messages in one place a little below the middle of your view, bigger (after fix177 the notifications were still
// too small and too far left to see; chosen: "Below centre, bigger"). Setting MessagesBlock.
//
// The HUD's messages (HUDMessagesMenu: the stacked notifications, Messages_mc, pinned to its stage's right; the mission updates, made by its
// script one under another and pinned to its left; the skill challenge flyout at the left; the level-up at the top; the XP and the location's
// name in the middle) are drawn in the HUD's box, the HUDScale setting's share of the eye picture (0.4): their text about 0.5 to 0.6 degrees, the
// notifications 18-29 degrees right, the mission updates 22-31 degrees left. They stay in the HUD layer (no panels of their own, fps the same);
// MenuClean, right after the game advances that movie, arranges them inside it (its ActionScript, as the movie lays itself out):
// - The movie's menu clip (Menu_mc, which holds them all) made larger (about 1.4 times: their 18-unit text about 0.85 degrees) and moved so
//   the point the mission updates start from (its own 0,0) lands at the block's top left.
// - Its margins from the edges (SafeX, the safe zone the movie pins its parts with: IMenu.SetSafeRect) widened, so the mission updates
//   (pinned left) and the notifications (pinned right) come in to two columns side by side below the middle: the movie lays them out
//   again with the new margin (its SetSafeRect, called through its Flash player), and keeps it for the mission updates that come later.
// - The notifications' column's top put at the block's top; the skill challenge flyout and the level-up placed under the notifications as
//   they show (their heights read each look); the subtitles kept at their size and place (made smaller by as much: they are pinned to the
//   box's bottom middle); the XP and the location's name stay in the middle (bigger).
// Not in a menu's box (16:9: menus on the panel) nor on the flat screen: the movie put back as it was (its own margins again).
namespace MessagesBlock
{
    // The arrangement, in the movie's stage units (its stage 1920 x 1080; y down) unless said.
    struct Plan
    {
        bool  valid{ false };
        float deg_per_unit{ 0.0f };  // the HUD's own size in your view (degrees per stage unit, near the middle)
        float scale{ 1.0f };         // the menu clip's scale
        float origin[2]{ 0.0f, 0.0f }; // the menu clip's place (stage)
        float safe_x{ 0.0f };        // the margin the mission updates (left) and the notifications (right) are pinned with
        float top{ 0.0f };           // the block's top (stage y)
        float left_col[2]{};         // the mission updates' column (stage x)
        float right_col[2]{};        // the notifications' column (stage x)
        float shown_bottom{ 1080.0f }; // the bottom of the part of the stage the box shows (the subtitles pinned there)
        float region[4]{};           // the whole block (stage units: x0, y0, x1, y1), for the build console's crop
    };
    void Install();
    // (The viewport hook, every movie, its viewport as given to the game.) HUDMessagesMenu's box noted (tall: the HUD's or the build
    // console's; 16:9: a menu's).
    void NoteViewport(void* movie, const Scaleform::Gfx::Viewport* viewport);
    // (MenuClean) Arranged now: the setting on, in the headset, the movie drawn in a tall box lately; and the plan for it.
    bool Wanted();
    Plan Current();
    // (MenuClean) Arranged (or put back): the plan in use, for the build console's crops.
    void NoteApplied(bool applied, const Plan& plan);
    // (BuildConsole) Arranged lately: true and its plan.
    bool Applied(Plan& plan);
    std::string Status();
}
