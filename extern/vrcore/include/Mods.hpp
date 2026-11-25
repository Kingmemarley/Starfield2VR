// vrcore - mod container. The constructor (Mods::Mods) is defined by the game project,
// which registers its own mods alongside VR and VRConfig.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Mod.hpp"

class Mods {
public:
    Mods();
    virtual ~Mods() = default;

    std::optional<std::string> on_initialize() const;
    std::optional<std::string> on_initialize_d3d_thread() const;

    void on_frame() const;
    void on_present() const;
    void on_post_present() const;
    void on_pre_imgui_frame() const;
    void on_draw_ui() const;
    void on_draw_quick_ui() const;
    void on_device_reset() const;
    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) const;

    void on_config_load(const utility::Config& cfg, bool set_defaults) const;
    void on_config_save(utility::Config& cfg) const;

    const auto& get_mods() const { return m_mods; }

private:
    std::vector<std::shared_ptr<Mod>> m_mods;
};
