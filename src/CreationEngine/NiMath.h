#pragma once

// Conversions between the game's node transforms and glm (column vectors: world = R * local + t).
//
// Starfield stores a node's rotation with the node's own axes in the rows of NiMatrix3: entry[r]
// is local axis r written in the parent's coordinates (the DirectX row-vector layout). Mutar's
// camera code relies on this (it copies glm's column-major memory straight into an NiMatrix3),
// and the first-person yaw in the trace logs only has the player's heading sign when read this
// way. The SDK's NiMatrix3 operators assume the opposite, so they are not used for maths here.
//
// MotionWeapon checks the layout against the game's own result every frame and flips
// g_rows_are_axes (with a log line) if the game disagrees.

#include <atomic>

#include <glm/glm.hpp>

#include "RE/N/NiMatrix3.h"
#include "RE/N/NiPoint3.h"

namespace NiMath {
    inline std::atomic<bool> g_rows_are_axes{true};

    inline glm::mat3 to_glm(const RE::NiMatrix3& m, bool rows_are_axes) {
        glm::mat3 g{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (rows_are_axes) {
                    g[r][c] = m.entry[r].pt[c]; // glm column r = axis r
                } else {
                    g[c][r] = m.entry[r].pt[c];
                }
            }
        }
        return g;
    }

    inline glm::mat3 to_glm(const RE::NiMatrix3& m) { return to_glm(m, g_rows_are_axes.load()); }

    inline void from_glm(const glm::mat3& g, RE::NiMatrix3& m) {
        const bool rows_are_axes = g_rows_are_axes.load();
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                m.entry[r].pt[c] = rows_are_axes ? g[r][c] : g[c][r];
            }
            m.entry[r].pt[3] = 0.0f;
        }
    }

    inline glm::vec3 to_glm(const RE::NiPoint3& p) { return {p.x, p.y, p.z}; }
    inline RE::NiPoint3 to_ni(const glm::vec3& v) { return {v.x, v.y, v.z}; }
}
