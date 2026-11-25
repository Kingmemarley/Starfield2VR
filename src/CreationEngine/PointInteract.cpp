#include "PointInteract.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <glm/glm.hpp>
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>
#include <utility/RTTI.hpp>

#include <CreationEngine/memory/ScanHelper.h>

#include "CreationEngineSingletonManager.h"
#include "GrenadeHand.h"
#include "RE/N/NiCamera.h"
#include "body/SafeScan.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

// What was found with the test build (Starfield 1.16.236):
// - the game's ViewCaster is a single static object; its pick function is at rva 12e2ed0:
//     pick(ViewCaster* caster, PickData* out, const NiPoint3* origin, const NiPoint3* direction, ...)
//   It sweeps the caster's shape (pointer at caster+0x58, radius at shape+0x20, set from the game's
//   settings on every call) from origin along direction for a length kept in xmm15 (the game's reach).
//   The game passes the rendered camera's position and forward, which is your head in VR.
// - after a pick the caster holds the target's handle at +0x10, the distance at +0x34 and the point
//   hit at +0x40 (the log showed that point on the head's line of sight).
// At function + 0x4fb the reach (xmm15) and the shape's radius are final and not used yet; the second
// hook replaces both there.
namespace PointInteract
{
    namespace
    {
        constexpr uintptr_t   kReachHookOffset   = 0x4fb;
        constexpr uint8_t     kReachHookBytes[]  = { 0x48, 0x8B, 0x85, 0x08, 0x0D, 0x00, 0x00, 0xC5, 0xFA, 0x10, 0x28 };
        constexpr std::size_t kShapeOffset       = 0x58;
        constexpr std::size_t kShapeRadiusOffset = 0x20;

        // Around the hand: the sweep starts this far behind the controller (so things the hand is
        // already touching count) and the shape has this radius (metres, before the height scale).
        constexpr float kBehindHand = 0.15f;
        constexpr float kHandRadius = 0.12f;
        // A steered far cast (gravity gloves) reaches this far past the point it is aimed at (m): past an object's middle, not
        // on into what is behind it.
        constexpr float kSteerPast = 0.3f;
        // While a grenade's copy is in your hand (GrenadeHand) the game's interaction pick finds nothing: its cast is made this
        // short and thin (game units) from where the game starts it, so no prompt shows and nothing can be activated or grabbed.
        constexpr float kBlankReach  = 0.001f;
        constexpr float kBlankRadius = 0.001f;

        safetyhook::MidHook    g_pick_hook{};
        safetyhook::MidHook    g_reach_hook{};
        uintptr_t              g_caster_vtable{ 0 };
        std::atomic<int>       g_calls{ 0 };
        std::atomic<int>       g_redirected{ 0 };
        std::atomic<int>       g_other_callers{ 0 };
        std::atomic<int>       g_blanked{ 0 };
        std::atomic<float>     g_game_reach{ 0.0f };
        std::atomic<float>     g_game_radius{ 0.0f };
        std::atomic<bool>      g_left_hand{ false };
        std::atomic<float>     g_far_reach{ 0.0f };  // aiming from afar (gravity gloves): this reach...
        std::atomic<float>     g_far_radius{ 0.0f }; // ...and radius (0: the hand's own)
        // A held cast (a target found from afar stays the target while the hand flicks): the ray it keeps, and the ray of
        // the latest cast made at the hand, whose result the game shows now (g_ray_mutex).
        std::mutex             g_ray_mutex;
        bool                   g_hold_ray{ false };
        bool                   g_have_held_ray{ false };
        glm::vec3              g_held_origin{ 0.0f }, g_held_direction{ 0.0f, 0.0f, -1.0f };
        bool                   g_have_last_ray{ false };
        glm::vec3              g_last_origin{ 0.0f }, g_last_direction{ 0.0f, 0.0f, -1.0f };
        // A far cast steered at a point, with its own radius (g_ray_mutex: taken together by the cast).
        bool                   g_steer{ false };
        glm::vec3              g_steer_target{ 0.0f };
        uint32_t               g_steer_tag{ 0 };
        float                  g_steer_radius{ 0.0f };
        // The latest cast made at the hand that reached its sweep: its radius (before the height scale), the steer it took,
        // and how many so far (g_cast_mutex: read together).
        std::mutex             g_cast_mutex;
        float                  g_cast_radius{ 0.0f };
        uint32_t               g_cast_steer_tag{ 0 };
        int                    g_cast_count{ 0 };

