// vrcore - engine-agnostic VR framework for starfield2vr.
// Mod/option model modelled on REFramework's Mod.hpp (MIT, praydog), rewritten for vrcore.
#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <utility/Config.hpp>

class IModValue {
public:
    using Ptr = std::unique_ptr<IModValue>;

    virtual ~IModValue() = default;
    virtual bool draw(std::string_view name) = 0;
    virtual void draw_value(std::string_view name) = 0;
    virtual void config_load(const utility::Config& cfg, bool set_defaults) = 0;
    virtual void config_save(utility::Config& cfg) = 0;
};

using ValueList = std::vector<std::reference_wrapper<IModValue>>;

template <typename T>
class ModValue : public IModValue {
public:
    using Ptr = std::unique_ptr<ModValue<T>>;

    static auto create(std::string_view config_name, T default_value = T{}) {
        return std::make_unique<ModValue<T>>(config_name, default_value);
    }

    ModValue(std::string_view config_name, T default_value)
        : m_value{default_value}, m_default_value{default_value}, m_config_name{config_name} {}

    void config_load(const utility::Config& cfg, bool set_defaults) override {
        if (set_defaults) {
            m_value = m_default_value;
        }

        if (auto v = cfg.get<T>(m_config_name)) {
            m_value = *v;
        }
    }

    void config_save(utility::Config& cfg) override { cfg.set<T>(m_config_name, m_value); }

    bool draw(std::string_view) override { return false; }
    void draw_value(std::string_view) override {}

    operator T&() { return m_value; }
    T& value() { return m_value; }
    const T& value() const { return m_value; }
    const T& default_value() const { return m_default_value; }
    const auto& get_config_name() const { return m_config_name; }

protected:
    T m_value{};
    T m_default_value{};
    std::string m_config_name{"Default_ModValue"};
};

class ModToggle : public ModValue<bool> {
public:
    using Ptr = std::unique_ptr<ModToggle>;

    ModToggle(std::string_view config_name, bool default_value) : ModValue<bool>{config_name, default_value} {}

    static auto create(std::string_view config_name, bool default_value = false) {
        return std::make_unique<ModToggle>(config_name, default_value);
    }

    bool draw(std::string_view name) override {
        ImGui::PushID(this);
        const auto ret = ImGui::Checkbox(name.data(), &m_value);
        ImGui::PopID();
        return ret;
    }

    void draw_value(std::string_view name) override { ImGui::Text("%s: %i", name.data(), (int)m_value); }

    bool toggle() { return m_value = !m_value; }
};

class ModFloat : public ModValue<float> {
public:
    using Ptr = std::unique_ptr<ModFloat>;

    ModFloat(std::string_view config_name, float default_value) : ModValue<float>{config_name, default_value} {}

    static auto create(std::string_view config_name, float default_value = 0.0f) {
        return std::make_unique<ModFloat>(config_name, default_value);
    }

    bool draw(std::string_view name) override {
        ImGui::PushID(this);
        const auto ret = ImGui::InputFloat(name.data(), &m_value);
        ImGui::PopID();
        return ret;
    }

    void draw_value(std::string_view name) override { ImGui::Text("%s: %f", name.data(), m_value); }
};

class ModSlider : public ModFloat {
public:
    using Ptr = std::unique_ptr<ModSlider>;

    static auto create(std::string_view config_name, float mn = 0.0f, float mx = 1.0f, float default_value = 0.0f) {
        return std::make_unique<ModSlider>(config_name, mn, mx, default_value);
    }

    ModSlider(std::string_view config_name, float mn = 0.0f, float mx = 1.0f, float default_value = 0.0f)
        : ModFloat{config_name, default_value}, m_range{mn, mx} {}

    bool draw(std::string_view name) override {
        ImGui::PushID(this);
        const auto ret = ImGui::SliderFloat(name.data(), &m_value, m_range.x, m_range.y);
        ImGui::PopID();
        return ret;
    }

    void draw_value(std::string_view name) override {
        ImGui::Text("%s: %f [%f, %f]", name.data(), m_value, m_range.x, m_range.y);
    }

    auto& range() { return m_range; }

protected:
    ImVec2 m_range{0.0f, 1.0f};
};

class ModInt32 : public ModValue<int32_t> {
public:
    using Ptr = std::unique_ptr<ModInt32>;

    static auto create(std::string_view config_name, int32_t default_value = 0) {
        return std::make_unique<ModInt32>(config_name, default_value);
    }

    ModInt32(std::string_view config_name, int32_t default_value = 0) : ModValue{config_name, default_value} {}

    bool draw(std::string_view name) override {
        ImGui::PushID(this);
        const auto ret = ImGui::InputInt(name.data(), &m_value);
        ImGui::PopID();
        return ret;
    }

    void draw_value(std::string_view name) override { ImGui::Text("%s: %i", name.data(), m_value); }
};

class ModCombo : public ModValue<int32_t> {
public:
    using Ptr = std::unique_ptr<ModCombo>;

