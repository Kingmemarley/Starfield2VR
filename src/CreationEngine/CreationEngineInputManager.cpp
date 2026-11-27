//
// Created by sergp on 6/13/2024.
//
#include <algorithm>
#include <memory>
#include <cstdint>
#include <chrono>
#include <thread>

#include "CreationEngineInputManager.h"
#include <CreationEngine/memory/offsets.h>
#include <REL/Relocation.h>
#include <mods/VR.hpp>

#include "Haptics.h"
#include "VirtualGamepad.h"

namespace {
    // The controller plugged in by PlugInEarly (no logging there: the log is not open yet).
    PVIGEM_CLIENT g_early_client{nullptr};
    PVIGEM_TARGET g_early_pad{nullptr};
    VIGEM_ERROR   g_early_error{VIGEM_ERROR_NONE};
    bool          g_early_tried{false};

    // (fix193) How long the mod waits, after its start, for the game to ask for its Windows gamepad list before it falls back to
    // the ViGEm pad (the game asks the first time its input runs, normally well before the mod starts).
    constexpr auto kWaitForList = std::chrono::seconds(30);

    // (fix195) After the virtual pad is given: a warning when the game has not read it at all within 15 s; and, only when the game's
    // request reached the watch through another module's hook (which may keep its own list), the ViGEm pad instead when the game has
    // still not read it after 30 s and ViGEmBus works.
    constexpr auto kNoReadWarning  = std::chrono::seconds(15);
    constexpr auto kNoReadFallback = std::chrono::seconds(30);

    // The game sets the virtual controller's rumble motors (hits, explosions, firing); passed on to the VR controllers.
    VOID CALLBACK OnGameRumble(PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR large_motor, UCHAR small_motor, UCHAR, LPVOID) {
        haptics::SetGameRumble(large_motor, small_motor);
    }

    void ListenForRumble(PVIGEM_CLIENT client, PVIGEM_TARGET pad) {
        const auto result = vigem_target_x360_register_notification(client, pad, &OnGameRumble, nullptr);
        if (VIGEM_SUCCESS(result)) {
            spdlog::info("[Haptics] listening for the game's controller rumble");
        } else {
            spdlog::warn("[Haptics] could not listen for the game's controller rumble (error 0x{:X}); shots and reloads still vibrate", (unsigned)result);
        }
    }
}

void CreationEngineInputManager::PlugInEarly() {
    // (fix193) The virtual pad in the game's own gamepad list needs no driver: the ViGEm pad is plugged in early only when the
    // watch for that list could not be put in place (Init then tells why).
    if (VirtualGamepad::InstallEarly()) {
        return;
    }
    g_early_tried = true;
    auto client = vigem_alloc();
    if (client == nullptr) {
        g_early_error = VIGEM_ERROR_BUS_NOT_FOUND;
        return;
    }
    const auto connect = vigem_connect(client);
    if (!VIGEM_SUCCESS(connect)) {
        g_early_error = connect;
        vigem_free(client);
        return;
    }
    auto pad = vigem_target_x360_alloc();
    const auto added = vigem_target_add(client, pad);
    if (!VIGEM_SUCCESS(added)) {
        g_early_error = added;
        vigem_target_free(pad);
        vigem_disconnect(client);
        vigem_free(client);
        return;
    }
    g_early_client = client;
    g_early_pad = pad;
}

CreationEngineInputManager::~CreationEngineInputManager() {
    if (pad == nullptr) {
        return;
    }
    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client, pad);
    vigem_target_free(pad);
}

// (The ViGEm pad only: the virtual pad in the game's list is never unplugged.)
void CreationEngineInputManager::Reset() {
    if(!connected || pad == nullptr) {
        return;
    }
    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client, pad);
    vigem_target_free(pad);
    if (pad == g_early_pad) {
        g_early_pad = nullptr;  // (fix193) not to be taken again once freed
    }
    pad = nullptr;
    connected = false;
    ConnectViGEm();
}