        // The replacement cast, one per thread (the game may pick from several threads).
        thread_local float     t_origin[4]{};
        thread_local float     t_direction[4]{};
        thread_local bool      t_redirected{ false };
        thread_local uintptr_t t_caster{ 0 };
        thread_local float     t_reach{ 0.0f };
        thread_local float     t_radius{ 0.0f };
        thread_local float     t_radius_raw{ 0.0f };
        thread_local uint32_t  t_steer_tag{ 0 };
        thread_local bool      t_blank{ false }; // this cast finds nothing (a grenade's copy in your hand)

        bool FromPlayerCamera(const float* origin)
        {
            auto root   = CreationEngineSingletonManager::GetSceneGraphRoot();
            auto camera = root ? root->worldCamera : nullptr;
            if (!camera || !origin) {
                return false;
            }
            const auto& p  = camera->world.translate;
            const float dx = origin[0] - p.x, dy = origin[1] - p.y, dz = origin[2] - p.z;
            return dx * dx + dy * dy + dz * dz < 1.0f;
        }

        // Function entry: the cast's origin (r8) and direction (r9).
        void OnPick(safetyhook::Context& ctx)
        {
            t_redirected = false;
            t_blank      = false;
            g_calls.fetch_add(1, std::memory_order_relaxed);
            if (GrenadeHand::PicksOff() && ctx.rcx != 0) {
                // (Weapon out or not: the game's own interaction caster only, or a pick starting at the camera if it was not found.)
                const bool caster = g_caster_vtable != 0 ? *reinterpret_cast<const uintptr_t*>(ctx.rcx) == g_caster_vtable
                                                         : ctx.r8 != 0 && FromPlayerCamera(reinterpret_cast<const float*>(ctx.r8));
                if (caster) {
                    t_reach      = kBlankReach;
                    t_radius     = kBlankRadius;
                    t_radius_raw = 0.0f;
                    t_steer_tag  = 0;
                    t_caster     = ctx.rcx;
                    t_redirected = true;
                    t_blank      = true;
                    g_blanked.fetch_add(1, std::memory_order_relaxed);
                    return; // (from where and along where the game starts it)
                }
            }
            if (!tracking::HandInteract() || ctx.rcx == 0 || ctx.r8 == 0 || ctx.r9 == 0) {
                return;
            }
            // Only the game's own interaction caster (its vtable), or, if that was not found, a pick
            // that starts at the player's camera.
            const bool caster = g_caster_vtable != 0 ? *reinterpret_cast<const uintptr_t*>(ctx.rcx) == g_caster_vtable
                                                     : FromPlayerCamera(reinterpret_cast<const float*>(ctx.r8));
            if (!caster) {
                g_other_callers.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // The hand's pointing ray; aiming from afar (gravity gloves), the gloves' pointing line (the setting's).
            const float far_reach  = g_far_reach.load(std::memory_order_relaxed);
            const bool  left       = g_left_hand.load(std::memory_order_relaxed);
            glm::vec3   origin{}, direction{};
            if (!(far_reach > 0.0f ? tracking::GloveRayWorld(std::clamp(GameFlow::gStore.internalSettings.gloveAimLine, 0, tracking::kGloveLines - 1), left,
                                         origin, direction)
                                   : tracking::HandRayWorld(origin, direction, left))) {
                return;
            }
            const float scale      = std::max(0.1f, tracking::TrackingScale());
            const float far_radius = g_far_radius.load(std::memory_order_relaxed);
            const float hand_reach = std::clamp(GameFlow::gStore.internalSettings.handReach, 0.05f, 3.0f);
            // Near: from a little behind the hand out to the hand reach. Far (aiming to pull something): from the end of
            // the hand reach (anything nearer is the near grab's) out to the far reach; a wide sphere starts a radius further
            // out, so it does not sit on things beside the hand from the start.
            uint32_t  steer_tag = 0;
            glm::vec3 steer_target{ 0.0f };
            float     steer_radius = 0.0f;
            {
                // A held cast keeps the hand's ray (where it starts along it still follows the cast's own radius).
                std::scoped_lock _{ g_ray_mutex };
                if (g_hold_ray) {
                    if (g_have_held_ray) {
                        origin    = g_held_origin;
                        direction = g_held_direction;
                    } else {
                        g_held_origin    = origin;
                        g_held_direction = direction;
                        g_have_held_ray  = true;
                    }
                }
                g_last_origin    = origin; // the hand's own ray (a cast held later keeps this, not a steered one)
                g_last_direction = direction;
                g_have_last_ray  = true;
                if (far_reach > 0.0f && g_steer && !g_hold_ray) {
                    steer_tag    = g_steer_tag;
                    steer_target = g_steer_target;
                    steer_radius = g_steer_radius;
                }
            }
            float start  = -kBehindHand; // along the ray from the hand
            float length = kBehindHand + hand_reach;
            float radius = kHandRadius;
            if (far_reach > 0.0f) {
                // (A steered cast's radius is the steer's own, set with it.)
                const float wanted = steer_tag != 0 ? steer_radius : far_radius;
                radius = wanted > 0.0f ? std::clamp(wanted, 0.01f, 0.5f) : kHandRadius;
                start  = std::min(hand_reach, 1.0f) + (radius > kHandRadius ? radius : 0.0f);
                length = std::max(std::clamp(far_reach, 0.05f, 20.0f) - start, 0.1f);
            }
            if (steer_tag != 0) {
                // Steered (gravity gloves): from the hand straight at the point, a little past it.
                const glm::vec3 to       = steer_target - origin;
                const float     distance = glm::length(to);
                if (std::isfinite(distance) && distance > 0.01f * scale) {
                    direction = to / distance;
                    length    = std::max(distance / scale - start + kSteerPast, 0.1f);
                } else {
                    steer_tag = 0;
                }
            }
            origin += direction * (start * scale);
            t_origin[0]    = origin.x;
            t_origin[1]    = origin.y;
            t_origin[2]    = origin.z;
            t_direction[0] = direction.x;
            t_direction[1] = direction.y;
            t_direction[2] = direction.z;
            t_reach        = length * scale;
            t_radius       = radius * scale;
            t_radius_raw   = radius;
            t_steer_tag    = steer_tag;
            t_caster       = ctx.rcx;
            t_redirected   = true;
            ctx.r8         = reinterpret_cast<uintptr_t>(t_origin);
            ctx.r9         = reinterpret_cast<uintptr_t>(t_direction);
            g_redirected.fetch_add(1, std::memory_order_relaxed);
        }

        // Function + 0x4fb: the reach (xmm15) and the shape radius the game chose for this cast.
        void OnReach(safetyhook::Context& ctx)
        {
            if (!t_redirected) {
                return;
            }
            t_redirected = false;
            if (ctx.rdx != t_caster) {
                return; // not the call we redirected (rdx holds the caster here)
            }
            g_game_reach = ctx.xmm15.f32[0];
            ctx.xmm15.f32[0] = t_reach;
            if (const auto shape = *reinterpret_cast<const uintptr_t*>(t_caster + kShapeOffset)) {
                auto& radius  = *reinterpret_cast<float*>(shape + kShapeRadiusOffset);
                g_game_radius = radius;
                radius        = t_radius;
            }
            if (t_blank) {
                t_blank = false;
                return; // (not a cast made at the hand: the hand grab does not count it)
            }
            std::scoped_lock _{ g_cast_mutex };
            g_cast_radius    = t_radius_raw;
            g_cast_steer_tag = t_steer_tag;
            ++g_cast_count;
        }
    }

