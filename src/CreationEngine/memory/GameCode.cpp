#include "GameCode.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

namespace GameCode
{
    namespace Table
    {
        // A place in the port table: its rva in 1.16.236 and in the ported version; for a checked run, its bytes there (the ported
        // version's own) at kBytes[bytes_at], n of them.
        struct Entry
        {
            uint32_t rva236;
            uint32_t rva;
            uint32_t bytes_at;
            uint32_t n;
        };
    }
}

#if defined(SFVR_GAME_1_16_244)
#include "GameCode_1_16_244.inc"
#endif

namespace GameCode
{
    namespace
    {
        struct Image
        {
            uintptr_t base{ 0 };
            size_t    size{ 0 };
        };

        const Image& TheImage()
        {
            static const Image image = [] {
                Image      out;
                const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                if (module != 0) {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
                    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
                    out.base        = module;
                    out.size        = nt->OptionalHeader.SizeOfImage;
                }
                return out;
            }();
            return image;
        }

        // The game's version from its own version resource (VS_FIXEDFILEINFO, found by its signature), read from the loaded image.
        std::string ReadVersion()
        {
            const HMODULE module = GetModuleHandleW(nullptr);
            HRSRC         info   = module != nullptr ? FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */) : nullptr;
            HGLOBAL       data   = info != nullptr ? LoadResource(module, info) : nullptr;
            const auto*   bytes  = data != nullptr ? static_cast<const uint8_t*>(LockResource(data)) : nullptr;
            const DWORD   size   = info != nullptr ? SizeofResource(module, info) : 0;
            if (bytes == nullptr || size < sizeof(VS_FIXEDFILEINFO)) {
                return "unknown";
            }
            for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
                uint32_t signature = 0;
                std::memcpy(&signature, bytes + i, sizeof(signature));
                if (signature == 0xFEEF04BD) {
                    VS_FIXEDFILEINFO fixed{};
                    std::memcpy(&fixed, bytes + i, sizeof(fixed));
                    return fmt::format("{}.{}.{}.{}", HIWORD(fixed.dwFileVersionMS), LOWORD(fixed.dwFileVersionMS), HIWORD(fixed.dwFileVersionLS),
                        LOWORD(fixed.dwFileVersionLS));
                }
            }
            return "unknown";
        }

        bool InImage(uintptr_t at, size_t n)
        {
            const auto& image = TheImage();
            return image.base != 0 && at >= image.base && n <= image.size && at - image.base <= image.size - n;
        }

        // (No C++ objects here: a structured exception handler around the read.)
        bool SameBytes(uintptr_t at, const uint8_t* bytes, size_t n)
        {
            __try {
                return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // What was asked for and not known (for the status line), each once; and places known but checked longer than the table has
        // their bytes for (the review).
        std::mutex             g_missed_mutex;
        std::vector<uintptr_t> g_missed;
        std::vector<uintptr_t> g_short;

        void Note(std::vector<uintptr_t>& list, uintptr_t rva236)
        {
            // (fix190) Some places are asked for every frame (a ported build's unknown ones too): the last few this thread noted are
            // passed by without the lock (the review).
            thread_local std::array<uintptr_t, 16> recent{};
            thread_local size_t                    next = 0;
            const uintptr_t                        key  = rva236 * 2 + (&list == &g_short ? 1 : 0);
            if (std::find(recent.begin(), recent.end(), key) != recent.end()) {
                return;
            }
            recent[next++ % recent.size()] = key;
            std::scoped_lock _{ g_missed_mutex };
            if (list.size() < 4096 && std::find(list.begin(), list.end(), rva236) == list.end()) {
                list.push_back(rva236);
            }
        }
        void Missed(uintptr_t rva236) { Note(g_missed, rva236); }

#if defined(SFVR_GAME_1_16_244)
        const Table::Entry* Find(uintptr_t rva236)
        {
            const auto it = std::lower_bound(Table::kEntries.begin(), Table::kEntries.end(), rva236,
                [](const Table::Entry& e, uintptr_t r) { return e.rva236 < r; });
            return it != Table::kEntries.end() && it->rva236 == rva236 ? &*it : nullptr;
        }
#endif
    }

    uintptr_t Base() { return TheImage().base; }

    const std::string& GameVersion()
    {
        static const std::string version = ReadVersion();
        return version;
    }

    bool VersionMatches()
    {
        // (A version that cannot be read is taken to be this build's: as before fix188, each feature's own check of the game's code
        // still decides.)
        static const bool matches = GameVersion() == "unknown" || GameVersion().rfind(std::string{ kTargetVersion } + ".", 0) == 0;
        return matches;
    }

    uintptr_t Rva(uintptr_t rva236)
    {
        if (!VersionMatches()) {
            Missed(rva236);
            return 0;
        }
#if defined(SFVR_GAME_1_16_244)
        if (const auto* e = Find(rva236)) {
            return e->rva;
        }
        Missed(rva236);
        return 0;
#else
        if (rva236 == 0 || rva236 >= TheImage().size) {
            Missed(rva236);
            return 0;
        }
        return rva236;
#endif
    }

    uintptr_t At(uintptr_t rva236)
    {
        const uintptr_t rva = Rva(rva236);
        return rva != 0 ? Base() + rva : 0;
    }

    bool Known(std::initializer_list<uintptr_t> rvas236)
    {
        bool all = true;
        for (const uintptr_t r : rvas236) {
            all = Rva(r) != 0 && all; // (each one asked for: the missing ones all noted)
        }
        return all;
    }

    uintptr_t Code(uintptr_t rva236, const uint8_t* bytes236, size_t n)
    {
        if (bytes236 == nullptr || n == 0) {
            return 0;
        }
        const uintptr_t at = At(rva236);
        if (at == 0 || !InImage(at, n)) {
            return 0;
        }
#if defined(SFVR_GAME_1_16_244)
        const auto* e = Find(rva236);
        if (e == nullptr || e->n < n || static_cast<size_t>(e->bytes_at) + n > Table::kBytes.size()) {
            Note(g_short, rva236); // (a place known, but not as a checked run this long: bake the table again)
            return 0;
        }
        return SameBytes(at, Table::kBytes.data() + e->bytes_at, n) ? at : 0;
#else
        return SameBytes(at, bytes236, n) ? at : 0;
#endif
    }

    std::string Status()
    {
        std::string missed, shorter;
        size_t      count = 0, short_count = 0;
        {
            std::scoped_lock _{ g_missed_mutex };
            count       = g_missed.size();
            short_count = g_short.size();
            for (size_t i = 0; i < g_missed.size() && i < 12; ++i) {
                missed += fmt::format("{}{:x}", i == 0 ? "" : ", ", g_missed[i]);
            }
            for (size_t i = 0; i < g_short.size() && i < 6; ++i) {
                shorter += fmt::format("{}{:x}", i == 0 ? "" : ", ", g_short[i]);
            }
        }
        std::string table;
#if defined(SFVR_GAME_1_16_244)
        table = fmt::format("; the port table: {} places ({})", Table::kEntries.size(), Table::kFrom);
#else
        table = " (the mod's own places)";
#endif
        return fmt::format("Game version: Starfield {}; this build is for {}{}{}{}{}", GameVersion(), kTargetVersion, table,
            VersionMatches() ? "" : ": another version, so nothing of the game's is used (get the build for your version)",
            count > 0 ? fmt::format("; {} place(s) asked for and not known (1.16.236 rvas: {}{})", count, missed, count > 12 ? ", ..." : "")
                      : std::string{},
            short_count > 0 ? fmt::format("; {} checked longer than the port table has them (bake it again: {})", short_count, shorter) : std::string{});
    }

    void LogOnce()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            if (VersionMatches()) {
                spdlog::info("[GameCode] (fix188) {}", Status());
            } else {
                spdlog::warn("[GameCode] (fix188) {}", Status());
            }
        });
    }
}
