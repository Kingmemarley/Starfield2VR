#pragma once

#include <cstdint>
#include <optional>

#include <spdlog/spdlog.h>
#include <utility/Scan.hpp>

#include <CreationEngine/memory/ScanHelper.h>

namespace body
{
    // Signature scan that does not just take the first match.
    // SFVR's signatures were made for Starfield 1.16.244; on another version a short pattern could
    // also match an unrelated function first. A single match is almost certainly the same function;
    // with several, only the one at SFVR's own offset is accepted. Anything else is refused and the
    // feature that needs it stays off (with a log line).
    inline uintptr_t ScanUnique(const char* what, const char* pattern, uintptr_t sfvr_offset, const char* known_as = "SFVR's 1.16.244 offset")
    {
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        const auto size = MemoryScan::mod_size;
        if (base == 0 || size == 0) {
            spdlog::error("[Body] {}: game module not found", what);
            return 0;
        }
        const auto first = utility::scan(base, size, pattern);
        if (!first) {
            spdlog::error("[Body] {}: signature not found; this part stays off", what);
            return 0;
        }
        // Every match (a few at most), so a second one can be told apart from the one SFVR used.
        uintptr_t matches[32]{};
        int       count = 0;
        std::optional<uintptr_t> at = first;
        while (at && count < 32) {
            matches[count++] = *at;
            const uintptr_t next = *at + 1;
            if (next < base + size) {
                at = utility::scan(next, base + size - next, pattern);
            } else {
                at.reset();
            }
        }
        if (count == 1) {
            spdlog::info("[Body] {} found at {:x} ({} {:x})", what, matches[0] - base, known_as, sfvr_offset);
            return matches[0];
        }
        // Several matches: on SFVR's own game version the right one is at its offset.
        for (int i = 0; i < count; ++i) {
            if (matches[i] - base == sfvr_offset) {
                spdlog::info("[Body] {}: {} matches, using the one at {} {:x}", what, count, known_as, sfvr_offset);
                return matches[i];
            }
        }
        spdlog::error("[Body] {}: signature matches {}{} times (first at {:x}), none at SFVR's offset {:x}; this part stays off", what, count,
            count == 32 ? "+" : "", matches[0] - base, sfvr_offset);
        return 0;
    }
}
