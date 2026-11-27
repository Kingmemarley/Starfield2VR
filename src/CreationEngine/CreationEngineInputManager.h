#pragma once
#include <ViGEm/Client.h>
#include <memory/FunctionHook.h>

#include <atomic>
#include <string>

#pragma comment(lib, "setupapi.lib")

// The VR controllers reach the game as an Xbox pad. (fix193) Two ways: first, a virtual pad given as the game's own list of Windows
// gamepads (VirtualGamepad.h; no driver needed; fix195: the only pad in it); else, as before, a virtual Xbox 360 pad plugged in through the
// ViGEmBus driver, fed from the game's gamepad poll. Never both.
class CreationEngineInputManager
{
public:
    // Chooses the way (once, from the mod's start) and logs which one and why.
    void Init();
    void Reset();
    void UpdateDeviceState();

    // (fix193) The VR controllers read as an Xbox pad for this poll of the game (the buttons, triggers and sticks the mod's VR part
    // makes of them, in an Xbox 360 report), with the controllers' own vibration updated. False, with the report all zero, while
    // the headset's session is off. On the game's input thread (its gamepad poll), for either way.
    static bool ReadVrPad(XUSB_REPORT& report);

    static CreationEngineInputManager* Get()
    {
        static auto instance(new CreationEngineInputManager);
        return instance;
    }

    // Run when the game starts (from the DLL's start thread, before the mod's 5 second wait). (fix193) Puts the watch on the
    // game's request for its Windows gamepad list (VirtualGamepad::InstallEarly); only when that cannot be done, plugs in the
    // ViGEm pad right away as before (some modlists only set up the gamepad for a controller that is connected when the game
    // starts). Init() then reuses it.
    static void PlugInEarly();

    CreationEngineInputManager(const CreationEngineInputManager&)            = delete;
    CreationEngineInputManager& operator=(const CreationEngineInputManager&) = delete;
    ~CreationEngineInputManager();

private:
    CreationEngineInputManager();
    void UseVirtualPad(const char* when);
    bool UseViGEm(const std::string& why);  // (fix195) true when the ViGEm pad is plugged in and fed
    bool ConnectViGEm();
    void UnplugViGEm();
    static bool ViGEmBusWorks();

    std::atomic<bool>             m_chosen{ false };
    bool                          connected = false;
    PVIGEM_CLIENT                 client{ nullptr };
    PVIGEM_TARGET                 pad{ nullptr };
    std::unique_ptr<FunctionHook> m_poll_events_hook{ nullptr };
    bool                          Hook();
    static void                   onPollGamepadState(__int64 a1, double xmm2);
};
