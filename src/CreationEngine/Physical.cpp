#include "Physical.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <mods/VR.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include <CreationEngine/memory/ScanHelper.h>
#include <ModSettings.h>

#include "CreationEngineSingletonManager.h"
#include "GrenadeHand.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "LeftWeapon.h"
#include "MeleeThrow.h"
#include "RE/N/NiCamera.h"
#include "WorkbenchCamera.h"
#include "body/BodyIK.h"
#include "body/BodyMath.h"
#include "body/SceneGraph.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace Physical
{
    namespace
    {
        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Reads of game objects go through ReadProcessMemory on our own process: a bad address fails instead of crashing the game.
        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }
        template <class T>
        bool Read(uintptr_t at, T& out)
        {
            return SafeRead(&out, at, sizeof(T));
        }

        // Many physics records a frame (the probe): straight from memory, a record that is not there any more caught here (no system
        // call per read, as SafeRead makes: they are thousands a pass).
        bool GuardedCopy(void* dst, uintptr_t src, size_t n)
        {
            if (src == 0) {
                return false;
            }
            __try {
                std::memcpy(dst, reinterpret_cast<const void*>(src), n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        uintptr_t Base() { return reinterpret_cast<uintptr_t>(MemoryScan::mod); }
        bool      InImage(uintptr_t a) { return a >= Base() && a < Base() + MemoryScan::mod_size; }

        bool Same(uintptr_t at, const uint8_t* bytes, size_t n)
        {
            if (at < Base() || at + n > Base() + MemoryScan::mod_size) {
                return false;
            }
            return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
        }

        // The target of a rip-relative operand (its rel32 `disp_at` bytes into an instruction of `length` bytes at `at`).
        uintptr_t RipTarget(uintptr_t at, size_t disp_at, size_t length)
        {
            if (at < Base() || at + length > Base() + MemoryScan::mod_size) {
                return 0;
            }
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(at + disp_at), sizeof(rel));
            return at + length + rel;
        }

        uintptr_t CallTarget(uintptr_t at)
        {
            static constexpr uint8_t kCall[] = { 0xE8 };
            return Same(at, kCall, 1) ? RipTarget(at, 1, 5) : 0;
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there (inside the
        // game's image, as Same checks), and the address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        bool Tell(std::atomic<int>& n, int cap) { return n.fetch_add(1) < cap; }

        // A node with children (a NiNode), read under a guard (as HandGrab walks a 3D).
        bool IsNodeGuarded(uintptr_t object)
        {
            __try {
                return object != 0 && utility::rtti::derives_from(reinterpret_cast<const void*>(object), "class NiNode");
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ---- What the game keeps of a person (Starfield 1.16.236; each read checked against the game's own code) ----
        // A reference: its form type (+0x2E: 0x4B an actor), its flags (+0x20: deleted 1<<5, disabled 1<<11), its form id (+0x28), its
        // handle (+0x24, as the game's handle table and every script job take it), its place (+0x8C, x y z; PushActorAway's native reads
        // the pusher's there), its 3D (+0xB8 -> +0x08) and that one's world bound (+0x100: centre, radius).
        constexpr uintptr_t kRefFormType = 0x2E;
        constexpr uint8_t   kActorType   = 0x4B;
        constexpr uintptr_t kRefFlags    = 0x20;
        constexpr uintptr_t kRefHandle   = 0x24;
        constexpr uintptr_t kRefFormId   = 0x28;
        constexpr uintptr_t kRefPlace    = 0x8C;
        constexpr uintptr_t kRefLoaded   = 0xB8;
        constexpr uintptr_t kLoaded3D    = 0x08;
        constexpr uintptr_t kNodeBound   = 0x100;
        constexpr uintptr_t kNodeCollision = 0x110;
        // An actor's states (+0xF8): its life state in bits 17-20 (Actor::IsDead 0x1418E6D50: 1 dying, 2 dead, 5 recycled are dead; 3
        // unconscious, 6 restrained (the stagger function refuses it), 7 essential down, 8 bleeding out: the IsBleedingOut native), its
        // knock state in bits 21-24 (the knock 0x141A051F0 sets 2 and refuses while it is 2: 0 normal, 2 knocked (its lead-in); the
        // others (1, 3, 4 down; 5, 6 getting up) inferred from Skyrim's numbering: logged as they are seen). +0xFC & 0x1800: in
        // furniture (sitting, lying, getting in or out: the sit state's own test, ActorState 0x1418EEC20). +0x208: bit 3 in a scene (the
        // IsInScene native), bit 26 your teammate (IsPlayerTeammate). +0x378 bit 18 essential (IsEssential); +0x37A bit 0 commanded
        // (IsCommandedActor); +0x4B0 bit 2 a child (IsChild, Actor slot 0x4F8 0x141960150). +0x228 its AI process, +0x08 that one's
        // middle-high data, +0x4A8 there how long it stays down (the knock writes force x fMagnitudeToIdleTimeScaler there).
        constexpr uintptr_t kActorState   = 0xF8;
        constexpr uintptr_t kActorState2  = 0xFC;
        constexpr uintptr_t kActorBits    = 0x208;
        constexpr uintptr_t kActorBools   = 0x378;
        constexpr uintptr_t kActorCommand = 0x37A;
        constexpr uintptr_t kActorRace    = 0x4B0;
        constexpr uintptr_t kActorProcess = 0x228;
        constexpr uintptr_t kProcessMiddle = 0x08;
        constexpr uintptr_t kProcessGone  = 0xE7; // (PushActorAway's native refuses a process with this byte set)
        constexpr uintptr_t kMiddleDownTime = 0x4A8;

        struct Person
        {
            bool      actor{ false };
            bool      player{ false };
            uint32_t  id{ 0 };
            uint32_t  handle{ 0 };
            uint32_t  life{ 0 };
            uint32_t  knock{ 0 };
            bool      furniture{ false };
            uint32_t  seat{ 0 }; // (fix176) the furniture state: 0 none, 1 getting in, 2 sitting or lying, 3 getting out
            bool      scene{ false };      // (fix178) in a scene that protects them (not a random chat between people)
            bool      chat_scene{ false }; // (fix178) in a random chat between people (not protecting them)
            bool      teammate{ false };
            bool      essential{ false };
            bool      commanded{ false };
            bool      child{ false };
            bool      gone{ false }; // deleted or disabled
            glm::vec3 place{ 0.0f };
            uintptr_t root{ 0 };
            glm::vec3 centre{ 0.0f };
            float     radius{ 0.0f };
            bool      have_bound{ false };
        };

        // (fix178) The scene someone is in (after fix177 other NPCs could no longer be grabbed once one had been grabbed out of a
        // chair: the log, two people near each other refused "in a scene" 6 s after the pull-up; a helper's research: once free, the game's
        // social AI starts a random NPC-to-NPC chat, a quest started by the story event "ADIA" (Actor Dialogue Event) that casts both in one
        // scene). Actor::GetCurrentScene (Actor vtable +0x3D0, 0x14194CA00; null unless +0x208 bit 3). The scene (BGSScene): +0x28 its form
        // id; +0x50 bit 0 playing; +0xA8 its quest; +0xB0 the scene it is made from; +0xB8 its flags (bit 3 Interruptable, bit 4 Player
        // Dialogue, the game reading bit 4 at the end of the +0xB0 chain, 0x1409BA1A0). The quest: +0x130 the event that started it (4
        // letters), +0x114 its flags, +0x118 its priority. Both name themselves with vfunc +0x268 (scene 0x1402D2330, quest 0x1402B4EC0).
        // The game itself lets another scene take someone only out of a playing, Interruptable, not Player Dialogue scene (0x140CB93D3).
        constexpr uintptr_t kGetSceneSlot    = 0x3D0;
        constexpr uintptr_t kGetSceneRva     = 0x194CA00;
        constexpr uintptr_t kSceneEditorRva  = 0x2D2330;
        constexpr uintptr_t kQuestEditorRva  = 0x2B4EC0;
        constexpr uintptr_t kEditorIdSlot    = 0x268;
        constexpr uintptr_t kSceneFormId     = 0x28;
        constexpr uintptr_t kSceneRun        = 0x50;
        constexpr uintptr_t kSceneQuest      = 0xA8;
        constexpr uintptr_t kSceneTemplate   = 0xB0;
        constexpr uintptr_t kSceneFlags      = 0xB8;
        constexpr uintptr_t kQuestFormId     = 0x28;
        constexpr uintptr_t kQuestFlags      = 0x114;
        constexpr uintptr_t kQuestPriority   = 0x118;
        constexpr uintptr_t kQuestEvent      = 0x130;

        struct SceneFacts
        {
            uintptr_t scene{ 0 }, quest{ 0 };
            uint32_t  scene_id{ 0 }, quest_id{ 0 }, flags{ 0 }, root_flags{ 0 }, run{ 0 }, quest_flags{ 0 };
            uint8_t   priority{ 0 };
            char      event[5]{};
            bool      playing{ false }, interruptable{ false }, player_dialogue{ false }, ambient{ false };
        };

        uintptr_t CallGetter(uintptr_t fn, uintptr_t self)
        {
            __try {
                return reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(self);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // The game's own function of that object's vtable slot, if it is the one checked for 1.16.236 (else 0). (fix190) `want`: that
        // function's place in this build's game version (At(its rva): 0 when not known there, and then nothing is called).
        uintptr_t SlotIs(uintptr_t object, uintptr_t slot, uintptr_t want)
        {
            uintptr_t vtable = 0, fn = 0;
            if (object == 0 || want == 0 || !Read(object, vtable) || !InImage(vtable) || !Read(vtable + slot, fn) || fn != want) {
                return 0;
            }
            return fn;
        }

        bool ReadScene(uintptr_t actor, SceneFacts& out)
        {
            out             = SceneFacts{};
            const uintptr_t get = SlotIs(actor, kGetSceneSlot, At(kGetSceneRva));
            // (The review: the getter takes the actor's extra-data list's read lock (its list at +0xC8): only called with that list there,
            // so it cannot fault while holding it.)
            uintptr_t extra = 0, extra_first = 0;
            if (get == 0 || !Read(actor + 0xC8, extra) || extra == 0 || !Read(extra, extra_first) || (out.scene = CallGetter(get, actor)) == 0) {
                return false;
            }
            Read(out.scene + kSceneFormId, out.scene_id);
            Read(out.scene + kSceneRun, out.run);
            Read(out.scene + kSceneFlags, out.flags);
            out.root_flags = out.flags;
            uintptr_t from = out.scene;
            for (int i = 0; i < 8; ++i) {
                uintptr_t made_from = 0;
                if (!Read(from + kSceneTemplate, made_from) || made_from == 0 || made_from == from) {
                    break;
                }
                from = made_from;
                Read(from + kSceneFlags, out.root_flags);
            }
            if (Read(out.scene + kSceneQuest, out.quest) && out.quest != 0) {
                Read(out.quest + kQuestFormId, out.quest_id);
                Read(out.quest + kQuestFlags, out.quest_flags);
                Read(out.quest + kQuestPriority, out.priority);
                SafeRead(out.event, out.quest + kQuestEvent, 4);
                for (int i = 0; i < 4; ++i) {
                    out.event[i] = out.event[i] >= 32 && out.event[i] < 127 ? out.event[i] : '?';
                }
            }
            out.playing         = (out.run & 1u) != 0;
            out.interruptable   = (out.flags & (1u << 3)) != 0;
            out.player_dialogue = (out.root_flags & (1u << 4)) != 0;
            out.ambient         = out.playing && out.interruptable && !out.player_dialogue && std::memcmp(out.event, "ADIA", 4) == 0;
            return true;
        }

        const char* EditorId(uintptr_t form, uintptr_t want)
        {
            const uintptr_t fn = SlotIs(form, kEditorIdSlot, want);
            const uintptr_t s  = fn != 0 ? CallGetter(fn, form) : 0;
            return s != 0 ? reinterpret_cast<const char*>(s) : "";
        }

        // Once per person and scene (at most 40 lines): which scene, and whether it is a chat the hold may break into.
        void TellScene(uint32_t person, const SceneFacts& s)
        {
            static std::mutex                                 mutex;
            static std::vector<std::pair<uint32_t, uint32_t>> told;
            {
                std::scoped_lock lock{ mutex };
                if (told.size() >= 40 || std::find(told.begin(), told.end(), std::make_pair(person, s.scene_id)) != told.end()) {
                    return;
                }
                told.emplace_back(person, s.scene_id);
            }
            char scene_name[64]{}, quest_name[64]{};
            SafeRead(scene_name, reinterpret_cast<uintptr_t>(EditorId(s.scene, At(kSceneEditorRva))), sizeof(scene_name) - 1);
            if (s.quest != 0) {
                SafeRead(quest_name, reinterpret_cast<uintptr_t>(EditorId(s.quest, At(kQuestEditorRva))), sizeof(quest_name) - 1);
            }
            spdlog::info("[Physical] (fix178) {:08X} is in scene {:08X} '{}' (flags {:x}, from its template {:x}; {}{}{}) of quest {:08X} '{}' (started by "
                         "'{}', flags {:x}, priority {}): {}",
                person, s.scene_id, scene_name, s.flags, s.root_flags, s.playing ? "playing" : "not playing", s.interruptable ? ", interruptable" : "",
                s.player_dialogue ? ", player dialogue" : "", s.quest_id, quest_name, s.event, s.quest_flags, s.priority,
                s.ambient ? "a random chat between people: they may be held (the chat left to the game)" : "protected (held, shoved or knocked: no)");
        }

        // (fix178) `read_scene` false: from inside the game's own code (a hook): its scene not read (the getter takes a lock of the game's
        // in the middle of its code; the review), so anyone in a scene counts as protected.
        bool ReadPerson(uintptr_t ref, Person& out, bool read_scene = true)
        {
            out = Person{};
            uint8_t  type = 0;
            uint32_t flags = 0, state = 0, state2 = 0, bits = 0, bools = 0;
            uint8_t  command = 0, race = 0;
            float    place[3]{};
            if (ref == 0 || !Read(ref + kRefFormType, type) || type != kActorType || !Read(ref + kRefFlags, flags) || !Read(ref + kActorState, state) ||
                !Read(ref + kActorState2, state2) || !Read(ref + kActorBits, bits) || !Read(ref + kActorBools, bools) || !Read(ref + kActorCommand, command) ||
                !Read(ref + kActorRace, race) || !SafeRead(place, ref + kRefPlace, sizeof(place))) {
                return false;
            }
            auto* player   = CreationEngineSingletonManager::GetPlayerRef();
            out.actor      = true;
            out.player     = player != nullptr && ref == reinterpret_cast<uintptr_t>(player);
            Read(ref + kRefFormId, out.id);
            Read(ref + kRefHandle, out.handle);
            out.life       = (state >> 17) & 0xF;
            out.knock      = (state >> 21) & 0xF;
            out.furniture  = (state2 & 0x1800u) != 0;
            out.seat       = (state2 >> 11) & 3u;
            out.scene      = (bits >> 3) & 1u;
            // (fix178) A random chat between people (see ReadScene) is not a scene that protects them.
            if (out.scene && !out.player && read_scene) {
                SceneFacts s;
                if (ReadScene(ref, s)) {
                    uint32_t who = 0;
                    Read(ref + kRefFormId, who);
                    TellScene(who, s);
                    if (s.ambient) {
                        out.scene      = false;
                        out.chat_scene = true;
                    }
                }
            }
            out.teammate   = (bits >> 26) & 1u;
            out.essential  = (bools >> 18) & 1u;
            out.commanded  = (command & 1u) != 0;
            out.child      = (race & 4u) != 0;
            out.gone       = (flags & ((1u << 5) | (1u << 11))) != 0;
            out.place      = glm::vec3{ place[0], place[1], place[2] };
            uintptr_t loaded = 0;
            float     bound[4]{};
            if (Read(ref + kRefLoaded, loaded) && loaded != 0 && Read(loaded + kLoaded3D, out.root) && out.root != 0 && SafeRead(bound, out.root + kNodeBound, sizeof(bound)) &&
                std::isfinite(bound[0]) && std::isfinite(bound[1]) && std::isfinite(bound[2]) && bound[3] > 0.05f && bound[3] < 20.0f) {
                out.centre     = glm::vec3{ bound[0], bound[1], bound[2] };
                out.radius     = bound[3];
                out.have_bound = true;
            }
            return Finite(out.place);
        }

        bool Dead(const Person& p) { return p.life == 1 || p.life == 2 || p.life == 5; }
        // Down on the ground by a knock (alive): the knock's own value and the ones after it (see above).
        bool KnockedDown(const Person& p) { return p.knock >= 1 && p.knock <= 4; }
        // Ragdolled and lying (not the knock's lead-in, 2, when its ragdoll may not be in the world yet).
        bool LyingDown(const Person& p) { return p.knock == 1 || p.knock == 3 || p.knock == 4; }
        // People never staggered, knocked down or made angry (a light bump only): broken quests and stuck people otherwise.
        const char* Protected(const Person& p)
        {
            if (p.player) return "you";
            if (p.gone) return "disabled";
            if (Dead(p)) return "dead";
            if (p.life != 0) return p.life == 3 ? "unconscious" : p.life == 6 ? "restrained" : p.life == 7 ? "down (essential)" : p.life == 8 ? "bleeding out" : "not in a normal state";
            if (p.teammate) return "your follower";
            if (p.commanded) return "commanded by you";
            if (p.child) return "a child";
            if (p.scene) return "in a scene";
            if (p.furniture) return "in furniture (sitting or lying)";
            return nullptr;
        }
        // Too big to push around (a large creature: its bound's radius).
        bool Big(const Person& p) { return p.have_bound && p.radius > 1.6f; }

        // ---- The game's functions (found and checked at Install) ----
        // PushActorAway's code (the script native 0x142080C20 calls it at +0x171 with the task queue [0x145E794E8] it loads at +0x16A):
        // 0x1418C19A0(task queue, actor, x, y, z, force): off the main thread it queues itself; on it, it calls the knock 0x141A051F0
        // (at 0x1418C1B3E) with the actor's process and the point the push comes from. Force 0 knocks down in place.
        constexpr uintptr_t kPushNativeRva   = 0x2080c20;
        constexpr uintptr_t kPushQueueLoad   = 0x16a; // mov rcx,[rip+X]
        constexpr uintptr_t kPushQueueCall   = 0x171; // call 0x1418C19A0
        constexpr uintptr_t kKnockQueueRva   = 0x18c19a0;
        constexpr uint8_t   kKnockQueueStart[] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0xD1, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89,
              0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kKnockCallInQueue = 0x19e; // call 0x141A051F0
        constexpr uintptr_t kKnockRva        = 0x1a051f0;
        // The knock's time down: 0x141A05300 vmulss xmm1,xmm7,[fMagnitudeToIdleTimeScaler] ; mov rax,[rsi+8] ; vmovss [rax+0x4A8],xmm1.
        constexpr uintptr_t kKnockDownTimeAt = 0x1a05300;
        constexpr uint8_t   kKnockDownTimeHead[] = { 0xC5, 0xC2, 0x59, 0x0D };
        constexpr uint8_t   kKnockDownTimeTail[] = { 0x48, 0x8B, 0x46, 0x08, 0xC5, 0xFA, 0x11, 0x88, 0xA8, 0x04, 0x00, 0x00 };
        constexpr uintptr_t kIdleScalerRva   = 0x5eff4d8; // (its value: Setting fMagnitudeToIdleTimeScaler at 0x145EFF4D0, +8)
        // The game's stagger (0x1419038A0: actor, magnitude (an int: the hit's stagger level), the action to perform, the hit or null), as
        // the melee hit's pipeline calls it (0x141983D89: the default objects' +0x238, "Action Stagger Start") and the DamageValue
        // native with no hit (0x14206E74A: magnitude 4, "Action Limb Critical").
        constexpr uintptr_t kStaggerRva      = 0x19038a0;
        constexpr uint8_t   kStaggerStart[]  = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54,
             0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kStaggerNoHitAt  = 0x206e74a; // call GetSingleton ; xor r9d,r9d ; mov r8,[rax+0x4F8] ; lea edx,[r9+4] ; mov rcx,rsi ; call stagger
        constexpr uint8_t   kStaggerNoHitMid[] = { 0x45, 0x33, 0xC9, 0x4C, 0x8B, 0x80, 0xF8, 0x04, 0x00, 0x00, 0x41, 0x8D, 0x51, 0x04, 0x48, 0x8B, 0xCE };
        constexpr uintptr_t kStaggerHitAt    = 0x1983d89; // call GetSingleton ; mov r8,[rax+0x238]
        constexpr uint8_t   kStaggerHitMid[] = { 0x4C, 0x8B, 0x80, 0x38, 0x02, 0x00, 0x00 };
        // BGSDefaultObjectManager::GetSingleton (0x1404F0F20; the manager is a static at 0x1461E64E0, its objects from +0x38: 364 of them,
        // its constructor 0x140BCF900 and size 0xB98 agree) and the default objects' table (name, form type, ...: 0x20 bytes each).
        constexpr uintptr_t kDefaultsRva     = 0x4f0f20;
        constexpr uint8_t   kDefaultsStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
        constexpr uintptr_t kDefaultObjects  = 0x38;
        constexpr uintptr_t kDefaultTableRva = 0x58d7ab0;
        constexpr int       kActionStaggerStart = 64, kActionRecoil = 67, kActionLargeRecoil = 68, kActionBumpedInto = 84;
        constexpr uint8_t   kFormTypeAction  = 7;
        // Performing an action, as the game does it in about 150 places (KnockdownHandler 0x141AEEE41..0x141AEEEE8, the stagger
        // function 0x141903A91..0x141903B42): its action data built on the stack (0x68 bytes: +0x00 type table 0x144AFEE18, +0x08 the
        // actor doing it, +0x10 a target, both held, +0x18 the action, +0x28 a second type table 0x144AFEE08, +0x30 and +0x38 the empty
        // string with its count raised, the rest 0), then the function the global 0x145FB7BF0 points to, called with it (true: done);
        // after it the two strings let go (0x1428CEF30) and the actors (0x140384BD0).
        constexpr uintptr_t kPerformAt       = 0x1aeeebf;
        constexpr uint8_t   kPerform[]       = { 0x48, 0x8D, 0x05, 0x52, 0xFF, 0x00, 0x03, 0x48, 0x89, 0x45, 0xB7, 0x48, 0x8D, 0x05, 0x37, 0xFF, 0x00, 0x03,
                  0x48, 0x89, 0x45, 0xDF, 0x89, 0x7D, 0x17, 0x48, 0x8B, 0x05, 0x11, 0x8D, 0x4C, 0x04, 0x48, 0x85, 0xC0, 0x74, 0x07, 0x48, 0x8D,
                  0x4D, 0xB7, 0xFF, 0xD0 };
        constexpr uintptr_t kEmptyStringCall = 0x1aeee7e;
        constexpr uintptr_t kReleaseStringCall = 0x1aeeeef;
        constexpr uintptr_t kReleaseRefCall  = 0x1aeef10;
        // (fix190) Where those go, as found there in 1.16.236 (each one checked at Install): the two type tables, the performer's global,
        // the empty string, the strings' let-go and the actors'.
        constexpr uintptr_t kActionTableRva   = 0x4afee18;
        constexpr uintptr_t kActionTable2Rva  = 0x4afee08;
        constexpr uintptr_t kPerformerRva     = 0x5fb7bf0;
        constexpr uintptr_t kEmptyStringRva   = 0x2b8150;
        constexpr uintptr_t kReleaseStringRva = 0x28cef30;
        constexpr uintptr_t kReleaseRefRva    = 0x384bd0;
        // Script SendAssaultAlarm's native (0x141FCCDE0, the actor in r8; the VM's own arguments not used): its code compared whole.
        constexpr uintptr_t kAssaultRva      = 0x1fccde0;
        constexpr uint8_t   kAssault[]       = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xC8, 0x49, 0x8B, 0xD8, 0xE8, 0xCF, 0x94, 0x94, 0xFF, 0x84,
                  0xC0, 0x75, 0x12, 0x48, 0x8B, 0x15, 0xD4, 0xE1, 0xF7, 0x03, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xCB, 0xE8, 0x69, 0x91, 0x96, 0xFF, 0x8B,
                  0x83, 0x08, 0x02, 0x00, 0x00, 0xC1, 0xE8, 0x1E, 0xA8, 0x01, 0x75, 0x23, 0x48, 0x8B, 0x8B, 0x28, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC9,
                  0x74, 0x17, 0x4C, 0x8B, 0x05, 0xA9, 0xE1, 0xF7, 0x03, 0x45, 0x33, 0xC9, 0x48, 0x8B, 0xD3, 0xC6, 0x44, 0x24, 0x20, 0x00, 0xE8, 0x69,
                  0x3C, 0xA0, 0xFF, 0x48, 0x83, 0xC4, 0x30, 0x5B, 0xC3 };
        // The game's setting bAllowHavokGrabTheLiving:GamePlay (a bool Setting at 0x145F4E548: its type table 0x144AFF448, its value at +8,
        // its name at +0x18, made by 0x140306650 at 0x1401C5EE0): the spring maker (0x141A51A70) reads its value at +0xE2 (cmp [rip+X],bl)
        // and grabs a living actor only when it is set.
        constexpr uintptr_t kLivingSettingRva = 0x5f4e548;
        constexpr uintptr_t kLivingTableRva  = 0x4aff448;
        constexpr uintptr_t kLivingNameRva   = 0x4cc0a60;
        constexpr uintptr_t kLivingReadAt    = 0x1a51b52;
        constexpr uint8_t   kLivingRead[]    = { 0x38, 0x1D };
        // The game's ProcessLists (0x145FE1908, its type table 0x144CC41B0): the actors near you at +0x60 (count), +0x68 (their handles),
        // as the game's own loops read them (MeleeThrow's aim reads the same).
        constexpr uintptr_t kListsRva        = 0x5fe1908;
        constexpr uintptr_t kListsTableRva   = 0x4cc41b0;
        constexpr uintptr_t kHighCount       = 0x60;
        constexpr uintptr_t kHighHandles     = 0x68;
        // The physics update of every frame (0x142354360, profiled by the game as "bhkWorld::Update": its zone name loaded at +0xEE), called
        // from the main loop (0x1417CAC19) and 0x142357F55: (a list of worlds, a number, a flag) -> a bool.
        constexpr uintptr_t kWorldUpdateRva  = 0x2354360;
        constexpr uint8_t   kWorldUpdateStart[] = { 0x48, 0x8B, 0xC4, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
               0x57, 0x48, 0x8D, 0xA8, 0x78, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x48, 0x02, 0x00, 0x00 };
        constexpr uintptr_t kWorldUpdateZone = 0xee; // lea rdx,[zone name]
        constexpr char      kWorldUpdateName[] = "TtbhkWorld::Update";
        // (fix173) The game's queued actor move (a helper's research): 0x1418C5440(unused, actor, cell, const float place[3], heading, bool,
        // bool) holds the actor, fills a pooled command (its type table 0x144CB9958, at 0x1418C55A8) and posts it (0x1406634C0, at
        // 0x1418C5601), which runs it at once inside the game's own update or queues it for the game; its run (0x1418CFD40), only while
        // the actor is still in that cell (+0xB0), calls Actor::SetPosition(place, true) and sets the heading (+0x88). Actor MoveTo and
        // six other game functions use it. Off the game's main thread (this frame step) it is queued.
        constexpr uintptr_t kQueueMoveRva     = 0x18c5440;
        constexpr uint8_t   kQueueMoveStart[] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x0A, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xDC };
        constexpr uintptr_t kQueueMoveTableAt = 0x18c55a8; // lea rcx,[the command's type table]
        constexpr uintptr_t kQueueMoveTable   = 0x4cb9958;
        constexpr uintptr_t kQueueMovePostAt  = 0x18c5601; // call the post
        constexpr uintptr_t kQueueMovePost    = 0x6634c0;
        constexpr uintptr_t kRefHeading       = 0x88;
        constexpr uintptr_t kRefCell          = 0xB0;
        // (fix176) A seated or lying person got up, as the game's own code gets them up (a helper's research of the exe): the furniture state
        // is (actor+0xFC >> 11) & 3 (0 none, 1 getting in, 2 sitting or lying, 3 getting out); the game's "stop interacting" 0x141996BB0
        // (actor, instant, warp, bool; 62 callers) with instant 0 asks the exit idle ("Action Interaction Exit Quick", default action 142)
        // and the game's own fallback if refused; the chair's and bed's exit handlers (0x141AE90B0, 0x141AE9170) clear the state as they
        // stand. Off the game's main thread the game queues it (at 0x1412D6604..0x1412D66A8): its per-thread block (gs:[0x58] -> [0]) made
        // ready (+0xBC, else 0x143661188), its +0x1D4 saved and set to 0x55 (0x1402CC8B0), a 0x18-byte command taken from the task queue
        // ([0x145E794E8] -> its buffer: 0x1422D05A0(buffer, &cookie, 0x18, 8)), filled {type table 0x144CBA048, run 0x1418CF1F0, handle |
        // instant << 32 | warp << 40}, committed (0x1403503B0(&{buffer, &cookie})) and +0x1D4 put back; the game's main thread runs it
        // (0x1418CAEF0: the handle looked up, 0x141996BB0 called; a gone person, nothing). Copied here exactly, each piece checked.
        // (fix191) In 1.16.244 (0x12D4B20) these steps are the same: the same per-thread block offsets (+0xBC, +0x1D4), 0x55, sizes and
        // command. Only the block's counter that the function raises at its start moved there (+0x278 -> +0x27C), and the copy does not use it.
        constexpr uintptr_t kSeatQueueSite  = 0x12d6594; // mov rsi,[task queue]
        constexpr uintptr_t kSeatTlsCheck   = 0x12d6623; // mov r15d,0BCh
        constexpr uintptr_t kSeatTlsInit    = 0x12d6630; // call (the block made ready)
        constexpr uintptr_t kSeatSlotLoad   = 0x12d6635; // mov eax,1D4h
        constexpr uintptr_t kSeatSetCall    = 0x12d6647; // call (+0x1D4 set)
        constexpr uintptr_t kSeatAllocCall  = 0x12d6676; // call (the command taken)
        constexpr uintptr_t kSeatRunLea     = 0x12d667b; // lea rcx,[run]
        constexpr uintptr_t kSeatTableLea   = 0x12d6686; // lea rcx,[type table]
        constexpr uintptr_t kSeatCommitCall = 0x12d66a0; // call (committed)
        constexpr uintptr_t kSeatExitRva    = 0x1996bb0;
        constexpr uint8_t   kSeatExitStart[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x44, 0x88, 0x48, 0x20, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xB8, 0xFE, 0xFF, 0xFF };
        constexpr uintptr_t kSeatRunRva     = 0x18cf1f0; // add rcx,10h; jmp 0x1418CAEF0
        constexpr uintptr_t kSeatRunCallAt  = 0x18caf63; // call 0x141996BB0
        constexpr uintptr_t kTlsReady       = 0xbc;
        constexpr uintptr_t kTlsContext     = 0x1d4;

        // ---- A physics world's bodies (as HandGrab reads them, Starfield 1.16.236) ----
        // Your character controller (PlayerCharacter+0x228 -> +0x8 -> +0x458 -> +0x4D0, its filter info at +0x3C) and its world (+0x160).
        // The world (hknpBSWorld): its bodies at +0x38 (0xB0 bytes each: the rotation's columns at +0x00, +0x10, +0x20, the place at +0x30,
        // its id at +0x50 (the index in bits 0-23), flags at +0x54 (bits 0-1 in use; static 1, moving 2, moved by animation 4; active 8),
        // the filter info at +0x8C (layer bits 0-6)), how many at +0x40.
        constexpr uintptr_t kMiddleController = 0x458;
        constexpr uintptr_t kControllerProxy  = 0x4d0;
        constexpr uintptr_t kProxyFilter      = 0x3c;
        constexpr uintptr_t kProxyWorld       = 0x160;
        constexpr uintptr_t kWorldBodyArray   = 0x38;
        constexpr uintptr_t kWorldBodyCount   = 0x40;
        constexpr uintptr_t kBodySize         = 0xb0;
        constexpr uintptr_t kBodyPlace        = 0x30;
        constexpr uintptr_t kBodyId           = 0x50;
        constexpr uintptr_t kBodyFlags        = 0x54;
        constexpr uintptr_t kBodyFilter       = 0x8c;
        constexpr uint32_t  kBodyIndexMask    = 0xffffff;
        // A collision object (bhkNPCollisionObject): its physics system (+0x20), its body's index there (+0x28); the system's instance (+0x20),
        // that one's world (+0x28: the hknpBSWorld + 0x18), its bodies' ids (+0x30) and how many (+0x38).
        constexpr uintptr_t kCollisionSystem  = 0x20;
        constexpr uintptr_t kSystemInstance   = 0x20;
        constexpr uintptr_t kInstanceWorld    = 0x28;
        constexpr uintptr_t kInstanceIds      = 0x30;
        constexpr uintptr_t kInstanceCount    = 0x38;
        constexpr uintptr_t kWorldFromInstance = 0x18;
        // The player's grab (HandGrab.h): its springs at +0xD80 (count) / +0xD88, the grabbed reference's handle at +0xDA4, the grab mode
        // at +0xF54; a spring's body id at +0x20, its target at +0xA0, its grab point (the body's own space) at +0xB0.
        constexpr uintptr_t kSpringCount  = 0xd80;
        constexpr uintptr_t kSpringArray  = 0xd88;
        constexpr uintptr_t kGrabHandle   = 0xda4;
        constexpr uintptr_t kGrabMode     = 0xf54;
        constexpr uintptr_t kSpringBody   = 0x20;
        constexpr uintptr_t kSpringTarget = 0xa0;
        constexpr uintptr_t kSpringPoint  = 0xb0;

        // ---- Tuning (PLANCK and HIGGS's numbers where they have them) ----
        constexpr float   kAutoDrop        = 0.7f;      // the held bone this far (m) from where your hand holds it: let go (HIGGS)
        constexpr int     kAutoDropFrames  = 3;         // ...for this many frames running
        constexpr int64_t kAutoDropAfterUs = 300'000;   // ...and not in the first moments of a grab
        constexpr float   kKeepDownMax     = 30.0f;     // a person held stays down at most this long (s)
        // (fix173) From 1.0 m/s (fix172's test, seated, pushed at 1.6-1.9 m/s and nothing happened: the game refused every bump). (fix174)
        // After fix173 the shoves at 1.2 and 1.5 m/s staggered, too strong a reaction: a shove under 2.5 m/s (PLANCK's) is the
        // game's bump, else its recoil (the flinch of a blocked attack, as blocks give attackers); a stagger (the game's lightest level,
        // magnitude 1: the stagger function sets the graph's level to magnitude - 1) only from 2.5 m/s, a harder one from 4.
        constexpr float   kShoveLight      = 1.0f;      // m/s of an open hand into someone: bumped or flinched (PLANCK's controller speed for hits: 1 m/s)
        constexpr float   kShoveFirm       = 2.5f;      // ...staggered (PLANCK's shove: 2.5 m/s)
        constexpr float   kShoveHard       = 4.0f;      // ...staggered harder
        constexpr int64_t kShoveCooldownUs = 1'000'000; // the same person again (PLANCK: 2 s between shoves)
        constexpr int64_t kHandCooldownUs  = 700'000;   // the same hand again (PLANCK: hands ignore them for 0.7 s)
        constexpr float   kPersonRadius    = 0.30f;     // a person as an upright capsule this wide (m), and the hand's own size
        constexpr float   kHandRadius      = 0.08f;
        constexpr int64_t kKnockCooldownUs = 3'000'000; // a person knocked down is not knocked again this soon
        constexpr int     kAngerAlarm      = 6;         // points within a minute: the game's assault alarm (PLANCK: "very miffed", then assault)
        constexpr int64_t kAngerWindowUs   = 60'000'000;
        constexpr float   kBlockRadius     = 0.25f;     // your blade this near the attack's path (m): blocked
        constexpr float   kBlockAimedAtYou = 1.2f;      // the attack's path passes this near your body (m): aimed at you
        constexpr int64_t kRegrabWaitUs    = 2'500'000; // a person knocked down by a grip: grabbed if they are down within this
        constexpr int64_t kCensusEveryUs   = 10'000'000;
        constexpr uint32_t kCensusChunk    = 2048;      // body records looked at per frame by the probe
        constexpr float   kCensusRadius    = 8.0f;      // m around you
        constexpr int     kCensusPeople    = 6;
        constexpr int     kCensusLogs      = 60;

        struct Game
        {
            bool      knock{ false };
            uintptr_t knock_fn{ 0 };
            uintptr_t queue_at{ 0 };
            uintptr_t queue_flag_at{ 0 }, main_thread_at{ 0 }; // (fix173) the knock's own test of the thread (logged once)
            bool      move{ false };                           // (fix173) the game's queued actor move
            uintptr_t move_fn{ 0 };
            // (fix176) The game's queued furniture exit (see kSeatExit...): its pieces, from the game's own queuing of it.
            bool      seat_exit{ false };
            uintptr_t seat_queue_at{ 0 }, seat_tls_init{ 0 }, seat_set_context{ 0 }, seat_alloc{ 0 }, seat_commit{ 0 }, seat_table{ 0 }, seat_run{ 0 };
            bool      down_time{ false };
            bool      stagger{ false };
            uintptr_t stagger_fn{ 0 };
            bool      defaults{ false };
            uintptr_t defaults_fn{ 0 };
            bool      actions{ false };
            uintptr_t action_table{ 0 }, action_table2{ 0 }, perform_at{ 0 };
            uintptr_t empty_string_fn{ 0 }, release_string_fn{ 0 }, release_ref_fn{ 0 };
            bool      assault{ false };
            bool      living{ false };
            uintptr_t living_value{ 0 };
            bool      lists{ false };
            bool      timer{ false };
        };
        Game g{};
        bool g_installed{ false };

        // ---- Calling the game ----
        using KnockFn    = void (*)(void* queue, void* actor, float x, float y, float z, float force);
        using StaggerFn  = void (*)(void* actor, int32_t magnitude, void* action, void* hit);
        using DefaultsFn = uintptr_t (*)();
        using PerformFn  = bool (*)(void* data);
        using RefFn      = void (*)(void* ref);
        using StringFn   = uintptr_t* (*)();
        using ReleaseStringFn = void (*)(uintptr_t* string);
        using NativeFn   = void (*)(void* vm, uint64_t stack, void* self);

        uintptr_t DefaultObject(int index)
        {
            if (!g.defaults || index < 0 || index >= 364) {
                return 0;
            }
            const uintptr_t manager = reinterpret_cast<DefaultsFn>(g.defaults_fn)();
            uintptr_t       form    = 0;
            uint8_t         type    = 0;
            if (manager == 0 || !Read(manager + kDefaultObjects + static_cast<uintptr_t>(index) * 8, form) || form == 0 || !Read(form + kRefFormType, type) ||
                type != kFormTypeAction) {
                return 0;
            }
            return form;
        }

        // A reference held for the game's action data (the builders' "lock xadd [ref+8],0x80000200001"), let go with 0x140384BD0.
        void HoldRef(uintptr_t ref) { _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(ref + 8), 0x80000200001ll); }

        std::atomic<int> g_told_action{ 0 };

        // The game performs `action` for `source` (the actor doing it; `target` the other one, or 0), as its own code does (main thread).
        // False: not performed (not available, or the game said no).
        bool Perform(uintptr_t source, uintptr_t target, int index, const char* what)
        {
            const uintptr_t action = DefaultObject(index);
            uintptr_t       perform = 0;
            if (!g.actions || source == 0 || action == 0 || !Read(g.perform_at, perform) || perform == 0 || !InImage(perform)) {
                if (Tell(g_told_action, 12)) {
                    spdlog::warn("[Physical] (fix172) {}: the game's action could not be performed (actions found {}, the action {:#x}, the performer {:#x})", what,
                        g.actions, action, perform);
                }
                return false;
            }
            alignas(16) uint8_t buffer[0x100]{};
            auto* d = reinterpret_cast<uintptr_t*>(buffer);
            HoldRef(source);
            if (target != 0) {
                HoldRef(target);
            }
            uintptr_t* empty   = reinterpret_cast<StringFn>(g.empty_string_fn)();
            uintptr_t  string  = empty != nullptr ? *empty : 0;
            for (int k = 0; k < 2; ++k) {
                if (string != 0) {
                    _InterlockedIncrement(reinterpret_cast<volatile long*>(string + 0x10));
                }
            }
            d[0x00 / 8] = g.action_table;
            d[0x08 / 8] = source;
            d[0x10 / 8] = target;
            d[0x18 / 8] = action;
            d[0x28 / 8] = g.action_table2;
            d[0x30 / 8] = string;
            d[0x38 / 8] = string;
            d[0x90 / 8] = reinterpret_cast<uintptr_t>(&d[0x28 / 8]); // (the builders keep the second part's address beside it)
            // (No exception handler around the game's code: one inside, after it took a lock, would leave the game frozen; a crash says more.)
            const bool done = reinterpret_cast<PerformFn>(perform)(buffer);
            reinterpret_cast<ReleaseStringFn>(g.release_string_fn)(&d[0x38 / 8]);
            reinterpret_cast<ReleaseStringFn>(g.release_string_fn)(&d[0x30 / 8]);
            if (d[0x10 / 8] != 0) {
                reinterpret_cast<RefFn>(g.release_ref_fn)(reinterpret_cast<void*>(d[0x10 / 8]));
            }
            if (d[0x08 / 8] != 0) {
                reinterpret_cast<RefFn>(g.release_ref_fn)(reinterpret_cast<void*>(d[0x08 / 8]));
            }
            if (Tell(g_told_action, 12)) {
                uint32_t id = 0;
                Read(action + kRefFormId, id);
                spdlog::info("[Physical] (fix172) {}: the game performed default action {} (form {:08X}): {}", what, index, id, done ? "done" : "the game said no");
            }
            return done;
        }

        std::atomic<int> g_told_knock{ 0 }, g_told_stagger{ 0 }, g_told_assault{ 0 }, g_told_refused{ 0 };

        // The first knockdowns watched for a few seconds (main thread): the knock states they go through (their numbering past 0 and 2 is
        // inferred) and how long the game keeps them down, for the log.
        struct Watch
        {
            uint32_t    handle{ 0 };
            uint32_t    id{ 0 };
            int64_t     since_us{ 0 };
            uint32_t    last_knock{ 0xffffffffu };
            uint32_t    last_life{ 0xffffffffu };
            std::string seen;
            const char* what{ "" };
        };
        Watch            g_watch[3]{};
        std::atomic<int> g_watches{ 0 };
        constexpr int64_t kWatchUs = 6'000'000;

        void StartWatch(uint32_t handle, uint32_t id, const char* what)
        {
            if (g_watches.fetch_add(1) >= 12) {
                return;
            }
            for (auto& w : g_watch) {
                if (w.handle == 0) {
                    w          = Watch{};
                    w.handle   = handle;
                    w.id       = id;
                    w.since_us = NowUs();
                    w.what     = what;
                    return;
                }
            }
        }

        // Knocked down by PushActorAway's code (any thread; it queues itself off the main thread): pushed away from `from` with `force`.
        bool Knock(uintptr_t actor, const glm::vec3& from, float force, const char* why)
        {
            uintptr_t queue = 0, process = 0, middle = 0;
            uint8_t   gone  = 0;
            if (!g.knock || actor == 0 || !Finite(from) || !Read(g.queue_at, queue) || queue == 0 || !Read(actor + kActorProcess, process) || process == 0 ||
                !Read(process + kProcessGone, gone) || gone != 0 || !Read(process + kProcessMiddle, middle) || middle == 0) {
                if (Tell(g_told_refused, 20)) {
                    spdlog::info("[Physical] (fix172) {}: not knocked down (the knock {}, its process {:#x})", why, g.knock ? "found" : "not found", process);
                }
                return false;
            }
            reinterpret_cast<KnockFn>(g.knock_fn)(reinterpret_cast<void*>(queue), reinterpret_cast<void*>(actor), from.x, from.y, from.z, force);
            return true;
        }

        // A person knocked down kept down at least `min_down` seconds (the knock writes force x fMagnitudeToIdleTimeScaler there: 0 for force
        // 0). (fix173) Called once they are seen down: the knock is queued from this thread and made a moment later (fix172 read it at once,
        // took it for a refusal, and never wrote this).
        constexpr float kGripDownTime = 2.5f;
        void KeepDown(uintptr_t actor, float min_down)
        {
            uintptr_t process = 0, middle = 0;
            float     down    = 0.0f;
            if (g.down_time && min_down > 0.0f && Read(actor + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0 &&
                Read(middle + kMiddleDownTime, down) && std::isfinite(down) && down >= 0.0f && down < min_down) {
                *reinterpret_cast<float*>(middle + kMiddleDownTime) = min_down;
            }
        }

        bool Stagger(uintptr_t actor, int magnitude, const char* why)
        {
            const uintptr_t action = DefaultObject(kActionStaggerStart);
            if (!g.stagger || actor == 0 || action == 0) {
                if (Tell(g_told_stagger, 10)) {
                    spdlog::info("[Physical] (fix172) {}: not staggered (the game's stagger {}, its action {:#x})", why, g.stagger ? "found" : "not found", action);
                }
                return false;
            }
            reinterpret_cast<StaggerFn>(g.stagger_fn)(reinterpret_cast<void*>(actor), magnitude, reinterpret_cast<void*>(action), nullptr);
            return true;
        }

        bool AssaultAlarm(uintptr_t actor)
        {
            if (!g.assault || actor == 0) {
                return false;
            }
            reinterpret_cast<NativeFn>(At(kAssaultRva))(nullptr, 0, reinterpret_cast<void*>(actor));
            return true;
        }

        // ---- The physics update's time (stage 0) ----
        safetyhook::InlineHook g_world_hook{};
        std::atomic<int64_t>   g_world_calls{ 0 }, g_world_ticks{ 0 }, g_world_max_ticks{ 0 };
        // (fix173) The same kept apart for the frame-rate watch (TakePhysicsTimes: each second it looks), the probe's own 10 s left as they are.
        std::atomic<int64_t>   g_watch_calls{ 0 }, g_watch_ticks{ 0 }, g_watch_max_ticks{ 0 };
        int64_t                g_qpc_freq{ 1 };

        void AddTime(std::atomic<int64_t>& calls, std::atomic<int64_t>& sum, std::atomic<int64_t>& longest, int64_t ticks)
        {
            calls.fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(ticks, std::memory_order_relaxed);
            int64_t max = longest.load(std::memory_order_relaxed);
            while (ticks > max && !longest.compare_exchange_weak(max, ticks, std::memory_order_relaxed)) {
            }
        }

        uint64_t OnWorldUpdate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
        {
            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            const uint64_t result = g_world_hook.unsafe_call<uint64_t>(a, b, c, d);
            QueryPerformanceCounter(&t1);
            const int64_t ticks = t1.QuadPart - t0.QuadPart;
            AddTime(g_world_calls, g_world_ticks, g_world_max_ticks, ticks);
            AddTime(g_watch_calls, g_watch_ticks, g_watch_max_ticks, ticks);
            return result;
        }

        // ---- State ----
        // The hand grab's latest take (OnGrabTaken), and the body held now (Update, main thread).
        std::atomic<bool>     g_taken_body{ false };
        std::atomic<uint32_t> g_taken_handle{ 0 };
        std::atomic<int64_t>  g_taken_us{ 0 };
        std::atomic<int>      g_body_grabs{ 0 };
        struct Held
        {
            uintptr_t spring{ 0 };
            uint32_t  handle{ 0 };
            uint32_t  id{ 0 };
            bool      living{ false };
            int64_t   start_us{ 0 };
            int       far_frames{ 0 };
            float     kept{ 0.0f };      // seconds of being kept down added
            bool      down_told{ false };
            float     farthest{ 0.0f };
            int64_t   last_us{ 0 };
            bool      letting_go{ false }; // asked the hand grab to let go (it does in its next update)...
            int64_t   letting_go_us{ 0 };  // ...since then
        };
        Held g_held{};

        // The game's grab-the-living setting while the hand grab's own start of a grab of a person down runs.
        bool    g_living_set{ false };
        uint8_t g_living_before{ 0 };
        std::atomic<int> g_told_living{ 0 }, g_told_body{ 0 }, g_told_drop{ 0 }, g_told_keep{ 0 }, g_told_pull{ 0 }, g_told_grip{ 0 };

        // A person a grip knocked down, to be grabbed once down (main thread).
        struct Regrab
        {
            uint32_t handle{ 0 };
            int      hand{ -1 };
            int64_t  since_us{ 0 };
            uint32_t states_seen{ 0 }; // knock states seen meanwhile (bits)
            bool     asked{ false };
        };
        Regrab g_regrab{};

        // Per person: cooldowns and anger (main thread).
        struct Mood
        {
            uint32_t handle{ 0 };
            int64_t  shoved_us{ 0 };
            int64_t  knocked_us{ 0 };
            int      points{ 0 };
            int64_t  first_us{ 0 };
            int64_t  alarm_us{ 0 };
        };
        constexpr int kMoods = 24;
        Mood g_moods[kMoods]{};

        Mood& MoodOf(uint32_t handle, int64_t now)
        {
            Mood* oldest = &g_moods[0];
            for (auto& m : g_moods) {
                if (m.handle == handle) {
                    return m;
                }
                const int64_t a = std::max({ m.shoved_us, m.knocked_us, m.first_us });
                const int64_t b = std::max({ oldest->shoved_us, oldest->knocked_us, oldest->first_us });
                if (m.handle == 0 || a < b) {
                    oldest = &m;
                }
            }
            *oldest        = Mood{};
            oldest->handle = handle;
            (void)now;
            return *oldest;
        }

        std::atomic<int> g_shoves{ 0 }, g_staggers{ 0 }, g_knockdowns{ 0 }, g_blocks{ 0 }, g_alarms{ 0 }, g_spared{ 0 };
        std::atomic<int> g_told_shove{ 0 }, g_told_punch{ 0 }, g_told_block{ 0 }, g_told_npc_hit{ 0 }, g_told_anger{ 0 }, g_told_spare{ 0 };

        // Anger (PLANCK's): points for a shove or a knockdown not made by a hit; enough within a minute raises the game's assault alarm.
        void AddAnger(uintptr_t actor, const Person& p, int points, int64_t now, const char* what)
        {
            if (!GameFlow::gStore.internalSettings.shoveAnger || p.teammate || p.commanded || Protected(p) != nullptr) {
                return;
            }
            Mood& m = MoodOf(p.handle, now);
            if (m.first_us == 0 || now - m.first_us > kAngerWindowUs) {
                m.first_us = now;
                m.points   = 0;
            }
            m.points += points;
            if (Tell(g_told_anger, 30)) {
                spdlog::info("[Physical] (fix172) {:08X} {}: {} anger point(s) within a minute (the game's assault alarm at {})", p.id, what, m.points, kAngerAlarm);
            }
            if (m.points >= kAngerAlarm && now - m.alarm_us > 2 * kAngerWindowUs) {
                m.alarm_us = now;
                m.points   = 0;
                m.first_us = 0;
                const bool raised = AssaultAlarm(actor);
                g_alarms.fetch_add(raised ? 1 : 0);
                if (Tell(g_told_assault, 20)) {
                    spdlog::info("[Physical] (fix172) {:08X} has had enough of being shoved: {}", p.id,
                        raised ? "the game's assault alarm raised (as script SendAssaultAlarm)" : "the assault alarm is not available");
                }
            }
        }

        // ---- Your body and blades, for blocking (written each frame on the main thread, read in the melee hook's thread) ----
        struct BlockView
        {
            bool      have_you{ false };
            glm::vec3 feet{ 0.0f }, head{ 0.0f };
            bool      blade[2]{ false, false };
            glm::vec3 hilt[2]{}, tip[2]{};
            int64_t   us{ 0 };
        };
        std::mutex g_block_mutex; // a leaf: nothing else is locked while it is held
        BlockView  g_view{};
        struct BlockAsk
        {
            uint32_t attacker{ 0 };
            int      hand{ -1 };
            bool     large{ false };
        };
        BlockAsk g_block_asks[8]{};
        int      g_block_ask_count{ 0 };

        float SegmentDistance(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1, float& s_out, float& t_out)
        {
            // The nearest points of two segments (Ericson's Real-Time Collision Detection, 5.1.9).
            const glm::vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
            const float     a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
            float           s = 0.0f, t = 0.0f;
            if (a <= 1e-8f && e <= 1e-8f) {
                s_out = t_out = 0.0f;
                return glm::length(p0 - q0);
            }
            if (a <= 1e-8f) {
                t = std::clamp(f / e, 0.0f, 1.0f);
            } else {
                const float c = glm::dot(d1, r);
                if (e <= 1e-8f) {
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                } else {
                    const float b     = glm::dot(d1, d2);
                    const float denom = a * e - b * b;
                    s                 = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                    t                 = (b * s + f) / e;
                    if (t < 0.0f) {
                        t = 0.0f;
                        s = std::clamp(-c / a, 0.0f, 1.0f);
                    } else if (t > 1.0f) {
                        t = 1.0f;
                        s = std::clamp((b - c) / a, 0.0f, 1.0f);
                    }
                }
            }
            s_out = s;
            t_out = t;
            return glm::length((p0 + d1 * s) - (q0 + d2 * t));
        }

        // ---- The probe (stage 0) ----
        struct Census
        {
            bool      running{ false };
            uintptr_t world{ 0 };
            uint32_t  next{ 0 }, count{ 0 };
            glm::vec3 you{ 0.0f };
            int64_t   started_us{ 0 };
            int       frames{ 0 };
            uint32_t  looked{ 0 }, unread{ 0 }, in_use{ 0 };
            int       counts[5][4]{}; // layers 8, 32, 33, 30, other x static, moving, moved by animation, other
            int       active[5]{};
        };
        Census           g_census{};
        int64_t          g_census_due_us{ 0 };
        std::atomic<int> g_census_logs{ 0 };

        int LayerSlot(uint32_t layer)
        {
            switch (layer) {
            case 8: return 0;
            case 32: return 1;
            case 33: return 2;
            case 30: return 3;
            default: return 4;
            }
        }
        int KindSlot(uint32_t flags)
        {
            switch (flags & 7u) {
            case 1: return 0;
            case 2: return 1;
            case 4:
            case 6: return 2;
            default: return 3;
            }
        }
        const char* kLayerNames[5] = { "living bodies (8)", "corpses (32)", "bodies without a controller (33)", "character controllers (30)", "others" };

        bool YourWorld(uintptr_t& world)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uintptr_t  process = 0, middle = 0, controller = 0, proxy = 0;
            uint32_t   info = 0;
            world           = 0;
            return player != 0 && Read(player + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0 &&
                   Read(middle + kMiddleController, controller) && controller != 0 && Read(controller + kControllerProxy, proxy) && proxy != 0 &&
                   Read(proxy + kProxyFilter, info) && (info & 0x7fu) == 30u && Read(proxy + kProxyWorld, world) && world != 0;
        }

        // The actors near the game's player (its high-process list): see(handle, ref) with the reference held. -1: the list is not readable.
        template <class F>
        int ForEachActor(F&& see)
        {
            uintptr_t lists = 0, table = 0, handles = 0;
            uint32_t  count = 0;
            if (!g.lists || !Read(At(kListsRva), lists) || lists == 0 || !Read(lists, table) || table != At(kListsTableRva) ||
                !Read(lists + kHighCount, count) || !Read(lists + kHighHandles, handles) || handles == 0) {
                return -1;
            }
            std::array<uint32_t, 256> copy{};
            count = std::min<uint32_t>(count, 256);
            if (count == 0) {
                return 0;
            }
            if (!SafeRead(copy.data(), handles, count * sizeof(uint32_t))) {
                return -1;
            }
            for (uint32_t i = 0; i < count; ++i) {
                GrenadeHand::WithReference(copy[i], [&](uintptr_t ref, uintptr_t) { see(copy[i], ref); });
            }
            return static_cast<int>(count);
        }

        // A person's ragdoll as the physics world has it: the first collision object on the nodes of their 3D, and its system's bodies.
        std::string DescribeRagdoll(uintptr_t root, uintptr_t world)
        {
            std::vector<uintptr_t> stack{ root };
            int                    visited = 0;
            uintptr_t              object  = 0;
            while (!stack.empty() && visited < 96 && object == 0) {
                const uintptr_t node = stack.back();
                stack.pop_back();
                ++visited;
                uintptr_t collision = 0;
                if (node != 0 && Read(node + kNodeCollision, collision) && collision != 0) {
                    uintptr_t system = 0;
                    if (Read(collision + kCollisionSystem, system) && system != 0) {
                        object = collision;
                        break;
                    }
                }
                RE::NiAVObject* kids[64]{};
                const auto      n = IsNodeGuarded(node) ? body::ReadChildren(reinterpret_cast<const RE::NiAVObject*>(node), kids, 64) : 0;
                for (uint16_t i = 0; i < n; ++i) {
                    stack.push_back(reinterpret_cast<uintptr_t>(kids[i]));
                }
            }
            if (object == 0) {
                return fmt::format("no collision object on the first {} nodes of their 3D", visited);
            }
            uintptr_t system = 0, instance = 0, ids = 0, instance_world = 0, bodies = 0;
            uint32_t  count = 0, records = 0;
            if (!Read(object + kCollisionSystem, system) || !Read(system + kSystemInstance, instance) || instance == 0) {
                return fmt::format("a collision object ({} nodes looked at), its physics system not made (not in any world)", visited);
            }
            Read(instance + kInstanceWorld, instance_world);
            if (!Read(instance + kInstanceCount, count) || !Read(instance + kInstanceIds, ids) || ids == 0 || count == 0 || count > 512) {
                return fmt::format("a physics system ({} bodies listed) with no bodies readable", count);
            }
            const bool in_yours = world != 0 && instance_world == world + kWorldFromInstance;
            int        layers[5]{}, kinds[4]{}, found = 0;
            if (in_yours && Read(world + kWorldBodyArray, bodies) && Read(world + kWorldBodyCount, records)) {
                for (uint32_t i = 0; i < std::min<uint32_t>(count, 64); ++i) {
                    uint32_t id = 0, rid = 0, flags = 0, filter = 0;
                    if (!Read(ids + i * sizeof(uint32_t), id) || (id & kBodyIndexMask) >= records) {
                        continue;
                    }
                    const uintptr_t record = bodies + static_cast<uintptr_t>(id & kBodyIndexMask) * kBodySize;
                    if (!Read(record + kBodyId, rid) || rid != id || !Read(record + kBodyFlags, flags) || (flags & 3u) == 0 || !Read(record + kBodyFilter, filter)) {
                        continue;
                    }
                    ++found;
                    ++layers[LayerSlot(filter & 0x7fu)];
                    ++kinds[KindSlot(flags)];
                }
            }
            return fmt::format("ragdoll of {} bodies {}{}", count, in_yours ? "in your physics world" : instance_world != 0 ? "in another world" : "not in a world",
                in_yours ? fmt::format(" ({} found: layers living {}, corpse {}, no-controller {}, others {}; static {}, moving {}, moved by animation {})", found,
                               layers[0], layers[1], layers[2], layers[3] + layers[4], kinds[0], kinds[1], kinds[2])
                         : std::string{});
        }

        void UpdateCensus(int64_t now)
        {
            const auto& s = GameFlow::gStore.internalSettings;
            if (!s.physicalProbe || g_census_logs.load() >= kCensusLogs) {
                g_census.running = false;
                return;
            }
            auto* player = CreationEngineSingletonManager::GetPlayerRef();
            if (!g_census.running) {
                if (now < g_census_due_us || player == nullptr || GameFlow::isShowingMenu() || GameFlow::isLoadingScreenShown()) {
                    return;
                }
                uintptr_t world = 0;
                float     place[3]{};
                if (!YourWorld(world) || !SafeRead(place, reinterpret_cast<uintptr_t>(player) + kRefPlace, sizeof(place))) {
                    g_census_due_us = now + kCensusEveryUs;
                    return;
                }
                g_census            = Census{};
                g_census.running    = true;
                g_census.world      = world;
                g_census.you        = glm::vec3{ place[0], place[1], place[2] };
                g_census.started_us = now;
                Read(world + kWorldBodyCount, g_census.count);
                g_census.count = std::min<uint32_t>(g_census.count, kBodyIndexMask);
            }
            ++g_census.frames;
            uintptr_t world = 0, bodies = 0;
            if (!YourWorld(world) || world != g_census.world || !Read(world + kWorldBodyArray, bodies) || bodies == 0) {
                g_census.running = false; // (your world changed meanwhile: the next pass)
                g_census_due_us  = now + kCensusEveryUs;
                return;
            }
            const uint32_t end = std::min<uint32_t>(g_census.count, g_census.next + kCensusChunk);
            uint8_t        raw[kBodyFilter + 4 - kBodyPlace]{};
            for (uint32_t i = g_census.next; i < end; ++i) {
                ++g_census.looked;
                if (!GuardedCopy(raw, bodies + static_cast<uintptr_t>(i) * kBodySize + kBodyPlace, sizeof(raw))) {
                    ++g_census.unread;
                    continue;
                }
                uint32_t  id = 0, flags = 0, filter = 0;
                glm::vec3 place{};
                std::memcpy(&place, raw, sizeof(place));
                std::memcpy(&id, raw + (kBodyId - kBodyPlace), sizeof(id));
                std::memcpy(&flags, raw + (kBodyFlags - kBodyPlace), sizeof(flags));
                std::memcpy(&filter, raw + (kBodyFilter - kBodyPlace), sizeof(filter));
                if ((id & kBodyIndexMask) != i || (flags & 3u) == 0 || !Finite(place)) {
                    continue;
                }
                ++g_census.in_use;
                if (glm::length(place - g_census.you) > kCensusRadius) {
                    continue;
                }
                const int layer = LayerSlot(filter & 0x7fu);
                ++g_census.counts[layer][KindSlot(flags)];
                g_census.active[layer] += (flags & 8u) != 0 ? 1 : 0;
            }
            g_census.next = end;
            if (end < g_census.count) {
                return;
            }
            // A whole pass: one line for the bodies, the physics time, and a line for each person near you.
            g_census.running = false;
            g_census_due_us  = now + kCensusEveryUs;
            const int     n     = g_census_logs.fetch_add(1) + 1;
            const int64_t calls = g_world_calls.exchange(0);
            const int64_t ticks = g_world_ticks.exchange(0);
            const int64_t max   = g_world_max_ticks.exchange(0);
            std::string   layers;
            for (int l = 0; l < 5; ++l) {
                const auto& c = g_census.counts[l];
                if (c[0] + c[1] + c[2] + c[3] == 0) {
                    continue;
                }
                layers += fmt::format("{}{}: {} static, {} moving, {} moved by animation{} ({} awake)", layers.empty() ? "" : "; ", kLayerNames[l], c[0], c[1], c[2],
                    c[3] > 0 ? fmt::format(", {} other", c[3]) : std::string{}, g_census.active[l]);
            }
            spdlog::info("[Physical] (fix172) probe {}: {} bodies in your physics world ({} looked at over {} frames{}), within {:.0f} m of you: {}; the physics "
                         "update {}",
                n, g_census.in_use, g_census.looked, g_census.frames, g_census.unread > 0 ? fmt::format(", {} unreadable", g_census.unread) : std::string{},
                kCensusRadius, layers.empty() ? "none" : layers,
                !g.timer ? std::string{ "not timed (its code not as in 1.16.236)" }
                : calls > 0 ? fmt::format("ran {} times since the last line, {:.2f} ms on average, {:.2f} ms at most", calls,
                                  static_cast<double>(ticks) / static_cast<double>(calls) * 1000.0 / static_cast<double>(g_qpc_freq),
                                  static_cast<double>(max) * 1000.0 / static_cast<double>(g_qpc_freq))
                            : std::string{ "did not run since the last line" });
            int listed = 0;
            const int actors = ForEachActor([&](uint32_t handle, uintptr_t ref) {
                Person p;
                if (listed >= kCensusPeople || !ReadPerson(ref, p) || p.player || glm::length(p.place - g_census.you) > kCensusRadius) {
                    return;
                }
                ++listed;
                spdlog::info("[Physical] (fix172) probe {}: person {:08X} (handle {:x}) {:.1f} m from you, life state {}, knock state {}{}{}{}: {}", n, p.id, handle,
                    glm::length(p.place - g_census.you), p.life, p.knock, p.teammate ? ", your follower" : "", p.essential ? ", essential" : "",
                    p.furniture ? ", in furniture" : "", p.root != 0 ? DescribeRagdoll(p.root, g_census.world) : std::string{ "no 3D" });
            });
            if (actors < 0) {
                spdlog::info("[Physical] (fix172) probe {}: the game's list of people near you could not be read", n);
            }
        }

        // ---- The held body (stage 1) ----
        uintptr_t PlayerSpring(uintptr_t player, uint32_t& handle)
        {
            uint32_t  count = 0, mode = 0;
            uintptr_t array = 0, spring = 0;
            handle          = 0;
            if (player == 0 || !Read(player + kSpringCount, count) || count == 0 || count > 64 || !Read(player + kGrabMode, mode) || mode != 1 ||
                !Read(player + kSpringArray, array) || array == 0 || !Read(array, spring) || spring == 0) {
                return 0;
            }
            Read(player + kGrabHandle, handle);
            return spring;
        }

        // Where the spring's held point is now and where the hand wants it (game world).
        bool HeldPoint(uintptr_t spring, uintptr_t world, glm::vec3& held, glm::vec3& target)
        {
            uint32_t  body = 0, records = 0, rid = 0, flags = 0;
            uintptr_t bodies = 0;
            float     local[3]{}, aim[3]{}, xf[16]{};
            if (spring == 0 || world == 0 || !Read(spring + kSpringBody, body) || (body & kBodyIndexMask) == kBodyIndexMask || !SafeRead(local, spring + kSpringPoint, sizeof(local)) ||
                !SafeRead(aim, spring + kSpringTarget, sizeof(aim)) || !Read(world + kWorldBodyArray, bodies) || !Read(world + kWorldBodyCount, records) ||
                (body & kBodyIndexMask) >= records) {
                return false;
            }
            const uintptr_t record = bodies + static_cast<uintptr_t>(body & kBodyIndexMask) * kBodySize;
            if (!Read(record + kBodyId, rid) || rid != body || !Read(record + kBodyFlags, flags) || (flags & 3u) == 0 || !SafeRead(xf, record, sizeof(xf))) {
                return false;
            }
            const glm::vec3 c0{ xf[0], xf[1], xf[2] }, c1{ xf[4], xf[5], xf[6] }, c2{ xf[8], xf[9], xf[10] }, t{ xf[12], xf[13], xf[14] };
            held   = c0 * local[0] + c1 * local[1] + c2 * local[2] + t;
            target = glm::vec3{ aim[0], aim[1], aim[2] };
            return Finite(held) && Finite(target);
        }

        void ForgetHeld() { g_held = Held{}; }

        void UpdateHeld(int64_t now, float dt)
        {
            const auto& s      = GameFlow::gStore.internalSettings;
            const auto  player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uint32_t    handle = 0;
            const uintptr_t spring = PlayerSpring(player, handle);
            // A save asked for (its moment: an autosave can stay asked for a long time while the game waits, and a body grabbed meanwhile
            // stays held).
            static bool saving_was = false;
            const bool  saving     = GrenadeHand::SavePending();
            const bool  save_asked = saving && !saving_was;
            saving_was             = saving;
            if (spring == 0 && now - g_taken_us.load() > 500'000) {
                g_taken_body.store(false); // (nothing held: the next take says again what it is)
            }
            if (spring == 0 || !g_taken_body.load() || !s.grabBodies) {
                if (g_held.spring != 0 && Tell(g_told_drop, 40)) {
                    spdlog::info("[Physical] (fix172) the body {:08X} is let go after {:.1f} s (held at most {:.2f} m from your hand{})", g_held.id,
                        (now - g_held.start_us) * 1e-6, g_held.farthest, g_held.kept > 0.0f ? fmt::format(", kept down {:.1f} s more", g_held.kept) : std::string{});
                }
                ForgetHeld();
                return;
            }
            if (spring != g_held.spring) {
                ForgetHeld();
                g_held.spring   = spring;
                g_held.handle   = handle;
                g_held.start_us = now;
            }
            g_held.last_us = now;
            // Let go at once: a menu, the game about to save (a loading screen: the game's own cleanup lets go of it). (fix175: or build mode,
            // no menu by the count since fix175.)
            if (GameFlow::isShowingMenu() || GameFlow::isWorkshopShown() || save_asked) {
                const bool  menu = GameFlow::isShowingMenu() || GameFlow::isWorkshopShown();
                const char* what = GameFlow::isShowingMenu() ? "a menu opened" : GameFlow::isWorkshopShown() ? "build mode opened (fix175)" : "the game is about to save";
                const bool dropped = HandGrab::DropNow(menu ? "(fix172) a body is let go: a menu or build mode opened" : "(fix172) a body is let go: the game saves");
                if (Tell(g_told_drop, 40)) {
                    spdlog::info("[Physical] (fix172) the body you hold is let go at once: {} ({})", what, dropped ? "dropped" : "the drop was not possible");
                }
                ForgetHeld();
                return;
            }
            // The person held: alive (knocked down) or dead.
            bool   living = false, up = false;
            Person person;
            GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t) {
                if (!ReadPerson(ref, person)) {
                    return;
                }
                living = !Dead(person);
                up     = living && !KnockedDown(person);
                g_held.id = person.id;
                // Kept down while held (PLANCK keeps a held ragdoll down): the time it stays down moved on with the time held, so the game's
                // get-up waits (whether the game counts it down or compares it with the time since the knock).
                if (living && !up && s.keepHeldDown && g.down_time && g_held.kept < kKeepDownMax) {
                    uintptr_t process = 0, middle = 0;
                    float     left    = 0.0f;
                    if (Read(ref + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0 &&
                        Read(middle + kMiddleDownTime, left) && std::isfinite(left) && left >= 0.0f && left < 120.0f) {
                        const float add = std::clamp(dt, 0.0f, 0.1f);
                        *reinterpret_cast<float*>(middle + kMiddleDownTime) = left + add;
                        g_held.kept += add;
                        if (!g_held.down_told && Tell(g_told_keep, 10)) {
                            g_held.down_told = true;
                            spdlog::info("[Physical] (fix172) {:08X} is held while knocked down (knock state {}): kept down (its time down {:.2f} s, moved on as you hold "
                                         "them, at most {:.0f} s)",
                                person.id, person.knock, left, kKeepDownMax);
                        }
                    }
                }
            });
            g_held.living = living;
            if (g_held.letting_go) {
                // (Asked already: the hand grab lets go in its next update. That update does not run while the hand grab is not active
                // (third person, the flat screen, a ladder, the setting off): still held a moment later, the game's drop now (the review).)
                if (now - g_held.letting_go_us > 250'000) {
                    const bool dropped = HandGrab::DropNow("(fix172) a body is let go: the hand grab's own let-go did not happen");
                    if (Tell(g_told_drop, 40)) {
                        spdlog::info("[Physical] (fix172) {:08X} was still held 0.25 s after it was let go: {}", g_held.id,
                            dropped ? "dropped with the game's drop" : "the drop was not possible");
                    }
                    ForgetHeld();
                }
                return;
            }
            if (up) {
                g_held.letting_go    = true;
                g_held.letting_go_us = now;
                HandGrab::LetGo("(fix172) the person you held got up");
                if (Tell(g_told_drop, 40)) {
                    spdlog::info("[Physical] (fix172) {:08X} got up (knock state {}): let go", person.id, person.knock);
                }
                return;
            }
            // HIGGS's auto-drop: the bone you hold this far from where your hand holds it (pulled through a wall, stuck, stretched).
            uintptr_t world = 0;
            glm::vec3 held{}, target{};
            if (YourWorld(world) && HeldPoint(spring, world, held, target)) {
                const float off  = glm::length(held - target);
                g_held.farthest  = std::max(g_held.farthest, now - g_held.start_us > kAutoDropAfterUs ? off : 0.0f);
                g_held.far_frames = off > kAutoDrop && now - g_held.start_us > kAutoDropAfterUs ? g_held.far_frames + 1 : 0;
                if (g_held.far_frames >= kAutoDropFrames) {
                    g_held.letting_go    = true;
                    g_held.letting_go_us = now;
                    HandGrab::LetGo("(fix172) the bone you held was 0.7 m from your hand");
                    if (Tell(g_told_drop, 40)) {
                        spdlog::info("[Physical] (fix172) {:08X}: the bone you hold is {:.2f} m from your hand ({} frames): let go (HIGGS's auto-drop)", g_held.id, off,
                            g_held.far_frames);
                    }
                    g_held.far_frames = 0;
                }
            }
        }

        // A person a grip knocked down: grabbed once they are down, if the grip is still held and nothing else is.
        void UpdateRegrab(int64_t now)
        {
            if (g_regrab.handle == 0) {
                return;
            }
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            uint32_t   held   = 0;
            if (now - g_regrab.since_us > kRegrabWaitUs || !HandGrab::GripDown(g_regrab.hand == 0) || PlayerSpring(player, held) != 0) {
                if (Tell(g_told_grip, 30)) {
                    spdlog::info("[Physical] (fix172) the grip's knockdown: {} (knock states seen: {:#x})",
                        now - g_regrab.since_us > kRegrabWaitUs ? "they were not down in time, not grabbed" : !HandGrab::GripDown(g_regrab.hand == 0) ? "the grip was let go" : "something is held",
                        g_regrab.states_seen);
                }
                g_regrab = Regrab{};
                return;
            }
            bool down = false;
            GrenadeHand::WithReference(g_regrab.handle, [&](uintptr_t ref, uintptr_t) {
                Person p;
                if (ReadPerson(ref, p)) {
                    // (fix173) Kept down a while the first time they are seen knocked (the knock is made a moment after the call).
                    if (KnockedDown(p) && g_regrab.states_seen == 0) {
                        KeepDown(ref, kGripDownTime);
                    }
                    if (KnockedDown(p) || Dead(p)) {
                        g_regrab.states_seen |= 1u << std::min<uint32_t>(p.knock, 31);
                    }
                    down = LyingDown(p) || Dead(p);
                }
            });
            if (down && !g_regrab.asked) {
                g_regrab.asked = true;
                HandGrab::GrabAgain(g_regrab.hand == 0);
                if (Tell(g_told_grip, 30)) {
                    spdlog::info("[Physical] (fix172) the person your {} grip knocked down is down after {:.2f} s (knock states seen {:#x}): grabbing them",
                        g_regrab.hand == 0 ? "left" : "right", (now - g_regrab.since_us) * 1e-6, g_regrab.states_seen);
                }
            } else if (g_regrab.asked && now - g_regrab.since_us > 400'000 + kRegrabWaitUs / 4) {
                g_regrab = Regrab{}; // (asked: the grab went its way)
            }
        }

        // (fix176) The game's furniture exit for this person (by handle), queued for the game's main thread exactly as its own code queues it
        // (kSeatExit... above). False: not posted.
        bool PostSeatExit(uint32_t handle, bool instant, bool warp)
        {
            if (!g.seat_exit || handle == 0) {
                return false;
            }
            uintptr_t queue = 0, buffer = 0;
            if (!Read(g.seat_queue_at, queue) || queue == 0 || !Read(queue, buffer) || buffer == 0) {
                return false;
            }
            const auto* tls = reinterpret_cast<const uintptr_t*>(__readgsqword(0x58));
            if (tls == nullptr || tls[0] == 0) {
                return false;
            }
            auto* block = reinterpret_cast<uint8_t*>(tls[0]);
            if (block[kTlsReady] == 0) {
                reinterpret_cast<void (*)()>(g.seat_tls_init)();
            }
            using SetFn    = void (*)(uint32_t);
            using AllocFn  = uintptr_t* (*)(uintptr_t buffer, void* cookie, uint32_t size, uint32_t align);
            using CommitFn = void (*)(void* pair);
            const uint32_t saved = *reinterpret_cast<const uint32_t*>(block + kTlsContext);
            reinterpret_cast<SetFn>(g.seat_set_context)(0x55);
            alignas(16) uint8_t cookie[16]{};
            uintptr_t*          cmd = reinterpret_cast<AllocFn>(g.seat_alloc)(buffer, cookie, 0x18, 8);
            if (cmd != nullptr) {
                cmd[1] = g.seat_run;
                cmd[0] = g.seat_table;
                cmd[2] = static_cast<uint64_t>(handle) | (static_cast<uint64_t>(instant ? 1 : 0) << 32) | (static_cast<uint64_t>(warp ? 1 : 0) << 40);
                struct
                {
                    uintptr_t buffer;
                    void*     cookie;
                } pair{ buffer, cookie };
                reinterpret_cast<CommitFn>(g.seat_commit)(&pair);
            }
            reinterpret_cast<SetFn>(g.seat_set_context)(saved);
            return cmd != nullptr;
        }

        // ---- (fix173) Holding a standing person (no knockdown) ----
        // After fix172 a right grip close to an npc made them collapse; the grip should hold them, not knock them down.
        // As PLANCK holds the living (a helper's research of its source): their limb pulled toward your hand (cosmetic: their AI does not
        // know), and the person walked where you pull. Here: the held part bent toward your hand in their own model's update (BodyIK:
        // an arm by two-bone IK, at most 0.3 m; the chest, neck or head leaned at most 20 degrees), and the pull beyond 0.15 m drags them
        // (at most 1 m/s, their own heading kept; fix175: with your own walk too, 3 m/s) with the game's queued actor move (no knock, no grab,
        // nothing written into them from this thread). Let go: the grip opened; their held point 0.7 m from your hand for 3 frames (HIGGS's
        // auto-drop); (fix175: no longer at 3 m dragged); a
        // menu, conversation, loading or third person; they die, are knocked down, sit, enter a scene or lose their 3D. Not your
        // followers' anger, but held all the same; never children, people in scenes or furniture, or big creatures. Their anger as
        // PLANCK's: a point each 1.5 s held (the game's assault alarm at 6: about 9 s).
        // (fix174) After fix173 grabbing someone worked but the hand did not look like it was holding them (not touching): the holds
        // began with the palm 0.16-0.26 m from the bone (0.45 allowed) and kept that gap (the held point was the palm's own place in the air),
        // the bend capped at 0.3 m (the upper arm reached 0.18 of 0.39). Now the palm must be at most 0.12 m off their surface (each part's
        // thickness from its bone), the held point is on their surface, and your drawn hand is put on it (BodyIK, as the second hand on a
        // held object, fix170): the two meet. Fingers closed part way round it.
        constexpr float   kHoldReach      = 0.12f;       // the palm at most this far off a part of them (its surface) to hold them by it
        // (fix176) After fix175 dragging worked but the hand did not stay on the body; the grip was floaty (the log: the held point
        // 0.09-0.32 m from the hand all along): the pull now drags them from 0.04 m (was 0.15) and much faster, so the held point stays at
        // your hand, the bend and your drawn hand closing the rest.
        constexpr float   kHoldDeadZone   = 0.04f;       // the pull (m, level) the bend takes before they are dragged
        // (fix175) After fix174: an npc grabbed is dragged as you walk: at 1 m/s at most they fell behind your
        // walk and the hold let go at 0.7 m (the log: "let go (they were 0.7 m from your hand)" after 2-3 s, 0.9 m dragged). Now they come with
        // your own walk, the pull catches up faster, and there is no 3 m limit.
        // (fix182) After fix181 a person's arm bent oddly when grabbed by the hand (the held point 0.15-0.41 m from your hand most of
        // the time, the bend taking it up to its cap): their arm now gives only a little (BodyIK's gentle bend, 12 cm), and the drag brings the
        // whole of them nearer your hand: faster (25 a second, 5 m/s; was 15, 4). (The review) At most 0.4 of the pull a frame: the queued move
        // can land a frame or two late, and a larger share overshoots and rings (at 30-43 updates a second 0.6 overshot 4-23 cm, and your
        // hand, locked on them, jerked with them).
        constexpr float   kHoldDragGain   = 25.0f;       // dragged at this many m/s per m of pull past the dead zone (fix176: was 6)...
        constexpr float   kHoldDragMax    = 5.0f;        // ...at most (on top of your own walk; fix176: was 3)
        constexpr float   kHoldDragFrame  = 0.4f;        // (fix182) ...and at most this share of the pull a frame
        constexpr int     kHoldBlockedFrames = 12;       // (fix182, the review) frames running they move under 0.3 of the drag asked: held back that way
        constexpr float   kHoldDragLimit  = 20.0f;       // (fix175, the review) let go after dragging them this far in all
        constexpr int64_t kHoldEaseUs     = 150'000;     // the bend eased in and out
        constexpr int64_t kHoldAngerUs    = 3'000'000;   // a point of anger each this long held (fix174: 1.5 s made them hostile in ~10 s)
        constexpr uintptr_t kModelNode    = 0x180;       // a 3D root's model node (BodyIK's kModelNodeOffset)

        struct LiveHold
        {
            bool      active{ false };
            uint32_t  handle{ 0 };
            uint32_t  id{ 0 };
            int       hand{ 0 };
            char      bone[24]{};
            glm::vec3 held_local{ 0.0f }; // the held point in the bone's own frame (its scale removed)
            glm::vec3 anchor{ 0.0f };     // the held point from their place, in their heading's frame
            glm::vec3 start_place{ 0.0f };
            glm::vec3 palm{ 0.0f };       // your hand, last frame (game world)
            int64_t   start_us{ 0 };
            int64_t   anger_us{ 0 };
            int64_t   log_us{ 0 };
            int64_t   end_us{ 0 };        // easing out since (0: held)
            uintptr_t model{ 0 };
            int       far_frames{ 0 };
            int       moves{ 0 };
            float     asked_drag{ 0.0f }; // m asked to drag them in all
            float     pull{ 0.0f };
            float     weight{ 0.0f };     // the bend's weight last sent
            bool      angers{ false };
            RE::NiAVObject* bone_node{ nullptr }; // (fix174) the held bone's node, its 3D root when found: the contact read from it
            uintptr_t root{ 0 };
            glm::vec3 you{ 0.0f };   // (fix175) your place last frame
            float     thick{ 0.05f }; // (fix177) the held part's thickness (fix179: its outer surface's, clothes in)
            int       grab{ 0 };     // (fix177) a number for each hold (the hand's grip state starts anew with it)
            bool      have_you{ false };
            // (fix179) The held part's outer surface in the held bone's own frame (its scale removed): the capsule's line from cap_a to
            // cap_b, cap_r thick (the body), and the clothes' margin over it (m, game world); the way out of them at the held point; the
            // trunk or a limb; up to three surfaces near it (their bones, lines, thickness and margins).
            glm::vec3 cap_a{ 0.0f }, cap_b{ 0.0f };
            float     cap_r{ 0.05f };
            float     margin{ 0.0f };
            glm::vec3 normal_local{ 0.0f, 0.0f, 1.0f };
            bool      flat{ false };
            int       near_count{ 0 };
            RE::NiAVObject* near_node[3]{};
            glm::vec3 near_a[3]{}, near_b[3]{};
            float     near_r[3]{}, near_margin[3]{};
            // (fix179) Where their held part is bent to: your palm, plus (when your palm began inside them) the way from it out to the
            // held point, kept in your hand's own frame (out of the palm, to the fingers, across), so they are not pushed back by it.
            glm::vec3 bend_offset{ 0.0f };
            glm::vec3 bend_target{ 0.0f };
            // (fix179) The contact last frame (the one-frame lead) and what was given, for the log.
            glm::vec3 last_contact{ 0.0f };
            bool      have_last_contact{ false };
            float     lead{ 0.0f }; // (the review) the lead given, smoothed
            // (fix182, the review) The drag's last move asked (where to, how far) and where they were then: how late each move lands and
            // whether they move at all (a wall: the queued move has no collision check, the game puts them back); the drag held back the
            // way they cannot go.
            glm::vec3 last_to{ 0.0f }, last_step{ 0.0f }, last_place{ 0.0f };
            bool      have_last_to{ false };
            float     late_sum{ 0.0f }, late_max{ 0.0f };
            int       late_n{ 0 };
            int       blocked_frames{ 0 };
            bool      blocked{ false }, told_blocked{ false };
            glm::vec2 blocked_dir{ 0.0f };
            float     lead_sum{ 0.0f }, lead_max{ 0.0f };
            int       lead_frames{ 0 };
        };
        LiveHold         g_live{};
        // (fix174) The held point on them as last drawn (game world), for BodyIK to put your drawn hand on it (any thread).
        std::mutex       g_contact_mutex;
        glm::vec3        g_contact{ 0.0f };
        int              g_contact_hand{ -1 };
        glm::vec3        g_contact_centre{ 0.0f }; // (fix177) the held part's line under the contact, its way and thickness
        glm::vec3        g_contact_axis{ 0.0f, 0.0f, 1.0f };
        float            g_contact_radius{ 0.05f };
        int              g_contact_grab{ 0 };
        float            g_contact_weight{ 0.0f }; // (the hold's ease-in: the drawn hand eased onto them with it)
        int64_t          g_contact_us{ 0 };
        glm::vec3        g_contact_normal{ 0.0f, 0.0f, 1.0f }; // (fix179) the way out of them, the trunk or a limb, the surfaces near
        bool             g_contact_flat{ false };
        int              g_contact_caps{ 0 };
        glm::vec3        g_contact_cap_a[4]{}, g_contact_cap_b[4]{};
        float            g_contact_cap_r[4]{};
        glm::vec3        g_contact_bone_t{ 0.0f }; // (fix180) the held bone as last drawn (moved on by the lead)
        glm::mat3        g_contact_bone_r{ 1.0f };
        float            g_contact_bone_s{ 1.0f };
        uintptr_t        g_contact_model{ 0 };     // (fix181) their model, the held part's bare body's thickness, the held bone's name
        float            g_contact_bare_r{ 0.05f };
        char             g_contact_bone_name[24]{};
        std::atomic<int> g_told_hold{ 0 };
        std::atomic<bool> g_live_hand[2]{ false, false };

        glm::vec3 TurnZ(const glm::vec3& v, float angle)
        {
            const float c = std::cos(angle), s = std::sin(angle);
            return { v.x * c - v.y * s, v.x * s + v.y * c, v.z };
        }

        // Your hand's grip point in the game world (false: not tracked).
        bool HandWorldPoint(bool left, glm::vec3& out)
        {
            const glm::mat4 grip = tracking::GripPose(left);
            const glm::vec3 at{ grip[3] };
            glm::vec3       unused{};
            return Finite(at) && tracking::StageToWorld(at, -glm::vec3{ tracking::AimPose(left)[2] }, out, unused) && Finite(out);
        }

        // (fix174) Your palm's surface where a held thing rests (the hand grab's own, as a held object is placed; game world), else the grip.
        bool PalmPoint(bool left, glm::vec3& out)
        {
            glm::vec3 facing{}, forward{}, side{};
            if (HandGrab::HoldFrame(left, out, facing, forward, side) && Finite(out)) {
                return true;
            }
            return HandWorldPoint(left, out);
        }

        // A bone's world transform, read guarded.
        bool NodeWorld(RE::NiAVObject* node, glm::vec3& t, glm::mat3& r, float& s)
        {
            RE::NiTransform w{};
            if (node == nullptr || !SafeRead(&w, reinterpret_cast<uintptr_t>(node) + offsetof(RE::NiAVObject, world), sizeof(w))) {
                return false;
            }
            const body::Xf x = body::FromNi(w); // (the game's rows are glm's columns, as BodyIK reads them)
            t                = x.t;
            r                = x.r;
            s                = x.s;
            return Finite(t) && std::isfinite(s) && s > 0.01f && s < 100.0f;
        }

        float SegmentDistance(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
        {
            const glm::vec3 ab = b - a;
            const float     k  = glm::dot(ab, ab) > 1e-8f ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + ab * k));
        }

        // Who is never held (as Protected, but your followers and those you command are held, without anger; the review: everything else
        // of Protected still refuses them: down, restrained, in a scene, sitting...). Checked as the hold starts and each frame.
        // (fix176) `seated_ok`: someone sitting or lying is not refused (they are got up first: the pull-up).
        const char* HoldRefusal(const Person& p, bool seated_ok = false)
        {
            if (p.player) return "you";
            if (p.gone) return "disabled";
            if (Dead(p)) return "dead";
            if (p.life != 0) return p.life == 3 ? "unconscious" : p.life == 6 ? "restrained" : p.life == 7 ? "down (essential)" : p.life == 8 ? "bleeding out" : "not in a normal state";
            if (p.child) return "a child";
            if (p.scene) return "in a scene";
            if (p.furniture && !seated_ok) return "in furniture (sitting or lying)";
            if (Big(p)) return "too big";
            return nullptr;
        }

        // The bend handed to BodyIK with this weight (the held point to your hand as last seen).
        void SendBend(float weight)
        {
            body::HeldPersonBend b{};
            b.model = g_live.model;
            std::snprintf(b.bone, sizeof(b.bone), "%s", g_live.bone);
            // (fix179) Your palm, moved out by the way your palm was inside them as the hold began (bend_target, HoldOneFrame).
            b.target[0]     = g_live.bend_target.x;
            b.target[1]     = g_live.bend_target.y;
            b.target[2]     = g_live.bend_target.z;
            b.held_local[0] = g_live.held_local.x;
            b.held_local[1] = g_live.held_local.y;
            b.held_local[2] = g_live.held_local.z;
            b.weight        = weight;
            body::SetHeldPersonBend(b);
        }

        // ---- (fix179) Their outer surface: the game's own body, and their clothes over it ----
        // After fix178 grabbing and pulling people worked but the hand still clipped through far too much; a held npc's clothing
        // and body should not be clipped through. The log: most holds began with your palm inside the old model of them (round
        // segments 0.03-0.14 m thick: "your palm 0.00 m off their surface, 0.05 m from its bone"); the held point was then your palm itself,
        // so your drawn hand was put inside them, and its fingers, starting inside, stayed straight. The game's own model of a person is
        // thicker (a helper's research of skeleton.nif, Meshes01 #214 and #236, the same for men and women): its ragdoll has a body on 20
        // bones (each bone's bhkNPCollisionObject names its body), each a capsule (two points in the bone's own frame and a radius). Clothes
        // are not in it (an outfit has no collision): each worn mesh's skin keeps a bounding sphere for the part of it each bone moves
        // (BSSkin::BoneData), compared here with the same spheres of the bare body (naked_m/f.nif, hands_3rd_m/f.nif) to see how far the
        // clothes stand out where you hold. As HIGGS (Skyrim VR) takes the point on the person's visible mesh nearest your palm's line, and
        // ROCK (Fallout 4 VR) seats the palm on it: here on these surfaces.

        // The game's ragdoll capsules (skeleton.nif's bhkRagdollSystem): two points in the bone's own frame (x along the bone, y to the
        // front, z to the side; metres, the skeleton's own size) and the radius; the trunk's parts flat for the fingers; and the bones the
        // worn meshes move there (their spheres measure the clothes over this part). C_Spine1 and the clavicles have no body of their own
        // (the trunk's capsules and the upper arms' cover them).
        struct BodyCapsule
        {
            const char* bone;
            float       a[3];
            float       b[3];
            float       r;
            bool        flat;
            const char* skin[4];
        };
        constexpr BodyCapsule kBodyCapsules[] = {
            { "R_Biceps", { 0.211f, 0.003f, 0.012f }, { 0.010f, 0.003f, 0.012f }, 0.0627f, false, { "R_Biceps", "R_Biceps_Twist1", nullptr, nullptr } },
            { "R_Forearm", { 0.176f, -0.001f, -0.006f }, { 0.039f, -0.001f, -0.006f }, 0.0505f, false, { "R_Forearm", "R_Elbow", "R_Wrist_Twist", "R_Wrist_Twist1" } },
            { "R_Wrist", { 0.063f, 0.0f, -0.004f }, { 0.015f, 0.0f, -0.004f }, 0.0349f, false, { "R_Wrist", "R_Wrist_Twist2", nullptr, nullptr } },
            { "L_Biceps", { 0.211f, -0.003f, 0.012f }, { 0.010f, -0.003f, 0.012f }, 0.0627f, false, { "L_Biceps", "L_Biceps_Twist1", nullptr, nullptr } },
            { "L_Forearm", { 0.176f, 0.001f, -0.006f }, { 0.039f, 0.001f, -0.006f }, 0.0505f, false, { "L_Forearm", "L_Elbow", "L_Wrist_Twist", "L_Wrist_Twist1" } },
            { "L_Wrist", { 0.063f, 0.0f, -0.004f }, { 0.015f, 0.0f, -0.004f }, 0.0349f, false, { "L_Wrist", "L_Wrist_Twist2", nullptr, nullptr } },
            { "C_Spine", { 0.004f, 0.004f, 0.053f }, { 0.004f, 0.004f, -0.061f }, 0.1065f, true, { "C_Spine", "C_Spine1", nullptr, nullptr } },
            { "C_Spine2", { 0.030f, 0.008f, 0.065f }, { 0.030f, 0.008f, -0.066f }, 0.1071f, true, { "C_Spine2", "C_Spine1", nullptr, nullptr } },
            { "C_Chest", { 0.155f, 0.010f, 0.037f }, { 0.155f, 0.010f, -0.034f }, 0.1488f, true, { "C_Chest", "L_Clavicle", "R_Clavicle", nullptr } },
            { "C_Neck", { 0.060f, 0.007f, 0.0f }, { 0.037f, 0.007f, 0.0f }, 0.0719f, false, { "C_Neck_Twist", "C_Neck1", nullptr, nullptr } },
            { "C_Head", { 0.058f, 0.030f, 0.0f }, { 0.011f, 0.030f, 0.0f }, 0.1015f, false, { "C_Head", nullptr, nullptr, nullptr } },
        };
        constexpr int kParts = static_cast<int>(sizeof(kBodyCapsules) / sizeof(kBodyCapsules[0]));

        // The bare body's skin spheres on those bones (centre in the bone's own frame and radius, m; sex 0 a man, 1 a woman), read from
        // naked_m/f.nif and hands_3rd_m/f.nif (Meshes01 #563, #560, #559, #558) by a helper.
        struct BareSphere
        {
            const char* bone;
            int         sex;
            float       c[3];
            float       r;
        };
        constexpr BareSphere kBareSpheres[] = {
            { "C_Chest", 0, { 0.105f, 0.008f, -0.004f }, 0.258f }, // naked_m
            { "C_Neck_Twist", 0, { -0.024f, -0.006f, -0.005f }, 0.180f }, // naked_m
            { "C_Neck1", 0, { -0.051f, -0.027f, -0.001f }, 0.108f }, // naked_m
            { "L_Clavicle", 0, { 0.118f, -0.016f, -0.044f }, 0.217f }, // naked_m
            { "C_Spine", 0, { 0.000f, 0.003f, 0.000f }, 0.203f }, // naked_m
            { "C_Spine1", 0, { 0.010f, 0.003f, 0.000f }, 0.250f }, // naked_m
            { "C_Spine2", 0, { 0.034f, 0.001f, 0.000f }, 0.246f }, // naked_m
            { "R_Clavicle", 0, { 0.118f, 0.016f, -0.044f }, 0.213f }, // naked_m
            { "L_Biceps_Twist1", 0, { -0.016f, -0.002f, 0.006f }, 0.168f }, // naked_m
            { "L_Biceps", 0, { 0.205f, 0.005f, -0.002f }, 0.131f }, // naked_m
            { "R_Biceps_Twist1", 0, { -0.022f, 0.001f, 0.010f }, 0.166f }, // naked_m
            { "R_Biceps", 0, { 0.205f, -0.004f, -0.007f }, 0.130f }, // naked_m
            { "L_Elbow", 0, { 0.001f, 0.036f, 0.000f }, 0.103f }, // naked_m
            { "L_Forearm", 0, { 0.007f, -0.002f, 0.002f }, 0.065f }, // naked_m
            { "L_Wrist_Twist", 0, { -0.010f, 0.000f, -0.009f }, 0.060f }, // naked_m
            { "L_Wrist_Twist1", 0, { -0.089f, -0.001f, -0.010f }, 0.050f }, // naked_m
            { "R_Elbow", 0, { -0.001f, -0.037f, -0.005f }, 0.108f }, // naked_m
            { "R_Forearm", 0, { 0.007f, 0.002f, 0.002f }, 0.065f }, // naked_m
            { "R_Wrist_Twist", 0, { -0.011f, 0.000f, -0.009f }, 0.058f }, // naked_m
            { "R_Wrist_Twist1", 0, { -0.090f, 0.001f, -0.009f }, 0.050f }, // naked_m
            { "R_Forearm", 0, { 0.097f, -0.007f, -0.001f }, 0.089f }, // hands_3rd_m
            { "R_Wrist_Twist", 0, { 0.079f, -0.001f, -0.007f }, 0.109f }, // hands_3rd_m
            { "R_Wrist_Twist1", 0, { 0.004f, 0.001f, -0.011f }, 0.124f }, // hands_3rd_m
            { "R_Wrist_Twist2", 0, { -0.032f, -0.003f, -0.007f }, 0.095f }, // hands_3rd_m
            { "R_Biceps", 0, { 0.312f, 0.016f, 0.001f }, 0.051f }, // hands_3rd_m
            { "R_Elbow", 0, { 0.005f, 0.042f, 0.001f }, 0.054f }, // hands_3rd_m
            { "R_Wrist", 0, { 0.043f, 0.000f, -0.006f }, 0.076f }, // hands_3rd_m
            { "L_Wrist", 0, { 0.043f, 0.000f, -0.006f }, 0.076f }, // hands_3rd_m
            { "L_Wrist_Twist2", 0, { -0.032f, 0.003f, -0.007f }, 0.095f }, // hands_3rd_m
            { "L_Wrist_Twist1", 0, { 0.004f, -0.001f, -0.011f }, 0.123f }, // hands_3rd_m
            { "L_Wrist_Twist", 0, { 0.080f, 0.002f, -0.007f }, 0.108f }, // hands_3rd_m
            { "L_Forearm", 0, { 0.098f, 0.007f, -0.001f }, 0.089f }, // hands_3rd_m
            { "L_Elbow", 0, { 0.005f, -0.042f, 0.001f }, 0.054f }, // hands_3rd_m
            { "L_Biceps", 0, { 0.312f, -0.016f, 0.001f }, 0.051f }, // hands_3rd_m
            { "C_Chest", 1, { 0.093f, 0.021f, 0.000f }, 0.251f }, // naked_f
            { "C_Neck_Twist", 1, { -0.017f, -0.008f, 0.000f }, 0.143f }, // naked_f
            { "L_Clavicle", 1, { 0.124f, -0.036f, -0.041f }, 0.213f }, // naked_f
            { "C_Spine", 1, { 0.023f, 0.004f, 0.000f }, 0.193f }, // naked_f
            { "C_Spine1", 1, { 0.017f, 0.009f, 0.000f }, 0.203f }, // naked_f
            { "C_Spine2", 1, { 0.034f, 0.017f, 0.000f }, 0.208f }, // naked_f
            { "L_Biceps_Twist1", 1, { -0.037f, 0.002f, 0.017f }, 0.167f }, // naked_f
            { "L_Biceps", 1, { 0.208f, 0.004f, 0.004f }, 0.121f }, // naked_f
            { "R_Clavicle", 1, { 0.124f, 0.036f, -0.041f }, 0.214f }, // naked_f
            { "R_Biceps_Twist1", 1, { -0.037f, -0.002f, 0.016f }, 0.167f }, // naked_f
            { "R_Biceps", 1, { 0.209f, -0.004f, 0.004f }, 0.121f }, // naked_f
            { "R_Elbow", 1, { -0.010f, -0.008f, -0.004f }, 0.080f }, // naked_f
            { "R_Forearm", 1, { 0.007f, 0.007f, 0.003f }, 0.055f }, // naked_f
            { "R_Wrist_Twist", 1, { -0.014f, 0.010f, -0.001f }, 0.047f }, // naked_f
            { "L_Elbow", 1, { -0.010f, 0.008f, -0.004f }, 0.079f }, // naked_f
            { "L_Forearm", 1, { 0.007f, -0.006f, 0.004f }, 0.055f }, // naked_f
            { "L_Wrist_Twist", 1, { -0.014f, -0.010f, -0.001f }, 0.047f }, // naked_f
            { "R_Wrist_Twist2", 1, { -0.041f, 0.004f, -0.003f }, 0.099f }, // hands_3rd_f
            { "R_Biceps", 1, { 0.309f, 0.008f, 0.002f }, 0.044f }, // hands_3rd_f
            { "R_Forearm", 1, { 0.135f, 0.001f, -0.002f }, 0.118f }, // hands_3rd_f
            { "R_Wrist_Twist", 1, { 0.068f, 0.004f, -0.001f }, 0.093f }, // hands_3rd_f
            { "R_Wrist_Twist1", 1, { 0.014f, 0.002f, -0.004f }, 0.093f }, // hands_3rd_f
            { "R_Elbow", 1, { 0.007f, 0.046f, -0.009f }, 0.037f }, // hands_3rd_f
            { "R_Wrist", 1, { 0.041f, 0.001f, -0.006f }, 0.076f }, // hands_3rd_f
            { "L_Wrist", 1, { 0.040f, -0.002f, -0.006f }, 0.076f }, // hands_3rd_f
            { "L_Wrist_Twist2", 1, { -0.041f, -0.004f, -0.003f }, 0.099f }, // hands_3rd_f
            { "L_Forearm", 1, { 0.135f, -0.001f, -0.002f }, 0.118f }, // hands_3rd_f
            { "L_Wrist_Twist1", 1, { 0.014f, -0.002f, -0.004f }, 0.093f }, // hands_3rd_f
            { "L_Wrist_Twist", 1, { 0.068f, -0.004f, -0.001f }, 0.094f }, // hands_3rd_f
            { "L_Biceps", 1, { 0.309f, -0.008f, 0.002f }, 0.044f }, // hands_3rd_f
            { "L_Elbow", 1, { 0.007f, -0.047f, -0.008f }, 0.037f }, // hands_3rd_f
        };

        constexpr float kClothMin   = 0.010f; // always this much over the body's capsule (thin cloth; the mesh is rounder than a capsule)
        constexpr float kClothMax   = 0.040f; // the clothes' margin at most (a bounding sphere overstates: a loose piece's far corner;
                                              // fix181: was 6 cm)
        constexpr float kClothShare = 0.6f;   // (fix181) the share of the spheres' difference taken as the clothes' thickness
        constexpr float kHeadBare   = 0.040f; // the head (no bare spheres: the face is made per person): its capsule plus this counts as bare
        constexpr int   kWalkBudget = 3000;   // nodes of their 3D looked at, at most (once per hold; the review: was 1500)

        // The game's types, each checked by its RTTI name at Install (0 when not as in 1.16.236: the table's capsules then, no clothes).
        // A body's record (as HandGrab reads them): its shape at +0x78, its collision object at +0x98; its collision object
        // (bhkNPCollisionObject, its group setter in slot 65 at rva 2379a60): the node it moves at +0x10, its body's index at +0x28. A
        // capsule (hknpCapsuleShape, a convex shape): its radius at +0x20, its points (hkFloat3) at the int offset at +0x3C from there, how
        // many at +0x40 (two in skeleton.nif). A mesh (BSGeometry): its skin at +0x1B0 (the setter at 0x142BDB380 resets the mesh's own
        // transform with it); a skin (BSSkin::Instance, its link at 0x142BFB930): how many bones at +0x10, their nodes at +0x18, its
        // BoneData at +0x30; a BoneData (its load at 0x142BFAA00): how many at +0x10, its entries at +0x18, 0x50 each, the sphere's centre
        // at +0 and its radius at +0xC (the bone's own frame), the skin-to-bone transform after.
        constexpr uintptr_t kCapsuleShapeVtRva = 0x3bbeac0;
        constexpr uintptr_t kSkinInstanceVtRva = 0x4ef64f0;
        constexpr uintptr_t kSkinBoneDataVtRva = 0x4ef6330;
        constexpr uintptr_t kBodyShape         = 0x78;
        constexpr uintptr_t kBodyOwner         = 0x98;
        constexpr uintptr_t kCollisionNode     = 0x10;
        constexpr uintptr_t kCollisionIndex    = 0x28;
        constexpr size_t    kSetGroupSlot      = 65;
        constexpr uintptr_t kSetGroupRva       = 0x2379a60;
        constexpr uintptr_t kShapeRadius       = 0x20;
        constexpr uintptr_t kShapeCorners      = 0x3c;
        constexpr uintptr_t kGeometrySkin      = 0x1b0;
        constexpr uintptr_t kSkinCount         = 0x10;
        constexpr uintptr_t kSkinBones         = 0x18;
        constexpr uintptr_t kSkinData          = 0x30;
        constexpr uintptr_t kBoneDataCount     = 0x10;
        constexpr uintptr_t kBoneDataItems     = 0x18;
        constexpr uintptr_t kBoneDataSize      = 0x50;
        uintptr_t g_capsule_vt{ 0 }, g_skin_vt{ 0 }, g_bone_data_vt{ 0 };

        // A type table's class (its RTTI object locator just before it, the type's decorated name), as `decorated` (".?AV...@@").
        bool VtableNamed(uintptr_t vt, const char* decorated)
        {
            uintptr_t   col = 0;
            int32_t     td  = 0;
            char        name[64]{};
            const size_t len = std::strlen(decorated);
            return InImage(vt) && Read(vt - 8, col) && InImage(col) && Read(col + 0x0c, td) && td > 0 && len + 1 <= sizeof(name) &&
                   SafeRead(name, Base() + static_cast<uint32_t>(td) + 0x10, len + 1) && std::memcmp(name, decorated, len + 1) == 0;
        }

        // A node's name, copied under a guard ("" when it cannot be read).
        void NodeNameGuarded(uintptr_t node, char* out, size_t n)
        {
            out[0] = 0;
            __try {
                const char* s = reinterpret_cast<const RE::NiAVObject*>(node)->name.c_str();
                size_t      i = 0;
                for (; s != nullptr && i + 1 < n && s[i] != 0; ++i) {
                    out[i] = s[i];
                }
                out[i] = 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out[0] = 0;
            }
        }

        bool IsGeometryGuarded(uintptr_t object)
        {
            __try {
                return object != 0 && utility::rtti::derives_from(reinterpret_cast<const void*>(object), "class BSGeometry");
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
        {
            const glm::vec3 ab = b - a;
            const float     k  = glm::dot(ab, ab) > 1e-8f ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            return a + ab * k;
        }

        // From a point inside a capsule (a to b, `radius` thick), along `way`: where it comes out (a capsule is convex: once).
        glm::vec3 ExitAlong(const glm::vec3& p, const glm::vec3& way, const glm::vec3& a, const glm::vec3& b, float radius)
        {
            float lo = 0.0f, hi = 2.0f * radius + glm::length(b - a) + 0.05f;
            for (int i = 0; i < 30; ++i) {
                const float     mid = 0.5f * (lo + hi);
                const glm::vec3 at  = p + way * mid;
                if (glm::length(at - ClosestOnSegment(at, a, b)) < radius) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return p + way * hi;
        }

        struct BodyPart
        {
            RE::NiAVObject* node{ nullptr };
            glm::vec3       t{ 0.0f };
            glm::mat3       r{ 1.0f };
            float           s{ 1.0f };
            bool            ok{ false };
            glm::vec3       a{ 0.0f }, b{ 0.0f }; // the capsule in the bone's own frame (its scale removed)
            float           radius{ 0.0f };
            bool            live{ false };        // read from the game (else skeleton.nif's numbers)
            RE::NiAVObject* skin[4]{};            // the bones the worn meshes move here, and where they are
            glm::vec3       skin_t[4]{};
            glm::mat3       skin_r[4]{ glm::mat3{ 1.0f }, glm::mat3{ 1.0f }, glm::mat3{ 1.0f }, glm::mat3{ 1.0f } };
            float           skin_s[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
            bool            skin_ok[4]{};
        };
        // (The review: each with the part and slot of kBodyCapsules' skin bones it is on, and where its own bone is: the skin's bone found by
        // its own pointer or name, not the walk's node of that name, which may be an attached item's.)
        struct WornSphere
        {
            RE::NiAVObject* bone{ nullptr };
            int             part{ -1 }, slot{ -1 };
            glm::vec3       c{ 0.0f };
            float           r{ 0.0f };
            glm::vec3       bone_t{ 0.0f };
            glm::mat3       bone_r{ 1.0f };
            float           bone_s{ 1.0f };
        };
        struct BodyModel
        {
            BodyPart                parts[kParts];
            std::vector<WornSphere> worn;
            int                     found{ 0 }, live{ 0 }, nodes{ 0 }, meshes{ 0 }, skins{ 0 };
            const char*             live_why{ nullptr }; // the first reason a capsule was not read from the game
            double                  ms{ 0.0 };
        };

        // The game's own capsule on this bone (its ragdoll body), found from the bone's collision object as the game links them, every
        // step checked; taken only when the body is where the bone is (the ragdoll's bodies are made in their bones' frames: the capsule's
        // points are then in the bone's own frame). False: why.
        bool LiveCapsule(const BodyPart& part, glm::vec3& a, glm::vec3& b, float& radius, const char*& why)
        {
            const uintptr_t node = reinterpret_cast<uintptr_t>(part.node);
            uintptr_t collision = 0, table = 0, setter = 0, owner = 0, system = 0, instance = 0, ids = 0, world18 = 0, bodies = 0, rec_owner = 0, shape = 0,
                      shape_vt = 0;
            int32_t   index = -1, at = 0, corners = 0;
            uint32_t  count = 0, body = 0, records = 0, rid = 0, flags = 0;
            if (g_capsule_vt == 0) {
                why = "the game's capsule type was not found";
                return false;
            }
            if (!Read(node + kNodeCollision, collision) || collision == 0) {
                why = "no collision object on the bone";
                return false;
            }
            if (!Read(collision, table) || !InImage(table) || !Read(table + kSetGroupSlot * sizeof(uintptr_t), setter) || setter != At(kSetGroupRva) ||
                !Read(collision + kCollisionNode, owner) || owner != node) {
                why = "the bone's collision object is not the kind expected";
                return false;
            }
            if (!Read(collision + kCollisionSystem, system) || system == 0 || !Read(collision + kCollisionIndex, index) || !Read(system + kSystemInstance, instance) ||
                instance == 0 || !Read(instance + kInstanceCount, count) || count == 0 || count > 512 || !Read(instance + kInstanceIds, ids) || ids == 0 || index < 0 ||
                static_cast<uint32_t>(index) >= count || !Read(ids + static_cast<uintptr_t>(index) * sizeof(uint32_t), body) || !Read(instance + kInstanceWorld, world18) ||
                world18 < kWorldFromInstance) {
                why = "its body is not in a physics world";
                return false;
            }
            const uintptr_t world = world18 - kWorldFromInstance;
            if (!Read(world + kWorldBodyArray, bodies) || bodies == 0 || !Read(world + kWorldBodyCount, records) || (body & kBodyIndexMask) >= records) {
                why = "its world's bodies could not be read";
                return false;
            }
            const uintptr_t record = bodies + static_cast<uintptr_t>(body & kBodyIndexMask) * kBodySize;
            float           rec[16]{};
            // (Its record's collision object (+0x98, as HandGrab checks a held object's) is not required to be the bone's: a ragdoll's
            // bodies may name their system's; the body's place against the bone, below, is the check.)
            if (!Read(record + kBodyId, rid) || rid != body || !Read(record + kBodyFlags, flags) || (flags & 3u) == 0 || !Read(record + kBodyOwner, rec_owner) ||
                rec_owner == 0 || !SafeRead(rec, record, sizeof(rec)) || !Read(record + kBodyShape, shape) || shape == 0 || !Read(shape, shape_vt)) {
                why = "its body's record did not check out";
                return false;
            }
            if (shape_vt != g_capsule_vt) {
                why = "its body's shape is not a capsule";
                return false;
            }
            float r = 0.0f, pts[6]{};
            if (!Read(shape + kShapeRadius, r) || !Read(shape + kShapeCorners, at) || !Read(shape + kShapeCorners + 4, corners) || corners != 2 || std::abs(at) > 4096 ||
                !SafeRead(pts, static_cast<uintptr_t>(static_cast<intptr_t>(shape + kShapeCorners) + at), sizeof(pts))) {
                why = "its capsule's two points could not be read (not two, or not readable)";
                return false;
            }
            const glm::vec3 pa{ pts[0], pts[1], pts[2] }, pb{ pts[3], pts[4], pts[5] };
            if (!std::isfinite(r) || r < 0.005f || r > 0.4f || !Finite(pa) || !Finite(pb) || glm::length(pa) > 0.6f || glm::length(pb) > 0.6f) {
                why = "its capsule's numbers looked wrong";
                return false;
            }
            // Where the body is against the bone: its turn (Havok's columns, or the other way) within 15 degrees, its place within 8 cm (it
            // follows the animation a step behind).
            const glm::mat3 cols{ glm::vec3{ rec[0], rec[1], rec[2] }, glm::vec3{ rec[4], rec[5], rec[6] }, glm::vec3{ rec[8], rec[9], rec[10] } };
            const glm::vec3 place{ rec[12], rec[13], rec[14] };
            const auto turn_cos = [&](const glm::mat3& m) {
                const glm::mat3 d = glm::transpose(part.r) * m;
                return 0.5f * (d[0][0] + d[1][1] + d[2][2] - 1.0f);
            };
            const float best_cos = std::max(turn_cos(cols), turn_cos(glm::transpose(cols)));
            if (!Finite(place) || !(best_cos > std::cos(glm::radians(15.0f))) || !(glm::length(place - part.t) < 0.08f)) {
                why = "its body is not on the bone";
                return false;
            }
            a      = pa / part.s;
            b      = pb / part.s;
            radius = r / part.s;
            return true;
        }

        // Their parts as the game has them: one walk of their 3D (the parts' bones and the bones their worn meshes move, by name; the
        // worn meshes), each part's capsule (the game's, else skeleton.nif's), and each worn mesh's skin spheres on those bones.
        void BuildBodyModel(uintptr_t root, BodyModel& m)
        {
            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            std::vector<uintptr_t> stack{ root }, meshes;
            stack.reserve(256);
            char name[48]{};
            // (The review: children taken in their own order (pushed last first), the skeleton's bones before an attached item's; a part's
            // bone with a collision object (the ragdoll's) kept over one of the same name without.)
            while (!stack.empty() && m.nodes < kWalkBudget) {
                const uintptr_t node = stack.back();
                stack.pop_back();
                ++m.nodes;
                NodeNameGuarded(node, name, sizeof(name));
                if (name[0] != 0) {
                    for (int i = 0; i < kParts; ++i) {
                        auto& part = m.parts[i];
                        if (std::strcmp(name, kBodyCapsules[i].bone) == 0) {
                            uintptr_t held_collision = 0, new_collision = 0;
                            if (part.node == nullptr) {
                                part.node = reinterpret_cast<RE::NiAVObject*>(node);
                            } else if ((!Read(reinterpret_cast<uintptr_t>(part.node) + kNodeCollision, held_collision) || held_collision == 0) &&
                                       Read(node + kNodeCollision, new_collision) && new_collision != 0) {
                                part.node = reinterpret_cast<RE::NiAVObject*>(node);
                            }
                        }
                        for (int k = 0; k < 4; ++k) {
                            if (kBodyCapsules[i].skin[k] != nullptr && part.skin[k] == nullptr && std::strcmp(name, kBodyCapsules[i].skin[k]) == 0) {
                                part.skin[k] = reinterpret_cast<RE::NiAVObject*>(node);
                            }
                        }
                    }
                }
                if (IsNodeGuarded(node)) {
                    RE::NiAVObject* kids[128]{};
                    const auto      n = body::ReadChildren(reinterpret_cast<const RE::NiAVObject*>(node), kids, 128);
                    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                        stack.push_back(reinterpret_cast<uintptr_t>(kids[i]));
                    }
                } else if (meshes.size() < 64 && IsGeometryGuarded(node)) {
                    meshes.push_back(node);
                }
            }
            for (int i = 0; i < kParts; ++i) {
                auto& part = m.parts[i];
                if (part.node == nullptr || !NodeWorld(part.node, part.t, part.r, part.s)) {
                    continue;
                }
                const auto& def = kBodyCapsules[i];
                const char* why = nullptr;
                part.ok         = true;
                ++m.found;
                if (LiveCapsule(part, part.a, part.b, part.radius, why)) {
                    part.live = true;
                    ++m.live;
                } else {
                    part.a      = glm::vec3{ def.a[0], def.a[1], def.a[2] };
                    part.b      = glm::vec3{ def.b[0], def.b[1], def.b[2] };
                    part.radius = def.r;
                    if (m.live_why == nullptr) {
                        m.live_why = why;
                    }
                }
                for (int k = 0; k < 4; ++k) {
                    part.skin_ok[k] = part.skin[k] != nullptr && NodeWorld(part.skin[k], part.skin_t[k], part.skin_r[k], part.skin_s[k]);
                }
            }
            // The worn meshes' skins: each sphere on one of those bones.
            m.meshes = static_cast<int>(meshes.size());
            if (g_skin_vt != 0 && g_bone_data_vt != 0) {
                std::vector<uintptr_t> bones;
                for (const uintptr_t mesh : meshes) {
                    uintptr_t skin = 0, skin_vt = 0, list = 0, data = 0, data_vt = 0, items = 0;
                    uint32_t  count = 0, data_count = 0;
                    if (!Read(mesh + kGeometrySkin, skin) || skin == 0 || !Read(skin, skin_vt) || skin_vt != g_skin_vt || !Read(skin + kSkinCount, count) || count == 0 ||
                        count > 512 || !Read(skin + kSkinBones, list) || list == 0 || !Read(skin + kSkinData, data) || data == 0 || !Read(data, data_vt) ||
                        data_vt != g_bone_data_vt || !Read(data + kBoneDataCount, data_count) || data_count != count || !Read(data + kBoneDataItems, items) || items == 0) {
                        continue;
                    }
                    bones.assign(count, 0);
                    if (!SafeRead(bones.data(), list, count * sizeof(uintptr_t))) {
                        continue;
                    }
                    ++m.skins;
                    for (uint32_t j = 0; j < count; ++j) {
                        if (bones[j] == 0) {
                            continue;
                        }
                        // The skin's bone by its own name (one name can be on two parts: C_Spine1 is the waist's and the belly's).
                        NodeNameGuarded(bones[j], name, sizeof(name));
                        if (name[0] == 0) {
                            continue;
                        }
                        float     sphere[4]{};
                        glm::vec3 bt{};
                        glm::mat3 br{ 1.0f };
                        float     bs    = 1.0f;
                        bool      read  = false, ok = false;
                        for (int i = 0; i < kParts; ++i) {
                            for (int k = 0; k < 4; ++k) {
                                if (kBodyCapsules[i].skin[k] == nullptr || std::strcmp(name, kBodyCapsules[i].skin[k]) != 0) {
                                    continue;
                                }
                                if (!read) {
                                    read = true;
                                    ok   = SafeRead(sphere, items + static_cast<uintptr_t>(j) * kBoneDataSize, sizeof(sphere)) &&
                                         NodeWorld(reinterpret_cast<RE::NiAVObject*>(bones[j]), bt, br, bs);
                                }
                                const glm::vec3 c{ sphere[0], sphere[1], sphere[2] };
                                if (ok && Finite(c) && std::isfinite(sphere[3]) && sphere[3] > 0.0f && sphere[3] < 2.0f && glm::length(c) < 2.0f && m.worn.size() < 512) {
                                    m.worn.push_back(WornSphere{ reinterpret_cast<RE::NiAVObject*>(bones[j]), i, k, c, sphere[3], bt, br, bs });
                                }
                            }
                        }
                    }
                }
            }
            QueryPerformanceCounter(&t1);
            m.ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(g_qpc_freq);
        }

        // (The review) Parts not kept apart from each other: the head's capsule (round, 0.10 m) covers the jaw and the front of the neck, so a
        // neck's held point was pushed out to it (several cm of air); the head is held by its own surface, the neck by its own.
        bool NotNeighbours(int i, int j)
        {
            const std::string_view a = kBodyCapsules[i].bone, b = kBodyCapsules[j].bone;
            return (a == "C_Neck" && b == "C_Head") || (a == "C_Head" && b == "C_Neck");
        }

        // A part's capsule in the game world (its line and the body's radius).
        void PartWorld(const BodyPart& part, glm::vec3& a, glm::vec3& b, float& radius)
        {
            a      = part.t + part.r * (part.a * part.s);
            b      = part.t + part.r * (part.b * part.s);
            radius = part.radius * part.s;
        }

        // How far their clothes stand out over part i's body at q (on its line), the way n: the worn meshes' spheres on its bones that
        // cover q, their farthest reach that way, against the bare body's spheres there (the smaller of a man's and a woman's: more
        // margin, not less); the head against its capsule plus kHeadBare. kClothMin at least, kClothMax at most. `worn_out`/`bare_out`
        // the reaches (m from q; -1 none) for the log.
        float ClothesAt(const BodyModel& m, int i, const glm::vec3& q, const glm::vec3& n, float body_radius, float* worn_out = nullptr, float* bare_out = nullptr)
        {
            const auto& part = m.parts[i];
            const auto& def  = kBodyCapsules[i];
            float       worn = -1.0f, bare[2]{ -1.0f, -1.0f };
            // A sphere (its bone's own frame) reaching over q: how far it reaches the way n from q (-1: it does not cover q).
            const auto reach = [&](const glm::vec3& t, const glm::mat3& r, float s, const glm::vec3& c_local, float r_local) {
                const glm::vec3 c      = t + r * (c_local * s);
                const float     radius = r_local * s;
                return glm::length(c - q) < radius ? glm::dot(c - q, n) + radius : -1.0f;
            };
            for (const auto& w : m.worn) {
                if (w.part == i) {
                    worn = std::max(worn, reach(w.bone_t, w.bone_r, w.bone_s, w.c, w.r));
                }
            }
            for (int k = 0; k < 4; ++k) {
                if (def.skin[k] == nullptr) {
                    continue;
                }
                // The bare body's spheres on the same bone: where the walk found it, else where a worn skin's bone of that name is.
                const glm::vec3* t = part.skin_ok[k] ? &part.skin_t[k] : nullptr;
                const glm::mat3* r = part.skin_ok[k] ? &part.skin_r[k] : nullptr;
                float            s = part.skin_ok[k] ? part.skin_s[k] : 1.0f;
                for (const auto& w : m.worn) {
                    if (t == nullptr && w.part == i && w.slot == k) {
                        t = &w.bone_t;
                        r = &w.bone_r;
                        s = w.bone_s;
                    }
                }
                if (t == nullptr) {
                    continue;
                }
                for (const auto& b : kBareSpheres) {
                    if (std::strcmp(b.bone, def.skin[k]) == 0) {
                        bare[b.sex] = std::max(bare[b.sex], reach(*t, *r, s, glm::vec3{ b.c[0], b.c[1], b.c[2] }, b.r));
                    }
                }
            }
            float bare_reach = -1.0f;
            if (std::strcmp(def.bone, "C_Head") == 0) {
                bare_reach = body_radius + kHeadBare;
            } else if (bare[0] > 0.0f && bare[1] > 0.0f) {
                bare_reach = std::min(bare[0], bare[1]);
            } else {
                bare_reach = std::max(bare[0], bare[1]);
            }
            if (worn_out != nullptr) {
                *worn_out = worn;
            }
            if (bare_out != nullptr) {
                *bare_out = bare_reach;
            }
            // (fix181) Six tenths of the bounding spheres' difference, 4 cm at most (after fix180 the hand was not quite on them;
            // fix180's log: half the holds at the 6 cm cap: a loose piece's sphere reaches its far corner, not the cloth where you hold).
            const float margin = worn > 0.0f && bare_reach > 0.0f ? std::clamp(kClothShare * (worn - bare_reach), 0.0f, kClothMax) : 0.0f;
            return std::max(kClothMin, margin);
        }

        void EndLiveHold(const char* why, int64_t now)
        {
            if (!g_live.active || g_live.end_us != 0) {
                return;
            }
            g_live.end_us = now;
            g_live_hand[g_live.hand] = false;
            // (fix177, the review: the contact kept through the ease-out, its weight going down with the bend's (UpdateLiveHold), so the
            // drawn hand eases back off them instead of jumping back by its shift and turn in one frame; cleared as the ease-out ends.)
            int   seen = 0, bent = 0;
            float asked = 0.0f, given = 0.0f;
            body::HeldPersonBendCounts(seen, bent, asked, given);
            if (Tell(g_told_hold, 60)) {
                spdlog::info("[Physical] (fix173) {:08X} let go ({}) after {:.1f} s held by the {}: {} moves asked ({:.2f} m in all); their model bent on {} of {} "
                             "updates (the last {:.2f} of {:.2f} m asked)",
                    g_live.id, why, (now - g_live.start_us) * 1e-6, g_live.bone, g_live.moves, g_live.asked_drag, bent, seen, given, asked);
            }
        }

        // The grip on a standing person (GripGrab, main frame step): held from here (true), or not (refused, logged).
        // (fix176) reach: how far off their surface the palm may be (kHoldReach; wider just after a pull-up: they rose away from the palm).
        bool StartLiveHold(uintptr_t ref, const Person& p, int hand, int64_t now, float reach = kHoldReach)
        {
            const bool  ours   = p.teammate || p.commanded;
            // (The review: not while the last hold eases out either: BodyIK puts one model's bones back at a time.)
            const char* refuse = HoldRefusal(p) != nullptr ? HoldRefusal(p) : g_live.active ? (g_live.end_us == 0 ? "someone is held already" : "the last hold is being let go") : nullptr;
            glm::vec3   palm{ 0.0f };
            if (refuse == nullptr && !PalmPoint(hand == 0, palm)) {
                refuse = "your hand is not tracked";
            }
            auto* root = reinterpret_cast<RE::NiAVObject*>(p.root);
            if (refuse == nullptr && root == nullptr) {
                refuse = "no 3D";
            }
            // (fix179) The part of them nearest your palm, by their outer surface: the game's own body capsule of each part (an upper
            // arm, a forearm, a hand, the waist, the belly, the chest, the neck, the head), made bigger by the clothes over it. The part
            // whose surface is nearest your palm (your palm inside more than one: the one it is least deep in). The held point: on that
            // surface where your palm is nearest; your palm inside it: where your palm came in (the line back out of your palm, as HIGGS
            // takes the point on the person's mesh on your palm's line), else straight out; then out of any part next to it too.
            // (Before: round segments from bone to bone, 0.03-0.14 m thick, and with your palm inside one the held point was your palm.)
            const char*     best_bone   = nullptr;
            float           best        = reach;
            float           best_d      = 0.0f;
            float           best_depth  = 0.0f;
            glm::vec3       surface_at{ 0.0f };
            RE::NiAVObject* best_node   = nullptr;
            int             best_part   = -1;
            float           best_thick  = 0.05f;
            glm::vec3       best_normal{ 0.0f, 0.0f, 1.0f };
            float           best_margin = 0.0f, best_worn = -1.0f, best_bare = -1.0f, palm_signed = 0.0f;
            const char*     moved_how   = "onto it (your palm outside them)";
            const char*     pushed_by   = nullptr;
            glm::vec3       hand_surface{ 0.0f }, hand_facing{ 0.0f }, hand_forward{ 0.0f }, hand_side{ 0.0f };
            const bool      have_frame = refuse == nullptr && HandGrab::HoldFrame(hand == 0, hand_surface, hand_facing, hand_forward, hand_side) && Finite(hand_facing) &&
                                    glm::length(hand_facing) > 0.5f && Finite(hand_forward) && Finite(hand_side);
            BodyModel       body_model;
            if (refuse == nullptr) {
                BuildBodyModel(p.root, body_model);
                for (int i = 0; i < kParts; ++i) {
                    const BodyPart& part = body_model.parts[i];
                    if (!part.ok) {
                        continue;
                    }
                    glm::vec3 a{}, b{};
                    float     body_r = 0.0f;
                    PartWorld(part, a, b, body_r);
                    const glm::vec3 q      = ClosestOnSegment(palm, a, b);
                    const float     d      = glm::length(palm - q);
                    const glm::vec3 n      = d > 1e-4f ? (palm - q) / d : (have_frame ? -glm::normalize(hand_facing) : glm::vec3{ 0.0f, 0.0f, 1.0f });
                    const float     margin = ClothesAt(body_model, i, q, n, body_r);
                    const float     outer  = body_r + margin;
                    // (The review) A limb within 2 cm of your palm counts as touched before the trunk around it: the chest's near-round
                    // capsule covers the shoulders, and a grab on the upper arm took the chest.
                    const float     prefer = kBodyCapsules[i].flat ? 0.0f : 0.02f;
                    const float     gap    = std::max(0.0f, d - outer - prefer);
                    const float     depth  = std::max(0.0f, outer - d - prefer);
                    if (gap < best || (gap <= 0.0f && best <= 0.0f && best_bone != nullptr && depth < best_depth)) {
                        best       = gap;
                        best_d     = d;
                        best_depth = depth;
                        best_bone  = kBodyCapsules[i].bone;
                        best_node  = part.node;
                        best_part  = i;
                    }
                }
                if (best_bone == nullptr) {
                    refuse = reach > kHoldReach + 0.01f ? "your palm is not near them (within 0.9 m of their arms, body or head, after they stood)" : "your palm is not on them (within 0.12 m of their arms, body or head)";
                }
            }
            if (refuse == nullptr && best_part >= 0) {
                const BodyPart& part = body_model.parts[best_part];
                glm::vec3       a{}, b{};
                float           body_r = 0.0f;
                PartWorld(part, a, b, body_r);
                const glm::vec3 q     = ClosestOnSegment(palm, a, b);
                const float     d     = glm::length(palm - q);
                const glm::vec3 n     = d > 1e-4f ? (palm - q) / d : (have_frame ? -glm::normalize(hand_facing) : glm::vec3{ 0.0f, 0.0f, 1.0f });
                best_margin           = ClothesAt(body_model, best_part, q, n, body_r, &best_worn, &best_bare);
                const float outer     = body_r + best_margin;
                palm_signed           = d - outer;
                glm::vec3   held      = q + n * outer;
                if (d < outer) {
                    const glm::vec3 back = have_frame ? -glm::normalize(hand_facing) : n;
                    if (have_frame && (d < 0.01f || glm::dot(back, n) > 0.3f)) {
                        held      = ExitAlong(palm, back, a, b, outer);
                        moved_how = "out where your palm came in (back along your palm's line)";
                    } else {
                        moved_how = "straight out (your palm faced away from where it came in)";
                    }
                }
                const glm::vec3 qh = ClosestOnSegment(held, a, b);
                glm::vec3       normal = glm::length(held - qh) > 1e-4f ? glm::normalize(held - qh) : n;
                // Out of any part next to it too (an arm against the trunk), twice at most; not by more than 6 cm (deeper, where two parts
                // overlap much, the held point stays and your drawn hand is seated out of both instead: BodyIK's HoldPersonShift).
                for (int pass = 0; pass < 2; ++pass) {
                    for (int j = 0; j < kParts; ++j) {
                        const BodyPart& other = body_model.parts[j];
                        if (j == best_part || !other.ok || NotNeighbours(best_part, j)) {
                            continue;
                        }
                        glm::vec3 oa{}, ob{};
                        float     other_r = 0.0f;
                        PartWorld(other, oa, ob, other_r);
                        const glm::vec3 oq = ClosestOnSegment(held, oa, ob);
                        const float     od = glm::length(held - oq);
                        const glm::vec3 on = od > 1e-4f ? (held - oq) / od : normal;
                        // (The review: the part next to it by its body only, 2 cm less, as BodyIK seats your hand against it.)
                        const float     o_outer = std::max(0.01f, other_r - 0.02f);
                        if (od < o_outer - 0.002f && o_outer - od < 0.06f) {
                            held      = oq + on * o_outer;
                            normal    = on;
                            pushed_by = kBodyCapsules[j].bone;
                        }
                    }
                }
                surface_at  = held;
                best_normal = normal;
                best_thick  = outer;
            }
            glm::vec3 bone_t{};
            glm::mat3 bone_r{ 1.0f };
            float     bone_s = 1.0f;
            uintptr_t model  = 0;
            float     heading = 0.0f;
            if (refuse == nullptr && (!NodeWorld(best_node, bone_t, bone_r, bone_s) || !Read(p.root + kModelNode, model) || model == 0 ||
                                      !Read(ref + kRefHeading, heading) || !std::isfinite(heading))) {
                refuse = "their bones could not be read";
            }
            if (refuse != nullptr) {
                haptics::MeleePulse(hand == 0, 0.12f, 0.03f); // (fix174: a faint tick: not held; the review)
                if (Tell(g_told_hold, 60)) {
                    spdlog::info("[Physical] (fix173) your {} grip on {:08X} (standing): not held ({})", hand == 0 ? "left" : "right", p.id, refuse);
                }
                return false;
            }
            g_live            = LiveHold{};
            g_live.active     = true;
            g_live.handle     = p.handle;
            g_live.id         = p.id;
            g_live.hand       = hand;
            std::snprintf(g_live.bone, sizeof(g_live.bone), "%s", best_bone);
            // (fix174: the held point on their surface, not your palm's place in the air.)
            g_live.held_local = glm::transpose(bone_r) * (surface_at - bone_t) / bone_s;
            // (Their heading turns clockwise, as the game's: forward = (sin h, cos h); TurnZ turns the other way: into their own frame with
            // +heading, back out with -heading. The review.)
            // (fix176, the review: from your palm as it took them, not their surface: the gap between the two (up to 0.45 m after a pull-up)
            // would be pulled out at once by the drag; the bend and your drawn hand close it.)
            g_live.anchor     = TurnZ(palm - p.place, heading);
            // (fix177) Just got up (the pull-up: the reach widened): from their surface, so they are pulled to your hand (the gap closed by
            // the drag's capped catch-up, a quarter of a second for 0.9 m).
            if (reach > kHoldReach + 0.01f) {
                g_live.anchor = TurnZ(surface_at - p.place, heading);
            }
            g_live.bone_node  = best_node;
            g_live.root       = p.root;
            g_live.thick      = best_thick;
            // (fix179) The held part's outer surface and the way out at the held point, in the held bone's own frame; the parts near it
            // (their outer surface within 0.2 m of the held point, nearest first) for your drawn hand to be kept out of them too.
            {
                const BodyPart& part = body_model.parts[best_part];
                g_live.cap_a        = part.a;
                g_live.cap_b        = part.b;
                g_live.cap_r        = part.radius;
                g_live.margin       = best_margin;
                g_live.normal_local = glm::transpose(bone_r) * best_normal;
                g_live.flat         = kBodyCapsules[best_part].flat;
                struct Near
                {
                    int   part;
                    float gap;
                };
                std::vector<Near> near_parts;
                for (int j = 0; j < kParts; ++j) {
                    const BodyPart& other = body_model.parts[j];
                    if (j == best_part || !other.ok || NotNeighbours(best_part, j)) {
                        continue;
                    }
                    glm::vec3 oa{}, ob{};
                    float     other_r = 0.0f;
                    PartWorld(other, oa, ob, other_r);
                    const glm::vec3 oq = ClosestOnSegment(surface_at, oa, ob);
                    const float     od = glm::length(surface_at - oq);
                    const glm::vec3 on = od > 1e-4f ? (surface_at - oq) / od : best_normal;
                    const float     om = ClothesAt(body_model, j, oq, on, other_r);
                    if (od - (other_r + om) < 0.2f) {
                        near_parts.push_back(Near{ j, od - (other_r + om) });
                    }
                }
                std::sort(near_parts.begin(), near_parts.end(), [](const Near& x, const Near& y) { return x.gap < y.gap; });
                for (const Near& n : near_parts) {
                    if (g_live.near_count >= 3) {
                        break;
                    }
                    const BodyPart& other = body_model.parts[n.part];
                    glm::vec3       oa{}, ob{};
                    float           other_r = 0.0f;
                    PartWorld(other, oa, ob, other_r);
                    const glm::vec3 oq = ClosestOnSegment(surface_at, oa, ob);
                    const float     od = glm::length(surface_at - oq);
                    const glm::vec3 on = od > 1e-4f ? (surface_at - oq) / od : best_normal;
                    const int       k  = g_live.near_count++;
                    g_live.near_node[k]   = other.node;
                    g_live.near_a[k]      = other.a;
                    g_live.near_b[k]      = other.b;
                    g_live.near_r[k]      = other.radius;
                    g_live.near_margin[k] = ClothesAt(body_model, n.part, oq, on, other_r);
                }
                // Your palm began inside them (not after a pull-up: they rose away from it): the bend aims at your palm moved out by that
                // much, kept in your hand's own frame, so the hold does not begin by pushing them back.
                if (palm_signed < 0.0f && have_frame && reach <= kHoldReach + 0.01f) {
                    const glm::vec3 out = surface_at - palm;
                    g_live.bend_offset  = glm::vec3{ glm::dot(out, glm::normalize(hand_facing)), glm::dot(out, glm::normalize(hand_forward)),
                        glm::dot(out, glm::normalize(hand_side)) };
                }
                g_live.bend_target = palm + (surface_at - palm) * (glm::length(g_live.bend_offset) > 0.0f ? 1.0f : 0.0f);
            }
            static int holds  = 0;
            g_live.grab       = ++holds;
            g_live.start_place = p.place;
            g_live.palm       = palm;
            g_live.start_us   = now;
            g_live.anger_us   = now;
            g_live.log_us     = now;
            g_live.model      = model;
            g_live.angers     = !ours;
            g_live_hand[hand] = true;
            HandGrab::MarkBusy(hand == 0);
            haptics::MeleePulse(hand == 0, 0.35f, 0.05f);
            if (Tell(g_told_hold, 60)) {
                spdlog::info("[Physical] (fix174) your {} grip holds {:08X} (standing) by the {} (your palm {:.2f} m off their surface, {:.2f} m from its bone): no "
                             "knockdown; their {} bent toward your hand and your hand drawn on them, {}{}",
                    hand == 0 ? "left" : "right", p.id, best_bone, best, best_d, std::string_view{ best_bone }.substr(0, 2) == "C_" ? "body" : "arm",
                    g.move ? "dragged where you pull past 0.04 m" : "not dragged (the queued move not found)", ours ? " (your follower: no anger)" : "");
                const BodyPart& part = body_model.parts[best_part];
                spdlog::info("[Physical] (fix179) holding {:08X} by the {}: its body as the game has it ({}: {:.3f} m thick), clothes {:.3f} m over it (worn "
                             "reach {:.3f}, bare {:.3f}; {} meshes, {} skins, {} spheres on the parts' bones); your palm {:.3f} m {} that surface: the held "
                             "point put {}{}{}, {:.3f} m from your palm; {} parts found ({} read from the game{}{}), {} near it; {} nodes looked at in {:.2f} ms",
                    p.id, best_bone, part.live ? "its ragdoll capsule, read from the game" : "skeleton.nif's capsule", part.radius * part.s, best_margin,
                    best_worn, best_bare, body_model.meshes, body_model.skins, body_model.worn.size(), std::abs(palm_signed),
                    palm_signed > 0.0f ? "off" : "inside", moved_how, pushed_by != nullptr ? ", then out of the " : "", pushed_by != nullptr ? pushed_by : "",
                    glm::length(surface_at - palm), body_model.found, body_model.live, body_model.live_why != nullptr ? "; not read: " : "",
                    body_model.live_why != nullptr ? body_model.live_why : "", g_live.near_count, body_model.nodes, body_model.ms);
            }
            return true;
        }

        // One frame of a hold, inside the person's reference's hold (WithReference): nullptr while held on, else why it ends.
        const char* HoldOneFrame(uintptr_t ref, int64_t now, float dt)
        {
            Person p;
            if (!ReadPerson(ref, p)) {
                return "they are gone";
            }
            if (Dead(p) || KnockedDown(p)) {
                return Dead(p) ? "they died" : "they were knocked down";
            }
            if (const char* no = HoldRefusal(p)) {
                return no;
            }
            uintptr_t model = 0;
            if (p.root == 0 || !Read(p.root + kModelNode, model) || model == 0) {
                return "their 3D is gone";
            }
            if (p.root != g_live.root) {
                return "their 3D changed";
            }
            glm::vec3 palm{ 0.0f };
            float     heading = 0.0f;
            if (!PalmPoint(g_live.hand == 0, palm) || !Read(ref + kRefHeading, heading) || !std::isfinite(heading)) {
                return "your hand or their heading could not be read";
            }
            // (fix174) The held point as last drawn (their bone after the bend), for your drawn hand to be put on.
            {
                glm::vec3 t{};
                glm::mat3 r{ 1.0f };
                float     s = 1.0f;
                if (NodeWorld(g_live.bone_node, t, r, s)) {
                    glm::vec3 contact = t + r * (g_live.held_local * s);
                    // (fix179) The held part's outer surface (the game's body capsule and the clothes over it) and the way out of them here.
                    const glm::vec3 a      = t + r * (g_live.cap_a * s);
                    const glm::vec3 b      = t + r * (g_live.cap_b * s);
                    const float     outer  = g_live.cap_r * s + g_live.margin;
                    glm::vec3       normal = r * g_live.normal_local;
                    normal                 = glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3{ 0.0f, 0.0f, 1.0f };
                    glm::vec3       axis   = glm::length(b - a) > 1e-3f ? glm::normalize(b - a) : glm::normalize(r[0]);
                    // (fix179) A frame late: this is where they were drawn last frame, your hand is drawn after their next update. Where the
                    // held point came out toward you since the last frame, it is moved on by that much (5 cm at most; a jump of 15 cm or more:
                    // nothing), so your drawn hand is not left inside them while you pull them; never moved into them. (The review: smoothed,
                    // half of each frame's, so a frame's uneven step does not shake the hand.)
                    float raw_lead = 0.0f;
                    if (g_live.have_last_contact && Finite(g_live.last_contact)) {
                        const glm::vec3 moved = contact - g_live.last_contact;
                        if (glm::length(moved) < 0.15f) {
                            raw_lead = std::clamp(glm::dot(moved, normal), 0.0f, 0.05f);
                        }
                    }
                    g_live.lead              = 0.5f * g_live.lead + 0.5f * raw_lead;
                    const float lead         = std::isfinite(g_live.lead) ? std::clamp(g_live.lead, 0.0f, 0.05f) : 0.0f;
                    g_live.last_contact      = contact;
                    g_live.have_last_contact = true;
                    g_live.lead_sum += lead;
                    g_live.lead_max = std::max(g_live.lead_max, lead);
                    ++g_live.lead_frames;
                    contact += normal * lead;
                    const glm::vec3 centre = ClosestOnSegment(contact, a + normal * lead, b + normal * lead);
                    // The held part first, then the parts near it (each as last drawn, moved on as the held point was; the review: their
                    // bodies only, not made bigger by their clothes, and 2 cm less: the trunk's near-round capsules overlap the parts next to
                    // them (the chest's covers the shoulders), and a hand on the upper arm was seated out of the chest into the air).
                    // (The review: worked out before the contact's lock is taken: NodeWorld reads through system calls.)
                    int       caps = 1;
                    glm::vec3 cap_a[4]{ a + normal * lead }, cap_b[4]{ b + normal * lead };
                    float     cap_r[4]{ outer };
                    for (int k = 0; k < g_live.near_count && caps < 4; ++k) {
                        glm::vec3 nt{};
                        glm::mat3 nr{ 1.0f };
                        float     ns = 1.0f;
                        if (!NodeWorld(g_live.near_node[k], nt, nr, ns)) {
                            continue;
                        }
                        cap_a[caps] = nt + nr * (g_live.near_a[k] * ns) + normal * lead;
                        cap_b[caps] = nt + nr * (g_live.near_b[k] * ns) + normal * lead;
                        cap_r[caps] = std::max(0.01f, g_live.near_r[k] * ns - 0.02f);
                        ++caps;
                    }
                    if (Finite(contact) && Finite(centre) && Finite(axis) && Finite(normal)) {
                        std::scoped_lock lock{ g_contact_mutex };
                        g_contact        = contact;
                        g_contact_hand   = g_live.hand;
                        g_contact_us     = now;
                        g_contact_weight = std::min(1.0f, static_cast<float>(now - g_live.start_us) / static_cast<float>(kHoldEaseUs));
                        g_contact_centre = centre;
                        g_contact_axis   = axis;
                        g_contact_radius = outer;
                        g_contact_grab   = g_live.grab;
                        g_contact_normal = normal;
                        g_contact_flat   = g_live.flat;
                        g_contact_caps   = caps;
                        for (int c = 0; c < caps; ++c) {
                            g_contact_cap_a[c] = cap_a[c];
                            g_contact_cap_b[c] = cap_b[c];
                            g_contact_cap_r[c] = cap_r[c];
                        }
                        g_contact_bone_t = t + normal * lead; // (fix180)
                        g_contact_bone_r = r;
                        g_contact_bone_s = s;
                        g_contact_model  = model; // (fix181)
                        g_contact_bare_r = g_live.cap_r * s;
                        std::snprintf(g_contact_bone_name, sizeof(g_contact_bone_name), "%s", g_live.bone);
                    }
                }
            }
            // (fix179) Where their held part is bent to: your palm, moved out (in your hand's own frame) by the way your palm began inside them.
            // (The review: a frame without your hand's frame keeps the last one: the bare palm would yank their part 5-25 cm for a frame.)
            if (glm::length(g_live.bend_offset) <= 0.0f) {
                g_live.bend_target = palm;
            } else {
                glm::vec3 surface{}, facing{}, forward{}, side{};
                if (HandGrab::HoldFrame(g_live.hand == 0, surface, facing, forward, side) && Finite(facing) && Finite(forward) && Finite(side)) {
                    g_live.bend_target = palm + facing * g_live.bend_offset.x + forward * g_live.bend_offset.y + side * g_live.bend_offset.z;
                }
            }
            // Where the held point is, as they stand now (their place and heading, the animation aside), against your hand.
            const glm::vec3 anchor = p.place + TurnZ(g_live.anchor, -heading);
            const glm::vec3 pull   = palm - anchor;
            g_live.pull            = glm::length(pull);
            g_live.far_frames      = g_live.pull > kAutoDrop && now - g_live.start_us > kAutoDropAfterUs ? g_live.far_frames + 1 : 0;
            if (g_live.far_frames >= kAutoDropFrames) {
                return "they were 0.7 m from your hand (HIGGS's auto-drop)";
            }
            g_live.palm   = palm;
            g_live.model  = model;
            g_live.weight = std::min(1.0f, static_cast<float>(now - g_live.start_us) / static_cast<float>(kHoldEaseUs));
            SendBend(g_live.weight);
            // (fix175) Your own walk since the last frame (your place; a jump over 1 m in a frame (a teleport, a load) not counted; at most
            // 0.25 m a frame, a run's). (The review) Your climb or fall too (stairs, ramps: they come up or down with you); let go when you two
            // overlap (0.35 m apart: the game pushing you apart would read as your walk and drift you both), when your feet and theirs are over
            // 0.8 m apart in height (a ledge, a lift), or after 20 m dragged: the queued move places them without a collision check.
            glm::vec3 your_move{ 0.0f };
            glm::vec3 you_now{ 0.0f };
            bool      have_you_now = false;
            {
                float      at[3]{};
                const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
                if (player != 0 && SafeRead(at, player + kRefPlace, sizeof(at)) && std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2])) {
                    you_now      = glm::vec3{ at[0], at[1], at[2] };
                    have_you_now = true;
                    if (g_live.have_you && glm::length(you_now - g_live.you) < 1.0f) {
                        your_move = you_now - g_live.you;
                        if (glm::length(your_move) > 0.25f) {
                            your_move *= 0.25f / glm::length(your_move);
                        }
                    }
                    g_live.you      = you_now;
                    g_live.have_you = true;
                }
            }
            if (have_you_now) {
                if (glm::length(glm::vec2{ p.place - you_now }) < 0.35f) {
                    return "they and you were in one place (0.35 m)";
                }
                if (std::abs(p.place.z - you_now.z) > 0.8f) {
                    return "your feet and theirs were 0.8 m apart in height";
                }
            }
            if (g_live.asked_drag > kHoldDragLimit) {
                return "dragged 20 m";
            }
            // The level pull past the dead zone drags them, their own heading kept; (fix175) and they come along with your own walk (PLANCK walks
            // a held person with 0.8 x your own movement plus the offset's catch-up: here all of it, so they keep up seated at the stick's speed).
            const glm::vec2 level{ pull.x, pull.y };
            const float     level_len = glm::length(level);
            glm::vec3       step{ your_move };
            // (fix182, the review) How late the last move landed (they from where it asked them to be), and whether they moved at all the
            // way it asked: under 0.3 of it for kHoldBlockedFrames frames running, the drag holds back that way (a wall, or the game putting
            // them back) until you pull another way; logged once a hold.
            if (g_live.have_last_to) {
                const float late = glm::length(glm::vec2{ p.place - g_live.last_to });
                if (std::isfinite(late) && late < 2.0f) {
                    g_live.late_sum += late;
                    g_live.late_max = std::max(g_live.late_max, late);
                    ++g_live.late_n;
                }
                const glm::vec2 asked_way{ g_live.last_step.x, g_live.last_step.y };
                const float     asked_len = glm::length(asked_way);
                if (asked_len > 0.01f) {
                    const float went = glm::dot(glm::vec2{ p.place - g_live.last_place }, asked_way / asked_len);
                    g_live.blocked_frames = went < 0.3f * asked_len ? g_live.blocked_frames + 1 : 0;
                    if (g_live.blocked_frames >= kHoldBlockedFrames && !g_live.blocked) {
                        g_live.blocked     = true;
                        g_live.blocked_dir = asked_way / asked_len;
                        if (!g_live.told_blocked && Tell(g_told_hold, 60)) {
                            g_live.told_blocked = true;
                            spdlog::info("[Physical] (fix182) {:08X} did not move the way they were dragged for {} frames (under 0.3 of each move asked): "
                                         "the drag held back that way (something in the way?) until you pull another way",
                                g_live.id, g_live.blocked_frames);
                        }
                    }
                }
            }
            if (g_live.blocked && level_len > 1e-4f && glm::dot(level / level_len, g_live.blocked_dir) < 0.7f) {
                g_live.blocked        = false;
                g_live.blocked_frames = 0;
            }
            if (level_len > kHoldDeadZone && dt > 0.0f && !(g_live.blocked && glm::dot(level / level_len, g_live.blocked_dir) >= 0.7f)) {
                // (fix176, the review: at most a share of the pull a frame, so a frame's lag of the queued move cannot make it ring at a low
                // frame rate.)
                const float     frame_dt = std::min(dt, 0.05f);
                const float     gap      = level_len - kHoldDeadZone;
                const float     move     = std::min({ gap * std::min(kHoldDragGain * frame_dt, kHoldDragFrame), kHoldDragMax * frame_dt });
                const glm::vec2 catch_up = level / level_len * move;
                step += glm::vec3{ catch_up.x, catch_up.y, 0.0f };
            }
            g_live.have_last_to = false; // (fix182: set again below when a move is asked this frame)
            if (g.move && glm::length(step) > 1e-4f) {
                uintptr_t cell = 0;
                if (Read(ref + kRefCell, cell) && cell != 0) {
                    const float to[3] = { p.place.x + step.x, p.place.y + step.y, p.place.z + step.z };
                    using MoveFn      = void (*)(void* unused, void* actor, void* cell, const float* place, float heading, bool a, bool b);
                    reinterpret_cast<MoveFn>(g.move_fn)(nullptr, reinterpret_cast<void*>(ref), reinterpret_cast<void*>(cell), to, heading, false, false);
                    ++g_live.moves;
                    g_live.asked_drag += glm::length(step);
                    g_live.last_to      = glm::vec3{ to[0], to[1], to[2] }; // (fix182, the review)
                    g_live.last_step    = step;
                    g_live.last_place   = p.place;
                    g_live.have_last_to = true;
                }
            }
            if (g_live.angers && now - g_live.anger_us >= kHoldAngerUs) {
                g_live.anger_us = now;
                AddAnger(ref, p, 1, now, "held by you");
            }
            HandGrab::MarkBusy(g_live.hand == 0); // (no punches or shoves with the holding hand)
            if (now - g_live.log_us >= 1'000'000 && Tell(g_told_hold, 60)) {
                g_live.log_us = now;
                int   seen = 0, bent = 0;
                float asked = 0.0f, given = 0.0f;
                body::HeldPersonBendCounts(seen, bent, asked, given);
                spdlog::info("[Physical] (fix173) holding {:08X} by the {}: the held point {:.2f} m from your hand (bent {:.2f} of {:.2f} asked), {} moves "
                             "asked ({:.2f} m), their place moved {:.2f} m from where you took them, (fix176) their feet {:+.2f} m from yours in height; their model bent on {} of {} updates; "
                             "(fix179) the held point moved on toward you by {:.3f} m a frame on average (at most {:.3f}) for the frame it is late",
                    g_live.id, g_live.bone, g_live.pull, given, asked, g_live.moves, g_live.asked_drag, glm::length(glm::vec2{ p.place - g_live.start_place }),
                    have_you_now ? p.place.z - you_now.z : 0.0f, bent, seen, g_live.lead_frames > 0 ? g_live.lead_sum / static_cast<float>(g_live.lead_frames) : 0.0f,
                    g_live.lead_max);
                g_live.lead_sum    = 0.0f;
                g_live.lead_max    = 0.0f;
                g_live.lead_frames = 0;
                // (fix182) A held forearm or hand: their elbow as animated and as bent.
                float elbow_anim = 0.0f, elbow_bent = 0.0f;
                int   elbow_n    = 0;
                if (body::HeldPersonElbow(elbow_anim, elbow_bent, elbow_n)) {
                    spdlog::info("[Physical] (fix182) holding {:08X} by the {}: their elbow {:.0f} degrees as animated, {:.0f} as bent on average ({} updates; "
                                 "0 a straight arm; bent within 5-150 degrees, 25 from the animation at most), the held point given {:.2f} m of {:.2f} asked",
                        g_live.id, g_live.bone, elbow_anim, elbow_bent, elbow_n, given, asked);
                }
                // (fix182, the review) How late the drag's moves land (they a frame on from where the last move asked them to be).
                spdlog::info("[Physical] (fix182) holding {:08X}: the drag's moves landed {:.3f} m from where they were asked to on average ({:.3f} at most; "
                             "{} moves){}",
                    g_live.id, g_live.late_n > 0 ? g_live.late_sum / static_cast<float>(g_live.late_n) : 0.0f, g_live.late_max, g_live.late_n,
                    g_live.blocked ? "; held back the way they would not go" : "");
                g_live.late_sum = 0.0f;
                g_live.late_max = 0.0f;
                g_live.late_n   = 0;
            }
            return nullptr;
        }

        // Each frame (main frame step): the bend handed to BodyIK, the drag, the let-go tests, the anger.
        void UpdateLiveHold(int64_t now, float dt)
        {
            if (!g_live.active) {
                return;
            }
            // Easing out after the let-go: the bend's weight from where it was to 0 (the review: not from full), then their bones put back on
            // their next update.
            if (g_live.end_us != 0) {
                const float w = g_live.weight * (1.0f - static_cast<float>(now - g_live.end_us) / static_cast<float>(kHoldEaseUs));
                if (w <= 0.0f) {
                    body::EndHeldPersonBend();
                    {
                        std::scoped_lock lock{ g_contact_mutex };
                        g_contact_hand = -1;
                    }
                    g_live = LiveHold{};
                    return;
                }
                SendBend(w);
                {
                    std::scoped_lock lock{ g_contact_mutex };
                    g_contact_us     = now;
                    g_contact_weight = w;
                }
                return;
            }
            static auto vr  = VR::get();
            const char* why = nullptr;
            if (!HandGrab::GripDown(g_live.hand == 0)) {
                why = "you let go";
            } else if (!vr->is_hmd_active() || GameFlow::isShowingMenu() || GameFlow::isInDialogue() || !GameFlow::isInFirstPerson() ||
                       GameFlow::isPilotingShip()) {
                why = "a menu, conversation or third person";
            } else if (GameFlow::isWorkshopShown()) {
                why = "build mode opened (fix175)";
            }
            if (why == nullptr) {
                // (Everything that touches them inside the reference's own hold: the review, as shoves do.)
                bool found = false;
                GrenadeHand::WithReference(g_live.handle, [&](uintptr_t ref, uintptr_t) {
                    found = true;
                    why   = HoldOneFrame(ref, now, dt);
                });
                if (!found && why == nullptr) {
                    why = "they are gone";
                }
            }
            if (why != nullptr) {
                EndLiveHold(why, now);
            }
        }

        // ---- (fix176) A seated or lying person got up, then held ----
        // After fix175 npcs could not be grabbed while sitting; now they are pulled up out of the seat. The grip on someone sitting or
        // lying asks the game to get them up (PostSeatExit: its exit idle, animated); once they stand (the furniture state 0), the hold
        // starts as on someone standing (the palm allowed 0.45 m off them: they rose away from it). Not standing after 0.75 s: asked again;
        // after 3 s: the instant exit the game's own MoveTo uses (placed at the furniture's exit point); after 5 s: given up. Let go of the
        // grip meanwhile: no hold (they stay up, or sit again as their AI wants).
        struct PullUp
        {
            uint32_t handle{ 0 };
            uint32_t id{ 0 };
            int      hand{ 0 };
            int64_t  since_us{ 0 };
            int64_t  posted_us{ 0 };
            int      posts{ 0 };
            bool     instant{ false };
            uint32_t seat{ 0 };
            std::string seen;
        };
        PullUp           g_pull{};
        std::atomic<int> g_told_pull_up{ 0 };
        constexpr float  kPullUpReach = 0.9f;  // (fix177: the instant exit puts them at the seat's exit point, often farther off; was 0.45)
        constexpr int64_t kPullUpAtOnceUs = 400'000; // (fix177) not standing by then: the game's instant exit (was 3 s, only while seated)

        void StartPullUp(uintptr_t /*ref*/, const Person& p, int hand, int64_t now)
        {
            if (const char* no = HoldRefusal(p, true)) {
                haptics::MeleePulse(hand == 0, 0.12f, 0.03f);
                if (Tell(g_told_pull_up, 40)) {
                    spdlog::info("[Physical] (fix176) your {} grip on {:08X} (seated): not got up ({})", hand == 0 ? "left" : "right", p.id, no);
                }
                return;
            }
            if (g_pull.handle == p.handle) {
                return; // (already being got up)
            }
            // (The review: not while the other hand holds someone, nor while someone else is being got up.)
            if (g_live.active || g_pull.handle != 0) {
                haptics::MeleePulse(hand == 0, 0.12f, 0.03f);
                if (Tell(g_told_pull_up, 40)) {
                    spdlog::info("[Physical] (fix176) your {} grip on {:08X} (seated): not got up ({})", hand == 0 ? "left" : "right", p.id,
                        g_live.active ? "someone is held already" : "someone else is being got up");
                }
                return;
            }
            const bool posted = p.seat == 3 || PostSeatExit(p.handle, false, false); // (3: getting up already: waited for)
            if (!posted) {
                if (Tell(g_told_pull_up, 40)) {
                    spdlog::info("[Physical] (fix176) your {} grip on {:08X} (seated): the game's furniture exit could not be asked", hand == 0 ? "left" : "right", p.id);
                }
                return;
            }
            g_pull           = PullUp{};
            g_pull.handle    = p.handle;
            g_pull.id        = p.id;
            g_pull.hand      = hand;
            g_pull.since_us  = now;
            g_pull.posted_us = now;
            g_pull.posts     = p.seat == 3 ? 0 : 1;
            g_pull.seat      = p.seat;
            g_pull.seen      = fmt::format("{}", p.seat);
            haptics::MeleePulse(hand == 0, 0.3f, 0.05f);
            if (Tell(g_told_pull_up, 40)) {
                spdlog::info("[Physical] (fix176) your {} grip on {:08X} ({}): the game asked to get them up (its furniture exit, animated); held once they stand",
                    hand == 0 ? "left" : "right", p.id, p.seat == 3 ? "getting up already" : p.seat == 1 ? "sitting down" : "sitting or lying");
            }
        }

        // ---- (fix178) You, stuck getting into a seat ----
        // Getting into a seat caused a game-breaking glitch: no Data menu, only the save menu, and nothing else could be done (the
        // log, 06:39:45: the ship's pilot seat in space; the sit state "getting in" (1) and the camera in the game's animated camera (6, a
        // second third-person state riding the entry animation's camera node) for 32 s, no movement; fix165's pilot-seat hang the same).
        // A helper's research: "getting in" ends only by the entry animation's event (PlayerChairEnterHandler 0x141AE89B0 ->
        // SetSitSleepState 0x141A069E0); the game's own way out, a 25/48 s timer (middle-high +0x4B4) that ends in the instant exit
        // 0x141996BB0(actor, 1, 1, 1), ticks only in your AI update, which the game skips while its camera is in the transition (4) or the
        // animated camera with your third-person mark (+0x112A bit 3) clear, or a first/third-person switch is queued (+0x1110). What held
        // it is not known: logged here. The way out: after 15 s of play (no menu) getting in, the last 10 s without moving, the game's own
        // queued exit for you, instant (what its timer would do; it also ends the animated camera, 0x141996F13); at most every 10 s, 3 times.
        constexpr uintptr_t kPlayerThirdPerson = 0x112A; // bit 3: the game's "third person" mark; bit 0: driven by its AI
        constexpr uintptr_t kPlayerViewQueued  = 0x1110; // first/third-person switches queued, not run yet
        constexpr uintptr_t kPlayerApproach    = 0x1129; // bits 6, 7: walking to furniture
        constexpr uintptr_t kMiddleSeatTimer   = 0x4B4;  // the furniture timer (s)
        constexpr uintptr_t kMiddleSeat5A5     = 0x5A5;
        constexpr uintptr_t kCameraTarget      = 0xB0;   // the player camera's target (a handle: you or your ship)
        constexpr uintptr_t kCameraRva         = 0x61E51F0;
        constexpr float     kSeatStuckS        = 15.0f;
        constexpr float     kSeatStillS        = 10.0f;
        constexpr int64_t   kSeatRetryUs       = 10'000'000;
        constexpr int       kSeatTries         = 3;

        struct SeatWatch
        {
            int     seat{ -1 };
            float   getting_in_s{ 0.0f }; // play (no menu) in "getting in"
            float   still_s{ 0.0f };      // ...without moving
            int64_t posted_us{ 0 }, check_us{ 0 }, told_us{ 0 };
            int     tries{ 0 };
            int     lines{ 0 }, important_lines{ 0 };
        };
        SeatWatch g_seat_watch{};

        // (The review: `important` lines (stuck, the check after the exit) have their own budget, not used up by ordinary sits.)
        void TellSeat(const char* why, uintptr_t player, uint32_t state, uint32_t state2, bool important = false)
        {
            int& lines = important ? g_seat_watch.important_lines : g_seat_watch.lines;
            if (lines >= (important ? 60 : 200)) {
                return;
            }
            ++lines;
            uint8_t   third = 0, approach = 0, b5a5 = 0;
            uint32_t  queued = 0, h424 = 0, h428 = 0, target = 0, handle = 0;
            float     timer  = 0.0f;
            uintptr_t process = 0, middle = 0, camera = 0;
            Read(player + kPlayerThirdPerson, third);
            Read(player + kPlayerViewQueued, queued);
            Read(player + kPlayerApproach, approach);
            Read(player + kRefHandle, handle);
            if (Read(player + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0) {
                Read(middle + kMiddleSeatTimer, timer);
                Read(middle + kMiddleSeat5A5, b5a5);
                Read(middle + 0x424, h424);
                Read(middle + 0x428, h428);
            }
            if (Read(At(kCameraRva), camera) && camera != 0) {
                Read(camera + kCameraTarget, target);
            }
            spdlog::info("[Seat] (fix178) {}: sit state {} (+0xFC {:08x}), moving {:02x} (+0xF8 {:08x}), camera state {}, the camera on {} ({:x}); third-person "
                         "mark {} (+0x112A {:02x}), view switches queued {}, walking to furniture {:02x} (+0x1129); its furniture timer {:.1f} s, +0x5A5 {:02x}, "
                         "+0x424 {:x} +0x428 {:x}; getting in {:.1f} s of play, still {:.1f} s, a menu {}",
                why, (state2 >> 11) & 3u, state2, state & 0xFFu, state, WorkbenchCamera::CameraStateIndex(), target == handle ? "you" : "something else",
                target, (third >> 3) & 1u, third, queued, approach, timer, b5a5, h424, h428, g_seat_watch.getting_in_s, g_seat_watch.still_s,
                GameFlow::isShowingMenu());
        }

        void UpdateSeatRescue(int64_t now, float dt)
        {
            auto* player_ref = CreationEngineSingletonManager::GetPlayerRef();
            if (player_ref == nullptr) {
                return;
            }
            const uintptr_t player = reinterpret_cast<uintptr_t>(player_ref);
            uint32_t        state = 0, state2 = 0;
            if (!Read(player + kActorState, state) || !Read(player + kActorState2, state2)) {
                return;
            }
            const int  seat = static_cast<int>((state2 >> 11) & 3u);
            // (The review: a conversation counts as a menu here: not counted, never rescued in one.)
            const bool menu = GameFlow::isShowingMenu() || GameFlow::isInDialogue();
            auto&      w    = g_seat_watch;
            if (seat != w.seat) {
                if (w.seat >= 0) {
                    TellSeat(w.seat == 1 && seat != 1 ? "you are past getting in" : "your sit state changed", player, state, state2);
                }
                w.seat         = seat;
                w.getting_in_s = 0.0f;
                w.still_s      = 0.0f;
                if (seat != 1) {
                    w.tries = 0;
                }
            }
            if (seat != 1) {
                return;
            }
            const float step = std::clamp(dt, 0.0f, 0.25f);
            if (!menu) {
                w.getting_in_s += step;
                w.still_s = (state & 0xFFu) != 0 ? 0.0f : w.still_s + step;
            }
            // (The review: every 2 s only past 5 s getting in: ordinary sits take 1-6 s.)
            if (w.getting_in_s >= 5.0f && now - w.told_us >= 2'000'000) {
                w.told_us = now;
                TellSeat("getting into a seat", player, state, state2);
            }
            if (w.check_us != 0 && now >= w.check_us) {
                w.check_us = 0;
                TellSeat("still getting in 1.5 s after the game's exit was asked", player, state, state2, true);
            }
            // (The review: only with the hang's own mark, seen both times: the game's camera in its animated camera (6) or the transition to it
            // (4); a quest's scripted sit or a slow walk to the seat keeps first person.)
            const int camera = WorkbenchCamera::CameraStateIndex();
            if (!menu && (camera == 6 || camera == 4) && w.getting_in_s >= kSeatStuckS && w.still_s >= kSeatStillS && w.tries < kSeatTries &&
                now - w.posted_us >= kSeatRetryUs) {
                uint32_t handle = 0;
                Read(player + kRefHandle, handle);
                const bool posted = PostSeatExit(handle, true, true);
                w.posted_us       = now;
                w.check_us        = now + 1'500'000;
                ++w.tries;
                TellSeat("stuck getting into a seat", player, state, state2, true);
                spdlog::warn("[Seat] (fix178) you were stuck getting into a seat ({:.0f} s, still {:.0f} s): the game's own seat exit asked for you, instant ({}; "
                             "try {} of {})",
                    w.getting_in_s, w.still_s, posted ? "posted" : "NOT posted: the exit is not available", w.tries, kSeatTries);
            }
        }

        void UpdatePullUp(int64_t now)
        {
            if (g_pull.handle == 0) {
                return;
            }
            const char* end = nullptr;
            static auto vr = VR::get();
            if (!HandGrab::GripDown(g_pull.hand == 0)) {
                end = "you let go before they stood";
            } else if (!vr->is_hmd_active() || GameFlow::isShowingMenu() || GameFlow::isInDialogue() || !GameFlow::isInFirstPerson() ||
                       GameFlow::isWorkshopShown()) {
                end = "a menu, conversation, build mode or third person"; // (the review)
            }
            bool found = false;
            if (end == nullptr) {
                GrenadeHand::WithReference(g_pull.handle, [&](uintptr_t ref, uintptr_t) {
                    found = true;
                    Person p;
                    if (!ReadPerson(ref, p)) {
                        end = "they are gone";
                        return;
                    }
                    if (const char* no = HoldRefusal(p, true)) {
                        end = no;
                        return;
                    }
                    if (p.seat != g_pull.seat) {
                        g_pull.seat = p.seat;
                        g_pull.seen += fmt::format(" -> {} at {:.2f} s", p.seat, (now - g_pull.since_us) * 1e-6);
                    }
                    HandGrab::MarkBusy(g_pull.hand == 0);
                    if (p.seat == 0) {
                        const bool held = StartLiveHold(ref, p, g_pull.hand, now, kPullUpReach);
                        AddAnger(ref, p, 1, now, "pulled up out of their seat"); // (the review: once standing: seated, Protected refuses them)
                        if (Tell(g_told_pull_up, 40)) {
                            spdlog::info("[Physical] (fix176) {:08X} stood up after {:.2f} s (furniture states {}): {}", g_pull.id, (now - g_pull.since_us) * 1e-6,
                                g_pull.seen, held ? "held" : "not held (see the line before)");
                        }
                        end = "";
                        return;
                    }
                    if (now - g_pull.since_us > 6'000'000) {
                        end = "they did not stand within 6 s";
                    } else if (now - g_pull.since_us > kPullUpAtOnceUs && !g_pull.instant) {
                        // (The review: asked once, posted or not.) (fix177) After 0.4 s whatever they are doing (getting up too): after
                        // fix176 a seated person grabbed stood up but was not dragged (the log: one still getting up at
                        // 1.4 s as the grip was let go): finished at once, then held and pulled to your hand.
                        g_pull.instant    = true;
                        const bool posted = PostSeatExit(g_pull.handle, true, true);
                        if (Tell(g_told_pull_up, 40)) {
                            spdlog::info("[Physical] (fix177) {:08X} not standing after 0.4 s: the game's instant exit asked ({})", g_pull.id, posted ? "posted" : "not posted");
                        }
                    } else if (p.seat == 2 && now - g_pull.posted_us > 750'000 && g_pull.posts < 3) {
                        if (PostSeatExit(g_pull.handle, false, false)) {
                            ++g_pull.posts;
                            g_pull.posted_us = now;
                        }
                    }
                });
                if (!found && end == nullptr) {
                    end = "they are gone";
                }
            }
            if (end != nullptr) {
                if (end[0] != 0 && Tell(g_told_pull_up, 40)) {
                    spdlog::info("[Physical] (fix176) {:08X} not got up and held: {} (furniture states {}; asked {} times{})", g_pull.id, end, g_pull.seen, g_pull.posts,
                        g_pull.instant ? ", then instantly" : "");
                }
                g_pull = PullUp{};
            }
        }

        // ---- Shoves (stage 2): an open hand pushed into someone, weapons put away ----
        struct HandSeen
        {
            bool      have{ false };
            glm::vec3 stage{ 0.0f };
            int64_t   us{ 0 };
            glm::vec3 velocity{ 0.0f };
            int64_t   shoved_us{ 0 };
        };
        HandSeen g_hands[2]{};
        int      g_shove_frame{ 0 };

        void React(uintptr_t ref, const Person& p, int hand, float speed, const glm::vec3& way, int64_t now)
        {
            Mood& m = MoodOf(p.handle, now);
            if (now - m.shoved_us < kShoveCooldownUs) {
                return;
            }
            m.shoved_us             = now;
            g_hands[hand].shoved_us = now;
            const char* protect     = Protected(p);
            const bool  bump_only   = protect != nullptr || Big(p) || speed < kShoveFirm;
            bool        done        = false;
            const char* what        = "bumped";
            if (bump_only) {
                auto* player = CreationEngineSingletonManager::GetPlayerRef();
                done         = Perform(ref, reinterpret_cast<uintptr_t>(player), kActionBumpedInto, "a shove (bumped into you)");
                if (!done) {
                    done = Perform(ref, 0, kActionBumpedInto, "a shove (bumped into, no target)"); // (the game's own builders give no target)
                }
                // (fix174) The bump refused (the game refused every bump of fix172's and fix173's tests): the game's recoil, a flinch (fix173
                // staggered them: "too strong"). Not for the protected (your followers, people sitting or lying, ...): only ever bumped.
                if (!done && protect == nullptr && !Big(p)) {
                    done = Perform(ref, reinterpret_cast<uintptr_t>(player), kActionRecoil, "a light shove (the bump refused): recoil, at you");
                    if (!done) {
                        done = Perform(ref, 0, kActionRecoil, "a light shove (the bump refused): recoil, no target");
                    }
                    what = done ? "flinched (the game's recoil; the bump refused)" : "bumped";
                }
            } else {
                const int magnitude = speed >= kShoveHard ? 2 : 1;
                done                = Stagger(ref, magnitude, "a shove");
                what                = magnitude == 2 ? "staggered hard" : "staggered";
                g_staggers.fetch_add(done ? 1 : 0);
            }
            g_shoves.fetch_add(1);
            haptics::MeleePulse(hand == 0, bump_only ? 0.25f : 0.5f, 0.06f);
            if (Tell(g_told_shove, 40)) {
                spdlog::info("[Physical] (fix172) your {} hand shoved {:08X} at {:.1f} m/s (way {:.2f} {:.2f} {:.2f}): {}{}{}", hand == 0 ? "left" : "right", p.id,
                    speed, way.x, way.y, way.z, what, done ? "" : " (the game's reaction was not made)",
                    protect != nullptr ? fmt::format(" (only bumped: {})", protect) : Big(p) ? std::string{ " (only bumped: too big)" } : std::string{});
            }
            if (protect == nullptr) {
                AddAnger(ref, p, 1, now, "shoved");
            }
        }

        void UpdateShoves(int64_t now)
        {
            static auto vr = VR::get();
            const auto& s  = GameFlow::gStore.internalSettings;
            const bool  playing = s.shovePeople && vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() &&
                                 !GameFlow::isWorkshopShown() && // (fix175: nor build mode)
                                 !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && !GameFlow::isOnLadder() && !ModSettings::showFlatScreenDisplay() &&
                                 !GameFlow::isWeaponDrawn();
            bool  fast[2]{ false, false };
            float speed[2]{ 0.0f, 0.0f };
            glm::vec3 palm[2]{}, way[2]{};
            for (int hand = 0; hand < 2; ++hand) {
                const bool      left  = hand == 0;
                const glm::mat4 grip  = tracking::GripPose(left);
                const glm::mat4 aim   = tracking::AimPose(left);
                const glm::vec3 at{ grip[3] };
                auto&           h     = g_hands[hand];
                if (!Finite(at)) {
                    h.have = false;
                    continue;
                }
                if (!h.have || at != h.stage) {
                    const float since = static_cast<float>(now - h.us) * 1e-6f;
                    h.velocity        = h.have && since > 0.002f && since < 0.2f ? (at - h.stage) / since : glm::vec3{ 0.0f };
                    h.have            = true;
                    h.stage           = at;
                    h.us              = now;
                }
                const auto index = static_cast<uint32_t>(left ? vr->get_left_controller_index() : vr->get_right_controller_index());
                glm::vec3  v{ vr->get_velocity(index) };
                if (glm::length(v) < 1e-5f) {
                    v = now - h.us < 50'000 ? h.velocity : glm::vec3{ 0.0f }; // (the runtime gives none: from the poses)
                }
                speed[hand] = glm::length(v);
                if (!playing || !std::isfinite(speed[hand]) || speed[hand] < kShoveLight || now - h.shoved_us < kHandCooldownUs || HandGrab::HoldingWith(left) ||
                    HandGrab::HandBusy(left) || HandGrab::GripDown(left)) {
                    continue;
                }
                glm::vec3 unused{};
                const glm::vec3 forward = -glm::vec3{ aim[2] };
                if (!tracking::StageToWorld(at, forward, palm[hand], unused) || !tracking::StageToWorld(at, v / speed[hand], unused, way[hand])) {
                    continue;
                }
                way[hand]  = glm::length(way[hand]) > 1e-4f ? glm::normalize(way[hand]) : glm::vec3{ 0.0f };
                fast[hand] = glm::length(way[hand]) > 0.5f;
            }
            if (!fast[0] && !fast[1]) {
                return;
            }
            if (++g_shove_frame % 2 != 0) {
                return; // (the people near you looked through every other frame while a hand moves fast)
            }
            uint32_t best_handle[2]{ 0, 0 };
            float    best_off[2]{ 1e9f, 1e9f };
            MeleeThrow::PeopleNearby([&](uint32_t handle, uintptr_t ref, const glm::vec3& middle) {
                // (Only people near a moving hand are read further: a person's reads are a dozen system calls; the review.)
                bool near_hand = false;
                for (int hand = 0; hand < 2; ++hand) {
                    near_hand = near_hand || (fast[hand] && glm::length(glm::vec2{ middle.x - palm[hand].x, middle.y - palm[hand].y }) < 1.5f);
                }
                if (!near_hand) {
                    return;
                }
                Person p;
                if (!ReadPerson(ref, p) || p.player || Dead(p) || KnockedDown(p) || p.gone) {
                    return; // (someone down is grabbed, not shoved)
                }
                const float top    = p.place.z + std::clamp(p.have_bound ? 2.0f * p.radius : 1.8f, 1.0f, 2.4f);
                const float bottom = p.place.z + 0.3f;
                for (int hand = 0; hand < 2; ++hand) {
                    if (!fast[hand] || palm[hand].z < bottom || palm[hand].z > top) {
                        continue;
                    }
                    const glm::vec2 to{ p.place.x - palm[hand].x, p.place.y - palm[hand].y };
                    const float     off = glm::length(to);
                    if (off > kPersonRadius + kHandRadius || off >= best_off[hand]) {
                        continue;
                    }
                    // Moving into them (not pulling the hand back out).
                    const glm::vec2 into = off > 1e-3f ? to / off : glm::vec2{ 0.0f };
                    if (off > 0.05f && glm::dot(glm::vec2{ way[hand].x, way[hand].y }, into) < 0.3f) {
                        continue;
                    }
                    best_off[hand]    = off;
                    best_handle[hand] = handle;
                }
            });
            for (int hand = 0; hand < 2; ++hand) {
                if (best_handle[hand] == 0) {
                    continue;
                }
                GrenadeHand::WithReference(best_handle[hand], [&](uintptr_t ref, uintptr_t) {
                    Person p;
                    if (ReadPerson(ref, p) && !Dead(p)) {
                        React(ref, p, hand, speed[hand], way[hand], now);
                    }
                });
            }
        }

        // ---- Blocks: the attackers to make recoil (main thread) ----
        void UpdateBlocks()
        {
            BlockAsk asks[8]{};
            int      n = 0;
            {
                std::scoped_lock _{ g_block_mutex };
                n = g_block_ask_count;
                std::copy(g_block_asks, g_block_asks + n, asks);
                g_block_ask_count = 0;
            }
            auto* player = CreationEngineSingletonManager::GetPlayerRef();
            for (int i = 0; i < n; ++i) {
                GrenadeHand::WithReference(asks[i].attacker, [&](uintptr_t ref, uintptr_t) {
                    Person p;
                    if (!ReadPerson(ref, p) || Dead(p) || p.player) {
                        return;
                    }
                    const int action = asks[i].large ? kActionLargeRecoil : kActionRecoil;
                    bool      done   = Perform(ref, reinterpret_cast<uintptr_t>(player), action, "a blocked attack (recoil, at you)");
                    if (!done) {
                        done = Perform(ref, 0, action, "a blocked attack (recoil, no target)"); // (the game's own builders give no target)
                    }
                    const char* how = "recoils (the game's recoil action)";
                    if (!done && Protected(p) == nullptr && !Big(p)) {
                        done = Stagger(ref, 1, "a blocked attack");
                        how  = "staggers (the recoil was not taken)";
                    }
                    if (Tell(g_told_block, 40)) {
                        spdlog::info("[Physical] (fix172) your {} blade blocked {:08X}'s melee hit: {}{}", asks[i].hand == 0 ? "left" : "right", p.id, how,
                            done ? "" : " (neither was made)");
                    }
                });
            }
        }

        // Your body and blades as they are now, for the melee hook's thread (main thread).
        void UpdateBlockView(int64_t now)
        {
            BlockView v{};
            v.us          = now;
            auto* player  = CreationEngineSingletonManager::GetPlayerRef();
            float place[3]{};
            if (player != nullptr && SafeRead(place, reinterpret_cast<uintptr_t>(player) + kRefPlace, sizeof(place))) {
                v.feet = glm::vec3{ place[0], place[1], place[2] };
                v.head = v.feet + glm::vec3{ 0.0f, 0.0f, 1.7f };
                if (auto root = CreationEngineSingletonManager::GetSceneGraphRoot(); root && root->worldCamera) {
                    const auto& c = root->worldCamera->world.translate;
                    const glm::vec3 eye{ c.x, c.y, c.z };
                    if (Finite(eye) && glm::length(eye - v.head) < 1.5f) {
                        v.head = eye;
                    }
                }
                v.have_you = Finite(v.feet) && Finite(v.head);
            }
            if (GameFlow::gStore.internalSettings.saberBlock) {
                // The right hand's melee weapon (the game's, drawn, in your hand): its model's line through the hand (MeleeThrow keeps it).
                if (HandGrab::MeleeWeaponOut() != 0 && GameFlow::isWeaponDrawn() && !MeleeThrow::Away(false)) {
                    v.blade[1] = MeleeThrow::BladeSegment(false, v.hilt[1], v.tip[1]);
                }
                // The left hand's weapon (its copy in your hand).
                glm::vec3 grip{}, way{};
                float     length = 0.0f;
                if (LeftWeapon::Held() && !MeleeThrow::Away(true) && GrenadeHand::LeftBlade(grip, way, length) && Finite(grip) && Finite(way) && length > 0.2f &&
                    length < 2.5f) {
                    v.blade[0] = true;
                    v.hilt[0]  = grip;
                    v.tip[0]   = grip + glm::normalize(way) * length;
                }
            }
            std::scoped_lock _{ g_block_mutex };
            g_view = v;
        }

        // The watched knockdowns: each change of their knock or life state noted, with the time down the game keeps; logged at the end.
        void UpdateWatches(int64_t now)
        {
            for (auto& w : g_watch) {
                if (w.handle == 0) {
                    continue;
                }
                GrenadeHand::WithReference(w.handle, [&](uintptr_t ref, uintptr_t) {
                    Person p;
                    if (!ReadPerson(ref, p) || (p.knock == w.last_knock && p.life == w.last_life)) {
                        return;
                    }
                    w.last_knock = p.knock;
                    w.last_life  = p.life;
                    uintptr_t process = 0, middle = 0;
                    float     down    = -1.0f;
                    if (Read(ref + kActorProcess, process) && process != 0 && Read(process + kProcessMiddle, middle) && middle != 0) {
                        Read(middle + kMiddleDownTime, down);
                    }
                    w.seen += fmt::format("{}{:.2f} s: knock state {}, life state {}, time down {:.2f}", w.seen.empty() ? "" : "; ", (now - w.since_us) * 1e-6, p.knock,
                        p.life, down);
                });
                if (now - w.since_us > kWatchUs) {
                    spdlog::info("[Physical] (fix172) {:08X} after {}: {}", w.id, w.what, w.seen.empty() ? std::string{ "not found again" } : w.seen);
                    w = Watch{};
                }
            }
        }
    }

    void Install()
    {
        if (g_installed) {
            return;
        }
        g_installed       = true;
        const uintptr_t b = Base();
        LARGE_INTEGER   f{};
        QueryPerformanceFrequency(&f);
        g_qpc_freq = std::max<int64_t>(1, f.QuadPart);
        // The knock (PushActorAway's code).
        {
            static constexpr uint8_t kLoadRcx[] = { 0x48, 0x8B, 0x0D };
            if (SameCode(kPushNativeRva + kPushQueueLoad, kLoadRcx, sizeof(kLoadRcx)) && CallTarget(At(kPushNativeRva + kPushQueueCall)) == At(kKnockQueueRva) &&
                SameCode(kKnockQueueRva, kKnockQueueStart, sizeof(kKnockQueueStart)) && At(kKnockRva) != 0 && // (fix190: 0 never a match)
                CallTarget(At(kKnockQueueRva + kKnockCallInQueue)) == At(kKnockRva)) {
                g.queue_at = RipTarget(At(kPushNativeRva + kPushQueueLoad), 3, 7);
                g.knock_fn = At(kKnockQueueRva);
                g.knock    = g.queue_at != 0 && InImage(g.queue_at);
                // (fix173) Its choice to queue (0x1418C1A2D: cmp byte [flag],0; 0x1418C1A40: cmp [main thread id],eax after
                // GetCurrentThreadId): read once in play, for the log.
                static constexpr uint8_t kCmpByte[] = { 0x80, 0x3D };
                static constexpr uint8_t kCmpDword[] = { 0x39, 0x05 };
                if (SameCode(kKnockQueueRva + 0x8d, kCmpByte, sizeof(kCmpByte)) && SameCode(kKnockQueueRva + 0xa0, kCmpDword, sizeof(kCmpDword))) {
                    g.queue_flag_at  = RipTarget(At(kKnockQueueRva + 0x8d), 2, 7);
                    g.main_thread_at = RipTarget(At(kKnockQueueRva + 0xa0), 2, 6);
                }
            }
            g.down_time = SameCode(kKnockDownTimeAt, kKnockDownTimeHead, sizeof(kKnockDownTimeHead)) &&
                          RipTarget(At(kKnockDownTimeAt), 4, 8) == At(kIdleScalerRva) && SameCode(kKnockDownTimeAt + 8, kKnockDownTimeTail, sizeof(kKnockDownTimeTail));
        }
        // The default objects, the stagger and performing actions.
        {
            g.defaults = SameCode(kDefaultsRva, kDefaultsStart, sizeof(kDefaultsStart)) && CallTarget(At(kStaggerHitAt)) == At(kDefaultsRva) &&
                         SameCode(kStaggerHitAt + 5, kStaggerHitMid, sizeof(kStaggerHitMid));
            // The table's names where the indexes say (the objects' order is the table's).
            const auto named = [&](int index, const char* name) {
                const uintptr_t entry = At(kDefaultTableRva) + static_cast<uintptr_t>(index) * 0x20;
                uintptr_t       text  = 0;
                uint8_t         type  = 0;
                char            buffer[48]{};
                return Read(entry, text) && InImage(text) && Read(entry + 8, type) && type == kFormTypeAction && SafeRead(buffer, text, sizeof(buffer) - 1) &&
                       std::strcmp(buffer, name) == 0;
            };
            // (fix190) The table known in this game version (GameCode), else nothing read there.
            g.defaults = g.defaults && At(kDefaultTableRva) != 0 && named(kActionStaggerStart, "Action Stagger Start") && named(kActionRecoil, "Action Recoil") &&
                         named(kActionLargeRecoil, "Action Large Recoil") && named(kActionBumpedInto, "Action Bumped Into");
            if (g.defaults) {
                g.defaults_fn = At(kDefaultsRva);
            }
            g.stagger = g.defaults && SameCode(kStaggerRva, kStaggerStart, sizeof(kStaggerStart)) && CallTarget(At(kStaggerNoHitAt)) == At(kDefaultsRva) &&
                        SameCode(kStaggerNoHitAt + 5, kStaggerNoHitMid, sizeof(kStaggerNoHitMid)) &&
                        CallTarget(At(kStaggerNoHitAt + 5 + sizeof(kStaggerNoHitMid))) == At(kStaggerRva);
            if (g.stagger) {
                g.stagger_fn = At(kStaggerRva);
            }
            if (g.defaults && SameCode(kPerformAt, kPerform, sizeof(kPerform))) {
                g.action_table      = RipTarget(At(kPerformAt), 3, 7);
                g.action_table2     = RipTarget(At(kPerformAt + 11), 3, 7);
                g.perform_at        = RipTarget(At(kPerformAt + 25), 3, 7);
                g.empty_string_fn   = CallTarget(At(kEmptyStringCall));
                g.release_string_fn = CallTarget(At(kReleaseStringCall));
                g.release_ref_fn    = CallTarget(At(kReleaseRefCall));
                // (fix190) Each of them known in this game version (GameCode): else a call not found (0) would match a place not known (0).
                g.actions = GameCode::Known({ kActionTableRva, kActionTable2Rva, kPerformerRva, kEmptyStringRva, kReleaseStringRva, kReleaseRefRva }) &&
                            g.action_table == At(kActionTableRva) && g.action_table2 == At(kActionTable2Rva) && g.perform_at == At(kPerformerRva) &&
                            g.empty_string_fn == At(kEmptyStringRva) && g.release_string_fn == At(kReleaseStringRva) && g.release_ref_fn == At(kReleaseRefRva);
            }
        }
        g.assault = SameCode(kAssaultRva, kAssault, sizeof(kAssault));
        // The grab-the-living setting.
        {
            const uintptr_t setting = At(kLivingSettingRva);
            uintptr_t       table = 0, name = 0;
            char            buffer[40]{};
            g.living = Read(setting, table) && table == At(kLivingTableRva) && Read(setting + 0x18, name) && name == At(kLivingNameRva) &&
                       SafeRead(buffer, name, sizeof(buffer) - 1) && std::strcmp(buffer, "bAllowHavokGrabTheLiving:GamePlay") == 0 &&
                       SameCode(kLivingReadAt, kLivingRead, sizeof(kLivingRead)) && RipTarget(At(kLivingReadAt), 2, 6) == setting + 8;
            if (g.living) {
                g.living_value = setting + 8;
            }
        }
        // The people near you.
        {
            uintptr_t lists = 0, table = 0;
            g.lists = Read(At(kListsRva), lists) && (lists == 0 || (Read(lists, table) && table == At(kListsTableRva)));
        }
        // The physics update's time.
        {
            const uintptr_t fn = At(kWorldUpdateRva);
            static constexpr uint8_t kLeaRdx[] = { 0x48, 0x8D, 0x15 };
            char            zone[sizeof(kWorldUpdateName)]{};
            const uintptr_t name = RipTarget(At(kWorldUpdateRva + kWorldUpdateZone), 3, 7);
            if (SameCode(kWorldUpdateRva, kWorldUpdateStart, sizeof(kWorldUpdateStart)) && SameCode(kWorldUpdateRva + kWorldUpdateZone, kLeaRdx, sizeof(kLeaRdx)) &&
                InImage(name) && SafeRead(zone, name, sizeof(zone) - 1) && std::strcmp(zone, kWorldUpdateName) == 0) {
                // (Made disabled and turned on once stored: the game calls it every frame from its main loop.)
                g_world_hook = safetyhook::create_inline(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&OnWorldUpdate), safetyhook::InlineHook::StartDisabled);
                g.timer      = static_cast<bool>(g_world_hook) && g_world_hook.enable().has_value();
            }
        }
        spdlog::info("[Physical] (fix172) physical interaction with people: knocking down {} (PushActorAway's code at rva {:x}, its task queue at {:x}; time down {}), "
                     "staggers {} (rva {:x}), actions {} (the default objects at rva {:x}, the performer at {:x}), the assault alarm {}, the grab of the "
                     "living {} (the setting at {:x}), the people near you {}, the physics update's time {} (rva {:x})",
            g.knock ? "found" : "NOT found", kKnockQueueRva, g.queue_at ? g.queue_at - b : 0, g.down_time ? "found" : "not found", g.stagger ? "found" : "NOT found",
            kStaggerRva, g.actions ? "found" : "NOT found", kDefaultsRva, g.perform_at ? g.perform_at - b : 0, g.assault ? "found" : "NOT found",
            g.living ? "found" : "NOT found", kLivingSettingRva, g.lists ? "found" : "NOT found", g.timer ? "hooked" : "not hooked", kWorldUpdateRva);
        // (fix173) The game's queued actor move (holding a standing person drags them with it).
        {
            static constexpr uint8_t kLeaRcx[] = { 0x48, 0x8D, 0x0D };
            g.move    = SameCode(kQueueMoveRva, kQueueMoveStart, sizeof(kQueueMoveStart)) && SameCode(kQueueMoveTableAt, kLeaRcx, sizeof(kLeaRcx)) &&
                     At(kQueueMoveTable) != 0 && RipTarget(At(kQueueMoveTableAt), 3, 7) == At(kQueueMoveTable) && At(kQueueMovePost) != 0 &&
                     CallTarget(At(kQueueMovePostAt)) == At(kQueueMovePost); // (fix190: 0 never a match)
            g.move_fn = g.move ? At(kQueueMoveRva) : 0;
        }
        // (fix176) The game's queued furniture exit: every piece of its own queuing checked.
        {
            static constexpr uint8_t kMovRsi[]   = { 0x48, 0x8B, 0x35 };
            static constexpr uint8_t kMovR15Bc[] = { 0x41, 0xBF, 0xBC, 0x00, 0x00, 0x00 };
            static constexpr uint8_t kMovEax1D4[] = { 0xB8, 0xD4, 0x01, 0x00, 0x00 };
            static constexpr uint8_t kLeaRcx[]   = { 0x48, 0x8D, 0x0D };
            static constexpr uint8_t kRunStart[] = { 0x48, 0x83, 0xC1, 0x10, 0xE9 };
            const bool ok = SameCode(kSeatQueueSite, kMovRsi, sizeof(kMovRsi)) && RipTarget(At(kSeatQueueSite), 3, 7) == g.queue_at && g.queue_at != 0 &&
                            SameCode(kSeatTlsCheck, kMovR15Bc, sizeof(kMovR15Bc)) && SameCode(kSeatSlotLoad, kMovEax1D4, sizeof(kMovEax1D4)) &&
                            CallTarget(At(kSeatTlsInit)) != 0 && CallTarget(At(kSeatSetCall)) != 0 && CallTarget(At(kSeatAllocCall)) != 0 &&
                            CallTarget(At(kSeatCommitCall)) != 0 && SameCode(kSeatRunLea, kLeaRcx, sizeof(kLeaRcx)) &&
                            RipTarget(At(kSeatRunLea), 3, 7) == At(kSeatRunRva) && SameCode(kSeatTableLea, kLeaRcx, sizeof(kLeaRcx)) &&
                            SameCode(kSeatRunRva, kRunStart, sizeof(kRunStart)) && CallTarget(At(kSeatRunCallAt)) == At(kSeatExitRva) &&
                            SameCode(kSeatExitRva, kSeatExitStart, sizeof(kSeatExitStart));
            if (ok) {
                g.seat_queue_at    = g.queue_at;
                g.seat_tls_init    = CallTarget(At(kSeatTlsInit));
                g.seat_set_context = CallTarget(At(kSeatSetCall));
                g.seat_alloc       = CallTarget(At(kSeatAllocCall));
                g.seat_commit      = CallTarget(At(kSeatCommitCall));
                g.seat_run         = At(kSeatRunRva);
                g.seat_table       = RipTarget(At(kSeatTableLea), 3, 7);
                g.seat_exit        = InImage(g.seat_tls_init) && InImage(g.seat_set_context) && InImage(g.seat_alloc) && InImage(g.seat_commit) &&
                              InImage(g.seat_table);
            }
            spdlog::info("[Physical] (fix176) pulling seated people up: the game's queued furniture exit {} (its exit at rva {:x}, the command's type "
                         "table at {:x})",
                g.seat_exit ? "found" : "NOT found: people sitting or lying are not held", kSeatExitRva, g.seat_table ? g.seat_table - b : 0);
            spdlog::info("[Physical] (fix173) holding standing people: the game's queued actor move {} (rva {:x}); their held part bent in their own model's "
                         "update (BodyIK)",
                g.move ? "found: they are dragged where you pull" : "NOT found: held and bent only, not dragged", kQueueMoveRva);
        }
        // (fix179) The game's types for a held person's outer surface: the ragdoll's capsule shape, the worn meshes' skins and their bone data.
        {
            g_capsule_vt   = VtableNamed(At(kCapsuleShapeVtRva), ".?AVhknpCapsuleShape@@") ? At(kCapsuleShapeVtRva) : 0;
            g_skin_vt      = VtableNamed(At(kSkinInstanceVtRva), ".?AVInstance@BSSkin@@") ? At(kSkinInstanceVtRva) : 0;
            g_bone_data_vt = VtableNamed(At(kSkinBoneDataVtRva), ".?AVBoneData@BSSkin@@") ? At(kSkinBoneDataVtRva) : 0;
            spdlog::info("[Physical] (fix179) a held person's outer surface: the game's capsule shape {}, worn meshes' skins {} and their bone data {} (else "
                         "skeleton.nif's capsules{})",
                g_capsule_vt ? "found" : "NOT found", g_skin_vt ? "found" : "NOT found", g_bone_data_vt ? "found" : "NOT found",
                g_skin_vt && g_bone_data_vt ? "" : ", and no clothes measured: 1 cm over the body");
        }
    }

    void Update()
    {
        if (!g_installed) {
            return;
        }
        static int64_t last_us = 0;
        const int64_t  now     = NowUs();
        const float    dt      = last_us != 0 ? static_cast<float>(now - last_us) * 1e-6f : 0.0f;
        last_us                = now;
        // The game's grab-the-living setting is only ever set around one call (GripGrab): put back if anything left it.
        if (g_living_set && g.living_value != 0) {
            *reinterpret_cast<uint8_t*>(g.living_value) = g_living_before;
            g_living_set                                = false;
        }
        if (GameFlow::isLoadingScreenShown()) {
            // (The game's own cleanup lets go of what you hold; nothing of before counts after it.)
            ForgetHeld();
            g_regrab = Regrab{};
            g_pull   = PullUp{}; // (fix176)
            g_seat_watch.seat         = -1; // (fix178: counted again from the next frame of play)
            g_seat_watch.getting_in_s = 0.0f;
            g_seat_watch.still_s      = 0.0f;
            g_seat_watch.check_us     = 0;
            g_seat_watch.tries        = 0; // (the review: a save that starts getting in gets its own tries)
            g_seat_watch.posted_us    = 0;
            if (g_live.active) {
                EndLiveHold("a loading screen", now);
                body::EndHeldPersonBend();
                {
                    std::scoped_lock lock{ g_contact_mutex };
                    g_contact_hand = -1; // (fix177: no ease-out here)
                }
                g_live = LiveHold{};
            }
            return;
        }
        // (fix173) Once: the thread the game's knock takes for its main thread, against this frame step's (fix172's test: knocks queued).
        static bool told_thread = false;
        if (!told_thread && g.main_thread_at != 0 && g.queue_flag_at != 0) {
            told_thread = true;
            uint32_t main_id = 0;
            uint8_t  flag    = 0;
            Read(g.main_thread_at, main_id);
            Read(g.queue_flag_at, flag);
            spdlog::info("[Physical] (fix173) the game's main thread (as its knock sees it) is {}, this frame step runs on {}; the knock's queue flag {}: knocks "
                         "made here are {}",
                main_id, GetCurrentThreadId(), flag, flag != 0 && main_id != GetCurrentThreadId() ? "queued (made a moment later)" : "made at once");
        }
        UpdateCensus(now);
        UpdateHeld(now, dt);
        UpdateRegrab(now);
        UpdatePullUp(now); // (fix176)
        UpdateSeatRescue(now, dt); // (fix178)
        UpdateLiveHold(now, dt);
        UpdateShoves(now);
        UpdateBlockView(now);
        UpdateBlocks();
        UpdateWatches(now);
    }

    bool OnGrabTaken(uintptr_t ref, bool by_hand)
    {
        Person     p;
        const bool person = GameFlow::gStore.internalSettings.grabBodies && ReadPerson(ref, p, false) && !p.player; // (fix178: inside the game's grab)
        g_taken_body.store(person);
        g_taken_handle.store(person ? p.handle : 0);
        g_taken_us.store(NowUs());
        if (person && Tell(g_told_body, 30)) {
            g_body_grabs.fetch_add(1);
            spdlog::info("[Physical] (fix172) the game's grab takes {:08X} ({}, life state {}, knock state {}{}), by {}: held as a body, by the bone it touched", p.id,
                Dead(p) ? "dead" : KnockedDown(p) ? "knocked down" : "standing", p.life, p.knock, g_living_set ? "; the game's grab of the living on for it" : "",
                by_hand ? "your grip" : "the use button");
        } else if (person) {
            g_body_grabs.fetch_add(1);
        }
        return person;
    }

    bool HoldingBody() { return g_taken_body.load(std::memory_order_relaxed); }

    bool HoldingPerson(bool left) { return g_live_hand[left ? 0 : 1].load(std::memory_order_relaxed); }

    // (fix179, for Climbing; main thread) That hand's grip has a person on the way: a seated person being got up, or one knocked down to be
    // grabbed once down.
    bool PersonPending(bool left)
    {
        const int hand = left ? 0 : 1;
        return (g_pull.handle != 0 && g_pull.hand == hand) || (g_regrab.handle != 0 && g_regrab.hand == hand);
    }

    bool HeldPersonContact(bool left, HeldPersonGrip& out)
    {
        std::scoped_lock lock{ g_contact_mutex };
        if (g_contact_hand != (left ? 0 : 1) || NowUs() - g_contact_us > 200'000) {
            return false;
        }
        out.at     = g_contact;
        out.weight = g_contact_weight;
        out.centre = g_contact_centre;
        out.axis   = g_contact_axis;
        out.radius = g_contact_radius;
        out.grab   = g_contact_grab;
        out.normal = g_contact_normal; // (fix179)
        out.flat   = g_contact_flat;
        out.caps   = std::clamp(g_contact_caps, 0, 4);
        for (int k = 0; k < out.caps; ++k) {
            out.cap_a[k] = g_contact_cap_a[k];
            out.cap_b[k] = g_contact_cap_b[k];
            out.cap_r[k] = g_contact_cap_r[k];
        }
        out.bone_t = g_contact_bone_t; // (fix180)
        out.bone_r = g_contact_bone_r;
        out.bone_s = g_contact_bone_s;
        out.model  = g_contact_model; // (fix181)
        out.bare_r = g_contact_bare_r;
        std::memcpy(out.bone, g_contact_bone_name, sizeof(out.bone));
        out.bone[sizeof(out.bone) - 1] = 0;
        return true;
    }

    void BodySpring(float game_force, float game_elastic, float game_damping, float& force, float& elastic, float& damping)
    {
        // HIGGS holds a ragdoll by force-limited motors (they lag or slip instead of exploding when held too hard): from the game's own
        // (floaty) spring toward a firmer one, with the force capped.
        const float k = std::clamp(GameFlow::gStore.internalSettings.bodyGrabStrength, 0.0f, 1.0f);
        elastic       = std::clamp(game_elastic + (0.6f - game_elastic) * k, std::min(game_elastic, 0.6f), 0.6f);
        damping       = std::clamp(game_damping + (0.85f - game_damping) * k, std::min(game_damping, 0.85f), 0.85f);
        force         = std::max(std::min(game_force, 600.0f), std::min(game_force + (600.0f - game_force) * k, 600.0f));
        static std::atomic<int> told{ 0 };
        if (Tell(told, 10)) {
            spdlog::info("[Physical] (fix172) a body's springs: the game's force {:.0f}, elasticity {:.2f}, damping {:.2f} -> {:.0f}, {:.2f}, {:.2f} (strength {:.2f})",
                game_force, game_elastic, game_damping, force, elastic, damping, k);
        }
    }

    GripGrab::GripGrab(uint32_t handle, int hand)
    {
        const auto& s = GameFlow::gStore.internalSettings;
        if (handle == 0 || hand < 0 || hand > 1 || !s.grabBodies) {
            return;
        }
        GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t) {
            Person p;
            if (!ReadPerson(ref, p) || p.player || Dead(p)) {
                return; // (an object, or a corpse: the game's grab as it is)
            }
            const int64_t now = NowUs();
            if (KnockedDown(p) && !LyingDown(p)) {
                // The knock's lead-in (2: their ragdoll may not be in the world yet): grabbed once they lie (the review).
                m_handled = true;
                if (g_regrab.handle != p.handle) {
                    g_regrab = Regrab{ p.handle, hand, now, 0, false };
                }
                return;
            }
            if (LyingDown(p)) {
                // A person down: the game grabs the living only with its own setting, turned on for this one grab start.
                if (g.living && !g_living_set) {
                    g_living_before = *reinterpret_cast<const uint8_t*>(g.living_value);
                    *reinterpret_cast<uint8_t*>(g.living_value) = 1;
                    g_living_set = true;
                    m_living     = true;
                    if (Tell(g_told_living, 20)) {
                        spdlog::info("[Physical] (fix172) your {} grip takes {:08X}, knocked down (knock state {}): the game's grab of the living turned on for this grab "
                                     "(it was {})",
                            hand == 0 ? "left" : "right", p.id, p.knock, g_living_before);
                    }
                } else if (!g.living && Tell(g_told_living, 20)) {
                    spdlog::info("[Physical] (fix172) your {} grip on {:08X}, knocked down: the game's grab of the living setting was not found, so the game will "
                                 "not take them",
                        hand == 0 ? "left" : "right", p.id);
                }
                return;
            }
            m_handled = true; // (the game would not take a living, standing person anyway)
            // (fix173) Standing: held at your hand, not knocked down (after fix172: the grip should hold them, not knock them
            // down); see LiveHold.
            if (s.holdPeople) {
                // (fix176) Sitting or lying: got up first (the pull-up), then held.
                if (p.furniture && g.seat_exit) {
                    StartPullUp(ref, p, hand, now);
                } else {
                    StartLiveHold(ref, p, hand, now);
                }
                return;
            }
            // (HoldPeople off:) knocked down in place first (force 0), grabbed once down (UpdateRegrab).
            const char* protect = Protected(p);
            Mood&       m       = MoodOf(p.handle, now);
            if (!s.grabKnocksDown || protect != nullptr || Big(p) || now - m.knocked_us < kKnockCooldownUs) {
                if (Tell(g_told_grip, 30)) {
                    spdlog::info("[Physical] (fix172) your {} grip on {:08X} (standing): not knocked down ({})", hand == 0 ? "left" : "right", p.id,
                        !s.grabKnocksDown ? "the setting is off" : protect != nullptr ? protect : Big(p) ? "too big" : "knocked down a moment ago");
                }
                return;
            }
            glm::vec3 from = p.place;
            // (Force 0 gives no time down at all: they would be up again before the grab; the knock taken, they are given some (the review).)
            // (fix173) Not checked at once (the knock is made a moment later from this thread): UpdateRegrab waits for them to be down.
            if (Knock(ref, from, 0.0f, "your grip")) {
                m.knocked_us = now;
                g_knockdowns.fetch_add(1);
                StartWatch(p.handle, p.id, "your grip's knockdown");
                g_regrab = Regrab{ p.handle, hand, now, 0, false };
                haptics::MeleePulse(hand == 0, 0.45f, 0.08f);
                if (Tell(g_told_grip, 30)) {
                    spdlog::info("[Physical] (fix173) your {} grip on {:08X} (standing, knock state {}): knocked down in place (PushActorAway's code, force 0, made a "
                                 "moment later); grabbed once down if the grip is still held",
                        hand == 0 ? "left" : "right", p.id, p.knock);
                }
                AddAnger(ref, p, 2, now, "knocked down by your grip");
            }
        });
    }

    GripGrab::~GripGrab()
    {
        if (m_living && g_living_set && g.living_value != 0) {
            *reinterpret_cast<uint8_t*>(g.living_value) = g_living_before;
            g_living_set                                = false;
        }
    }

    bool RefusePull(uint32_t handle)
    {
        static uint32_t last_handle = 0;
        static bool     last_result = false;
        static int64_t  last_us     = 0;
        const int64_t   now         = NowUs();
        if (handle == 0 || !GameFlow::gStore.internalSettings.grabBodies) {
            return false;
        }
        if (handle == last_handle && now - last_us < 500'000) {
            return last_result;
        }
        bool person = false;
        GrenadeHand::WithReference(handle, [&](uintptr_t ref, uintptr_t) {
            Person p;
            person = ReadPerson(ref, p) && !p.player;
        });
        if (person && (handle != last_handle || !last_result) && Tell(g_told_pull, 10)) {
            spdlog::info("[Physical] (fix172) the gravity gloves found a person ({:x}): not pulled (bodies are grabbed with the hand, not flown by a limb)", handle);
        }
        last_handle = handle;
        last_result = person;
        last_us     = now;
        return person;
    }

    void OnHandHit(int hand, uintptr_t target, float speed, const glm::vec3& stage_direction)
    {
        const auto& s = GameFlow::gStore.internalSettings;
        if (!s.punchKnockdown || hand < 0 || hand > 1 || target == 0) {
            return;
        }
        // Only a fist: the right hand with your fists out (not a melee weapon or a gun), the left without its own weapon.
        const bool fist = hand == 0 ? !LeftWeapon::Held() : HandGrab::HandsFree() && HandGrab::MeleeWeaponOut() == 0;
        const float threshold = std::clamp(s.knockdownSpeed, 3.0f, 10.0f);
        Person      p;
        if (!fist || !ReadPerson(target, p) || p.player || Dead(p)) {
            return;
        }
        if (speed < threshold) {
            if (Tell(g_told_punch, 30)) {
                spdlog::info("[Physical] (fix172) your {} fist hit {:08X} at {:.1f} m/s: the game's hit only (a knockdown needs {:.1f})", hand == 0 ? "left" : "right", p.id,
                    speed, threshold);
            }
            return;
        }
        const int64_t now     = NowUs();
        const char*   protect = Protected(p);
        Mood&         m       = MoodOf(p.handle, now);
        if (protect != nullptr || Big(p) || KnockedDown(p) || now - m.knocked_us < kKnockCooldownUs) {
            if (Tell(g_told_punch, 30)) {
                spdlog::info("[Physical] (fix172) your {} fist hit {:08X} hard ({:.1f} m/s): not knocked down ({})", hand == 0 ? "left" : "right", p.id, speed,
                    protect != nullptr ? protect : Big(p) ? "too big" : KnockedDown(p) ? "already down" : "knocked down a moment ago");
            }
            return;
        }
        // Pushed away along the punch: from a point behind them on its line, the force from how far past the threshold it was.
        glm::vec3 unused{}, way{};
        const glm::mat4 grip = tracking::GripPose(hand == 0);
        if (!tracking::StageToWorld(glm::vec3{ grip[3] }, stage_direction, unused, way) || !Finite(way) || glm::length(way) < 0.5f) {
            way = glm::vec3{ 0.0f };
        }
        const glm::vec3 middle = p.have_bound ? p.centre : p.place + glm::vec3{ 0.0f, 0.0f, 1.0f };
        glm::vec3       level{ way.x, way.y, 0.0f };
        level                  = glm::length(level) > 0.1f ? glm::normalize(level) : glm::vec3{ 0.0f };
        const glm::vec3 from   = middle - level * 1.0f - glm::vec3{ 0.0f, 0.0f, 0.3f };
        const float     force  = std::clamp(1.0f + (speed - threshold), 1.0f, 4.0f);
        // (fix173) The game makes the knock a moment later (fix172's test: the knock state still 0 right after the call, 6 two seconds later):
        // PushActorAway's code queues it on the game's task queue unless called on the game's own main thread, which the mod's frame step
        // is not. So it is not checked at once any more (fix172 took that for a refusal: no cooldown, no anger); the watch logs what the
        // game made of it over the next seconds.
        if (Knock(target, from, force, "a hard punch")) {
            m.knocked_us = now;
            g_knockdowns.fetch_add(1);
            StartWatch(p.handle, p.id, "a punch's knockdown");
            haptics::MeleePulse(hand == 0, 0.9f, 0.12f);
            if (Tell(g_told_knock, 40)) {
                spdlog::info("[Physical] (fix173) your {} fist hit {:08X} at {:.1f} m/s (needs {:.1f}): knocked down (PushActorAway's code, force {:.1f}, from {:.2f} m "
                             "behind them; the game makes it a moment later); the game gets them up",
                    hand == 0 ? "left" : "right", p.id, speed, threshold, force, glm::length(middle - from));
            }
        }
    }

    bool SpareTarget(uintptr_t target)
    {
        if (!GameFlow::gStore.internalSettings.spareFollowers) {
            return false;
        }
        Person p;
        if (!ReadPerson(target, p) || p.player || !(p.teammate || p.commanded)) {
            return false;
        }
        g_spared.fetch_add(1);
        if (Tell(g_told_spare, 10)) {
            spdlog::info("[Physical] (fix172) your hand's swing reached {:08X} ({}): not hit (followers are spared: the setting)", p.id,
                p.teammate ? "your follower" : "commanded by you");
        }
        return true;
    }

    bool BlockAttack(uintptr_t attacker, const glm::vec3& origin, const glm::vec3& direction, float length, float reach)
    {
        if (!GameFlow::gStore.internalSettings.saberBlock || attacker == 0 || !Finite(origin) || !Finite(direction) || !std::isfinite(length)) {
            return false;
        }
        BlockView v;
        {
            std::scoped_lock _{ g_block_mutex };
            v = g_view;
        }
        if (!v.have_you || NowUs() - v.us > 200'000) {
            return false;
        }
        // Aimed at you: the attack's line passes near your body (feet to head) within its reach.
        const float     dlen  = glm::length(direction);
        if (!(dlen > 0.5f)) {
            return false;
        }
        const glm::vec3 way   = direction / dlen;
        const float     span  = std::clamp(std::max(length, std::isfinite(reach) ? reach : 0.0f), 0.5f, 6.0f);
        float           s = 0.0f, t = 0.0f;
        const glm::vec3 lo = v.feet + glm::vec3{ 0.0f, 0.0f, 0.5f };
        const float     to_you = SegmentDistance(origin, origin + way * span, lo, v.head, s, t);
        if (to_you > kBlockAimedAtYou) {
            return false;
        }
        const int seen = g_told_npc_hit.fetch_add(1);
        const glm::vec3 you = lo + (v.head - lo) * t; // (the nearest point of your body to the attack's line)
        // The attack's path, from where it starts to you; your blade across it, between the attacker and you.
        int   hand = -1;
        float best = 1e9f;
        for (int k = 0; k < 2; ++k) {
            if (!v.blade[k]) {
                continue;
            }
            float     sb = 0.0f, tb = 0.0f;
            const float d = SegmentDistance(origin, you, v.hilt[k], v.tip[k], sb, tb);
            if (d < kBlockRadius && sb > 0.05f && d < best) {
                best = d;
                hand = k;
            }
        }
        if (seen < 20) {
            spdlog::info("[Physical] (fix172) an NPC's melee hit at you (attacker {:x}): its path {:.2f} m long (reach {:.2f}) passes {:.2f} m from your body; your "
                         "blades {}{}: {}",
                attacker, length, reach, to_you, v.blade[1] ? "right" : "", v.blade[0] ? (v.blade[1] ? " and left" : "left") : (v.blade[1] ? "" : "none out"),
                hand >= 0 ? fmt::format("blocked by your {} blade ({:.2f} m from its path)", hand == 0 ? "left" : "right", best) : std::string{ "not blocked" });
        }
        if (hand < 0) {
            return false;
        }
        uint32_t handle = 0;
        Read(attacker + kRefHandle, handle);
        {
            std::scoped_lock _{ g_block_mutex };
            if (handle != 0 && g_block_ask_count < 8) {
                g_block_asks[g_block_ask_count++] = BlockAsk{ handle, hand, length > 2.5f };
            }
        }
        g_blocks.fetch_add(1);
        haptics::MeleePulse(hand == 0, 0.9f, 0.12f);
        return true;
    }

    bool TakePhysicsTimes(int& calls, float& average_ms, float& longest_ms)
    {
        calls      = 0;
        average_ms = longest_ms = 0.0f;
        if (!g_installed || !g.timer) {
            return false;
        }
        const int64_t n     = g_watch_calls.exchange(0, std::memory_order_relaxed);
        const int64_t sum   = g_watch_ticks.exchange(0, std::memory_order_relaxed);
        const int64_t most  = g_watch_max_ticks.exchange(0, std::memory_order_relaxed);
        const double  to_ms = 1000.0 / static_cast<double>(g_qpc_freq);
        calls               = static_cast<int>(n);
        average_ms          = n > 0 ? static_cast<float>(static_cast<double>(sum) / static_cast<double>(n) * to_ms) : 0.0f;
        longest_ms          = static_cast<float>(static_cast<double>(most) * to_ms);
        return true;
    }

    std::string Status()
    {
        if (!g_installed) {
            return "Physical interaction: not installed.";
        }
        return fmt::format("Physical interaction (fix172): {} bodies grabbed, {} shoves ({} staggers), {} knockdowns, {} blocks, {} assault alarms, {} follower hits "
                           "spared. Game code: knock {}, stagger {}, actions {}, grab of the living {}, physics timer {}.",
            g_body_grabs.load(), g_shoves.load(), g_staggers.load(), g_knockdowns.load(), g_blocks.load(), g_alarms.load(), g_spared.load(), g.knock ? "ok" : "missing",
            g.stagger ? "ok" : "missing", g.actions ? "ok" : "missing", g.living ? "ok" : "missing", g.timer ? "ok" : "missing");
    }
}
