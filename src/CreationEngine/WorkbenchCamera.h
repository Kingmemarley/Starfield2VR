#pragma once

#include <string>

// (fix143) Workbenches in first person. Starfield (1.16.236) turns its camera to the third-person one at every workbench (the weapon,
// armour, chemistry, industrial and research benches, the cooking station): the camera's "enter furniture" (0x141E91CE0) checks
// the furniture is a workbench and switches to the third-person state with no other condition (0x141E91D83: SetState(20)); the
// crafting menus then open over a view of your body from behind (the player's body in third person with the
// menu over that background; a workbench should just open like other menus). In the headset, with the setting on and the camera
// in first person, that switch (and the bench's blur, which only its third-person exit takes back) is skipped: leaving it, the camera
// is already in first person. Chairs and beds are left alone.
namespace WorkbenchCamera
{
    // Checks the game's code and hooks the switch (the mod's start).
    void Install();
    // The player camera's state now: its index in the camera's states (0 first person, 20 third person, 21 furniture), -1 not known.
    int CameraStateIndex();
    std::string Status();
}
