#include "SceneGraph.h"

#include <algorithm>
#include <cctype>

#include <RE/N/NiAVObject.h>

// Ported from SFVR (AetherianArtificer's fork of starfield2vr).
namespace body
{
    // Leaf objects have no child array; read it under SEH so a non-node never crashes the walk.
    // Slots after a gap (a removed child) are still visited; empty slots are skipped.
    std::uint16_t ReadChildren(const RE::NiAVObject* node, RE::NiAVObject** out, std::uint16_t capacity)
    {
        std::uint16_t count = 0;
        __try {
            auto       as_node = reinterpret_cast<const RE::NiNode*>(node);
            const auto slots   = as_node->children.slots_to_scan();
            if (as_node->children.entries) {
                for (std::uint16_t i = 0; i < slots && count < capacity; ++i) {
                    if (auto child = as_node->children.entries[i]) {
                        out[count++] = child;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count = 0;
        }
        return count;
    }

    RE::NiAVObject* FindDescendant(RE::NiAVObject* node, std::string_view name, int depth)
    {
        if (!node || depth > 40) {
            return nullptr;
        }
        RE::NiAVObject* children[128]{};
        const auto      count = ReadChildren(node, children, 128);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (children[i] && name == children[i]->name.c_str()) {
                return children[i];
            }
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            if (auto found = FindDescendant(children[i], name, depth + 1)) {
                return found;
            }
        }
        return nullptr;
    }

    RE::NiAVObject* FindChild(RE::NiAVObject* node, std::string_view name)
    {
        if (!node) {
            return nullptr;
        }
        RE::NiAVObject* children[128]{};
        const auto      count = ReadChildren(node, children, 128);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (children[i] && name == children[i]->name.c_str()) {
                return children[i];
            }
        }
        return nullptr;
    }

    RE::NiAVObject* FindDescendantOther(RE::NiAVObject* node, std::string_view name, const RE::NiAVObject* except, int depth)
    {
        if (!node || depth > 40) {
            return nullptr;
        }
        RE::NiAVObject* children[128]{};
        const auto      count = ReadChildren(node, children, 128);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (children[i] && children[i] != except && name == children[i]->name.c_str()) {
                return children[i];
            }
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            if (auto found = FindDescendantOther(children[i], name, except, depth + 1)) {
                return found;
            }
        }
        return nullptr;
    }

    RE::NiAVObject* ParentOf(const RE::NiAVObject* node)
    {
        if (!node) {
            return nullptr;
        }
        __try {
            return reinterpret_cast<RE::NiAVObject*>(node->parent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool IsHidden(const RE::NiAVObject* node)
    {
        if (!node) {
            return false;
        }
        __try {
            return (node->flags & 1u) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    RE::NiAVObject* FindWeaponBone(RE::NiAVObject* root)
    {
        auto chest = FindDescendant(root, "C_Chest");
        return chest ? FindChild(chest, "Weapon") : nullptr;
    }

    bool Contains(std::string_view name, std::string_view part)
    {
        const auto it = std::search(name.begin(), name.end(), part.begin(), part.end(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        return it != name.end();
    }
}
