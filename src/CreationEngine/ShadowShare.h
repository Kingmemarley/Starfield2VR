#pragma once

#include <string>

// Full stereo's right eye lit by the left eye's sun shadows (fix140), instead of drawing its own.
//
// Each view sets up the sun's shadow cascades for itself (SceneSetup's function 6, 0x1429A92B0: the cascades fitted around its camera,
// then a loop that asks the shadow-map pool for a 2048x2048 map per cascade due this frame, writes each cascade's record (its matrix,
// its map's bindless index, its split) into the view's row of CascadeShadowMapCache and pushes the job that draws the map; the records
// are then copied into the view's constants, which its lighting reads). The two eyes are 6.4 cm apart, at the same moment, under the
// same sun: the left's maps serve the right as well. So, in a full stereo frame, when the world's view (the left eye, listed first and
// set up first as a rule) has finished its loop, its records are kept (with how many, and its local-light shadow budget); when the right
// eye's view comes to its loop, they are put in its row and the loop skipped (its cascade count made 0 for the loop: 0x1429AA65A), so
// it draws no maps and its constants get the left's records. Only when kept this frame for the same light; else the right eye draws
// its own, as before (a frame it falls back on may show a shadow glitch for that frame: its own maps given back to the pool by then).
// The left's fit is widened by the eyes' distance (LensFit) so it covers the right eye's view too. The left's maps are drawn and made
// readable before either eye's lighting (the GPU runs the views in order), and nothing between them gives them back to the pool.
namespace ShadowShare
{
    // Checks the game's code (Starfield 1.16.236) and hooks the two places. Once, at start.
    void Install();
    void SetOn(bool on);
    bool On();
    std::string Status();
}
