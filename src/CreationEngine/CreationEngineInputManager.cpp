//
// Created by sergp on 6/13/2024.
//
#include <memory>
#include <cstdint>

#include "CreationEngineInputManager.h"
#include <CreationEngine/memory/offsets.h>
#include <REL/Relocation.h>
#include <mods/VR.hpp>

#include "Haptics.h"

namespace {
    // The controller plugged in by PlugInEarly (no logging there: the log is not open yet).
    PVIGEM_CLIENT g_early_client{nullptr};
    PVIGEM_TARGET g_early_pad{nullptr};
    VIGEM_ERROR   g_early_error{VIGEM_ERROR_NONE};
    bool          g_early_tried{false};

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

void CreationEngineInputManager::Reset() {
    if(!connected || pad == nullptr) {
        return;
    }
    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client, pad);
    vigem_target_free(pad);
    connected = false;
    Init();
}

void CreationEngineInputManager::Init() {
    if(connected || client == nullptr) {
        return;
    }
    if (client == g_early_client && g_early_pad != nullptr) {
        pad = g_early_pad;
        connected = true;
        spdlog::info("ViGEm Bus connection successful! (virtual controller plugged in at game start)");
        ListenForRumble(client, pad);
        return;
    }
    if (g_early_tried) {
        spdlog::warn("Virtual controller could not be plugged in at game start (error 0x{:X}); plugging it in now", (unsigned)g_early_error);
    }
    const auto retval = vigem_connect(client);

    if (!VIGEM_SUCCESS(retval))
    {
        spdlog::error("ViGEm Bus connection failed with error code: 0x{:X} (is the ViGEmBus driver installed?)", (unsigned)retval);
        return;
    }
    //
    // Add client to the bus, this equals a plug-in event
    //
    pad = vigem_target_x360_alloc();

    const auto pir = vigem_target_add(client, pad);

    if (!VIGEM_SUCCESS(pir))
    {
        spdlog::error("Target plugin failed with error code: 0x{:X}", (unsigned)pir);
    }
    spdlog::info("ViGEm Bus connection successful!");
    connected = true;
    if (VIGEM_SUCCESS(pir)) {
        ListenForRumble(client, pad);
    }

}

bool CreationEngineInputManager::Hook() {
    spdlog::info("Entering CreationEngineInputManager::Hook().");
    REL::Relocation<uintptr_t> gamepadDevicePollFuncAddr{ GameStore::MemoryOffsets::Gamepad::PollVfunc() };
    m_poll_events_hook = std::make_unique<FunctionHook>(gamepadDevicePollFuncAddr.address(), reinterpret_cast<uintptr_t>(&CreationEngineInputManager::onPollGamepadState));
    if(!m_poll_events_hook->create()) {
        spdlog::error("Failed to hook gamepadDevicePoll");
        return false;
    }
    return true;
}

CreationEngineInputManager::CreationEngineInputManager(): connected(false), client(g_early_client != nullptr ? g_early_client : vigem_alloc()), pad(nullptr)
{
    if (client == nullptr) {
        spdlog::error("Uh, not enough memory to do that?!");
        return;
    }
    Hook();
}

void CreationEngineInputManager::onPollGamepadState(__int64 a1, double xmm2) {
    static auto instance = CreationEngineInputManager::Get();
    instance->UpdateDeviceState();
    static auto original_fn = instance->m_poll_events_hook->get_original<decltype(CreationEngineInputManager::onPollGamepadState)>();
    return original_fn(a1, xmm2);
}

void CreationEngineInputManager::UpdateDeviceState() {
    if(!connected) {
        return;
    }
    XUSB_REPORT report = {0};
    static auto vr = VR::get();

    // (fix189) The virtual controller keeps the last report it was sent: when the headset's session stops, one empty report is sent,
    // so a stick, trigger or button held at that moment is let go (not held on until the session is back).
    static bool was_active = false;
    if (!vr->is_hmd_active()) {
        if (was_active) {
            was_active = false;
            vigem_target_x360_update(client, pad, report);
        }
        return;
    }
    was_active = true;
    vr->update_action_states();
//    spdlog::info("Updating input state fc[{}]", vr->m_frame_count);

    uint32_t retval;
    vr->on_xinput_get_state(&retval, 0, (XINPUT_STATE*)&report);
    vigem_target_x360_update(client, pad, report);
    haptics::Update(report.wButtons);
}
