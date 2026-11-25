#pragma once

#include <string>

// See-through menus. Starfield draws its menus and HUD (Scaleform) on a picture of their own, see-through where nothing is drawn
// (premultiplied alpha, in gamma space), and lays it over the frame in a render pass of its own,
// CreationRendererPrivate::ScaleformCompositeRenderPass (the Luma HDR mod hooks the same pass, its function 7, and replaces its
// shader: github.com/EndlesslyFlowering/Starfield-Luma, shaders/ScaleformComposite). Its function 7 (as the TAA pass's, which this
// mod already hooks) is hooked here: after the game's own work, while a see-through menu is open (the menu panel's setting, a menu
// drawn by the interface alone with the world kept behind it: ModSettings::menuSeeThrough), the pass's textures are read (its
// inputs and outputs: the one it reads, the size of the one it draws into, is the interface layer) and the layer is given to
// vrcore, on the game's own commands (D3D12Component::record_menu_layer). The menu's blue background and your character in the
// Data menu are the game's 3D view, in the frame under that layer: see-through, they do not show.
namespace MenuLayer
{
    void Install();
    // A see-through menu is wanted now (a menu that may be see-through is open, with the world behind it, and the settings ask for
    // it). Any thread.
    bool SeeThroughWanted();
    // (fix181) See-through menus can be shown at all: the pass hooked, not given up, the headset's panel and its settings (see-through, a
    // world behind it). Not whether a menu is shown this frame, nor which: what LiveWorld holds the world through a moment's drop with.
    bool SeeThroughPossible();
    // One line for the log and settings.
    std::string Status();
}
