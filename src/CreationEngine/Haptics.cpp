#include "Haptics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include <spdlog/spdlog.h>
#include <mods/VR.hpp>

#include "body/BodyIK.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"

namespace haptics
{
    namespace
    {
        using steady = std::chrono::steady_clock;

        int64_t now_us()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(steady::now().time_since_epoch()).count();
        }

        // Game rumble (motor levels 0-255), written by the ViGEm thread.
        std::atomic<std::uint8_t> g_rumble_large{ 0 };
        std::atomic<std::uint8_t> g_rumble_small{ 0 };
        std::atomic<bool>         g_rumble_logged{ false };

        // Shots: time of the last kick, and how many were launched (for the log).
        std::atomic<int64_t> g_fire_time_us{ -1'000'000'000 };
        std::atomic<int>     g_fire_count{ 0 };

        // A short vibration pattern: amplitude per hand over time.
        struct Pulse
        {
            int64_t start_us{ -1'000'000'000 };
            float   duration_s{ 0.0f };
            float   amplitude{ 0.0f };

            float at(int64_t t) const
            {
                const float since = (float)(t - start_us) * 1e-6f;
                if (since < 0.0f || since >= duration_s) {
                    return 0.0f;
                }
                // Full strength, easing off over the second half.
                const float half = duration_s * 0.5f;
                return since < half ? amplitude : amplitude * (1.0f - (since - half) / half * 0.6f);
            }
        };

        constexpr float kFireSeconds        = 0.07f;
        constexpr int64_t kFireRetriggerUs  = 45'000;  // shotgun pellets count as one kick
        constexpr int64_t kReloadTapMaxUs   = 400'000; // a longer hold puts the weapon away instead
        constexpr std::uint16_t kXboxX      = 0x4000;  // XINPUT_GAMEPAD_X: reload

        // Reload: a click in the support hand, then one in the gun hand (magazine out, magazine in).
        Pulse g_reload_support{};
        Pulse g_reload_gun{};

        // Melee pulses per physical hand (written from the game's melee code, read on the poll).
        std::atomic<int64_t> g_melee_start_us[2]{ -1'000'000'000, -1'000'000'000 };
        std::atomic<float>   g_melee_seconds[2]{ 0.0f, 0.0f };
        std::atomic<float>   g_melee_amplitude[2]{ 0.0f, 0.0f };

        // What was last sent to each hand, so an unchanged vibration is not re-sent every poll.
        float   g_sent_amplitude[2]{ 0.0f, 0.0f };
        int64_t g_sent_time_us[2]{ 0, 0 };
    }

    void SetGameRumble(std::uint8_t large_motor, std::uint8_t small_motor)
    {
        g_rumble_large = large_motor;
        g_rumble_small = small_motor;
        if ((large_motor || small_motor) && !g_rumble_logged.exchange(true)) {
            spdlog::info("[Haptics] the game is rumbling the controller (large {}, small {}); passed on to your controllers", large_motor, small_motor);
        }
    }

    void PlayerFired()
    {
        g_fire_count.fetch_add(1);
        const auto t    = now_us();
        auto       last = g_fire_time_us.load();
        if (t - last >= kFireRetriggerUs) {
            g_fire_time_us.compare_exchange_strong(last, t);
        }
    }

    void MeleePulse(bool left_controller, float amplitude, float seconds)
    {
        static auto vr   = VR::get();
        const int   hand = std::clamp((left_controller ? vr->get_left_controller_index() : vr->get_right_controller_index()) - 1, 0, 1);
        // A stronger pulse is not cut short by a weaker one that starts while it runs.
        const auto t = now_us();
        const Pulse running{ g_melee_start_us[hand].load(), g_melee_seconds[hand].load(), g_melee_amplitude[hand].load() };
        if (running.at(t) > amplitude) {
            return;
        }
        g_melee_seconds[hand]   = std::max(0.0f, seconds);
        g_melee_amplitude[hand] = std::clamp(amplitude, 0.0f, 1.0f);
        g_melee_start_us[hand]  = t;
    }

    void Update(std::uint16_t xbox_buttons)
    {
        static auto vr = VR::get();
        const auto& s  = GameFlow::gStore.internalSettings;
        const auto  t  = now_us();

        // Periodic log line while something happened, so the log shows the shots were seen.
        {
            static int64_t last_log = 0;
            if (t - last_log > 5'000'000) {
                last_log          = t;
                const int fired   = g_fire_count.exchange(0);
                if (fired > 0) {
                    spdlog::info("[Haptics] {} projectiles launched by you in the last 5 s (vibration {})", fired, s.haptics ? "on" : "off");
                }
            }
        }

        // Reload: the X button tapped (not held) with the weapon out, outside menus.
        {
            static bool    was_down  = false;
            static int64_t down_time = 0;
            const bool     down      = (xbox_buttons & kXboxX) != 0;
            if (down && !was_down) {
                down_time = t;
            } else if (!down && was_down && t - down_time < kReloadTapMaxUs && GameFlow::isWeaponDrawn() && !GameFlow::isShowingMenu() &&
                       !GameFlow::isWorkshopShown()) { // (fix175: X deletes in build mode, no reload)
                g_reload_support = { t, 0.05f, 0.55f };
                g_reload_gun     = { t + 140'000, 0.05f, 0.7f };
            }
            was_down = down;
        }

        if (!s.haptics || !vr->is_hmd_active() || !vr->is_using_controllers()) {
            return;
        }

        // Physical hands: the gun hand is the right one unless the controllers are swapped.
        const int gun     = std::clamp(vr->get_right_controller_index() - 1, 0, 1);
        const int support = 1 - gun;

        float amplitude[2]{ 0.0f, 0.0f };

        // Shot: a kick in the gun hand, and in the other hand while it holds the gun too.
        {
            const Pulse fire{ g_fire_time_us.load(), kFireSeconds, 1.0f };
            const float a = fire.at(t);
            amplitude[gun] = std::max(amplitude[gun], a);
            if (body::IsSupportHandOnGun()) {
                amplitude[support] = std::max(amplitude[support], a * 0.6f);
            }
        }

        for (int hand = 0; hand < 2; ++hand) {
            const Pulse melee{ g_melee_start_us[hand].load(), g_melee_seconds[hand].load(), g_melee_amplitude[hand].load() };
            amplitude[hand] = std::max(amplitude[hand], melee.at(t));
        }

        amplitude[support] = std::max(amplitude[support], g_reload_support.at(t));
        amplitude[gun]     = std::max(amplitude[gun], g_reload_gun.at(t));

        // The game's own rumble: the large (low) motor is the left side of a pad, the small one the right.
        // (fix189) Not while a menu or a loading screen is up: the game sets a level and sends 0 later, and the virtual pad only reports
        // changes, so a level left set as a menu or a loading screen opens kept both controllers buzzing (re-sent here every 25 ms) until
        // the game sent 0. In play it is passed on as before.
        if (s.hapticGameRumble && !GameFlow::isShowingMenu() && !GameFlow::isLoadingScreenShown()) {
            const float large_level = g_rumble_large.load() / 255.0f;
            const float small_level = g_rumble_small.load() / 255.0f;
            amplitude[0] = std::max(amplitude[0], std::max(large_level, small_level * 0.6f));
            amplitude[1] = std::max(amplitude[1], std::max(small_level, large_level * 0.6f));
        }

        const float strength = std::clamp(s.hapticStrength, 0.0f, 1.0f);
        for (int hand = 0; hand < 2; ++hand) {
            const float a = std::clamp(amplitude[hand] * strength, 0.0f, 1.0f);
            if (a < 0.02f) {
                // The last vibration (at most 50 ms) runs out by itself.
                g_sent_amplitude[hand] = 0.0f;
                continue;
            }
            // Re-sent while it lasts, a little longer than one poll so it does not stutter.
            if (std::abs(a - g_sent_amplitude[hand]) > 0.05f || t - g_sent_time_us[hand] > 25'000) {
                vr->trigger_haptic_vibration(0.0f, 0.05f, 0.0f, a, hand == 0 ? vr->get_left_joystick() : vr->get_right_joystick());
                g_sent_amplitude[hand] = a;
                g_sent_time_us[hand]   = t;
            }
        }
    }
}
