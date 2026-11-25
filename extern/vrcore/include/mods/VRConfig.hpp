// vrcore - extra VR display options (virtual screen used for menus and flat-screen mode).
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Mod.hpp"

class VRConfig : public Mod {
public:
    static std::shared_ptr<VRConfig>& get();

    [[nodiscard]] std::string_view get_name() const override { return "VRConfig"; }

    void on_draw_ui() override;
    void on_frame() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    float get_flat_screen_distance() const { return m_flat_screen_distance->value(); }
    float get_flat_screen_width() const { return m_flat_screen_width->value(); }

    // A line about the menu pointer for the menu (set by the game side).
    static inline std::function<std::string()> s_pointer_status{};

private:
    const ModSlider::Ptr m_flat_screen_distance{ModSlider::create(generate_name("FlatScreenDistance"), 0.5f, 10.0f, 1.6f)};
    const ModSlider::Ptr m_flat_screen_width{ModSlider::create(generate_name("FlatScreenWidth"), 0.5f, 10.0f, 1.8f)};

    // The menu panel (OpenXR: menus on a panel that stays in the room, as in Skyrim VR).
    const ModToggle::Ptr m_panel_enabled{ModToggle::create(generate_name("MenuPanel"), true)};
    const ModToggle::Ptr m_panel_curved{ModToggle::create(generate_name("MenuPanelCurved"), true)};
    // Virtual Desktop: an empty layer with this frame's head position sent first, so the panel stays where it was put.
    const ModToggle::Ptr m_vd_fix{ModToggle::create(generate_name("VirtualDesktopFix"), true)};
    // The menu drawn into the world's eye pictures behind see-through menus (else a panel layer of its own).
    const ModToggle::Ptr m_panel_draw_in{ModToggle::create(generate_name("MenuPanelDrawIn"), true)};
    const ModSlider::Ptr m_panel_distance{ModSlider::create(generate_name("MenuPanelDistance"), 0.6f, 4.0f, 1.5f)};
    const ModSlider::Ptr m_panel_width{ModSlider::create(generate_name("MenuPanelWidth"), 0.5f, 4.0f, 1.8f)};
    const ModSlider::Ptr m_panel_height{ModSlider::create(generate_name("MenuPanelHeight"), -0.6f, 0.4f, -0.1f)};
    // (A new name: the old setting's value was a part of the whole picture; this one is a part of the menu's box.)
    const ModSlider::Ptr m_panel_visible_height{ModSlider::create(generate_name("MenuPanelVisibleHeightOfBox"), 0.4f, 1.0f, 1.0f)};
    const ModToggle::Ptr m_panel_trim_to_box{ModToggle::create(generate_name("MenuPanelTrimToBox"), true)};
    const ModCombo::Ptr m_panel_backdrop{ModCombo::create(generate_name("MenuPanelBackdrop"),
        {"Darkness", "The world as it was", "The world, dimmed", "The world, dark"}, 2)};
    // (fix164) With nothing behind the panel (the main menu, the intro, loading screens): a soft light round it in the picture's own colours.
    const ModToggle::Ptr m_panel_ambient{ModToggle::create(generate_name("MenuAmbientLight"), true)};
    const ModSlider::Ptr m_panel_ambient_strength{ModSlider::create(generate_name("MenuAmbientStrength"), 0.0f, 2.0f, 1.0f)};
    const ModToggle::Ptr m_panel_pointer{ModToggle::create(generate_name("MenuPointer"), true)};
    const ModToggle::Ptr m_panel_see_through{ModToggle::create(generate_name("MenuPanelSeeThrough"), true)};
    // (A new name: the old setting's 0.3 left the world much too dark (the veil around the panel was added to it then); this one
    // starts at 0.05, the world at 95%.)
    const ModSlider::Ptr m_panel_see_through_darkness{ModSlider::create(generate_name("MenuPanelSeeThroughDim"), 0.0f, 0.9f, 0.05f)};
    // Behind a see-through menu: the world as it is now, drawn by the game (it stops drawing it for these menus otherwise).
    const ModToggle::Ptr m_panel_live_world{ModToggle::create(generate_name("MenuPanelLiveWorld"), true)};
    // On a see-through menu: its 3D pictures (you, the planet, the item) cut out of their own pictures.
    const ModToggle::Ptr m_panel_previews{ModToggle::create(generate_name("MenuPanelPreviews"), true)};
    // ...cut out by their depth (else by their background's colour).
    const ModToggle::Ptr m_panel_depth_cutout{ModToggle::create(generate_name("MenuPanelDepthCutout"), true)};
    // ...the light they cast on their background kept over the world (a glow around them).
    const ModToggle::Ptr m_panel_preview_glow{ModToggle::create(generate_name("MenuPanelPreviewGlow"), false)};
    // The pointer's laser and dot as at the main menu, in every menu.
    const ModToggle::Ptr m_panel_pointer_laser{ModToggle::create(generate_name("MenuPointerLaser"), true)};
    // Every menu with a 3D view drawn as the Data menu is (TAA at full size).
    const ModToggle::Ptr m_panel_menu_taa{ModToggle::create(generate_name("MenuPanelTaaEveryMenu"), true)};
    // Under the Inventory's faint item rows: a dark backing.
    const ModToggle::Ptr m_panel_button_backing{ModToggle::create(generate_name("MenuPanelButtonBacking"), true)};
    // Behind a see-through menu: the world's own last-frame pictures kept apart from the menu's 3D views' (no grain, no flicker).
    const ModToggle::Ptr m_panel_world_history{ModToggle::create(generate_name("MenuPanelWorldHistory"), true)};
    // ...the menu's 3D views' own ones too (you, the planet, the item: not the world's).
    const ModToggle::Ptr m_panel_view_histories{ModToggle::create(generate_name("MenuPanelViewHistories"), true)};
    // ...what was learned for a menu's views used again when they are seen again.
    const ModToggle::Ptr m_panel_history_plans{ModToggle::create(generate_name("MenuPanelHistoryPlans"), true)};
    // Behind a see-through menu: the world at its full size (not in the menu's 16:9 box: sharper, more work for the card).
    const ModToggle::Ptr m_panel_full_frame{ModToggle::create(generate_name("MenuPanelFullFrame"), true)};
    // Behind a see-through menu: the world drawn from where your head is now (the game would draw it from where you looked as the
    // menu opened).
    const ModToggle::Ptr m_panel_world_turns{ModToggle::create(generate_name("MenuPanelWorldTurns"), true)};

    ValueList m_options{*m_flat_screen_distance, *m_flat_screen_width, *m_panel_enabled, *m_panel_curved, *m_vd_fix, *m_panel_draw_in, *m_panel_distance, *m_panel_width,
        *m_panel_height, *m_panel_visible_height, *m_panel_trim_to_box, *m_panel_backdrop, *m_panel_pointer, *m_panel_see_through,
        *m_panel_see_through_darkness, *m_panel_live_world, *m_panel_previews, *m_panel_depth_cutout, *m_panel_button_backing, *m_panel_world_history,
        *m_panel_world_turns, *m_panel_view_histories, *m_panel_history_plans, *m_panel_full_frame, *m_panel_preview_glow, *m_panel_pointer_laser, *m_panel_menu_taa,
        *m_panel_ambient, *m_panel_ambient_strength};
};
