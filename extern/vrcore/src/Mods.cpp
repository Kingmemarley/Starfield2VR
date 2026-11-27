#include <spdlog/spdlog.h>

#include "Mods.hpp"

std::optional<std::string> Mods::on_initialize() const {
    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize()", mod->get_name().data());

        if (auto e = mod->on_initialize(); e) {
            spdlog::error("{:s}::on_initialize() failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    return std::nullopt;
}

std::optional<std::string> Mods::on_initialize_d3d_thread() const {
    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize_d3d_thread()", mod->get_name().data());

        if (auto e = mod->on_initialize_d3d_thread(); e) {
            spdlog::error("{:s}::on_initialize_d3d_thread() failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    return std::nullopt;
}

void Mods::on_frame() const {
    for (auto& mod : m_mods) {
        mod->on_frame();
    }
}

void Mods::on_present() const {
    for (auto& mod : m_mods) {
        mod->on_present();
    }
}

void Mods::on_post_present() const {
    for (auto& mod : m_mods) {
        mod->on_post_present();
    }
}

void Mods::on_pre_imgui_frame() const {
    for (auto& mod : m_mods) {
        mod->on_pre_imgui_frame();
    }
}

void Mods::on_draw_ui() const {
    for (auto& mod : m_mods) {
        mod->on_draw_ui();
    }
}

void Mods::on_draw_quick_ui() const {
    for (auto& mod : m_mods) {
        mod->on_draw_quick_ui();
    }
}

// (fix196) The F11 menu's tabs (Mod::on_draw_main_ui and the others), each mod's rows in the mods' order.
void Mods::on_draw_main_ui(int part) const {
    for (auto& mod : m_mods) {
        ImGui::PushID(mod.get());
        mod->on_draw_main_ui(part);
        ImGui::PopID();
    }
}

void Mods::on_draw_advanced_ui(int part) const {
    for (auto& mod : m_mods) {
        ImGui::PushID(mod.get());
        mod->on_draw_advanced_ui(part);
        ImGui::PopID();
    }
}

void Mods::on_draw_debug_ui() const {
    for (auto& mod : m_mods) {
        ImGui::PushID(mod.get());
        mod->on_draw_debug_ui();
        ImGui::PopID();
    }
}

void Mods::on_device_reset() const {
    for (auto& mod : m_mods) {
        mod->on_device_reset();
    }
}

bool Mods::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) const {
    bool any_false = false;

    for (auto& mod : m_mods) {
        if (!mod->on_message(wnd, message, w_param, l_param)) {
            any_false = true;
        }
    }

    return !any_false;
}

void Mods::on_config_load(const utility::Config& cfg, bool set_defaults) const {
    for (auto& mod : m_mods) {
        mod->on_config_load(cfg, set_defaults);
    }
}

void Mods::on_config_save(utility::Config& cfg) const {
    for (auto& mod : m_mods) {
        mod->on_config_save(cfg);
    }
}