// (fix193) The way the controllers reach the game, chosen once: the virtual pad when the game was given the stand-in list (the
// usual case: the game asks for it the first time its input runs); the ViGEm pad when the watch could not be put in place, or the
// game has still not asked when the wait is over (it may have got Windows' list before the watch); never both.
void CreationEngineInputManager::Init() {
    if (m_chosen.exchange(true)) {
        return;
    }
    spdlog::info("[Input] (fix193) {}", VirtualGamepad::Describe());
    if (VirtualGamepad::Wrapped()) {
        UseVirtualPad("when the mod started");
        return;
    }
    if (!VirtualGamepad::HookInstalled()) {
        UseViGEm("the watch on the game's Windows gamepad list could not be put in place");
        return;
    }
    spdlog::info("[Input] (fix193) controller: waiting up to {} s for the game to ask for its Windows gamepad list", kWaitForList.count());
    std::thread([this] {
        const auto until = std::chrono::steady_clock::now() + kWaitForList;
        while (std::chrono::steady_clock::now() < until) {
            if (VirtualGamepad::Wrapped()) {
                UseVirtualPad("while the mod waited");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        // (fix193, the review) The wait is given up for the ViGEm pad only when ViGEmBus works (a test connection, closed again):
        // without it, giving up would leave no controller at all, so the mod keeps waiting for the game's request instead.
        if (!ViGEmBusWorks()) {
            spdlog::warn("[Input] (fix193) controller: the game had not asked for its Windows gamepad list {} s after the mod started, and ViGEmBus is "
                         "not installed (or not working): still waiting for the game's request ({})",
                kWaitForList.count(), VirtualGamepad::Describe());
            while (!VirtualGamepad::Wrapped()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            UseVirtualPad("after the wait");
            return;
        }
        if (VirtualGamepad::GiveUp()) {
            spdlog::info("[Input] (fix193) {}", VirtualGamepad::Describe());
            UseViGEm(fmt::format("the game had not asked for its Windows gamepad list {} s after the mod started (it may have got it before the watch)",
                kWaitForList.count()));
        } else {
            UseVirtualPad("at the end of the wait");
        }
    }).detach();
}

// (fix193, the review) ViGEmBus is there and answers: a client connected and closed again (no pad is plugged in).
bool CreationEngineInputManager::ViGEmBusWorks() {
    auto test = vigem_alloc();
    if (test == nullptr) {
        return false;
    }
    const auto result = vigem_connect(test);
    if (VIGEM_SUCCESS(result)) {
        vigem_disconnect(test);
    }
    vigem_free(test);
    return VIGEM_SUCCESS(result);
}

// (No ViGEm pad is plugged in on this way: PlugInEarly plugs one in only when the watch could not be put in place, and then the
// game is never given the stand-in list.)
void CreationEngineInputManager::UseVirtualPad(const char* when) {
    VirtualGamepad::Activate();
    spdlog::info("[Input] (fix193) controller: the game's Windows gamepad list wrapped, the virtual pad given (no driver needed; found {})", when);
    const int hidden = VirtualGamepad::HiddenPads();
    spdlog::info("[Input] (fix195) controller: the game's list holds the virtual pad alone; a real Xbox pad (or another program's virtual pad) is hidden from "
                 "the game while the mod's pad is given, the VR controllers being the pad (the game makes no gamepad device when two or more pads are "
                 "listed); {}. A DualSense the game finds through Sony's library still comes first, as before; the game gets the VR input even with "
                 "its window in the background",
        hidden < 0 ? std::string{ "the game has not looked at its list yet" } : fmt::format("{} hidden now", hidden));
    // (fix195) A watch on the game's first reads of the virtual pad (see kNoReadWarning).
    std::thread([this] {
        std::this_thread::sleep_for(kNoReadWarning);
        if (VirtualGamepad::Reads() != 0) {
            return;
        }
        spdlog::warn("[Input] (fix195) controller: the game has not read the virtual pad {} s after it was given ({})", kNoReadWarning.count(),
            VirtualGamepad::Describe());
        if (!VirtualGamepad::ThroughHook()) {
            return;
        }
        std::this_thread::sleep_for(kNoReadFallback - kNoReadWarning);
        if (VirtualGamepad::Reads() != 0) {
            spdlog::info("[Input] (fix195) controller: the game reads the virtual pad now");
            return;
        }
        // (Without Windows' plug-in events the game would never hear of the ViGEm pad, nor read its list again.)
        if (!VirtualGamepad::EventsWork()) {
            spdlog::warn("[Input] (fix195) controller: still no read of the virtual pad {} s after it was given, but Windows refused the game's gamepad "
                         "events, so the game would not see a ViGEm pad either: the virtual pad stays",
                kNoReadFallback.count());
            return;
        }
        if (!ViGEmBusWorks()) {
            spdlog::warn("[Input] (fix195) controller: still no read of the virtual pad {} s after it was given, and ViGEmBus is not installed (or not "
                         "working): the virtual pad stays",
                kNoReadFallback.count());
            return;
        }
        if (!VirtualGamepad::Abandon()) {
            return;
        }
        spdlog::warn("[Input] (fix195) controller: the virtual pad given up for the ViGEm pad ({})", VirtualGamepad::Describe());
        if (!UseViGEm(fmt::format("the game had not read the virtual pad {} s after it was given, its request having come through another module's hook",
                kNoReadFallback.count()))) {
            // The virtual pad first, then the ViGEm pad (if it was plugged in) unplugged: its removal makes the game read its list
            // again, which then holds the virtual pad alone again.
            VirtualGamepad::Resume();
            UnplugViGEm();
            spdlog::error("[Input] (fix195) controller: the ViGEm pad could not be used; the virtual pad is given again");
        }
    }).detach();
}

// (fix195) The ViGEm pad unplugged, if it is plugged in (the client stays).
void CreationEngineInputManager::UnplugViGEm() {
    if (!connected || pad == nullptr) {
        connected = false;
        return;
    }
    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client, pad);
    vigem_target_free(pad);
    if (pad == g_early_pad) {
        g_early_pad = nullptr;
    }
    pad = nullptr;
    connected = false;
}

bool CreationEngineInputManager::UseViGEm(const std::string& why) {
    if (client == nullptr) {
        client = vigem_alloc();
    }
    if (client == nullptr) {
        spdlog::error("[Input] (fix193) controller: no controller path: the ViGEm client could not be made (out of memory) and the virtual pad is not in use ({})", why);
        return false;
    }
    if (!ConnectViGEm()) {
        spdlog::error("[Input] (fix193) controller: no controller path: ViGEmBus not installed (or not working) and the virtual pad is not in use ({})", why);
        return false;
    }
    spdlog::info("[Input] (fix193) controller: fallback to ViGEmBus because {}", why);
    // (fix195) The game makes no gamepad device when two or more pads are in Windows' list, so with the ViGEm pad any other pad
    // listed (a real Xbox pad, another program's virtual pad) means no controller at all.
    const int others = VirtualGamepad::HiddenPads();
    spdlog::log(others > 0 ? spdlog::level::warn : spdlog::level::info,
        "[Input] (fix195) controller: with the ViGEm pad, any other pad in Windows' gamepad list (a real Xbox pad, another program's virtual pad) "
        "means the game makes no gamepad device and no controller works: unplug other pads{}",
        others > 0 ? fmt::format(" ({} other pad(s) were listed at the game's last look)", others) : std::string{});
    return Hook();
}

// The ViGEm pad: the one plugged in at the start, or plugged in now. True when it is plugged in.
bool CreationEngineInputManager::ConnectViGEm() {
    if(connected) {
        return true;
    }
    if (client == g_early_client && g_early_pad != nullptr) {
        pad = g_early_pad;
        connected = true;
        spdlog::info("ViGEm Bus connection successful! (virtual controller plugged in at game start)");
        ListenForRumble(client, pad);
        return true;
    }
    if (g_early_tried) {
        spdlog::warn("Virtual controller could not be plugged in at game start (error 0x{:X}); plugging it in now", (unsigned)g_early_error);
    }
    const auto retval = vigem_connect(client);

    if (!VIGEM_SUCCESS(retval))
    {
        spdlog::error("ViGEm Bus connection failed with error code: 0x{:X} (is the ViGEmBus driver installed?)", (unsigned)retval);
        return false;
    }
    //
    // Add client to the bus, this equals a plug-in event
    //
    pad = vigem_target_x360_alloc();

    const auto pir = vigem_target_add(client, pad);

    if (!VIGEM_SUCCESS(pir))
    {
        // (fix195) Not plugged in: no pad to feed (the client is closed again, so a later try can connect it).
        spdlog::error("Target plugin failed with error code: 0x{:X}", (unsigned)pir);
        vigem_target_free(pad);
        pad = nullptr;
        connected = false;
        vigem_disconnect(client);
        return false;
    }
    spdlog::info("ViGEm Bus connection successful!");
    connected = true;
    ListenForRumble(client, pad);
    return true;
}

// (The ViGEm pad only: the virtual pad is read by the game's own poll, VirtualGamepad.)
bool CreationEngineInputManager::Hook() {
    if (m_poll_events_hook != nullptr) {
        return true;
    }
    spdlog::info("Entering CreationEngineInputManager::Hook().");
    REL::Relocation<uintptr_t> gamepadDevicePollFuncAddr{ GameStore::MemoryOffsets::Gamepad::PollVfunc() };
    m_poll_events_hook = std::make_unique<FunctionHook>(gamepadDevicePollFuncAddr.address(), reinterpret_cast<uintptr_t>(&CreationEngineInputManager::onPollGamepadState));
    if(!m_poll_events_hook->create()) {
        spdlog::error("Failed to hook gamepadDevicePoll");
        return false;
    }
    return true;
}

// (fix193) The ViGEm client is made only when the ViGEm pad is used (UseViGEm), and the poll hook with it.
CreationEngineInputManager::CreationEngineInputManager(): connected(false), client(g_early_client), pad(nullptr)
{
}

void CreationEngineInputManager::onPollGamepadState(__int64 a1, double xmm2) {
    static auto instance = CreationEngineInputManager::Get();
    instance->UpdateDeviceState();
    static auto original_fn = instance->m_poll_events_hook->get_original<decltype(CreationEngineInputManager::onPollGamepadState)>();
    return original_fn(a1, xmm2);
}

bool CreationEngineInputManager::ReadVrPad(XUSB_REPORT& report) {
    report = {};
    static auto vr = VR::get();
    if (!vr->is_hmd_active()) {
        return false;
    }
    vr->update_action_states();
//    spdlog::info("Updating input state fc[{}]", vr->m_frame_count);

    uint32_t retval = 0;
    vr->on_xinput_get_state(&retval, 0, (XINPUT_STATE*)&report);
    haptics::Update(report.wButtons);
    return true;
}

void CreationEngineInputManager::UpdateDeviceState() {
    if(!connected) {
        return;
    }
    XUSB_REPORT report = {0};

    // (fix189) The virtual controller keeps the last report it was sent: when the headset's session stops, one empty report is sent,
    // so a stick, trigger or button held at that moment is let go (not held on until the session is back).
    static bool was_active = false;
    if (!ReadVrPad(report)) {
        if (was_active) {
            was_active = false;
            vigem_target_x360_update(client, pad, report);
        }
        return;
    }
    was_active = true;
    vigem_target_x360_update(client, pad, report);
}
