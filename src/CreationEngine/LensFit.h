#pragma once

#include <string>

// The lens trimmed to what the eyes see (fix138): the headset's lenses look outward and down, so the frustum is off-centre: both eyes'
// lenses together (the shared lens) reach tan 0.966 up but 1.428 down (each eye's own, Quest 3's left: 1.376 out, 0.839 in, 0.966 up,
// 1.428 down). The game draws an off-centre frustum right (its projection,
// 0x1429FFD20, has the off-centre terms; its culling and level of detail use all four edges), but a few of its own calculations take
// the frustum's right and top edges as half its width and height, as for a centred one:
// - the sun's shadow cascades are fitted around +-right, +-top (0x1429A5B90), and a second shadow box likewise (0x1429A6CC0): the
//   lower part of the view (and with each eye's own lens, its outer side) would be outside them, with no sun shadows;
// - ambient occlusion rebuilds where each pixel is from the depth with the inverse projection's scale only, not its off-centre offset
//   (0x1429BADF0): its shading leaning up or down;
// - the sun's and planets' disks are sized by a field of view of 2 atan(right) (0x142B0BC60): with each eye's own lens a different
//   size in each eye (with the shared lens, centred sideways, the game's own already).
// These hooks give each the frustum's half-extents (for the shadows, ones that cover the whole off-centre frustum). With a centred
// frustum (the symmetric projection, or mono's sideways) each gives the game's own numbers: they change nothing then.
namespace LensFit
{
    // Checks the game's code (Starfield 1.16.236) and hooks the four places. Once, at start.
    void Install();
    // For the menu: what was hooked, and how often each was used in the last seconds.
    std::string Status();
}
