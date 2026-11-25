#include "MuzzleFire.h"

#include <safetyhook/easy.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include "HandActions.h"
#include "HandGrab.h"
#include "Haptics.h"
#include "body/BodyIK.h"
#include "body/SafeScan.h"
#include "models/GameFlow.h"
#include "vr/TrackingSpace.h"
#include <mods/VR.hpp>

// Ported from SFVR (AetherianArtificer's fork of starfield2vr, src/CreationEngine/MuzzleFire.cpp).
namespace MuzzleFire
{
    namespace
    {
        // ProjectileLaunchData offsets (SFVR, Starfield 1.16.244). The shooter is compared with the
        // player before anything is written, so a different layout only means nothing happens.
        constexpr std::size_t kShooterOffset       = 0x38;
        constexpr std::size_t kAimFromMuzzleOffset = 0xF2;
        constexpr std::size_t kMuzzleNodeOffset    = 0x88;  // NiPointer<NiAVObject>, checked before any lookup
        constexpr std::size_t kAnglesOffset        = 0x24;  // pitch, roll, yaw

        safetyhook::InlineHook g_compute_launch_origin_hook{};
        std::atomic<bool>      g_follow_barrel{ true };

        void CopyLaunchData(std::uint8_t* to, const std::uint8_t* from)
        {
            SIZE_T done = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), from, to, HandActions::kLaunchBytes, &done) || done != HandActions::kLaunchBytes) {
                std::memset(to, 0, HandActions::kLaunchBytes);
            }
        }

        // Player shots take the projectile-node path when this flag is set; NPCs never read it.
        bool ComputeLaunchOrigin(std::uint8_t* launch_data)
        {
            static auto vr  = VR::get();
            bool        aim = false;
            const auto  player = launch_data ? CreationEngineSingletonManager::GetPlayerRef() : nullptr;
            const bool  by_player = player && *reinterpret_cast<void**>(launch_data + kShooterOffset) == player;
            if (by_player) {
                haptics::PlayerFired();
            }
            // A grenade thrown from the shoulder or a power pushed out (HandActions): the launch data as it came (for the log of
            // the first few grenades, and to tell what is launched).
            const bool    grenade_pending = by_player && HandActions::ThrowPending();
            const bool    power_moment    = by_player && HandActions::PowerAimActive();
            std::uint8_t  before[HandActions::kLaunchBytes]{};
            if (grenade_pending || power_moment) {
                CopyLaunchData(before, launch_data);
            }
            // Only with the weapon out: with it put away the hand aims for pointing at things (grenades are left alone). Not a
            // grenade from the shoulder (it goes along the hand's throw, from where the game throws it), nor, just after a power
            // was pushed out, anything that is not the gun's (the power goes along the push).
            bool not_the_gun = grenade_pending && HandActions::IsThrownLaunch(before);
            if (!not_the_gun && power_moment) {
                const auto gun  = static_cast<std::uint64_t>(HandGrab::WeaponInHandsForm());
                bool       mine = false;
                for (std::size_t at = 0; gun != 0 && at + 8 <= HandActions::kLaunchBytes && !mine; at += 8) {
                    std::uint64_t value = 0;
                    std::memcpy(&value, before + at, sizeof(value));
                    mine = value == gun;
                }
                not_the_gun = !mine;
            }
            const bool aiming = by_player && !not_the_gun && tracking::AimingWithHand() && vr->is_using_controllers() && GameFlow::isWeaponDrawn();
            if (aiming && !ModConstants::shootFromMuzzle) {
                g_follow_barrel = false;
            }
            if (aiming && ModConstants::shootFromMuzzle) {
                {
                    launch_data[kAimFromMuzzleOffset] = 1;
                    // Fire from the weapon in the shown body's hand; the launch data releases this reference.
                    auto& node = *reinterpret_cast<RE::NiAVObject**>(launch_data + kMuzzleNodeOffset);
                    if (!node) {
                        if (auto muzzle = body::GetBodyMuzzle()) {
                            InterlockedIncrement(reinterpret_cast<volatile long*>(reinterpret_cast<std::uint8_t*>(muzzle) + 8));
                            node = muzzle;
                            aim  = true;
                        }
                    }
                    body::NotifyPlayerFired();
                }
            }
            const bool result = g_compute_launch_origin_hook.call<bool>(launch_data);

            // Direction along the aim ray, whatever the muzzle node's own axis: pitch = -asin(z), yaw = atan2(x, y).
            float forward[3];
            const bool barrel = aim && body::GetBodyAimForward(forward);
            if (barrel) {
                const float two_pi = 6.28318530718f;
                auto        angles = reinterpret_cast<float*>(launch_data + kAnglesOffset);
                angles[0]          = -std::asin(std::clamp(forward[2], -1.0f, 1.0f));
                angles[1]          = 0.0f;
                angles[2]          = std::fmod(std::atan2(forward[0], forward[1]) + two_pi, two_pi);
            }
            if (aiming && ModConstants::shootFromMuzzle) {
                g_follow_barrel = barrel; // what the gun sight follows
            }
            // Last, so a grenade thrown from the shoulder goes along the hand's throw.
            if (grenade_pending) {
                HandActions::OnPlayerLaunch(launch_data, before);
            }
            return result;
        }
    }

    bool ShotsFollowBarrel()
    {
        return g_follow_barrel.load();
    }

    void ResetShotsFollowBarrel()
    {
        g_follow_barrel = true;
    }

    void Install()
    {
        const auto address = body::ScanUnique("ComputeLaunchOrigin (shots from the gun muzzle)",
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FA FF FF 48 81 EC 08 06 00 00", 0x1b52510);
        if (!address) {
            return;
        }
        g_compute_launch_origin_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ComputeLaunchOrigin));
        if (!g_compute_launch_origin_hook) {
            spdlog::error("[MuzzleFire] Failed to hook ComputeLaunchOrigin");
        }
    }
}
