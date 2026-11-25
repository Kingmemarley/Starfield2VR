#include "GloveHighlight.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include "GrenadeHand.h"
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"
#include "models/ModSettingsStore.h"
#include <CreationEngine/models/GameFlow.h>

namespace GloveHighlight
{
    namespace
    {
        // Starfield 1.16.236.
        constexpr uintptr_t kMakeRva          = 0x653FC0;  // HighlightManager(memory, style)
        constexpr uintptr_t kClearRva         = 0x654160;  // HighlightManager::Clear: unmarks all it marked
        constexpr uintptr_t kStyleRva         = 0x654520;  // the style of a key (&key, style)
        constexpr uintptr_t kWalkRva          = 0x241A660; // visits each geometry of a 3D (&visitor, root)
        constexpr uintptr_t kKeyRva           = 0x7CBB50;  // a new key of the renderer's highlights (the manager's making calls it)
        constexpr uintptr_t kManagerTypeRva   = 0x4B339D8; // the manager's type table
        constexpr uintptr_t kMarkVisitorRva   = 0x4B33A48; // the visitor that marks each geometry with a manager's key
        constexpr uintptr_t kScannerMarkRva   = 0x17D775F; // where the hand scanner marks an object (lea that visitor; call the walk)
        constexpr uintptr_t kHighlightsRva    = 0x597D1F8; // the renderer's highlights (made once it is up)
        constexpr size_t    kManagerSize      = 0x48;
        constexpr size_t    kManagerKey       = 0x40;
        // The bytes these start with, as checked (the clear's: its first 26).
        constexpr uint8_t kMakeStart[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20 };
        constexpr uint8_t kClearStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                            0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x69, 0x28, 0x33, 0xDB };

        // The style (HighlightInputInfo): colours 0xAARRGGBB; a fill of alpha 0 leaves the outline only.
        struct Style
        {
            uint32_t fill{ 0 };
            uint32_t dim_dark{ 0 }; // 1: the fill fades out on the darkest pixels (not hazed)
            uint32_t outline{ 0 };
            float    width{ 1.0f };    // pixels
            float    hardness{ 0.1f };
            uint32_t rest[3]{};
        };
        static_assert(sizeof(Style) == 0x20);
        // The marking visitor, as the scanner builds it on its stack: its type table and its captures (the manager and its key).
        struct Captures
        {
            void*    manager{ nullptr };
            uint32_t key{ 0 };
            uint32_t pad{ 0 };
        };
        struct Visitor
        {
            uintptr_t type{ 0 };
            Captures* captures{ nullptr };
        };
        using MakeFn  = void* (*)(void*, const Style*);
        using ClearFn = void (*)(void*);
        using StyleFn = void (*)(uint32_t*, const Style*);
        using WalkFn  = void (*)(Visitor*, void*);

        // Aimed at: light blue (as the gloves' marker was); locked on: white. A soft outline round the object, wider than the
        // scanner's (3 pixels: the headset's picture is larger), and a light glow over it (GloveHighlightGlow). The game's highlight
        // shader (a helper's research: "Highlight" in Starfield - Shaders.ba2) blends the fill by its alpha, but first turns its
        // colour from the screen's into the scene's light by undoing the tonemapper: white becomes about 7 times the brightest the
        // screen shows, so fix130's 20% white made the object a solid white. As the game's own scanner fills do (0x201F8EE2: a
        // middling colour, a low alpha): the glow's colour below white (0xCC grey, a muted light blue), its alpha at most 0x40
        // (the setting's whole range), and dark parts not hazed (the style's +4 flag: the fill fades out on the darkest pixels).
        constexpr uint32_t kAimedColour  = 0x8CD2FF;
        constexpr uint32_t kLockedColour = 0xFFFFFF;
        constexpr uint32_t kAimedGlow    = 0x80B8D9;
        constexpr uint32_t kLockedGlow   = 0xCCCCCC;
        Style StyleOf(uint32_t colour, uint32_t glow, float fill)
        {
            const auto alpha = (uint32_t)std::lround(std::clamp(fill, 0.0f, 1.0f) * 64.0f);
            return Style{ (alpha << 24) | glow, 1, 0xFF000000u | colour, 6.0f, 0.1f, {} };
        }
        float g_fill_applied{ 0.0f };

