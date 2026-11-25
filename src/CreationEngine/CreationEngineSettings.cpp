#include "CreationEngineSettings.h"
#include "utility/RTTI.hpp"
#include <utility/Module.hpp>
#include "RE/I/INISettingCollection.h"
#include <cctype>
#include <string>
#include <unordered_map>

template<typename T>
uintptr_t find(const uintptr_t haystack, const uintptr_t endptr, uint64_t * const offset, const T needle) {
    const uintptr_t start(haystack + *offset);
    if (start >= endptr) return 0;
    const auto b = (const T*) start;
    const auto count = (unsigned)((endptr - start) / sizeof(T));
    for (unsigned i = 0; i < count; ++i) {
        if (b[i] == needle) {
            *offset += (i * sizeof(T));
            return *offset;
        }
    }
    return 0;
}

namespace {
    using SettingsMap = std::unordered_map<std::string, RE::Setting*>;

    SettingsMap scan_settings(const char* vtable) {
        SettingsMap settings_map{};

        auto module = utility::get_executable();
        auto vt_ptr = utility::rtti::find_vtable(module, vtable);
        if (!vt_ptr) {
            spdlog::error("Failed to find vtable: {}", vtable);
            return settings_map;
        }
        spdlog::debug("Found vtable: {} -> {:x}", vtable, vt_ptr.value());
        auto begin = reinterpret_cast<uintptr_t>(module);
        auto end = begin + *utility::get_module_size(module);
        uintptr_t offset = 0;
        uintptr_t instance_ptr = *vt_ptr;
        while (find(begin, end, &offset, instance_ptr)) {
            auto setting = reinterpret_cast<RE::Setting*>(begin + offset);
            auto val = setting->to_string();
            spdlog::debug("[{}] ptr={}: {}={}", vtable, fmt::ptr(setting), setting->GetKey().data(), val.data());
            std::string key = setting->GetKey().data();
            settings_map[key] = setting;
            offset += sizeof(RE::Setting);
        }
        return settings_map;
    }

    SettingsMap* settings_of(CreationEngineSettings::SettingType type) {
        using T = CreationEngineSettings::SettingType;
        switch (type) {
        case T::kINISetting: {
            static auto m = scan_settings(".?AV?$SettingT@VINISettingCollection@@@@");
            return &m;
        }
        case T::kINIPrefSetting: {
            static auto m = scan_settings(".?AVINIPrefSetting@@");
            return &m;
        }
        case T::kGameSetting: {
            static auto m = scan_settings(".?AV?$SettingT@VGameSettingCollection@@@@");
            return &m;
        }
        case T::kRendererQualitySetting: {
            static auto m = scan_settings(".?AVRendererQualitySetting@CreationRenderer@@");
            return &m;
        }
        case T::kRendererPerfSetting: {
            static auto m = scan_settings(".?AVRendererQualityPref@CreationRenderer@@");
            return &m;
        }
        default:
            return nullptr;
        }
    }
}

RE::Setting* CreationEngineSettings::get_setting(std::string_view id, CreationEngineSettings::SettingType type)
{
    auto map = settings_of(type);
    if (map == nullptr) {
        return nullptr;
    }
    auto it = map->find(std::string{id});
    return it != map->end() ? it->second : nullptr;
}

void CreationEngineSettings::log_settings_containing(std::initializer_list<std::string_view> words)
{
    using T = SettingType;
    const std::pair<T, const char*> types[] = {
        {T::kINISetting, "ini"}, {T::kINIPrefSetting, "ini pref"}, {T::kGameSetting, "game"},
        {T::kRendererQualitySetting, "renderer quality"}, {T::kRendererPerfSetting, "renderer pref"},
    };
    auto lower = [](std::string text) {
        for (auto& c : text) {
            c = (char)std::tolower((unsigned char)c);
        }
        return text;
    };
    int count = 0;
    for (const auto& [type, label] : types) {
        auto map = settings_of(type);
        if (map == nullptr) {
            continue;
        }
        for (const auto& [key, setting] : *map) {
            if (setting == nullptr) {
                continue;
            }
            const auto name = lower(key);
            for (auto word : words) {
                if (name.find(lower(std::string{word})) != std::string::npos) {
                    spdlog::info("[Settings] ({}) {} = {}", label, key, setting->to_string());
                    ++count;
                    break;
                }
            }
        }
    }
    spdlog::info("[Settings] {} settings listed", count);
}