    void Install()
    {
        if (auto vtable = utility::rtti::find_vtable(MemoryScan::mod, ".?AVViewCaster@@")) {
            g_caster_vtable = *vtable;
        } else {
            spdlog::warn("[Point] the game's ViewCaster type was not found; picks that start at the camera are used instead");
        }
        const auto address = body::ScanUnique("interaction pick (hand interaction)",
            "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 18 F3 FF FF 48 81 EC A8 0D 00 00",
            0x12e2ed0, "Starfield 1.16.236 offset");
        if (!address) {
            spdlog::error("[Point] hand interaction is off: the game's interaction pick was not found");
            return;
        }
        // The reach hook only goes in when the code there is exactly what it was in 1.16.236.
        const auto reach_address = address + kReachHookOffset;
        if (std::memcmp(reinterpret_cast<const void*>(reach_address), kReachHookBytes, sizeof(kReachHookBytes)) != 0) {
            spdlog::error("[Point] hand interaction is off: the interaction pick's code is not the one this was made for");
            return;
        }
        g_reach_hook = safetyhook::create_mid(reinterpret_cast<void*>(reach_address), &OnReach);
        if (!g_reach_hook) {
            spdlog::error("[Point] hand interaction is off: could not hook the pick's reach");
            return;
        }
        g_pick_hook = safetyhook::create_mid(reinterpret_cast<void*>(address), &OnPick);
        spdlog::info("[Point] hand interaction hooks: pick {}, reach {}", static_cast<bool>(g_pick_hook), static_cast<bool>(g_reach_hook));
    }

