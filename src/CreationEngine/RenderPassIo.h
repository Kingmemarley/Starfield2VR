#pragma once

#include <windows.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>

// The textures a render pass of the game uses (its inputs and outputs), read from the pass's data as the game's own steps look them up
// (Starfield 1.16.236; fix120's research of the exe). The pass's data: [0] its items, [1] the graph's resources.
//
// Its items: a small array (its count at +0; at +8 a word whose top bit says its items are inline at +0x10, else +0x10 points at them)
// of 0x20-byte items: a key (+0, 8 bytes: a small index in its low 10 bits; its upper half, +4, the item's kind: 0 a texture, 1 a
// buffer, 2 a token, 3 the processor's data), the declaring step's id (+8: a CRC32 of that step's address, so the same picture has
// another key and id in each step or graph that declares it), the state the step needs it in (+0x10: the game puts it in that state
// before the step runs, and it stays so after the step until the game's next planned change of it), +0x18 and the byte at +0x1c (3 in either:
// its state not tracked: the game plans no change of it there, 0x142A59D50, nor makes one as a picture is attached, 0x142A82029).
//
// The graph's resources (RenderGraph+0x218, shared with its sub-graphs): a counted registry: its reference count at +0 (not a number of
// entries), then arrays (each: its size at +0, its capacity at +4, its data at +8, always a pointer) of textures at +0x08 (0xd8-byte
// entries: the key at +0xb8, the id at +0xc0, the texture's wrapper at +0xc8, whose +0x58 is its context, whose +0x38 is the D3D12
// resource and +0x44 its home state), buffers at +0x18 (0x88-byte entries, parts of a larger shared resource), the processor's data at
// +0x28 and tokens at +0x38. A step finds an item's texture as the first texture entry with the same id and the same whole key
// (0x142B197A0); some steps attach their own pictures as they run (their entry's wrapper empty until then).
//
// (Used by MenuLayer, LiveWorld, ViewHistory and RenderSteps.)
namespace RenderPassIo
{
    // (A step's first 32 items: the anti-aliasing step alone has more than 8.)
    constexpr uint32_t kMaxItems = 32;

    enum Kind : uint32_t
    {
        kTexture = 0,
        kBuffer  = 1,
        kToken   = 2,
        kData    = 3,
    };

    enum Found : uint8_t
    {
        kFound    = 0, // its texture found
        kNotFound = 1, // a texture, but no entry with its key and id
        kEmpty    = 2, // a texture whose entry has no picture (yet)
        kNotOne   = 3, // not a texture (a buffer, a token, data): no picture
    };

    struct PassIo
    {
        uint32_t count{ 0 };
        uint32_t total{ 0 }; // how many it has (more than count: the rest not read)
        struct Item
        {
            uint64_t        key{ 0 };
            uint32_t        graph_index{ 0 }; // (the declaring step's id)
            uint32_t        flags{ 0 };       // the state it needs (the game's flags)
            uint32_t        kind{ 0 };
            bool            tracked{ true };  // (false: its state not tracked by the game)
            Found           found{ kNotFound };
            ID3D12Resource* resource{ nullptr };
            uintptr_t       entry{ 0 }; // (fix156) its texture's registry entry (0: not found)
        } items[kMaxItems]{};
    };

    // Bumped at each of the frame's top-level graphs (CreationEngineRendererModule): the lookup indexes are made again then.
    inline std::atomic<uint32_t> g_generation{ 0 };
    inline void                  NewFrame() { g_generation.fetch_add(1, std::memory_order_relaxed); }
    // (For the log: how many indexes were made, and of how many entries in all.)
    inline std::atomic<uint64_t> g_index_builds{ 0 };
    inline std::atomic<uint64_t> g_index_entries{ 0 };

    namespace detail
    {
        constexpr uintptr_t kTextures    = 0x08;
        constexpr uintptr_t kEntrySize   = 0xd8;
        constexpr uintptr_t kEntryKey    = 0xb8;
        constexpr uintptr_t kEntryId     = 0xc0;
        constexpr uintptr_t kEntryObject = 0xc8;
        constexpr uint32_t  kMaxEntries  = 1u << 16;

