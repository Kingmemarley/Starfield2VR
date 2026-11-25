// Writes the first-person scene-graph hierarchy (node names, types and positions) to the log.
// Used to find the weapon and hand nodes for motion controls on the running game version.
#include "NodeTreeDump.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include "CreationEngineSingletonManager.h"
#include "RE/P/PlayerCamera.h"
#include "body/BodyIK.h"
#include <mods/VR.hpp>

namespace {
    std::atomic<bool> g_dump_requested{false};
    std::atomic<const char*> g_hands_requested{nullptr};

    struct NodeInfo {
        const char*      name{};
        const char*      type{};
        bool             is_node{};
        float            local[3]{};
        float            world[3]{};
        float            previous[3]{};
        float            bound[4]{};
        uint64_t         flags{};
        RE::NiAVObject*  parent{};
        uint16_t         child_count{};    // slots to scan (some can be empty)
        uint16_t         non_null_count{}; // what the game counts as children
        RE::NiAVObject** children{};
    };

    // SEH can't be used in functions that need C++ unwinding, so every raw read is in here.
    bool read_node(RE::NiAVObject* obj, NodeInfo& out) {
        __try {
            out.name = obj->name.c_str();
            const auto ti = utility::rtti::get_type_info(obj);
            out.type = ti != nullptr ? ti->name() : "?";
            out.is_node = utility::rtti::derives_from(obj, "class NiNode");
            out.local[0] = obj->local.translate.x;
            out.local[1] = obj->local.translate.y;
            out.local[2] = obj->local.translate.z;
            out.world[0] = obj->world.translate.x;
            out.world[1] = obj->world.translate.y;
            out.world[2] = obj->world.translate.z;
            out.previous[0] = obj->previousWorld.translate.x;
            out.previous[1] = obj->previousWorld.translate.y;
            out.previous[2] = obj->previousWorld.translate.z;
            out.bound[0] = obj->worldBound.center.x;
            out.bound[1] = obj->worldBound.center.y;
            out.bound[2] = obj->worldBound.center.z;
            out.bound[3] = obj->worldBound.radius;
            out.flags = obj->flags;
            out.parent = (RE::NiAVObject*)obj->parent;
            if (out.is_node) {
                auto node = (RE::NiNode*)obj;
                out.child_count = node->children.slots_to_scan();
                out.non_null_count = node->children.m_size;
                out.children = (RE::NiAVObject**)node->children.entries;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    RE::NiAVObject* read_child(RE::NiAVObject** children, uint16_t i) {
        __try {
            return children[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    RE::NiAVObject* read_parent(RE::NiAVObject* obj) {
        __try {
            return (RE::NiAVObject*)obj->parent;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void dump(RE::NiAVObject* obj, int depth, int& count, const char* tag = "[FP-TREE]", int max_nodes = 2500, int max_depth = 40) {
        if (obj == nullptr || count >= max_nodes || depth > max_depth) {
            return;
        }
        NodeInfo info{};
        if (!read_node(obj, info)) {
            spdlog::info("{} {}<unreadable {}>", tag, std::string(depth * 2, ' '), fmt::ptr(obj));
            return;
        }
        ++count;
        spdlog::info("{} {}{} [{}] local ({:.3f} {:.3f} {:.3f}) world ({:.3f} {:.3f} {:.3f}) previous ({:.3f} {:.3f} {:.3f}) bound ({:.3f} {:.3f} {:.3f} r {:.3f}) flags {:#x} children {}{}",
            tag, std::string(depth * 2, ' '), info.name ? info.name : "", info.type ? info.type : "?",
            info.local[0], info.local[1], info.local[2], info.world[0], info.world[1], info.world[2],
            info.previous[0], info.previous[1], info.previous[2], info.bound[0], info.bound[1], info.bound[2], info.bound[3], info.flags, info.non_null_count,
            info.child_count != info.non_null_count ? fmt::format(" (in {} slots)", info.child_count) : std::string{});
        for (uint16_t i = 0; info.children != nullptr && i < info.child_count; ++i) {
            dump(read_child(info.children, i), depth + 1, count, tag, max_nodes, max_depth); // empty slots are skipped
        }
    }

    bool is_anchor(const char* name) {
        if (name == nullptr) {
            return false;
        }
        const std::string_view n{ name };
        return n.find("AnimObject") != std::string_view::npos || n == "Weapon" || n == "WeaponLeft";
    }

    // Every anchor under `obj` (searched to depth 40, at most 4000 nodes looked at), each with its subtree (8 levels).
    void dump_anchors(RE::NiAVObject* obj, int depth, int& seen, int& count, const char* tag) {
        if (obj == nullptr || depth > 40 || seen >= 4000 || count >= 600) {
            return;
        }
        ++seen;
        NodeInfo info{};
        if (!read_node(obj, info)) {
            return;
        }
        if (is_anchor(info.name)) {
            NodeInfo parent{};
            const bool has_parent = info.parent != nullptr && read_node(info.parent, parent);
            spdlog::info("{} anchor {} (under {}):", tag, info.name, has_parent && parent.name ? parent.name : "?");
            dump(obj, 1, count, tag, 600, 9);
            return; // (its subtree is in the dump)
        }
        for (uint16_t i = 0; info.children != nullptr && i < info.child_count; ++i) {
            dump_anchors(read_child(info.children, i), depth + 1, seen, count, tag);
        }
    }

    void dump_hands(const char* why) {
        RE::NiAVObject* first = nullptr;
        RE::NiAVObject* third = nullptr;
        body::DebugRoots(first, third);
        spdlog::info("[HAND-TREE] begin ({}): the hands' anchor points and what hangs on them; first-person rig {}, body {}", why,
            fmt::ptr(first), fmt::ptr(third));
        int seen = 0, first_count = 0, body_count = 0;
        dump_anchors(first, 0, seen, first_count, "[HAND-TREE] 1P");
        seen = 0;
        dump_anchors(third, 0, seen, body_count, "[HAND-TREE] body");
        spdlog::info("[HAND-TREE] end ({} nodes in the first-person rig, {} in the body)", first_count, body_count);
    }
}

namespace {
    RE::NiAVObject* find_rec(RE::NiAVObject* obj, std::string_view name, int depth) {
        if (obj == nullptr || depth > 40) {
            return nullptr;
        }
        NodeInfo info{};
        if (!read_node(obj, info)) {
            return nullptr;
        }
        if (info.name != nullptr && name == info.name) {
            return obj;
        }
        for (uint16_t i = 0; info.children != nullptr && i < info.child_count; ++i) {
            if (auto found = find_rec(read_child(info.children, i), name, depth + 1)) {
                return found;
            }
        }
        return nullptr;
    }
}

RE::NiAVObject* NodeTreeDump::find(RE::NiAVObject* root, std::string_view name) {
    return find_rec(root, name, 0);
}

RE::NiAVObject* NodeTreeDump::find_weapon_bone(RE::NiAVObject* root) {
    // The "Weapon" that is a child of "C_Chest": a modded weapon's own model can be named "Weapon" too (hung elsewhere).
    if (auto chest = find_rec(root, "C_Chest", 0)) {
        NodeInfo info{};
        if (read_node(chest, info)) {
            for (uint16_t i = 0; info.children != nullptr && i < info.child_count; ++i) {
                auto child = read_child(info.children, i);
                NodeInfo c{};
                if (child != nullptr && read_node(child, c) && c.name != nullptr && std::string_view{ c.name } == "Weapon") {
                    return child;
                }
            }
        }
        return nullptr;
    }
    return find_rec(root, "Weapon", 0); // (a rig without C_Chest)
}

void NodeTreeDump::request() {
    g_dump_requested = true;
}

void NodeTreeDump::request_hands(const char* why) {
    g_hands_requested = why != nullptr ? why : "asked";
}

// Called on the game thread at the start of a frame (nodes aren't being updated then).
void NodeTreeDump::run_if_requested() {
    if (const char* why = g_hands_requested.exchange(nullptr); why != nullptr) {
        dump_hands(why);
    }
    if (!g_dump_requested.exchange(false)) {
        return;
    }

    auto camera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
    if (camera == nullptr || camera->pFirstPersonModeState == nullptr) {
        spdlog::info("[FP-TREE] no first-person camera state (go to first person and try again)");
        return;
    }

    // Print the chain of parents above the first-person camera, then dump the first-person
    // skeleton ("Root", the node Mutar's code turns with the headset) and everything under it.
    RE::NiAVObject* fp_root = nullptr;
    {
        RE::NiAVObject* node = (RE::NiAVObject*)camera->pFirstPersonModeState->cameraRoot;
        for (int i = 0; i < 64 && node != nullptr; ++i) {
            NodeInfo info{};
            if (!read_node(node, info)) {
                break;
            }
            spdlog::info("[FP-TREE] ancestor {}: {} [{}]", i, info.name ? info.name : "", info.type ? info.type : "?");
            if (fp_root == nullptr && info.name != nullptr && std::string_view{info.name} == "Root") {
                fp_root = node;
            }
            node = info.parent;
        }
    }
    RE::NiAVObject* top = fp_root != nullptr ? fp_root : (RE::NiAVObject*)camera->pFirstPersonModeState->cameraRoot;

    auto vr = VR::get();
    const auto hmd = vr->get_transform(0);
    const auto left = vr->get_transform(1);
    const auto right = vr->get_transform(2);
    spdlog::info("[FP-TREE] begin. headset ({:.3f} {:.3f} {:.3f}) left hand ({:.3f} {:.3f} {:.3f}) right hand ({:.3f} {:.3f} {:.3f}) [VR space, metres]",
        hmd[3].x, hmd[3].y, hmd[3].z, left[3].x, left[3].y, left[3].z, right[3].x, right[3].y, right[3].z);

    if (auto root = CreationEngineSingletonManager::GetSceneGraphRoot(); root != nullptr && root->worldCamera != nullptr) {
        NodeInfo cam{};
        if (read_node((RE::NiAVObject*)root->worldCamera, cam)) {
            spdlog::info("[FP-TREE] world camera at ({:.3f} {:.3f} {:.3f})", cam.world[0], cam.world[1], cam.world[2]);
        }
    }

    int count = 0;
    dump(top, 0, count);
    spdlog::info("[FP-TREE] end ({} nodes)", count);
}
