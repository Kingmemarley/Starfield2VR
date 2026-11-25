#pragma once

#include <string>

// Fixed foveated shading (fix139): the parts of each eye's picture the lens never shows, or shows blurred at its edge, shaded more
// coarsely, through the game's own variable rate shading.
//
// The game shades each 16x16-pixel square of its picture at a rate it chooses from the picture's content (VRS: flat, dark or fast-moving
// squares shaded once per 2x2 or 4x4 pixels): ReprojectShadingRateImage (each view, after its depth, before its G-buffer) writes the
// rates into the view's shading-rate picture (R8_UINT, one texel a square, at the render resolution), which the G-buffer, decals,
// lighting, transparency and particles then use (the tiled lighting reads it too). Right after that step, the squares the eye's lens
// never shows (outside its field of view with a small margin, and its rounded corners) and an outer ring of what it shows (past a share of
// the way to the lens's edge, where the lens blurs) are set to 2x2, the game's own coarsest (its lighting knows no coarser); the rest
// keeps the game's own rates. Written by copying boxes of a pattern
// made once per eye and size (a small upload buffer) into the picture, between the barriers its state needs (UAV, as the step leaves it,
// to COPY_DEST and back). With the symmetric projection a third of each eye's picture is outside its lens, so that third costs little.
// Per view: the world's view is the eye whose turn it is (full stereo: the left), mono's both eyes (only what neither sees, or both
// see blurred); full stereo's right eye view the right eye's; the menus' 3D views are left alone. Only when the game's VRS is on.
namespace Foveation
{
    // Hooks the step (Starfield 1.16.236, its type table's function 7, chained). Once, at start.
    void Install();
    // The settings (GameSettingsComponent): on or off, and where the 2x2 ring starts (a share of the way from the centre to the lens's
    // edge, 0.6-1.2).
    void SetOptions(bool on, float ring);
    std::string Status();
}
