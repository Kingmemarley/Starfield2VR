#pragma once

#include <string_view>

namespace RE { class NiAVObject; }

// Logs the first-person node hierarchy once, on the game thread, when requested from the menu.
namespace NodeTreeDump {
    void request();
    // The hands' anchor points, once: in the first-person rig and in the body (the third-person model), every node named
    // *AnimObject*, Weapon or WeaponLeft and everything under it (what the game hangs on the hands, such as a grenade in its throw).
    // `why` must be a string literal (it is read at the next frame's start).
    void request_hands(const char* why);
    void run_if_requested();
    // Depth-first search for a node by name (reads are fault-guarded). Game thread only.
    RE::NiAVObject* find(RE::NiAVObject* root, std::string_view name);
    // The skeleton's weapon bone below `root`: the "Weapon" under "C_Chest" (a modded weapon's own model can be named "Weapon"
    // too, and is never taken for it); without C_Chest, the first node named Weapon. Game thread only.
    RE::NiAVObject* find_weapon_bone(RE::NiAVObject* root);
}
