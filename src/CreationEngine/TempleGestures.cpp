#include "TempleGestures.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <Windows.h>
#include <glm/glm.hpp>
#include <mods/VR.hpp>
#include <RE/N/NiCamera.h>
#include <spdlog/spdlog.h>

#include <ModSettings.h>

#include "CreationEngineSingletonManager.h"
#include "Haptics.h"
#include "body/BodyIK.h"
#include "body/BodyMath.h"
#include "memory/GameCode.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace TempleGestures
{
    namespace
    {
        // A temple: a box beside the head, in the head's own axes (metres from the eyes): out to the side, up, and forward. (fix143:
        // the hand within 0.11 m of a point 0.09 m beside the eyes before; the headset is 0.18 m wide, so the controller's middle
        // cannot come nearer than about 0.13 m, and fix142's pulls came 0.11-0.14 m (left) and 0.21-0.26 m (right) from it.) Never in
        // front of the face (a gun held up to the eye).
        constexpr float kSideMin    = 0.06f;
        constexpr float kSideMax    = 0.32f;
        constexpr float kUpMin      = -0.12f;
        constexpr float kUpMax      = 0.22f;
        constexpr float kForwardMin = -0.22f;
        constexpr float kForwardMax = 0.10f;
        // (For the log: a pull with the hand this near the eyes but not at a temple.)
        constexpr float kNear = 0.45f;
        // The trigger: pulled past this, let go below this.
        constexpr float kTriggerOn  = 0.6f;
        constexpr float kTriggerOff = 0.3f;
        // LB for the game: held for the flashlight (the left temple), tapped for the scanner (the right). The game tells a tap from a
        // hold as LB is let go (fix142's test: the scanner opened 60 ms after a 0.27 s press), so the left temple holds it a full
        // second whatever the trigger does (fix142 held it as long as the trigger: 0.2-0.27 s pulls, both temples the scanner).
        constexpr int64_t kHoldUs = 1'000'000;
        constexpr int64_t kTapUs  = 100'000;

        std::atomic<int64_t> g_lb_until{ 0 };
        std::atomic<bool>    g_held_back[2]{ false, false }; // [0] the left trigger, [1] the right
        std::atomic<bool>    g_at_temple[2]{ false, false }; // (the hand there now: its trigger held back from the first moment)
        // (fix143's review: the box is large; a trigger already held as the hand came into it (a gun fired beside the head) stays the
        // game's.)
        std::atomic<bool>    g_entered_held[2]{ false, false };
        bool                 g_was_there[2]{ false, false };
        bool                 g_trigger_down[2]{ false, false };
        int                  g_uses[2]{ 0, 0 };
        int                  g_near_logs{ 0 };

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ---- (fix173) The flashlight by the game's own switch ----
        // The flashlight still did not show (robes and a hood, no helmet). The game's "Monocle" input (LB, F; its handler 0x1412BD150)
        // takes a hold of 0.4 s or more for the flashlight, but first counts the worn items with the spacesuit helmet keyword
        // (0x140B6BE70 with KeywordSpaceSuitHelmet_DO): none, and it only plays UIMenuGeneralActivateFailure. With one, and its controls
        // check (0x1422F9940(controls [0x1461E70C0], 0x40)), it calls the switch 0x1418C88A0 with the game's task queue ([0x145E794E8],
        // at 0x1412BD3CB/0x1412BD3D2), which runs PlayerCharacter's flashlight toggle 0x141A68680 at once on the game's main thread or
        // queued for it from any other: the on byte 0x145EDBE77 flipped, the helmet light's sound, and the light made or removed
        // (0x141A68A10; it checks no helmet). So the left temple calls that switch itself (its controls check kept), helmet or not. Each
        // address is checked against the game's own code once.
        struct Flashlight
        {
            bool      checked{ false };
            bool      ok{ false };
            uintptr_t base{ 0 };
            uintptr_t queue_at{ 0 };   // the game's task queue (a global holding it)
            uintptr_t toggle{ 0 };     // 0x1418C88A0(queue)
            uintptr_t on_at{ 0 };      // the on byte
            uintptr_t controls_at{ 0 }; // the controls (a global holding them)
            uintptr_t controls_fn{ 0 }; // 0x1422F9940(controls, 0x40): true when the flashlight's controls are on
        };
        Flashlight g_light;
        // After a switch: what the game made of it, logged at these times (s).
        int64_t g_light_since_us{ 0 };
        int     g_light_checks{ 0 };
        bool    g_light_before{ false };
        int     g_light_logs{ 0 };
        constexpr uintptr_t kPlayerLight = 0xEB0; // PlayerCharacter's flashlight (0x141A68AD1: let go there when switched off)
        // (fix190) The places above by their 1.16.236 rva: in the Monocle handler the controls check's 0x40, its load of the controls and
        // its call, the load of the task queue and the call of the switch; the switch; in the flashlight toggle the load of the on byte;
        // the task queue's global, the on byte, the controls' global and the controls check.
        constexpr uintptr_t kMonocleControlsRva     = 0x12BD3B6;
        constexpr uintptr_t kMonocleControlsLoadRva = 0x12BD3BB;
        constexpr uintptr_t kMonocleControlsCallRva = 0x12BD3C2;
        constexpr uintptr_t kMonocleQueueLoadRva    = 0x12BD3CB;
        constexpr uintptr_t kMonocleSwitchCallRva   = 0x12BD3D2;
        constexpr uintptr_t kLightSwitchRva         = 0x18C88A0;
        constexpr uintptr_t kLightOnLoadRva         = 0x1A68689;
        constexpr uintptr_t kTaskQueueRva           = 0x5E794E8;
        constexpr uintptr_t kLightOnByteRva         = 0x5EDBE77;
        constexpr uintptr_t kControlsRva            = 0x61E70C0;
        constexpr uintptr_t kControlsCheckRva       = 0x22F9940;

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        uintptr_t RipAt(uintptr_t at, size_t disp_at, size_t length)
        {
            int32_t disp = 0;
            std::memcpy(&disp, reinterpret_cast<const void*>(at + disp_at), sizeof(disp));
            return at + length + static_cast<intptr_t>(disp);
        }

        void CheckFlashlight()
        {
            if (g_light.checked) {
                return;
            }
            g_light.checked = true;
            const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            static constexpr uint8_t kLoadRcx[]     = { 0x48, 0x8B, 0x0D };
            static constexpr uint8_t kMovEdx40[]    = { 0xBA, 0x40, 0x00, 0x00, 0x00 };
            static constexpr uint8_t kCall[]        = { 0xE8 };
            static constexpr uint8_t kLoadByte[]    = { 0x0F, 0xB6, 0x05 };
            static constexpr uint8_t kSwitchStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48,
                0x83, 0xEC, 0x30 };
            __try {
                if (b != 0 && SameCode(kMonocleControlsRva, kMovEdx40, sizeof(kMovEdx40)) && SameCode(kMonocleControlsLoadRva, kLoadRcx, sizeof(kLoadRcx)) &&
                    SameCode(kMonocleControlsCallRva, kCall, 1) && SameCode(kMonocleQueueLoadRva, kLoadRcx, sizeof(kLoadRcx)) && SameCode(kMonocleSwitchCallRva, kCall, 1) &&
                    RipAt(At(kMonocleSwitchCallRva), 1, 5) == At(kLightSwitchRva) && SameCode(kLightSwitchRva, kSwitchStart, sizeof(kSwitchStart)) &&
                    SameCode(kLightOnLoadRva, kLoadByte, sizeof(kLoadByte))) {
                    g_light.base        = b;
                    g_light.controls_at = RipAt(At(kMonocleControlsLoadRva), 3, 7);
                    g_light.controls_fn = RipAt(At(kMonocleControlsCallRva), 1, 5);
                    g_light.queue_at    = RipAt(At(kMonocleQueueLoadRva), 3, 7);
                    g_light.toggle      = At(kLightSwitchRva);
                    g_light.on_at       = RipAt(At(kLightOnLoadRva), 3, 7);
                    g_light.ok          = g_light.queue_at == At(kTaskQueueRva) && g_light.on_at == At(kLightOnByteRva) && g_light.controls_at == At(kControlsRva) &&
                                 g_light.controls_fn == At(kControlsCheckRva);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_light.ok = false;
            }
            spdlog::info("[Temple] (fix173) the game's flashlight switch {} (its task queue at rva {:x}, the on byte at {:x}, its controls check at {:x})",
                g_light.ok ? "found: the left temple switches it itself, helmet or not" : "not found: the left temple holds LB for the game as before",
                g_light.queue_at - b, g_light.on_at - b, g_light.controls_fn - b);
        }

        bool LightOn() { return g_light.ok && *reinterpret_cast<const volatile uint8_t*>(g_light.on_at) != 0; }

        // The game's flashlight switched (any thread: the game's switch queues itself for its main thread). False: its controls are off now
        // (as the game's own input checks), or nothing to switch with.
        bool SwitchFlashlight(bool& was_on)
        {
            using QueueFn    = void (*)(void* queue);
            using ControlsFn = bool (*)(void* controls, uint32_t which);
            was_on           = LightOn();
            uintptr_t queue = 0, controls = 0;
            std::memcpy(&queue, reinterpret_cast<const void*>(g_light.queue_at), sizeof(queue));
            std::memcpy(&controls, reinterpret_cast<const void*>(g_light.controls_at), sizeof(controls));
            if (queue == 0 || controls == 0 || !reinterpret_cast<ControlsFn>(g_light.controls_fn)(reinterpret_cast<void*>(controls), 0x40)) {
                return false;
            }
            reinterpret_cast<QueueFn>(g_light.toggle)(reinterpret_cast<void*>(queue));
            return true;
        }

        // The light's nodes up to the root, with each one's hidden mark (the flags' bit 0 at +0x118, as BodyIK reads it), for the log: a light
        // under a hidden node (the first-person body the mod hides) may not be drawn.
        void LightChain(char* out, size_t size)
        {
            out[0] = 0;
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (player == 0) {
                return;
            }
            __try {
                auto* node = *reinterpret_cast<RE::NiAVObject**>(player + kPlayerLight);
                if (node == nullptr) {
                    std::snprintf(out, size, "no light made");
                    return;
                }
                size_t used = 0;
                for (int i = 0; i < 12 && node != nullptr && used + 1 < size; ++i) {
                    const char*    name   = node->name.c_str();
                    const uint64_t flags  = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(node) + 0x118);
                    const int      wrote  = std::snprintf(out + used, size - used, "%s%s%s", i == 0 ? "" : " < ", name != nullptr && name[0] != 0 ? name : "(no name)",
                        (flags & 1) != 0 ? " (hidden)" : "");
                    if (wrote <= 0) {
                        break;
                    }
                    used = std::min(size - 1, used + static_cast<size_t>(wrote));
                    node = node->parent;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                std::snprintf(out, size, "(not readable)");
            }
        }

        // ---- (fix174) The light shown at your head ----
        // fix173's test: the flashlight clicked on but could hardly be seen; the log: the light made ("Shadow Spot Light < p-AttachLight <
        // Camera < Root < HumanExportRoot (hidden)"). A helper's research of the exe: SetAppCulled (slot 61) walks the whole subtree and
        // calls each node's +0x1E0(true); for a light (0x1424397D0 -> 0x1424397E0, edx 4) that sets bit 4 of its renderer state (a copy at
        // +0x358): hidden, as the first-person arms are; and attaching a node under a culled one (SetParent 0x142BD8590, 0x142BD8646) hides
        // it the same way: the game hangs the flashlight under the first-person root the mod hides, so it is hidden from birth. World
        // transforms still update under a culled parent (UpdateDownwardPass 0x142BD6510 does not look at it). A spot light shines along
        // its node's own +X (0x14243A378: column 0 of its matrix), as the game's camera looks along its own. The game's own p-AttachLight
        // hangs off the first-person Camera bone, which the mod does not turn with your head. So, as FRIK places its Fallout 4 light (never
        // moving the node: "safer than moving the node as that can result in game crash"): in the light's own update (here, before the
        // game's), while the body is shown, its hidden bit cleared with its own +0x1E0(false) (as the game's un-hide visitor does,
        // 0x142BD9630) and its local transform set so it sits just in front of your eyes and a little up (clear of a hood), shining where you
        // look. Nothing else is changed (no AppCulled flag, no parent).
        constexpr uintptr_t kLightTableRva       = 0x4D702E0; // BSLight
        constexpr uintptr_t kShadowLightTableRva = 0x4D77A80; // BSShadowLight ("Shadow Spot Light")
        constexpr uintptr_t kLightShowSlot       = 0x1E0;     // its show/hide (false: shown)
        constexpr uintptr_t kLightShowRva        = 0x24397D0;
        constexpr uintptr_t kLightState          = 0x358;     // its renderer state's copy (bit 4: hidden)
        constexpr float     kLightAhead          = 0.12f;     // m in front of your eyes
        constexpr float     kLightUp             = 0.05f;     // m above them
        std::atomic<uintptr_t> g_light_node{ 0 };             // the light (PlayerCharacter+0xEB0) while it is on and the body shown
        std::atomic<bool>      g_light_checked{ false };
        std::atomic<int>       g_light_placed{ 0 }, g_light_unhid{ 0 }, g_light_hidden_seen{ 0 };
        std::atomic<int64_t>   g_light_log_us{ 0 };
        std::atomic<bool>      g_light_told_game{ false };
        std::atomic<float>     g_light_last_from_eyes{ 0.0f };
        std::atomic<bool>      g_place_failed{ false };
        int                    g_place_logs{ 0 };
        uintptr_t              g_light_seen{ 0 };             // (the frame step's: the light of the last frame, for the log's first second)
        // (The review) The light's own local as the game had it, saved as it was first placed, and put back in its update once the mod no
        // longer places it (the body not shown: a ladder, a seat, third person), so the game's own flashlight is as it made it.
        // (Only ever touched in that light's own update, one at a time: the atomic orders the saved local between the frames' threads.)
        std::atomic<uintptr_t> g_light_restore{ 0 };
        RE::NiTransform        g_light_saved{};

        void RestoreLightInner(RE::NiAVObject* light)
        {
            const uintptr_t base  = g_light.base;
            const uintptr_t table = *reinterpret_cast<const uintptr_t*>(light);
            if (base == 0 || table == 0 || (table != At(kLightTableRva) && table != At(kShadowLightTableRva))) { // (fix190: 0 never a match)
                return;
            }
            light->local = g_light_saved;
        }

        bool RestoreLightTry(RE::NiAVObject* light)
        {
            __try {
                RestoreLightInner(light);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The light hangs under the first-person root (as the game makes it, and the research found): checked each frame (the review: a
        // pointer and its type table alone could be another light at a freed light's address).
        bool UnderFirstPersonGuarded(uintptr_t light, uintptr_t first_root)
        {
            __try {
                const auto* node = reinterpret_cast<const RE::NiAVObject*>(light);
                for (int i = 0; i < 16 && node != nullptr; ++i) {
                    if (reinterpret_cast<uintptr_t>(node) == first_root) {
                        return true;
                    }
                    node = node->parent;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return false;
        }

        // In the light's own update (the game's update thread): shown and placed. False: not this frame (logged by the caller's counters).
        void PlaceLightInner(RE::NiAVObject* light)
        {
            const uintptr_t base  = g_light.base;
            const uintptr_t table = *reinterpret_cast<const uintptr_t*>(light);
            if (base == 0 || table == 0 || (table != At(kLightTableRva) && table != At(kShadowLightTableRva)) || light->parent == nullptr) {
                return;
            }
            // Shown again, with its own call, whenever it is hidden (a new light, or the first-person root hidden again).
            const uint32_t state = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(light) + kLightState);
            if ((state & 4u) != 0) {
                g_light_hidden_seen.fetch_add(1, std::memory_order_relaxed);
                const uintptr_t show = *reinterpret_cast<const uintptr_t*>(table + kLightShowSlot);
                if (show == At(kLightShowRva)) {
                    using ShowFn = void (*)(RE::NiAVObject*, bool);
                    reinterpret_cast<ShowFn>(show)(light, false);
                    g_light_unhid.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Where you look from, and the way (game world).
            const glm::mat4 head = tracking::HeadPose();
            glm::vec3       eyes{}, look{};
            float           scale = 1.0f;
            if (!tracking::StageToWorld(glm::vec3{ head[3] }, -glm::vec3{ head[2] }, eyes, look, &scale) || !std::isfinite(eyes.x) || !std::isfinite(look.x) ||
                glm::length(look) < 1e-4f) {
                return;
            }
            look = glm::normalize(look);
            if (!std::isfinite(scale) || scale <= 0.0f) {
                scale = 1.0f;
            }
            // (Once: the game's own beam against your view, before the first placing: where p-AttachLight points in VR.)
            if (!g_light_told_game.exchange(true)) {
                const body::Xf  was  = body::FromNi(light->world);
                const glm::vec3 beam = glm::length(was.r[0]) > 1e-4f ? glm::normalize(was.r[0]) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                spdlog::info("[Temple] (fix174) the game's own flashlight: {:.2f} m from your eyes, its beam {:.0f} degrees off your view; now shown and put at "
                             "your head",
                    glm::length(was.t - eyes), glm::degrees(std::acos(std::clamp(glm::dot(beam, look), -1.0f, 1.0f))));
            }
            const glm::vec3 up_world{ 0.0f, 0.0f, 1.0f };
            glm::vec3       up = up_world - look * glm::dot(up_world, look);
            if (glm::length(up) < 1e-3f) {
                up = glm::vec3{ 0.0f, 1.0f, 0.0f } - look * look.y;
            }
            up = glm::normalize(up);
            body::Xf wanted{};
            wanted.r[0] = look;                   // (its beam: its own +X)
            wanted.r[1] = glm::cross(up, look);
            wanted.r[2] = up;
            wanted.t    = eyes + look * (kLightAhead * scale) + up_world * (kLightUp * scale);
            wanted.s    = 1.0f;
            const body::Xf parent = body::FromNi(light->parent->world);
            const body::Xf local  = body::Compose(body::Inverse(parent), wanted);
            if (!std::isfinite(local.t.x) || !std::isfinite(local.t.y) || !std::isfinite(local.t.z) || !std::isfinite(local.s) || local.s <= 0.0f) {
                return;
            }
            // (The game's own local saved before the first placing of this light: put back when placing stops.)
            if (g_light_restore.load() != reinterpret_cast<uintptr_t>(light)) {
                g_light_saved = light->local;
                g_light_restore.store(reinterpret_cast<uintptr_t>(light));
            }
            body::ToNi(local, light->local);
            g_light_placed.fetch_add(1, std::memory_order_relaxed);
            g_light_last_from_eyes.store(glm::length(wanted.t - eyes), std::memory_order_relaxed);
        }

        bool PlaceLightTry(RE::NiAVObject* light)
        {
            __try {
                PlaceLightInner(light);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // After a switch: the on byte and the light at 1.5 s and 8 s (the game can switch it off again itself, e.g. as it checks the helmet).
        void WatchFlashlight(int64_t now)
        {
            if (g_light_since_us == 0 || g_light_logs >= 20) {
                return;
            }
            static constexpr int64_t kAt[] = { 1'500'000, 8'000'000 };
            if (g_light_checks >= 2) {
                g_light_since_us = 0;
                return;
            }
            if (now - g_light_since_us < kAt[g_light_checks]) {
                return;
            }
            ++g_light_checks;
            ++g_light_logs;
            char chain[512];
            LightChain(chain, sizeof(chain));
            spdlog::info("[Temple] (fix173) the flashlight {:.1f} s after the switch: {} (was {}); its light: {}", (now - g_light_since_us) * 1e-6,
                LightOn() ? "on" : "off", g_light_before ? "on" : "off", chain);
        }

        bool InPlay()
        {
            static auto vr = VR::get();
            // (fix175: not in build mode either, no menu by the count: the right temple's LB there would change the build category.)
            return vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() &&
                   !GameFlow::isWorkshopShown() && !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && !ModSettings::showFlatScreenDisplay() &&
                   CreationEngineSingletonManager::GetPlayerRef() != nullptr;
        }
    }

    void Update()
    {
        static auto vr       = VR::get();
        const auto& settings = GameFlow::gStore.internalSettings;
        const glm::mat4 head = tracking::HeadPose();
        const glm::vec3 eyes{ head[3] };
        const glm::vec3 right   = glm::normalize(glm::vec3{ head[0] });
        const glm::vec3 up      = glm::normalize(glm::vec3{ head[1] });
        const glm::vec3 forward = -glm::normalize(glm::vec3{ head[2] });
        const bool      play    = settings.templeGestures && InPlay() && std::isfinite(eyes.x);
        WatchFlashlight(NowUs());
        // (fix174) The light to show at your head this frame: the game's flashlight while it is on and the body is shown.
        {
            uintptr_t light = 0;
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (g_light.ok && !g_place_failed.load() && player != 0 && LightOn() && body::IsActive() && vr->is_hmd_active() && !ModSettings::cameraFlat()) {
                uintptr_t first_root = 0;
                std::memcpy(&light, reinterpret_cast<const void*>(player + kPlayerLight), sizeof(light));
                std::memcpy(&first_root, reinterpret_cast<const void*>(player + 0xDE8), sizeof(first_root)); // (BodyIK's first-person root)
                if (light != 0 && (first_root == 0 || !UnderFirstPersonGuarded(light, first_root))) {
                    light = 0; // (not where the game makes it: left alone)
                }
            }
            g_light_node.store(light); // (after g_light was filled: seen in the update threads with it)
            // (fix189) The light whose own local is kept to put back: forgotten once it is no longer the player's light (switched off, the
            // game removes it and frees it; another light made later at its address would otherwise be given the old flashlight's local once,
            // the review). Its put-back is for the body not shown while the same light stays on (a ladder, a seat, third person).
            if (uintptr_t kept = g_light_restore.load(); kept != 0) {
                uintptr_t current = 0;
                if (player != 0) {
                    std::memcpy(&current, reinterpret_cast<const void*>(player + kPlayerLight), sizeof(current));
                }
                if (current != kept) {
                    g_light_restore.compare_exchange_strong(kept, 0); // (not if its update just kept a new one)
                }
            }
            // Once a second while it is on: shown and placed how often (from a second after it is first placed).
            const int64_t now_us = NowUs();
            if (light != g_light_seen) {
                g_light_seen = light;
                g_light_log_us = now_us;
                g_light_placed = 0;
                g_light_unhid = 0;
                g_light_hidden_seen = 0;
            }
            if (light != 0 && now_us - g_light_log_us.load() > 1'000'000) {
                const int placed = g_light_placed.exchange(0), unhid = g_light_unhid.exchange(0), hidden = g_light_hidden_seen.exchange(0);
                if (g_place_logs < 40) {
                    ++g_place_logs;
                    spdlog::info("[Temple] (fix174) the flashlight at your head: placed {} times in the last second ({} m in front of your eyes), found "
                                 "hidden {} times, shown again {} times{}",
                        placed, g_light_last_from_eyes.load(), hidden, unhid,
                        placed == 0 ? " (its update not seen: not placed; the game's own place)" : "");
                }
                g_light_log_us = now_us;
            }
        }
        // Each of the game's hands ([0] its left, [1] its right: as the triggers are sent to the game and held back) and the
        // controller in it: which one that is in your hands (swapped controllers: the game's left is your right) decides the
        // temple and what the gesture does, and its trigger and buzz.
        for (int hand = 0; hand < 2; ++hand) {
            const int   index      = hand == 0 ? vr->get_left_controller_index() : vr->get_right_controller_index();
            const bool  yours_left = index == 1; // (the physical left controller)
            const float trigger    = vr->get_finger_state(yours_left).trigger;
            const bool  down       = g_trigger_down[hand] ? trigger > kTriggerOff : trigger > kTriggerOn;
            const bool  pulled     = down && !g_trigger_down[hand];
            g_trigger_down[hand]   = down;
            // (Held back until it is let go; let go, a pull in the box is held back from its first moment again.)
            if (!down) {
                g_held_back[hand]    = false;
                g_entered_held[hand] = false;
            }
            // The hand's grip against the eyes, in the head's axes: out to its own side, up, forward.
            const glm::vec3 at{ tracking::GripPose(hand == 0)[3] };
            const glm::vec3 d      = at - eyes;
            const float     side   = glm::dot(d, right) * (yours_left ? -1.0f : 1.0f);
            const float     height = glm::dot(d, up);
            const float     ahead  = glm::dot(d, forward);
            const bool      known  = std::isfinite(at.x);
            const bool      there  = play && known && side >= kSideMin && side <= kSideMax && height >= kUpMin && height <= kUpMax &&
                                ahead >= kForwardMin && ahead <= kForwardMax;
            if (there && !g_was_there[hand]) {
                g_entered_held[hand] = down && !pulled;
            }
            g_was_there[hand] = there;
            g_at_temple[hand] = there;
            if (pulled && play && known && !there && glm::length(d) <= kNear && g_near_logs < 20) {
                ++g_near_logs;
                spdlog::info("[Temple] {} trigger pulled with your hand {:.2f} m out to the side, {:.2f} up, {:.2f} ahead of your eyes (a temple: "
                             "{:.2f}-{:.2f} out, {:.2f}-{:.2f} up, {:.2f}-{:.2f} ahead)",
                    yours_left ? "left" : "right", side, height, ahead, kSideMin, kSideMax, kUpMin, kUpMax, kForwardMin, kForwardMax);
            }
            if (!pulled || !there) {
                continue;
            }
            g_held_back[hand] = true;
            // (One at a time: the other's LB still pressed for the game, this one waits for a pull after it.)
            const int64_t now = NowUs();
            if (now < g_lb_until.load()) {
                continue;
            }
            // (fix173) The left temple: the game's flashlight switch itself (helmet or not), no LB.
            if (yours_left) {
                CheckFlashlight();
            }
            if (yours_left && g_light.ok) {
                bool       was_on   = false;
                const bool switched = SwitchFlashlight(was_on);
                haptics::MeleePulse(true, switched ? 0.4f : 0.15f, switched ? 0.06f : 0.03f);
                if (switched) {
                    g_light_since_us = now;
                    g_light_checks   = 0;
                    g_light_before   = was_on;
                }
                if (g_uses[hand]++ < 10) {
                    spdlog::info("[Temple] (fix173) left trigger at your left temple ({:.2f} out, {:.2f} up, {:.2f} ahead): {}", side, height, ahead,
                        switched ? (was_on ? "the flashlight switched off (the game's own switch)" : "the flashlight switched on (the game's own switch)")
                                 : "the flashlight's controls are off now (the game's own check): nothing done");
                }
                continue;
            }
            g_lb_until = now + (yours_left ? kHoldUs : kTapUs);
            haptics::MeleePulse(yours_left, 0.4f, 0.06f);
            if (g_uses[hand]++ < 10) {
                spdlog::info("[Temple] {} trigger at your {} temple ({:.2f} out, {:.2f} up, {:.2f} ahead): {} (LB {} for the game)",
                    yours_left ? "left" : "right", yours_left ? "left" : "right", side, height, ahead,
                    yours_left ? "the flashlight on or off" : "the scanner on or off", yours_left ? "held 1 s" : "tapped");
            }
        }
    }

    bool PressLeftShoulder() { return NowUs() < g_lb_until.load(); }

    void BeforeNodeUpdate(RE::NiAVObject* obj)
    {
        const uintptr_t light = g_light_node.load();
        const auto      at    = reinterpret_cast<uintptr_t>(obj);
        if (light == 0 || at != light) {
            // (A light placed before and not placed now: its own local put back, once.)
            if (at != 0 && at == g_light_restore.load() && at != light) {
                g_light_restore = 0;
                RestoreLightTry(obj);
            }
            return;
        }
        if (!PlaceLightTry(obj)) {
            g_light_node = 0;
            if (!g_place_failed.exchange(true)) {
                spdlog::error("[Temple] (fix174) the flashlight's light could not be read: it is left as the game has it for this session");
            }
        }
    }

    bool HoldsTrigger(bool left)
    {
        const int hand = left ? 0 : 1;
        return g_held_back[hand].load() || (g_at_temple[hand].load() && !g_entered_held[hand].load());
    }

    std::string Status()
    {
        return "Temple gestures: left trigger by the left temple, the flashlight (the game's own switch, no helmet needed); right trigger by the right temple, "
               "the scanner";
    }
}
