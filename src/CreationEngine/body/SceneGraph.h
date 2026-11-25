#pragma once

#include <cstdint>
#include <string_view>

namespace RE
{
    class NiAVObject;
}

namespace body
{
    // Children of a scene graph node; zero for leaf objects.
    std::uint16_t ReadChildren(const RE::NiAVObject* node, RE::NiAVObject** out, std::uint16_t capacity);

    // Depth-first search below `node` for a node with this exact name.
    RE::NiAVObject* FindDescendant(RE::NiAVObject* node, std::string_view name, int depth = 0);

    // The direct child of `node` with this exact name.
    RE::NiAVObject* FindChild(RE::NiAVObject* node, std::string_view name);

    // Depth-first search below `node` for a node with this exact name that is not `except`.
    RE::NiAVObject* FindDescendantOther(RE::NiAVObject* node, std::string_view name, const RE::NiAVObject* except, int depth = 0);

    // A node's parent (nullptr if it has none or cannot be read).
    RE::NiAVObject* ParentOf(const RE::NiAVObject* node);

    // A node is hidden by the game (app-culled: its flags' bit 0). False if it cannot be read.
    bool IsHidden(const RE::NiAVObject* node);

    // The weapon bone of a skeleton below `root`: the node named "Weapon" that is a child of "C_Chest". A weapon whose own
    // model's top node is also named "Weapon" (some modded weapons) is never taken for it. Null if there is no such bone.
    RE::NiAVObject* FindWeaponBone(RE::NiAVObject* root);

    // Case-insensitive substring test.
    bool Contains(std::string_view name, std::string_view part);
}
