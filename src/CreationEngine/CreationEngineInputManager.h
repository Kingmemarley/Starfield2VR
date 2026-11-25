#pragma once
#include <ViGEm/Client.h>
#include <memory/FunctionHook.h>

#pragma comment(lib, "setupapi.lib")

class CreationEngineInputManager
{
public:
    void Init();
    void Reset();
    void UpdateDeviceState();

    static CreationEngineInputManager* Get()
    {
        static auto instance(new CreationEngineInputManager);
        return instance;
    }

    // Plugs in the virtual Xbox controller right when the game starts (called from the DLL's start
    // thread, before the mod's 5 second wait). Some modlists only set up the
    // gamepad for a controller that is connected when the game starts. Init() then reuses it.
    static void PlugInEarly();

    CreationEngineInputManager(const CreationEngineInputManager&)            = delete;
    CreationEngineInputManager& operator=(const CreationEngineInputManager&) = delete;
    ~CreationEngineInputManager();

private:
    CreationEngineInputManager();
    bool                          connected = false;
    PVIGEM_CLIENT                 client;
    PVIGEM_TARGET                 pad;
    std::unique_ptr<FunctionHook> m_poll_events_hook{ nullptr };
    bool                          Hook();
    static void                   onPollGamepadState(__int64 a1, double xmm2);
};
