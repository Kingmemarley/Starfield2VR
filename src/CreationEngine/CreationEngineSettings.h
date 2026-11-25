#pragma once

#include <RE/S/Setting.h>
#include <initializer_list>
#include <memory>
#include <string_view>

class CreationEngineSettings
{
public:
    enum class SettingType
    {
        kINISetting,
        kINIPrefSetting,
        kRegSetting,
        kGameSetting,
        kRendererQualitySetting,
        kRendererPerfSetting,
    };

    inline static std::shared_ptr<CreationEngineSettings>& Get()
    {
      static std::shared_ptr<CreationEngineSettings> instance{ std::make_shared<CreationEngineSettings>() };
      return instance;
    }

    template <class T>
    inline T get_setting(std::string_view id, SettingType type, T fallback)
    {
        auto setting = get_setting(id, type);
        if (setting) {
            return setting->GetValue<T>(fallback);
        }
        return fallback;
    }

    template <class T>
    inline bool set_setting(std::string_view id, SettingType type, T value)
    {
        auto setting = get_setting(id, type);
        if (setting) {
            return setting->SetValue(value);
        }
        return false;
    }

    // Write every setting whose name contains one of the words (any case) to the log.
    static void log_settings_containing(std::initializer_list<std::string_view> words);

    static RE::Setting* get_setting(std::string_view id, SettingType type);

private:
    friend class GameSettingsComponent;
};