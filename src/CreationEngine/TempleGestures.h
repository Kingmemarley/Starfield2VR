#pragma once

#include <string>

namespace RE
{
    class NiAVObject;
}

// Gestures at your temples (fix132): the left trigger with the left hand beside the head turns the flashlight on or off;
// the right trigger with the right hand beside the head turns the hand scanner on or off. Starfield's own controls: the gamepad's LB
// held turns the flashlight on or off (with a helmet on, the guides say), LB tapped opens or closes the scanner; so LB is held (1 s)
// or tapped (0.1 s) for the game, as the mod's other gestures press its buttons (fix143: back to one each; fix142's "either temple,
// held as long as the trigger" made short pulls taps, the scanner from both). The trigger that did it is held back from the game until
// it is let go (no shot, no aim).
namespace TempleGestures
{
    // (The game's thread, each frame: tracking::Snapshot.) The hands at the temples and their triggers.
    void Update();
    // (The game's input.) LB pressed now for a gesture; a trigger held back (left: the left controller's).
    bool PressLeftShoulder();
    bool HoldsTrigger(bool left);
    std::string Status();
    // (fix174) The game's scene update of a node, before its own (CreationEngineCameraManager::onNiAVObjectUpdateWorld, the game's update
    // threads): the flashlight's light, while the body is shown, un-hidden and put at your head shining where you look.
    void BeforeNodeUpdate(RE::NiAVObject* obj);
}