        uintptr_t g_base{ 0 };
        bool      g_installed{ false };
        alignas(16) uint8_t g_storage[2][kManagerSize]{};
        void*     g_managers[2]{ nullptr, nullptr }; // [0] aimed at, [1] locked on (made once, kept)
        bool      g_make_failed{ false };
        int       g_kind{ 0 };
        uint32_t  g_handle{ 0 };
        bool      g_showing{ false };
        std::chrono::steady_clock::time_point g_tried_at{};
        int       g_logs{ 0 };
        std::mutex g_status_mtx{};
        std::string g_status{ "Gloves' highlight: not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // Where a call (E8) or a lea/mov with a RIP-relative operand (its last 4 bytes) leads.
        uintptr_t Target(uintptr_t at, size_t length, uint8_t first, size_t prefix = 0)
        {
            __try {
                const auto* b = reinterpret_cast<const uint8_t*>(at);
                if (b[prefix] != first) {
                    return 0;
                }
                const int32_t rel = *reinterpret_cast<const int32_t*>(at + length - 4);
                return at + length + rel;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        bool ReadPointer(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallMake(void* storage, const Style* style, void*& out)
        {
            __try {
                out = reinterpret_cast<MakeFn>(At(kMakeRva))(storage, style);
                return out != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out = nullptr;
                return false;
            }
        }

        // The two managers, once the renderer's highlights are there.
        bool MakeManagers()
        {
            if (g_managers[0] != nullptr) {
                return true;
            }
            uintptr_t highlights = 0;
            if (g_make_failed || !ReadPointer(At(kHighlightsRva), highlights) || highlights == 0) {
                return false;
            }
            void* made[2]{};
            const float fill   = GameFlow::gStore.internalSettings.gloveHighlightFill;
            const Style aimed  = StyleOf(kAimedColour, kAimedGlow, fill);
            const Style locked = StyleOf(kLockedColour, kLockedGlow, fill);
            g_fill_applied     = fill;
            if (!CallMake(g_storage[0], &aimed, made[0]) || !CallMake(g_storage[1], &locked, made[1])) {
                g_make_failed = true;
                SetStatus("Gloves' highlight: off (the game's highlights could not be made)");
                spdlog::error("[GloveHighlight] the game's highlights could not be made: the gloves' ring and glow pictures are used");
                return false;
            }
            g_managers[0] = made[0];
            g_managers[1] = made[1];
            uint32_t keys[2]{};
            for (int i = 0; i < 2; ++i) {
                std::memcpy(&keys[i], g_storage[i] + kManagerKey, sizeof(uint32_t));
            }
            spdlog::info("[GloveHighlight] the game's highlights made (keys {:#x}, {:#x}): blue on what the gloves aim at, white once locked on", keys[0], keys[1]);
            SetStatus("Gloves' highlight: ready");
            return true;
        }

        // (Both of ours, whatever was shown: a mark left halfway (a fault in the walk) goes too. An empty one is a short loop.)
        void ClearShown()
        {
            for (void* manager : g_managers) {
                if (manager == nullptr) {
                    continue;
                }
                __try {
                    reinterpret_cast<ClearFn>(At(kClearRva))(manager);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            g_showing = false;
        }

        bool CallStyle(void* manager, const Style* style)
        {
            __try {
                reinterpret_cast<StyleFn>(At(kStyleRva))(reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(manager) + kManagerKey), style);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The fill changed in the settings: both styles set again (as the scanner sets its own each frame).
        void Restyle(float fill)
        {
            if (g_managers[0] == nullptr || g_managers[1] == nullptr) {
                return;
            }
            const Style aimed  = StyleOf(kAimedColour, kAimedGlow, fill);
            const Style locked = StyleOf(kLockedColour, kLockedGlow, fill);
            // (Tried once for each change: a failure is not tried again every frame.)
            const bool aimed_set  = CallStyle(g_managers[0], &aimed);
            const bool locked_set = CallStyle(g_managers[1], &locked);
            g_fill_applied        = fill;
            if (aimed_set && locked_set) {
                spdlog::info("[GloveHighlight] its glow over the object now {:.0f}% (fill 0x{:08X} and 0x{:08X})", fill * 100.0f, aimed.fill, locked.fill);
            } else {
                spdlog::warn("[GloveHighlight] its glow could not be set to {:.0f}% (blue {}, white {})", fill * 100.0f, aimed_set, locked_set);
            }
        }

        // Something marked by it: its set of geometry not empty (its room +0x28 against its free places +0x30, as the scanner
        // tests it at 0x1417D7B8D). A 3D not yet known to the renderer marks nothing.
        bool AnyMarked(void* manager)
        {
            __try {
                const auto* m = static_cast<const uint8_t*>(manager);
                return *reinterpret_cast<const uint32_t*>(m + 0x28) != *reinterpret_cast<const uint32_t*>(m + 0x30);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix151) The frozen look (the gravity gloves' highlight replaced by a frozen effect): the game's own frost, the
        // effect shader the Starborn power Sunless Space freezes a target with (ArtifactPower_SunlessSpaceFrozenFXS, Starfield.esm
        // 0x00199E21: the only freeze among the game's 47 effect shaders; cryo weapons have none of their own), put on what the gloves
        // aim at and hold as the game's scripts do (EffectShader.Play, 0x1420496D0: after its loaded check it calls 0x140B790C0(reference,
        // shader, duration, null, -, -, -, false), TESObjectREFR::ApplyEffectShader, which makes the effect and adds it to the process
        // lists; -1: until stopped), taken off as EffectShader.Stop does (0x142049840: 0x141A953D0(process lists [0x145FE1908], reference,
        // shader)). The form found by its ID as Game.GetForm does (0x142017960: jmp 0x1405DEF30(id)), checked to be a TESEffectShader
        // (its type table 0x144B823D8).
        constexpr uintptr_t kLookupRva       = 0x5DEF30;
        constexpr uintptr_t kGetFormRva      = 0x2017960;
        constexpr uintptr_t kApplyRva        = 0xB790C0;
        constexpr uintptr_t kPlayNativeRva   = 0x20496D0;
        constexpr uintptr_t kPlayCallRva     = 0x20497B9; // (the native's call of the apply)
        constexpr uintptr_t kStopNativeRva   = 0x2049840;
        constexpr uintptr_t kStopRva         = 0x1A953D0;
        constexpr uintptr_t kProcessListsRva = 0x5FE1908;
        constexpr uintptr_t kShaderTypeRva   = 0x4B823D8;
        // (fix156) The choice after fix155 (the frost's only bright part is its dry-ice mist, smoke and glow in one emitter; its ice
        // coat is faint on clutter: a helper's research of SunlessSpaceDryIce.psfx and SunlessSpaceFrozen.mat): the game's pulsing signal
        // glow instead, CCT_SignalGlowFXS (Starfield.esm 0x003BDBB6: the self-lit CreatureGlow.mat membrane, icy white-blue, with a
        // pulsing light at COM; no particles), put on and taken off the same way. The frost's smoke is not skipped any more (the cryo
        // weapons' frost as the game has it).
        constexpr uint32_t  kFrozenShaderId  = 0x003BDBB6;
        constexpr uint8_t   kLookupStart[]   = { 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB };
        constexpr uint8_t   kApplyStart[]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50 };
        using LookupFn = void* (*)(uint32_t);
        using ApplyFn  = void* (*)(uintptr_t ref, void* shader, float duration, void* facing, uint64_t, uint64_t, uint64_t, bool);
        using StopFn   = void (*)(void* lists, uintptr_t ref, void* shader);
        bool      g_frozen_ok{ false };   // (the game's code checked)
        void*     g_frozen_shader{ nullptr };
        bool      g_frozen_lookup_failed{ false };
        uint32_t  g_frozen_handle{ 0 };   // the reference frozen now (0: none)
        bool      g_frost_mode{ false };   // (the frost in use this frame: the locked-on ring then still shown)
        int       g_frozen_logs{ 0 };

        bool CallLookup(uint32_t id, void*& out)
        {
            __try {
                out = reinterpret_cast<LookupFn>(At(kLookupRva))(id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out = nullptr;
                return false;
            }
        }

        // (fix153, fix155) The frost without its dry-ice smoke (after fix152: no smoke, only the white
        // frozen glow). The shader plays an effect sequence (EFSQ 0x00199E20, a helper's research): a material track (SunlessSpaceFrozen.mat
        // over the object: the glow) and a particle track (SunlessSpaceDryIce.psfx at "COM": the smoke) with one ParticleEvent. fix153
        // cleared the track's Visible byte, which only matters when a sequence is compiled, and every sequence is compiled once as the
        // game's data loads (its +0x59 "set up" was already set: nothing was changed). fix155: each play starts its events through the
        // event's type table slot +0x30 (0x14243FDB0); a ParticleEvent's (type table 0x144B5C278, slot 0x144B5C2A8) is 0x1403FA780
        // (mov rcx,[rcx+10h]; jmp 0x1403FA8C0: its particle system made into the play's state). Hooked at its start: for this frost's
        // smoke event, straight to a ret of the game's (0x1403FA800; the return address on top of the stack, its result not used):
        // no particle system is made, and the play's update (slot +0x40) and its end skip an empty one. The game's data is not changed.
        // The same frost is the cryo weapons' and cryo mines' (CryoSlow, CryoMine_Effect): their smoke goes too. The
        // walk to the event: the form (0x144B533C8, its type byte 0xC9 at +0x2E), its sequences' map (+0x38, 0x144B0DEF0: entries at
        // +0x18, their count +0x20, 0x18 bytes each, the sequence at +8, an empty one 0xFFFFFFFF at +0x10), a sequence's root
        // (0x144B0E248: its tracks +0x20 count, +0x28 array), the particle track (0x144B5C148: its events +0x10 count, +0x18 array), the
        // event (0x144B5C278, its track at +0x10).
        constexpr uint32_t  kFrozenSequenceId     = 0x00199E20;
        constexpr uintptr_t kSequenceFormTypeRva  = 0x4B533C8;
        constexpr uintptr_t kSequenceMapTypeRva   = 0x4B0DEF0;
        constexpr uintptr_t kTrackGroupTypeRva    = 0x4B0E248;
        constexpr uintptr_t kParticleTrackTypeRva = 0x4B5C148;
        constexpr uintptr_t kParticleEventTypeRva = 0x4B5C278;
        constexpr uintptr_t kEventStartSlot       = 0x30;
        constexpr uintptr_t kSmokeStartRva        = 0x3FA780;
        constexpr uint8_t   kSmokeStart[]         = { 0x48, 0x8B, 0x49, 0x10, 0xE9, 0x37, 0x01, 0x00, 0x00 };
        constexpr uintptr_t kSmokeRetRva          = 0x3FA800;
        constexpr uintptr_t kSmokeRetCheckRva     = 0x3FA7FB;
        constexpr uint8_t   kSmokeRetAt[]         = { 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0xCC }; // (add rsp,20h; pop rbx; ret: the ret at +5)
        constexpr int       kSmokeEvents          = 4;
        constexpr bool      kSkipFrostSmoke       = false; // (fix156: the gloves use the signal glow, which has no smoke)
        bool                   g_smoke_done{ false };
        safetyhook::MidHook    g_smoke_hook{};
        std::atomic<uintptr_t> g_smoke_events[kSmokeEvents]{};
        std::atomic<int>       g_smoke_skipped{ 0 };

        // A play starting a ParticleEvent (any thread that updates effects): the frost's smoke is not started.
        void OnSmokeStart(safetyhook::Context& ctx)
        {
            for (const auto& e : g_smoke_events) {
                if (const uintptr_t event = e.load(std::memory_order_relaxed); event != 0 && ctx.rcx == event) {
                    ctx.rax = 0;
                    ctx.rip = At(kSmokeRetRva);
                    if (const int n = g_smoke_skipped.fetch_add(1, std::memory_order_relaxed) + 1; n <= 3 || n % 100 == 0) {
                        spdlog::info("[GloveHighlight] (fix155) the frost's smoke not started ({} time(s); thread {})", n, GetCurrentThreadId());
                    }
                    return;
                }
            }
        }

        // (Under a guard: plain values only.) A sequence form's events of one kind on its tracks of one kind (up to kSmokeEvents into out; the
        // frost's smoke, fix157's glow light); -1: not as expected. (fix190: the two kinds' type tables given by their address, At.)
        int FindEventsGuarded(uintptr_t form, uintptr_t track_type, uintptr_t event_type, uintptr_t* out)
        {
            __try {
                if (*reinterpret_cast<const uint8_t*>(form + 0x2E) != 0xC9 || *reinterpret_cast<const uintptr_t*>(form) != At(kSequenceFormTypeRva)) {
                    return -1;
                }
                const uintptr_t map = form + 0x38;
                if (*reinterpret_cast<const uintptr_t*>(map) != At(kSequenceMapTypeRva)) {
                    return -1;
                }
                const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(map + 0x18);
                const uint64_t  count   = *reinterpret_cast<const uint64_t*>(map + 0x20);
                if (entries == 0 || count == 0 || count > 64) {
                    return -1;
                }
                int found = 0;
                for (uint64_t i = 0; i < count; ++i) {
                    const uintptr_t e = entries + i * 0x18;
                    if (*reinterpret_cast<const uint32_t*>(e + 0x10) == 0xFFFFFFFFu) {
                        continue;
                    }
                    const uintptr_t seq = *reinterpret_cast<const uintptr_t*>(e + 8);
                    if (seq == 0 || *reinterpret_cast<const uintptr_t*>(seq) != At(kTrackGroupTypeRva)) {
                        continue;
                    }
                    const uint32_t  tracks = *reinterpret_cast<const uint32_t*>(seq + 0x20);
                    const uintptr_t array  = *reinterpret_cast<const uintptr_t*>(seq + 0x28);
                    if (array == 0 || tracks > 16) {
                        continue;
                    }
                    for (uint32_t t = 0; t < tracks; ++t) {
                        const uintptr_t track = *reinterpret_cast<const uintptr_t*>(array + t * 8ull);
                        if (track == 0 || *reinterpret_cast<const uintptr_t*>(track) != track_type) {
                            continue;
                        }
                        const uint32_t  events = *reinterpret_cast<const uint32_t*>(track + 0x10);
                        const uintptr_t list   = *reinterpret_cast<const uintptr_t*>(track + 0x18);
                        if (list == 0 || events > 16) {
                            continue;
                        }
                        for (uint32_t k = 0; k < events && found < kSmokeEvents; ++k) {
                            const uintptr_t event = *reinterpret_cast<const uintptr_t*>(list + k * 8ull);
                            if (event != 0 && *reinterpret_cast<const uintptr_t*>(event) == event_type &&
                                *reinterpret_cast<const uintptr_t*>(event + 0x10) == track) {
                                out[found++] = event;
                            }
                        }
                    }
                }
                return found;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        void DropFrostSmoke()
        {
            if (g_smoke_done) {
                return;
            }
            g_smoke_done = true;
            if (!g_smoke_hook) {
                spdlog::warn("[GloveHighlight] (fix155) the start of an effect's smoke is not as in Starfield 1.16.236 (not hooked): the frost's smoke stays");
                return;
            }
            void* form = nullptr;
            if (!CallLookup(kFrozenSequenceId, form) || form == nullptr) {
                spdlog::warn("[GloveHighlight] (fix153) the frost's effect sequence {:08X} not found: its smoke stays", kFrozenSequenceId);
                return;
            }
            uintptr_t events[kSmokeEvents]{};
            const int found = FindEventsGuarded(reinterpret_cast<uintptr_t>(form), At(kParticleTrackTypeRva), At(kParticleEventTypeRva), events);
            for (int i = 0; i < std::max(found, 0); ++i) {
                g_smoke_events[i].store(events[i], std::memory_order_relaxed);
            }
            if (found > 0) {
                spdlog::info("[GloveHighlight] (fix155) the frost's dry-ice smoke is not started any more ({} smoke event{} skipped as each play starts it; "
                             "the cryo weapons' frost too): its frozen glow alone",
                    found, found == 1 ? "" : "s");
            } else {
                spdlog::warn("[GloveHighlight] (fix155) the frost's effect sequence is not as expected ({}): its smoke stays",
                    found < 0 ? "its layout or types differ" : "no smoke event found");
            }
        }


        // (fix157) The signal glow without its light (its added halo light lit the area around the
        // target, like a spotlight shining on the object). Its sequence (EFSQ 0x0028B231, CreatureFX_Lantern_SignalGlow_Seq01;
        // overridden by a modlist's plugin with the same tree): a membrane track (CreatureGlow.mat: the glow on the
        // object, self-lit, never using the light) and a LightEffectTrack (type table 0x144B5CDB8) with one LightSpawnEvent (0x144B5CEE0)
        // that makes the light CreatureFXSignalGlow_Light (LIGH 0x280DD6: intensity up to 90, radius 150) at COM (a helper's research).
        // Its start (the event's type table slot +0x30: 0x1403EFC10, `mov rcx,[rcx+10h]; jmp 0x1403F06C0`, the light made) is hooked as
        // fix155's smoke was: for that event, straight to the game's `ret` at 0x1403EFC4C (the update's last byte; the stack as at entry);
        // the update (+0x38), end (+0x40), the nested intensity/radius/colour/position tracks and the state's destructor do nothing
        // without a light. Only for the gloves' target: the play (the start's context's first word) whose +0x10 is that reference, or a
        // start within half a second of the glow put on (the light starts with the play); other users of this glow (signal creatures,
        // modded force effects) keep their light.
        constexpr uint32_t  kGlowSequenceId     = 0x0028B231;
        constexpr uintptr_t kLightTrackTypeRva  = 0x4B5CDB8;
        constexpr uintptr_t kLightEventTypeRva  = 0x4B5CEE0;
        constexpr uintptr_t kLightStartRva      = 0x3EFC10;
        constexpr uint8_t   kLightStart[]       = { 0x48, 0x8B, 0x49, 0x10, 0xE9, 0xA7, 0x0A, 0x00, 0x00 };
        constexpr uintptr_t kLightRetCheckRva   = 0x3EFC46;
        constexpr uint8_t   kLightRetAt[]       = { 0x0F, 0x85, 0x74, 0x6E, 0xF2, 0xFF, 0xC3 }; // (jne ...; ret: the ret at +6)
        constexpr uintptr_t kLightRetRva        = 0x3EFC4C;
        constexpr int64_t   kLightWindowUs      = 500'000;
        safetyhook::MidHook    g_light_hook{};
        std::atomic<uintptr_t> g_light_events[kSmokeEvents]{};
        std::atomic<uintptr_t> g_glow_ref{ 0 };       // (the reference the gloves' glow is on, 0: none)
        std::atomic<int64_t>   g_glow_applied_us{ 0 }; // (when it was put on)
        std::atomic<int>       g_light_skipped{ 0 };
        bool                   g_light_done{ false };

        int64_t SteadyUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (Under a guard.) The play's reference: the start's context (rdx) -> its play -> +0x10.
        uintptr_t PlayRefGuarded(uintptr_t context)
        {
            __try {
                const uintptr_t play = context != 0 ? *reinterpret_cast<const uintptr_t*>(context) : 0;
                return play != 0 ? *reinterpret_cast<const uintptr_t*>(play + 0x10) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // A LightSpawnEvent's start (any thread that updates effects): the glow's light not made on the gloves' target.
        void OnLightStart(safetyhook::Context& ctx)
        {
            bool ours = false;
            for (const auto& e : g_light_events) {
                if (const uintptr_t event = e.load(std::memory_order_relaxed); event != 0 && ctx.rcx == event) {
                    ours = true;
                    break;
                }
            }
            if (!ours) {
                return;
            }
            const uintptr_t target  = g_glow_ref.load(std::memory_order_relaxed);
            const uintptr_t ref     = PlayRefGuarded(ctx.rdx);
            const bool      by_ref  = target != 0 && ref == target;
            const bool      by_time = SteadyUs() - g_glow_applied_us.load(std::memory_order_relaxed) < kLightWindowUs;
            if (!by_ref && !by_time) {
                return; // (another effect's: its light kept)
            }
            ctx.rax = 0;
            ctx.rip = At(kLightRetRva);
            if (const int n = g_light_skipped.fetch_add(1, std::memory_order_relaxed) + 1; n <= 5 || n % 100 == 0) {
                spdlog::info("[GloveHighlight] (fix157) the glow's light not made ({} time(s); {}; thread {})", n,
                    by_ref ? "its play's reference is the gloves' target" : "just after the glow was put on (its play's reference not the target's)",
                    GetCurrentThreadId());
            }
        }


        // (fix158) The glow at once, on and off (the glow took a few seconds to build up and lingered after aiming or grabbing
        // ended; it should have no delay; a helper's research, checked): its sequence plays at 30 ticks a second, fading in over its
        // first 46 ticks (opacity 0 -> 1, emissive 0 -> 11; the loop is ticks 21-270, the fade out 271-301); the game's stop (0x141A953D0)
        // only marks the effect finished (its slot +0x270 sets +0x38), the effect lives on until its age (+0x1C, s) reaches the sequence's
        // length (301 ticks, about 10 s) and its removal lets a looping play (mode byte +0x14C 2) play out its loop and fade. So, for the
        // gloves' own effect and play only:
        // - the stop (hooked at 0x141A954BE, where the game calls that slot on each of the reference's effects for the shader; rsi the
        //   effect): our effect's age set past its end: removed at its next update;
        // - a play's first update (hooked at 0x14243FBFC, `mov edx,[rbx+148h]`: a start offset seeked to when not 0, as the game's random
        //   time offset is): our play (its compiled data [rbx+0x110] the glow sequence's +0x60, its reference [rbx+0x10] the target) starts
        //   at tick 1800 (full strength; fix162: 46 and 60 before, read as frames), and its mode 2 becomes 1 (loops the same; only the stop's play-out tests 2): its removal is
        //   immediate (0x14243F670 ends every event).
        constexpr uintptr_t kStopEachRva     = 0x1A954BE;
        constexpr uintptr_t kStopEachCheckRva = 0x1A954B5;
        constexpr uint8_t   kStopEach[]      = { 0x4C, 0x39, 0xA6, 0x80, 0x00, 0x00, 0x00, 0x75, 0x0E, 0x48, 0x8B, 0x06, 0xB2, 0x01, 0x48, 0x8B, 0xCE, 0xFF,
                                                 0x90, 0x70, 0x02, 0x00, 0x00 };
        constexpr uintptr_t kEffectAge       = 0x1C;
        constexpr uintptr_t kPlayStartRva    = 0x243FBFC;
        constexpr uintptr_t kPlayStartCheckRva = 0x243FBF3;
        constexpr uint8_t   kPlayStart[]     = { 0x83, 0xBB, 0x30, 0x01, 0x00, 0x00, 0x00, 0x75, 0x17, 0x8B, 0x93, 0x48, 0x01, 0x00, 0x00, 0x85, 0xD2, 0x74,
                                                 0x0D, 0x48, 0x8B, 0xCB };
        constexpr uintptr_t kPlayCompiled    = 0x110;
        constexpr uintptr_t kPlayRef         = 0x10;
        constexpr uintptr_t kPlayStartTick   = 0x148;
        constexpr uintptr_t kPlayMode        = 0x14C;
        // (fix162) The play counts 900 ticks a second (fix161's measurement: 60 ticks were 0.067 s), 30 for each of the sequence's frames:
        // its events are 0-600 (the fade-in, frames 0-20), 630-8100 (the loop, frames 21-270), then the fade-out. fix158 and fix159 started
        // at ticks 46 and 60, meant as frames: 0.07 s into the 0.67 s fade-in, the glow that took a second to build up. Frame 60
        // (tick 1800, the loop's t 0.157) is where both curves are flat and full (the research for fix159, in frames).
        constexpr uint32_t  kGlowFullTick    = 1800;
        constexpr uintptr_t kSequenceCompiled = 0x60;
        safetyhook::MidHook    g_stop_hook{};
        safetyhook::MidHook    g_play_hook{};
        std::atomic<uintptr_t> g_glow_effect{ 0 };   // (the effect our apply made, until stopped)
        std::atomic<uintptr_t> g_glow_compiled[kSmokeEvents]{}; // (the glow sequence's compiled data: its +0x60)
        std::atomic<int>       g_instant_off{ 0 }, g_instant_on{ 0 };

        // (Under a guard.) A play's compiled data and reference.
        bool PlayOfGuarded(uintptr_t play, uintptr_t& compiled, uintptr_t& ref, uint8_t& mode)
        {
            __try {
                compiled = *reinterpret_cast<const uintptr_t*>(play + kPlayCompiled);
                ref      = *reinterpret_cast<const uintptr_t*>(play + kPlayRef);
                mode     = *reinterpret_cast<const uint8_t*>(play + kPlayMode);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool WriteGuarded(uintptr_t at, const void* value, size_t n)
        {
            __try {
                std::memcpy(reinterpret_cast<void*>(at), value, n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The game's stop, on each of the reference's effects for the shader (the stopping thread: ours).
        void OnStopEach(safetyhook::Context& ctx)
        {
            const uintptr_t ours = g_glow_effect.load(std::memory_order_relaxed);
            if (ours == 0 || ctx.rsi != ours) {
                return;
            }
            const float past = 1.0e6f;
            if (WriteGuarded(ctx.rsi + kEffectAge, &past, sizeof(past))) {
                if (const int n = g_instant_off.fetch_add(1) + 1; n <= 3 || n % 100 == 0) {
                    spdlog::info("[GloveHighlight] (fix158) the glow taken off at once ({} time(s))", n);
                }
            }
        }

        // (fix162) The game's own clean-up as the gloves' glow ends (after fix161 the glove still could not grab: an item got stuck in the air
        // about 2 in off the ground; a helper's research, the exe): a play's start sets 0x10000 in its reference's loaded flags (LOADED_REF_DATA
        // +0x20, 0x1403E6444: "an effect sequence plays on it"; while set, the game's wake-up handler never puts the object in its cell's
        // moving objects, so its 3D stays while its body flies). The stop's command (0x1403E6950) ends a looping play's events through
        // 0x14243F670, which also zeroes the play's reference (+0x10) and compiled data (+0x110); it then reads that reference (0x1403E69D2)
        // to take the reference's record out and clear the mark (0x1403E6A56), finds 0 and skips both: every glow since fix151 left its
        // object marked until its 3D reloaded. Hooked where the end call returns (0x1403E69CF, `mov rax,[rdi]; mov rbx,[rax+10h]`, rdi the
        // play's slot): for one of the gloves' plays (recorded at its start: the play, its id at +0x158, its reference), its reference is
        // put back, so the game's own code takes the record out and clears the mark (unless another play still runs on it).
        constexpr uintptr_t kEndCheckRva   = 0x3E69B5;
        constexpr uint8_t   kEndAt[]       = { 0x80, 0xB9, 0x4C, 0x01, 0x00, 0x00, 0x02, 0x75, 0x0C, 0x44, 0x88, 0xB1, 0x4C, 0x01, 0x00, 0x00, 0xE9, 0x6D,
                                               0x01, 0x00, 0x00, 0xE8, 0xA1, 0x8C, 0x05, 0x02, 0x48, 0x8B, 0x07, 0x48, 0x8B, 0x58, 0x10, 0xEB, 0x07 };
        constexpr uintptr_t kEndRva        = 0x3E69CF;
        constexpr uintptr_t kClearCheckRva = 0x3E6A4B;
        constexpr uint8_t   kClearAt[]     = { 0x45, 0x33, 0xC0, 0xBA, 0x00, 0x00, 0x01, 0x00, 0x48, 0x8B, 0xCB, 0xE8, 0xC5, 0x3E, 0xF7, 0xFF };
        constexpr uintptr_t kPlayId        = 0x158;
        struct OurPlay {
            uintptr_t play{ 0 };
            uintptr_t ref{ 0 };
            uint32_t  id{ 0 };
            uintptr_t ref_table{ 0 }; // (the reference's type table and its +0x28 (what 0x1403EB770 finds its record by), as at the start:
            uint32_t  ref_key{ 0 };   // checked before it is put back, so a reference deleted meanwhile is never handed to the game; the review)
        };
        constexpr uintptr_t kRefKey        = 0x28;
        constexpr uintptr_t kRefLoadedData = 0xB8;
        constexpr uintptr_t kLoadedFlagsAt = 0x20;
        constexpr uint32_t  kLoadedEffect  = 0x10000;
        std::mutex             g_plays_mutex;
        std::array<OurPlay, 8> g_plays{};      // (the gloves' plays started lately: a stop's command can come after the next glow started)
        int                    g_plays_next{ 0 };
        std::atomic<int>       g_plays_count{ 0 };
        safetyhook::MidHook    g_end_hook{};
        std::atomic<int>       g_end_fixed{ 0 };

        bool ReadU32Guarded(uintptr_t at, uint32_t& out)
        {
            __try {
                out = *reinterpret_cast<const uint32_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void RecordPlay(uintptr_t play, uintptr_t ref)
        {
            uint32_t  id = 0, key = 0;
            uintptr_t table = 0;
            if (!g_end_hook || !ReadU32Guarded(play + kPlayId, id) || !ReadPointer(ref, table) || !ReadU32Guarded(ref + kRefKey, key)) {
                return;
            }
            std::scoped_lock _{ g_plays_mutex };
            for (auto& p : g_plays) {
                if (p.play == play) {
                    p = OurPlay{}; // (a pooled play used again)
                }
            }
            g_plays[g_plays_next] = OurPlay{ play, ref, id, table, key };
            g_plays_next          = (g_plays_next + 1) % static_cast<int>(g_plays.size());
            g_plays_count.store(1, std::memory_order_relaxed);
        }

        // A play's events just ended by its stop (the thread that runs the effect sequences' commands).
        void OnPlayEnded(safetyhook::Context& ctx)
        {
            if (g_plays_count.load(std::memory_order_relaxed) == 0) {
                return;
            }
            uintptr_t play = 0;
            if (!ReadPointer(ctx.rdi, play) || play == 0) {
                return;
            }
            std::scoped_lock _{ g_plays_mutex };
            for (auto& p : g_plays) {
                if (p.play != play) {
                    continue;
                }
                uintptr_t now = 1;
                uint32_t  id  = 0;
                const bool ours = ReadPointer(play + kPlayRef, now) && now == 0 && ReadU32Guarded(play + kPlayId, id) && id == p.id && p.ref != 0;
                // (The reference still the one marked at the start: its type table and key as then, loaded, the mark still on.)
                uintptr_t table = 0, loaded = 0;
                uint32_t  key = 0, flags = 0;
                const bool alive = ours && ReadPointer(p.ref, table) && table == p.ref_table && ReadU32Guarded(p.ref + kRefKey, key) && key == p.ref_key &&
                                   ReadPointer(p.ref + kRefLoadedData, loaded) && loaded != 0 && ReadU32Guarded(loaded + kLoadedFlagsAt, flags) &&
                                   (flags & kLoadedEffect) != 0;
                if (alive && WriteGuarded(play + kPlayRef, &p.ref, sizeof(p.ref))) {
                    if (const int n = g_end_fixed.fetch_add(1) + 1; n <= 5 || n % 100 == 0) {
                        spdlog::info("[GloveHighlight] (fix162) the glow's end: its reference put back for the game's clean-up (its \"effect playing\" "
                                     "mark cleared unless another effect still plays on it; {} time(s); thread {})",
                            n, GetCurrentThreadId());
                    }
                }
                p = OurPlay{};
                return;
            }
        }

        // A play's first update (any thread that updates effects): ours starts at full strength and loops so it can stop at once.
        void OnPlayStart(safetyhook::Context& ctx)
        {
            const uintptr_t target = g_glow_ref.load(std::memory_order_relaxed);
            if (target == 0) {
                return;
            }
            uintptr_t compiled = 0, ref = 0;
            uint8_t   mode     = 0;
            if (!PlayOfGuarded(ctx.rbx, compiled, ref, mode) || ref != target || compiled == 0) {
                return;
            }
            bool glow = false;
            for (const auto& c : g_glow_compiled) {
                glow = glow || (c.load(std::memory_order_relaxed) != 0 && c.load(std::memory_order_relaxed) == compiled);
            }
            if (!glow) {
                return;
            }
            const uint32_t tick = kGlowFullTick;
            WriteGuarded(ctx.rbx + kPlayStartTick, &tick, sizeof(tick));
            RecordPlay(ctx.rbx, ref); // (fix162: its end cleans up after it)
            if (mode == 2) {
                const uint8_t loop = 1;
                WriteGuarded(ctx.rbx + kPlayMode, &loop, sizeof(loop));
            }
            if (const int n = g_instant_on.fetch_add(1) + 1; n <= 3 || n % 100 == 0) {
                spdlog::info("[GloveHighlight] (fix158) the glow put on at full strength ({} time(s); its mode was {}; thread {})", n, mode, GetCurrentThreadId());
            }
        }

        // (fix161) What the game feeds the glow, frame by frame, measured (fix160: the glow did not change with DLSS's history
        // cleared as it first showed, so DLSS is ruled out too, as are the eye adaptation (instant in the tested INI) and bloom (no
        // history: helpers' research); left is what the effect itself does, which reading the code said starts at full). Where a
        // property event (the glow's opacity and emissive are two tracks) hands its value to the material: 0x1424C1A0D in its update
        // (0x1424C1990, the event type's slot +0x38), `mov rax,[r9]; lea r8,[rsp+30h]; mov rcx,r9; call [rax+18h]` (the curve
        // controller's evaluate: curve(t) x weight); rbx the play's context ({the play, play+0x118, the event's state, u32 ticks into
        // the event, float seconds}), rdi the event (+0x8 and +0xC its first and last tick, +0x10 its track), [rsp+0x30] t (ticks into the
        // event / its length), [rsp+0x34] the weight; rdx the bound target (not reached until the membrane exists). For the gloves' own
        // glow only, its first 1.5 s, the first glows: logged, nothing changed. (fix160 cleared DLSS's history here: taken out.)
        constexpr uintptr_t kReadyCheckRva = 0x24C1A01;
        constexpr uint8_t   kReadyAt[]     = { 0xC5, 0xFA, 0x11, 0x5C, 0x24, 0x30, 0xC5, 0xFA, 0x11, 0x54, 0x24, 0x34, 0x49, 0x8B, 0x01,
                                               0x4C, 0x8D, 0x44, 0x24, 0x30, 0x49, 0x8B, 0xC9, 0xFF, 0x50, 0x18 };
        constexpr uintptr_t kReadyRva      = 0x24C1A0D;
        constexpr int64_t   kMeasureUs     = 1'500'000; // (the first second and a half of each glow)
        constexpr int64_t   kMeasureEveryUs = 300'000;  // (every call in the first 0.3 s, then every fifth)
        constexpr int       kMeasureGlows  = 3;
        constexpr int       kMeasureLines  = 400;
        constexpr uintptr_t kPlayTick      = 0x130;
        constexpr uintptr_t kPlayFraction  = 0x134;
        safetyhook::MidHook    g_ready_hook{};
        std::atomic<uintptr_t> g_ready_play{ 0 };   // (the play of the glow put on last, 0 until its first value)
        std::atomic<int>       g_measured_glows{ 0 };
        std::atomic<int>       g_measure_lines{ 0 };
        std::atomic<int>       g_measure_calls{ 0 };

        struct Measured {
            uint32_t  first{ 0 }, last{ 0 };   // the event's ticks
            uintptr_t track{ 0 };
            uint32_t  ticks_in{ 0 };           // the context's ticks into the event
            float     seconds{ 0.0f };
            float     t{ 0.0f }, weight{ 0.0f };
            uint32_t  play_tick{ 0 };
            float     play_fraction{ 0.0f };
            uint8_t   mode{ 0 };
        };

        bool MeasureGuarded(uintptr_t event, uintptr_t context, uintptr_t stack, uintptr_t play, Measured& out)
        {
            __try {
                out.first         = *reinterpret_cast<const uint32_t*>(event + 0x8);
                out.last          = *reinterpret_cast<const uint32_t*>(event + 0xC);
                out.track         = *reinterpret_cast<const uintptr_t*>(event + 0x10);
                out.ticks_in      = *reinterpret_cast<const uint32_t*>(context + 0x18);
                out.seconds       = *reinterpret_cast<const float*>(context + 0x1C);
                out.t             = *reinterpret_cast<const float*>(stack + 0x30);
                out.weight        = *reinterpret_cast<const float*>(stack + 0x34);
                out.play_tick     = *reinterpret_cast<const uint32_t*>(play + kPlayTick);
                out.play_fraction = *reinterpret_cast<const float*>(play + kPlayFraction);
                out.mode          = *reinterpret_cast<const uint8_t*>(play + kPlayMode);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A property event handing its value to its target (any thread that updates effects).
        void OnPropertyReady(safetyhook::Context& ctx)
        {
            const uintptr_t target = g_glow_ref.load(std::memory_order_relaxed);
            if (target == 0 || g_measured_glows.load(std::memory_order_relaxed) > kMeasureGlows) { // (nothing to do once measured: the review)
                return;
            }
            uintptr_t play = 0, compiled = 0, ref = 0;
            uint8_t   mode = 0;
            if (!ReadPointer(ctx.rbx, play) || play == 0 || !PlayOfGuarded(play, compiled, ref, mode) || ref != target || compiled == 0) {
                return;
            }
            bool glow = false;
            for (const auto& c : g_glow_compiled) {
                const uintptr_t value = c.load(std::memory_order_relaxed);
                glow                  = glow || (value != 0 && value == compiled);
            }
            if (!glow) {
                return;
            }
            // (Each glow put on measured from its first value; the first kMeasureGlows of them.)
            uintptr_t armed = 0;
            if (g_ready_play.compare_exchange_strong(armed, play)) {
                if (g_measured_glows.fetch_add(1) >= kMeasureGlows) {
                    return;
                }
                g_measure_calls = 0;
            } else if (armed != play || g_measured_glows.load() > kMeasureGlows) {
                return;
            }
            const int64_t since = SteadyUs() - g_glow_applied_us.load(std::memory_order_relaxed);
            const int     call  = g_measure_calls.fetch_add(1);
            if (since > kMeasureUs || (since > kMeasureEveryUs && call % 5 != 0) || g_measure_lines.load() >= kMeasureLines) {
                return;
            }
            Measured m;
            if (!MeasureGuarded(ctx.rdi, ctx.rbx, ctx.rsp, play, m)) {
                return;
            }
            g_measure_lines.fetch_add(1);
            spdlog::info("[GloveHighlight] (fix161) glow {} at {:.0f} ms: event ticks {}-{} (track {:x}), {} ticks into it ({:.3f} s), t {:.3f}, weight {:.2f}; "
                         "the play at tick {} ({:.3f}), mode {}; thread {}",
                g_measured_glows.load(), since / 1000.0, m.first, m.last, m.track & 0xFFFFF, m.ticks_in, m.seconds, m.t, m.weight, m.play_tick,
                m.play_fraction, m.mode, GetCurrentThreadId());
        }

        // (Under a guard.) A sequence form's sequences (their objects: the map's entries, root type checked).
        int FindSequencesGuarded(uintptr_t form, uintptr_t* out)
        {
            __try {
                if (*reinterpret_cast<const uint8_t*>(form + 0x2E) != 0xC9 || *reinterpret_cast<const uintptr_t*>(form) != At(kSequenceFormTypeRva)) {
                    return -1;
                }
                const uintptr_t map = form + 0x38;
                if (*reinterpret_cast<const uintptr_t*>(map) != At(kSequenceMapTypeRva)) {
                    return -1;
                }
                const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(map + 0x18);
                const uint64_t  count   = *reinterpret_cast<const uint64_t*>(map + 0x20);
                if (entries == 0 || count == 0 || count > 64) {
                    return -1;
                }
                int found = 0;
                for (uint64_t i = 0; i < count && found < kSmokeEvents; ++i) {
                    const uintptr_t e = entries + i * 0x18;
                    if (*reinterpret_cast<const uint32_t*>(e + 0x10) == 0xFFFFFFFFu) {
                        continue;
                    }
                    const uintptr_t seq = *reinterpret_cast<const uintptr_t*>(e + 8);
                    if (seq != 0 && *reinterpret_cast<const uintptr_t*>(seq) == At(kTrackGroupTypeRva)) {
                        out[found++] = seq;
                    }
                }
                return found;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // The glow's light event found once the glow's form is (main thread).
        void DropGlowLight()
        {
            if (g_light_done) {
                return;
            }
            g_light_done = true;
            void* form   = nullptr;
            if (!CallLookup(kGlowSequenceId, form) || form == nullptr) {
                spdlog::warn("[GloveHighlight] (fix157) the glow's effect sequence {:08X} not found: its light stays", kGlowSequenceId);
                return;
            }
            // (fix158) Its sequences' compiled data, for its plays' first update (the glow at full strength at once).
            {
                uintptr_t seqs[kSmokeEvents]{};
                const int n = FindSequencesGuarded(reinterpret_cast<uintptr_t>(form), seqs);
                for (int i = 0; i < std::max(n, 0); ++i) {
                    g_glow_compiled[i].store(seqs[i] + kSequenceCompiled, std::memory_order_relaxed);
                }
                spdlog::info("[GloveHighlight] (fix158) the glow's sequence{}: {} found (on and off at once: {})", n == 1 ? "" : "s", std::max(n, 0),
                    g_stop_hook && g_play_hook && n > 0 ? "ready" : "not (the game's code or data not as expected)");
            }
            // (The light's skip apart from the on and off at once: one can be there without the other; the review.)
            if (!g_light_hook) {
                spdlog::warn("[GloveHighlight] (fix157) the start of an effect's light is not as in Starfield 1.16.236 (not hooked): the glow keeps its light");
                return;
            }
            uintptr_t events[kSmokeEvents]{};
            const int found = FindEventsGuarded(reinterpret_cast<uintptr_t>(form), At(kLightTrackTypeRva), At(kLightEventTypeRva), events);
            for (int i = 0; i < std::max(found, 0); ++i) {
                g_light_events[i].store(events[i], std::memory_order_relaxed);
            }
            if (found > 0) {
                spdlog::info("[GloveHighlight] (fix157) the glow's light is not made on the gloves' target any more ({} light event{}): the glow on the "
                             "object alone",
                    found, found == 1 ? "" : "s");
            } else {
                spdlog::warn("[GloveHighlight] (fix157) the glow's effect sequence is not as expected ({}): its light stays",
                    found < 0 ? "its layout or types differ" : "no light event found");
            }
        }

        // The frost's form, once the game's data is loaded: looked up, and its type checked.
        void* FrozenShader()
        {
            if (g_frozen_shader != nullptr || g_frozen_lookup_failed || !g_frozen_ok) {
                return g_frozen_shader;
            }
            void*     form  = nullptr;
            uintptr_t table = 0;
            if (!CallLookup(kFrozenShaderId, form) || form == nullptr) {
                return nullptr; // (not loaded yet: asked again)
            }
            if (!ReadPointer(reinterpret_cast<uintptr_t>(form), table) || table != At(kShaderTypeRva)) {
                g_frozen_lookup_failed = true;
                spdlog::warn("[GloveHighlight] (fix151) the form {:08X} is not the game's frost (its type table {:#x}): the outline is used",
                    kFrozenShaderId, table);
                return nullptr;
            }
            g_frozen_shader = form;
            if (kSkipFrostSmoke) {
                DropFrostSmoke(); // (fix153, fix155: the frost's smoke; not with fix156's signal glow)
            }
            DropGlowLight(); // (fix157: the glow's light)
            spdlog::info("[GloveHighlight] (fix151) the game's glow found (fix156: CCT_SignalGlowFXS, {:08X}): what the gloves aim at "
                         "and hold glows",
                kFrozenShaderId);
            return form;
        }

        // (Its effect back: null when the effect refused to start (then let go by the game): not put on; the review.)
        // (fix158) The effect made (0: none), kept for the stop.
        uintptr_t CallApply(uintptr_t ref, void* shader)
        {
            __try {
                return reinterpret_cast<uintptr_t>(reinterpret_cast<ApplyFn>(At(kApplyRva))(ref, shader, -1.0f, nullptr, 0, 0, 0, false));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        bool CallStop(uintptr_t ref, void* shader)
        {
            uintptr_t lists = 0;
            if (!ReadPointer(At(kProcessListsRva), lists) || lists == 0) {
                return false;
            }
            __try {
                reinterpret_cast<StopFn>(At(kStopRva))(reinterpret_cast<void*>(lists), ref, shader);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The frost off what it is on (if that is still there).
        void Unfreeze()
        {
            if (g_frozen_handle == 0 || g_frozen_shader == nullptr) {
                g_frozen_handle = 0;
                return;
            }
            const uint32_t handle = g_frozen_handle;
            g_frozen_handle       = 0;
            g_glow_ref.store(0, std::memory_order_relaxed);
            GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t) { CallStop(ref, g_frozen_shader); });
            g_glow_effect.store(0, std::memory_order_relaxed);
        }

        // (fix152) Any of the game's threads, as its scripts call it (EffectShader.Play runs on the script machine's threads): fix151 asked
        // for the main thread (its id at 0x145F4BB68), and the gloves' aim runs on another, so it was never frozen (its test: "the gloves'
        // aim is not on the game's main thread here"). Off the main thread, while the game's update runs, the effect is queued and added a
        // moment later (0x1418CE650); the target is frozen only after a tenth of a second as the target, so the stop never comes first.
        // The memory the effect is made in: the game's own for it, as the script call sets it for the call (the thread's memory context,
        // its TLS block +0x1D4, 0x5A; put back after): not set when this thread's block is not made yet.
        constexpr uint32_t kEffectMemoryContext = 0x5A;
        uint32_t* MemoryContextSlot()
        {
            __try {
                auto* const* tls = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
                uint8_t*      block = tls != nullptr ? tls[0] : nullptr;
                if (block == nullptr || block[0xBC] == 0) {
                    return nullptr;
                }
                return reinterpret_cast<uint32_t*>(block + 0x1D4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // The frost on this reference (its 3D there): true when put on.
        bool Freeze(uint32_t handle)
        {
            void* shader = FrozenShader();
            if (shader == nullptr) {
                return false;
            }
            bool applied = false;
            GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t root) {
                if (ref != 0 && root != 0) {
                    uint32_t* const context = MemoryContextSlot();
                    const uint32_t  was     = context != nullptr ? *context : 0;
                    if (context != nullptr) {
                        *context = kEffectMemoryContext;
                    }
                    g_glow_ref.store(ref, std::memory_order_relaxed); // (fix157: its light not made on it)
                    g_glow_applied_us.store(SteadyUs(), std::memory_order_relaxed);
                    g_ready_play.store(0, std::memory_order_relaxed); // (fix161: this glow measured from its first value)
                    const uintptr_t effect = CallApply(ref, shader);
                    applied                = effect != 0;
                    g_glow_effect.store(effect, std::memory_order_relaxed); // (fix158: taken off at once when stopped)
                    if (!applied) {
                        g_glow_ref.store(0, std::memory_order_relaxed);
                    }
                    if (context != nullptr) {
                        *context = was;
                    }
                }
            });
            if (applied) {
                g_frozen_handle = handle;
            }
            return applied;
        }

        bool Mark(void* manager, uintptr_t root)
        {
            Captures captures{ manager, 0, 0 };
            std::memcpy(&captures.key, static_cast<uint8_t*>(manager) + kManagerKey, sizeof(uint32_t));
            Visitor visitor{ At(kMarkVisitorRva), &captures };
            __try {
                reinterpret_cast<WalkFn>(At(kWalkRva))(&visitor, reinterpret_cast<void*>(root));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        // (fix151) The frost's code: the form lookup (as Game.GetForm jumps to it), the apply (as EffectShader.Play calls it), the stop
        // (as EffectShader.Stop jumps to it, with the process lists).
        if (g_base != 0) {
            // (fix190) The places used here without a check of their own code (the jumps and the call read, the process lists, the stop,
            // the frost's type table): known in this game version first, else the frost is off.
            const bool frozen_known =
                GameCode::Known({ kGetFormRva + 3, kPlayCallRva, kStopNativeRva + 5, kStopNativeRva + 15, kProcessListsRva, kStopRva, kShaderTypeRva });
            g_frozen_ok = frozen_known && SameCode(kLookupRva, kLookupStart, sizeof(kLookupStart)) &&
                          Target(At(kGetFormRva + 3), 5, 0xE9) == At(kLookupRva) &&
                          SameCode(kApplyRva, kApplyStart, sizeof(kApplyStart)) &&
                          Target(At(kPlayCallRva), 5, 0xE8) == At(kApplyRva) &&
                          Target(At(kStopNativeRva + 5), 7, 0x48) == At(kProcessListsRva) && Target(At(kStopNativeRva + 15), 5, 0xE9) == At(kStopRva);
            spdlog::info("[GloveHighlight] (fix151, fix156) the game's effect for the gloves' target: {}", g_frozen_ok
                ? "its effect shader's play and stop checked (form lookup rva 5def30, apply rva b790c0, stop rva 1a953d0)"
                : frozen_known ? "not as in Starfield 1.16.236 (the outline is used)"
                               : "its places not known in this game version (GameCode): off (the outline is used)");
            // (fix155) A ParticleEvent's start (its type table's slot +0x30 is this function) and the ret it is sent to; hooked now, its
            // events given once the frost is found (none until then: every start goes on as before).
            // (fix156: not with the signal glow, which has no smoke: kept for the frost, kSkipFrostSmoke.)
            // (fix190: the slot read and the ret sent to known in this game version first.)
            uintptr_t slot = 0;
            if (kSkipFrostSmoke && g_frozen_ok && SameCode(kSmokeStartRva, kSmokeStart, sizeof(kSmokeStart)) &&
                SameCode(kSmokeRetCheckRva, kSmokeRetAt, sizeof(kSmokeRetAt)) && kSmokeRetCheckRva + 5 == kSmokeRetRva &&
                GameCode::Known({ kParticleEventTypeRva, kSmokeRetRva }) &&
                ReadPointer(At(kParticleEventTypeRva) + kEventStartSlot, slot) && slot == At(kSmokeStartRva)) { // (fix190: the table, then its slot)
                g_smoke_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kSmokeStartRva)), &OnSmokeStart);
            }
            // (fix158) The glow on and off at once: the stop's call on each effect, and a play's first update (both as in 1.16.236).
            // (fix190: each hook's own place known in this game version too.)
            if (g_frozen_ok && SameCode(kStopEachCheckRva, kStopEach, sizeof(kStopEach)) && kStopEachCheckRva + 9 == kStopEachRva &&
                Target(At(kStopNativeRva + 15), 5, 0xE9) == At(kStopRva) && At(kStopEachRva) != 0) {
                g_stop_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kStopEachRva)), &OnStopEach);
            }
            if (g_frozen_ok && SameCode(kPlayStartCheckRva, kPlayStart, sizeof(kPlayStart)) && kPlayStartCheckRva + 9 == kPlayStartRva &&
                At(kPlayStartRva) != 0) {
                g_play_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kPlayStartRva)), &OnPlayStart);
            }
            // (fix162) Where a stopped play's events have just ended: the game's clean-up after the gloves' glow (only with the play's start hooked).
            if (g_play_hook && SameCode(kEndCheckRva, kEndAt, sizeof(kEndAt)) && kEndCheckRva + 26 == kEndRva &&
                SameCode(kClearCheckRva, kClearAt, sizeof(kClearAt)) && At(kEndRva) != 0) {
                g_end_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kEndRva)), &OnPlayEnded);
            }
            spdlog::info("[GloveHighlight] (fix162) the end of a stopped play (rva {:x}): {}", kEndRva,
                g_end_hook ? "hooked (the game's clean-up after the gloves' glow: the object moves with its body again)"
                           : "not as in Starfield 1.16.236 (glowed objects stay marked: pulls may not move them)");
            spdlog::info("[GloveHighlight] (fix158) the glow on and off at once: the stop {} (rva {:x}), a play's start {} (rva {:x})",
                g_stop_hook ? "hooked" : "not as in 1.16.236", kStopEachRva, g_play_hook ? "hooked" : "not as in 1.16.236", kPlayStartRva);
            // (fix161) Where a property event hands its value to its target: what the game feeds the glow logged for its first 1.5 s.
            if (g_frozen_ok && SameCode(kReadyCheckRva, kReadyAt, sizeof(kReadyAt)) && kReadyCheckRva + 12 == kReadyRva && At(kReadyRva) != 0) {
                g_ready_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kReadyRva)), &OnPropertyReady);
            }
            spdlog::info("[GloveHighlight] (fix161) a property event's value handed over (rva {:x}): {}", kReadyRva,
                g_ready_hook ? "hooked (what the game feeds the gloves' glow logged for its first 1.5 s)" : "not as in Starfield 1.16.236 (not measured)");
            // (fix157) A LightSpawnEvent's start (its type table's slot +0x30 is this function) and the ret it is sent to; its events given
            // once the glow is found (none until then: every start goes on as before). (fix190: the slot read and the ret sent to known in
            // this game version first.)
            uintptr_t light_slot = 0;
            if (g_frozen_ok && SameCode(kLightStartRva, kLightStart, sizeof(kLightStart)) &&
                SameCode(kLightRetCheckRva, kLightRetAt, sizeof(kLightRetAt)) && kLightRetCheckRva + 6 == kLightRetRva &&
                GameCode::Known({ kLightEventTypeRva, kLightRetRva }) &&
                ReadPointer(At(kLightEventTypeRva) + kEventStartSlot, light_slot) && light_slot == At(kLightStartRva)) { // (fix190: the table, then its slot)
                g_light_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kLightStartRva)), &OnLightStart);
            }
            spdlog::info("[GloveHighlight] (fix157) the start of an effect's light (a LightSpawnEvent's, rva {:x}): {}", kLightStartRva,
                g_light_hook ? "hooked (the glow's light is not made on the gloves' target once the glow is found)"
                             : "not as in Starfield 1.16.236 (the glow keeps its light)");
            if (kSkipFrostSmoke) {
                spdlog::info("[GloveHighlight] (fix155) the start of an effect's smoke (a ParticleEvent's, rva {:x}): {}", kSmokeStartRva,
                    g_smoke_hook ? "hooked (the frost's smoke is skipped once the frost is found)" : "not as in Starfield 1.16.236 (the frost keeps its smoke)");
            }
        }
        std::string why{};
        if (g_base == 0 || !SameCode(kMakeRva, kMakeStart, sizeof(kMakeStart))) {
            why = "the highlight's making is not where it was";
        } else if (!GameCode::Known({ kMakeRva + 0x1C, kMakeRva + 0xDD, kMakeRva + 0xF1, kManagerTypeRva, kKeyRva, kKeyRva + 0x1B, kStyleRva, kHighlightsRva,
                       kScannerMarkRva, kScannerMarkRva + 0x1E, kMarkVisitorRva, kWalkRva })) {
            // (fix190) The places read, called or used below without a check of their own code.
            why = "the highlight's places are not known in this game version (GameCode)";
        } else if (Target(At(kMakeRva + 0x1C), 7, 0x48) != At(kManagerTypeRva)) {
            why = "the highlight's making does not set its type as expected";
        } else if (Target(At(kMakeRva + 0xDD), 5, 0xE8) != At(kKeyRva) || Target(At(kMakeRva + 0xF1), 5, 0xE8) != At(kStyleRva)) {
            why = "the highlight's making does not call its key and style as expected";
        } else if (Target(At(kKeyRva + 0x1B), 7, 0x48) != At(kHighlightsRva)) {
            why = "the renderer's highlights are not where they were";
        } else if (!SameCode(kClearRva, kClearStart, sizeof(kClearStart))) {
            why = "the highlight's clearing is not where it was";
        } else if (Target(At(kScannerMarkRva), 7, 0x48) != At(kMarkVisitorRva) || Target(At(kScannerMarkRva + 0x1E), 5, 0xE8) != At(kWalkRva)) {
            why = "the hand scanner does not mark objects as expected";
        }
        if (!why.empty()) {
            SetStatus("Gloves' highlight: off (" + why + ")");
            spdlog::warn("[GloveHighlight] off: {} (not Starfield 1.16.236, or another mod changed it): the gloves' ring and glow pictures are used", why);
            return;
        }
        g_installed = true;
        SetStatus("Gloves' highlight: checked (made when first needed)");
        spdlog::info("[GloveHighlight] the game's highlight checked (made rva {:x}, cleared rva {:x}, marked as the hand scanner does at rva {:x})",
            kMakeRva, kClearRva, kScannerMarkRva);
    }

    void Show(int kind, uint32_t handle)
    {
        const auto& settings = GameFlow::gStore.internalSettings;
        if (g_installed && std::abs(settings.gloveHighlightFill - g_fill_applied) > 0.005f) {
            Restyle(settings.gloveHighlightFill);
        }
        // (fix151) The frost instead of the outline (the setting, the game's code checked, its form found): on the target while aimed at
        // and while held, off when it changes; put on again a quarter second on when it could not be (its 3D not there yet).
        const bool want_frost = g_frozen_ok && settings.gloveMarker && settings.gloveHighlight && settings.gloveFrozen &&
                                FrozenShader() != nullptr;
        g_frost_mode = want_frost;
        if (want_frost || g_frozen_handle != 0) {
            const uint32_t target = want_frost && kind >= 1 && kind <= 2 ? handle : 0;
            if (want_frost && g_showing && g_frozen_handle == 0) {
                ClearShown(); // (an outline left from before the setting changed)
            }
            if (target != g_frozen_handle) {
                Unfreeze();
                g_showing = false;
            }
            if (!want_frost) {
                g_showing = false; // (the setting off: the outline from now on, below)
            } else {
                // (A new target frozen once it has stayed the target a tenth of a second: an aim flicking between two objects does not start and
                // stop the effect, its particles and fade, every frame; the review.)
                static uint32_t                              picked{ 0 };
                static std::chrono::steady_clock::time_point picked_at{};
                const auto                                   now = std::chrono::steady_clock::now();
                if (target != picked) {
                    picked    = target;
                    picked_at = now;
                }
                // (fix159: at once when locked on (kind 2), 40 ms while only aimed: it took a little too long to start; the tenth of a
                // second here was most of the time from the grip to the glow, the effect itself starting at full strength since fix158.)
                const auto hold = kind == 2 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(40);
                if (target != 0 && g_frozen_handle == 0 && now - picked_at >= hold) {
                    static std::chrono::steady_clock::time_point tried_at{};
                    static uint32_t                              tried{ 0 };
                    if (target != tried || now - tried_at > std::chrono::milliseconds(250)) {
                        tried    = target;
                        tried_at = now;
                        const bool frozen = Freeze(target);
                        if (g_frozen_logs < 20) {
                            ++g_frozen_logs;
                            spdlog::info("[GloveHighlight] (fix151) {} {:#x}: {}", kind == 1 ? "aimed at" : "locked on", target,
                                frozen ? "frozen (the game's frost on it)" : "not frozen yet (its 3D not there: tried again)");
                        }
                    }
                }
                g_showing = g_frozen_handle != 0;
                g_kind    = kind;
                g_handle  = handle;
                return;
            }
        }
        if (!g_installed || !settings.gloveMarker || !settings.gloveHighlight || kind < 1 || kind > 2 || handle == 0) {
            kind   = 0;
            handle = 0;
        }
        // (Marked once: the object keeps it until cleared. Marked again a second on (something else may have taken it off: the
        // hand scanner's unmarking clears any), or a quarter second on when nothing could be marked (its 3D not there yet).)
        const auto now = std::chrono::steady_clock::now();
        const bool same = kind == g_kind && handle == g_handle;
        if (same && (kind == 0 || now - g_tried_at < (g_showing ? std::chrono::milliseconds(1000) : std::chrono::milliseconds(250)))) {
            return;
        }
        const bool was_showing = g_showing;
        ClearShown();
        g_kind     = kind;
        g_handle   = handle;
        g_tried_at = now;
        if (kind == 0 || !MakeManagers()) {
            return;
        }
        void* manager = g_managers[kind - 1];
        bool  marked  = false;
        GrenadeHand::WithRoot(handle, [&](uintptr_t root) { marked = Mark(manager, root); });
        g_showing = marked && AnyMarked(manager);
        marked    = g_showing;
        if (same && was_showing == g_showing) {
            return; // (a refresh: not logged)
        }
        if (g_logs < 20) {
            ++g_logs;
            spdlog::info("[GloveHighlight] {} {:#x}: {}", kind == 1 ? "aimed at" : "locked on", handle,
                marked ? (kind == 1 ? "highlighted blue" : "highlighted white") : "not highlighted (its 3D not found, or not known to the renderer yet: tried again)");
        }
    }

    // (fix153) With the frost, no ring or white glow at all, aimed at or locked on, frozen yet or not (after fix152: the
    // white glow removed completely; fix151 kept the locked-on ring to tell the lock).
    bool Showing() { return g_frost_mode ? g_kind != 0 : g_showing; }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return g_status;
    }
}