    bool PicksHooked() { return static_cast<bool>(g_pick_hook) && static_cast<bool>(g_reach_hook); }

    void SetPointingHand(bool left) { g_left_hand = left; }

    bool PointingHandIsLeft() { return g_left_hand.load(); }

    void SetFarCast(float reach, float radius)
    {
        g_far_radius.store(reach > 0.0f ? radius : 0.0f, std::memory_order_relaxed);
        g_far_reach.store(reach > 0.0f ? reach : 0.0f, std::memory_order_relaxed);
    }

    void LastCast(float& radius, int& count, uint32_t* steer)
    {
        std::scoped_lock _{ g_cast_mutex };
        count  = g_cast_count;
        radius = g_cast_radius;
        if (steer) {
            *steer = g_cast_steer_tag;
        }
    }

    void SetSteer(bool on, const glm::vec3& target, uint32_t tag, float radius)
    {
        std::scoped_lock _{ g_ray_mutex };
        const bool usable = on && tag != 0 && radius > 0.0f && std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z);
        g_steer           = usable;
        g_steer_target    = usable ? target : glm::vec3{ 0.0f };
        g_steer_tag       = usable ? tag : 0;
        g_steer_radius    = usable ? radius : 0.0f;
    }

    void HoldCast(bool hold)
    {
        std::scoped_lock _{ g_ray_mutex };
        if (hold && !g_hold_ray) {
            // Keep the ray of the latest cast: the one whose target the game has now (else the next cast's).
            g_have_held_ray  = g_have_last_ray;
            g_held_origin    = g_last_origin;
            g_held_direction = g_last_direction;
        } else if (!hold) {
            g_have_held_ray = false;
        }
        g_hold_ray = hold;
    }

    void Report()
    {
        static auto last = std::chrono::steady_clock::now();
        const auto  now  = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10)) {
            return;
        }
        last                 = now;
        const int redirected = g_redirected.exchange(0);
        const int calls      = g_calls.exchange(0);
        const int others     = g_other_callers.exchange(0);
        const int blanked    = g_blanked.exchange(0);
        if (redirected > 0 || others > 0) {
            spdlog::info("[Point] last 10 s: {} of {} interaction picks made at your hand ({} from other casters left alone); "
                         "the game's own reach {:.2f}, radius {:.2f}",
                redirected, calls, others, g_game_reach.load(), g_game_radius.load());
        }
        if (blanked > 0) {
            spdlog::info("[Point] last 10 s: {} interaction picks found nothing on purpose (a grenade's copy in your hand: nothing to take)", blanked);
        }
    }
}