        inline bool ReadArray(uintptr_t at, uint32_t& size, uintptr_t& data)
        {
            __try {
                size = *reinterpret_cast<const uint32_t*>(at);
                data = *reinterpret_cast<const uintptr_t*>(at + 8);
                return size <= kMaxEntries && (size == 0 || data != 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline bool ReadEntry(uintptr_t data, uint32_t j, uint64_t& key, uint32_t& id)
        {
            __try {
                const uintptr_t e = data + static_cast<uintptr_t>(j) * kEntrySize;
                key               = *reinterpret_cast<const uint64_t*>(e + kEntryKey);
                id                = *reinterpret_cast<const uint32_t*>(e + kEntryId);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The entry's picture (null: none attached), read as it is now.
        inline bool EntryResource(uintptr_t data, uint32_t j, ID3D12Resource*& out)
        {
            out = nullptr;
            __try {
                const uintptr_t e       = data + static_cast<uintptr_t>(j) * kEntrySize;
                const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(e + kEntryObject);
                const uintptr_t context = wrapper != 0 ? *reinterpret_cast<const uintptr_t*>(wrapper + 0x58) : 0;
                out                     = context != 0 ? *reinterpret_cast<ID3D12Resource* const*>(context + 0x38) : nullptr;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline bool ReadItems(const void* pass_data, uintptr_t& item_data, uint32_t& count, uintptr_t& registry)
        {
            __try {
                const uintptr_t pd    = reinterpret_cast<uintptr_t>(pass_data);
                const uintptr_t items = *reinterpret_cast<const uintptr_t*>(pd);
                registry              = *reinterpret_cast<const uintptr_t*>(pd + 8);
                if (items == 0) {
                    return false;
                }
                count     = *reinterpret_cast<const uint32_t*>(items);
                item_data = *reinterpret_cast<const int32_t*>(items + 8) < 0 ? items + 0x10 : *reinterpret_cast<const uintptr_t*>(items + 0x10);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline bool ReadItem(uintptr_t item_data, uint32_t i, PassIo::Item& o)
        {
            __try {
                const uintptr_t item = item_data + static_cast<uintptr_t>(i) * 0x20;
                o.key                = *reinterpret_cast<const uint64_t*>(item);
                o.graph_index        = *reinterpret_cast<const uint32_t*>(item + 8);
                o.flags              = *reinterpret_cast<const uint32_t*>(item + 0x10);
                o.tracked            = *reinterpret_cast<const uint32_t*>(item + 0x18) != 3 && *reinterpret_cast<const uint8_t*>(item + 0x1c) != 3;
                o.kind               = static_cast<uint32_t>(o.key >> 32) & 0xff; // (its byte at +4, as the game reads it)
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline uint64_t Mix(uint64_t key, uint32_t id)
        {
            return (key * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(id) * 0xC2B2AE3D27D4EB4Full);
        }

        // Each thread's index of a few registries' textures (by key and id: the first entry, as the game finds it), made again each frame
        // and whenever the registry's array moved or changed size; a match is checked against the entry before it is used.
        struct Index
        {
            uintptr_t                              registry{ 0 };
            uintptr_t                              data{ 0 };
            uint32_t                               size{ 0 };
            uint32_t                               generation{ 0 };
            bool                                   valid{ false };
            std::unordered_map<uint64_t, uint32_t> first{};
        };
        inline thread_local std::array<Index, 4> t_index{};
        inline thread_local int                  t_next{ 0 };

        inline Index* IndexFor(uintptr_t registry, uintptr_t data, uint32_t size)
        {
            const uint32_t generation = g_generation.load(std::memory_order_relaxed);
            for (auto& index : t_index) {
                if (index.valid && index.registry == registry && index.data == data && index.size == size && index.generation == generation) {
                    return &index;
                }
            }
            Index& index = t_index[t_next];
            t_next       = (t_next + 1) % static_cast<int>(t_index.size());
            index.valid  = false;
            index.first.clear();
            index.first.reserve(size);
            for (uint32_t j = 0; j < size; ++j) {
                uint64_t key = 0;
                uint32_t id  = 0;
                if (!ReadEntry(data, j, key, id)) {
                    return nullptr;
                }
                index.first.try_emplace(Mix(key, id), j);
            }
            index.registry   = registry;
            index.data       = data;
            index.size       = size;
            index.generation = generation;
            index.valid      = true;
            g_index_builds.fetch_add(1, std::memory_order_relaxed);
            g_index_entries.fetch_add(size, std::memory_order_relaxed);
            return &index;
        }

        // The texture entry of an item: its index, or -1 (none). (An index's hit is checked against the entry; a miss, which should be
        // rare, is looked through as the game does, and if found there the index is out of date: made again at the next lookup.)
        inline int64_t FindTexture(uintptr_t data, uint32_t size, Index* index, uint64_t key, uint32_t id)
        {
            if (index != nullptr) {
                const auto it = index->first.find(Mix(key, id));
                if (it != index->first.end()) {
                    uint64_t k = 0;
                    uint32_t i = 0;
                    if (ReadEntry(data, it->second, k, i) && k == key && i == id) {
                        return it->second;
                    }
                }
            }
            for (uint32_t j = 0; j < size; ++j) {
                uint64_t k = 0;
                uint32_t i = 0;
                if (!ReadEntry(data, j, k, i)) {
                    return -1;
                }
                if (k == key && i == id) {
                    if (index != nullptr) {
                        index->valid = false;
                    }
                    return j;
                }
            }
            return -1;
        }
    }

    inline bool ReadPassIo(const void* pass_data, PassIo& out)
    {
        uintptr_t item_data = 0, registry = 0;
        uint32_t  count     = 0;
        if (pass_data == nullptr || !detail::ReadItems(pass_data, item_data, count, registry)) {
            return false;
        }
        out.total = count; // (also when they cannot be read: the step has some)
        if (registry == 0 || item_data == 0 || count == 0 || count > 64) {
            return false;
        }
        uint32_t  size = 0;
        uintptr_t data = 0;
        if (!detail::ReadArray(registry + detail::kTextures, size, data)) {
            return false;
        }
        count     = count > kMaxItems ? kMaxItems : count;
        out.count = count;
        detail::Index* index = nullptr;
        bool           tried = false;
        for (uint32_t i = 0; i < count; ++i) {
            auto& o = out.items[i];
            o       = PassIo::Item{};
            if (!detail::ReadItem(item_data, i, o)) {
                out.count = i;
                return i > 0;
            }
            if (o.kind != kTexture) {
                o.found = kNotOne;
                continue;
            }
            if (!tried) {
                tried = true;
                index = detail::IndexFor(registry, data, size);
            }
            const int64_t j = detail::FindTexture(data, size, index, o.key, o.graph_index);
            if (j < 0) {
                o.found = kNotFound;
                continue;
            }
            ID3D12Resource* resource = nullptr;
            detail::EntryResource(data, static_cast<uint32_t>(j), resource);
            o.resource = resource;
            o.entry    = data + static_cast<uintptr_t>(j) * detail::kEntrySize;
            o.found    = resource != nullptr ? kFound : kEmpty;
        }
        return true;
    }

    inline bool DescOf(ID3D12Resource* resource, D3D12_RESOURCE_DESC& out)
    {
        __try {
            out = resource->GetDesc();
            return out.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && out.Width > 0 && out.Height > 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}