    static auto create(std::string_view config_name, std::vector<std::string> options, int32_t default_value = 0) {
        return std::make_unique<ModCombo>(config_name, std::move(options), default_value);
    }

    ModCombo(std::string_view config_name, std::vector<std::string> options, int32_t default_value = 0)
        : ModValue{config_name, default_value}, m_options{std::move(options)} {
        for (auto& o : m_options) {
            m_options_c.push_back(o.c_str());
        }
    }

    bool draw(std::string_view name) override {
        // clamp in case the config file holds an out of range value
        if (m_value < 0 || m_value >= (int32_t)m_options_c.size()) {
            m_value = 0;
        }

        ImGui::PushID(this);
        const auto ret = ImGui::Combo(name.data(), &m_value, m_options_c.data(), (int)m_options_c.size());
        ImGui::PopID();
        return ret;
    }

    void draw_value(std::string_view name) override {
        const auto idx = (m_value >= 0 && m_value < (int32_t)m_options.size()) ? m_value : 0;
        ImGui::Text("%s: %s", name.data(), m_options.empty() ? "" : m_options[idx].c_str());
    }

    const auto& options() const { return m_options; }

protected:
    std::vector<std::string> m_options{};
    std::vector<const char*> m_options_c{};
};

// A keyboard key binding (virtual-key code) that can be changed from the menu.
class ModKey : public ModInt32 {
public:
    using Ptr = std::unique_ptr<ModKey>;

    static constexpr int32_t UNBOUND_KEY = -1;

    static auto create(std::string_view config_name, int32_t default_value = UNBOUND_KEY) {
        return std::make_unique<ModKey>(config_name, default_value);
    }

    ModKey(std::string_view config_name, int32_t default_value = UNBOUND_KEY) : ModInt32{config_name, default_value} {}

    bool draw(std::string_view name) override {
        ImGui::PushID(this);
        ImGui::Button(name.data());

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hover and press a key to bind, Delete to unbind");

            for (int32_t k = 1; k < 255; ++k) {
                if (k == VK_LBUTTON || k == VK_RBUTTON) {
                    continue;
                }

                if (GetAsyncKeyState(k) & 0x8000) {
                    m_value = (k == VK_DELETE) ? UNBOUND_KEY : k;
                    break;
                }
            }
        }

        ImGui::SameLine();
        if (m_value == UNBOUND_KEY) {
            ImGui::Text("Not bound");
        } else {
            ImGui::Text("Key: 0x%X", m_value);
        }

        ImGui::PopID();
        return true;
    }

    bool is_key_down() const {
        return m_value != UNBOUND_KEY && (GetAsyncKeyState(m_value) & 0x8000) != 0;
    }

    bool is_key_down_once() {
        const auto down = is_key_down();
        const auto result = down && !m_was_down;
        m_was_down = down;
        return result;
    }

private:
    bool m_was_down{false};
};

// (fix196) The F11 menu's small helpers, the same in every tab.
namespace menu_ui {
// A grey line (wrapped) under a row: what it does, in plain words.
inline void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
// A group of rows, folded until opened (ImGui keeps it open or folded for the session).
inline bool group(const char* title, bool open_at_first = false) {
    return ImGui::CollapsingHeader(title, open_at_first ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
}
} // namespace menu_ui

class Mod {
public:
    virtual ~Mod() = default;

    [[nodiscard]] virtual std::string_view get_name() const { return "UnknownMod"; }

    virtual std::string generate_name(std::string_view name) const {
        return std::string{get_name()} + "_" + std::string{name};
    }

    // Called once the D3D12 device is available, on the render thread.
    virtual std::optional<std::string> on_initialize() { return std::nullopt; }
    virtual std::optional<std::string> on_initialize_d3d_thread() { return std::nullopt; }

    // Called every frame, before the game presents.
    virtual void on_frame() {}
    virtual void on_present() {}
    virtual void on_post_present() {}
    virtual void on_pre_imgui_frame() {}
    virtual void on_draw_ui() {}
    virtual void on_draw_quick_ui() {} // (before fix196: drawn at the top of the menu, always visible)
    // (fix196) The F11 menu's tabs. Main: the few settings most players need, drawn in parts so every mod's rows come in one order (0
    // recentring, 1 the view, 2 the body, 3 turning, 4 the controllers, 5 the HUD and this menu: Framework::draw_ui gives each its heading).
    // Advanced: everything else a player may change, by feature, in parts too (0 gameplay, 1 the view and performance, 2 menus, controllers
    // and the monitor). Debug: tests, diagnostics, troubleshooting. A mod not sorted into them
    // shows its old rows in Debug.
    virtual void on_draw_main_ui(int /*part*/) {}
    virtual void on_draw_advanced_ui(int /*part*/) {}
    virtual void on_draw_debug_ui() {
        on_draw_quick_ui();
        on_draw_ui();
    }
    virtual void on_device_reset() {}

    // return false to block the message from reaching the game
    virtual bool on_message(HWND, UINT, WPARAM, LPARAM) { return true; }

    virtual void on_config_load(const utility::Config&, bool set_defaults) {}
    virtual void on_config_save(utility::Config&) {}
};
