#include <algorithm>

#include <imgui.h>

#include "mods/VR.hpp"
#include "mods/VRConfig.hpp"
#include "ModSettings.h"

std::shared_ptr<VRConfig>& VRConfig::get() {
    static auto inst = std::make_shared<VRConfig>();
    return inst;
}

void VRConfig::on_draw_ui() {
    if (!ImGui::CollapsingHeader("Menus and Virtual Screen")) {
        return;
    }

    ImGui::SeparatorText("Menu panel");
    ImGui::TextWrapped("Game menus are shown on a panel that stays where it was put, in front of where you looked when the menu "
                       "opened (as in Skyrim VR), until the menu closes. Recenter (the headset's own, or the mod's) to bring it in front of you again. "
                       "Point at it with a controller and pull the trigger to click.");
    m_panel_enabled->draw("Menus on the panel (off: the old screen that moves with your head)");
    if (m_panel_enabled->value()) {
        m_panel_curved->draw("Curved around you");
        m_vd_fix->draw("Virtual Desktop fix (an empty layer with your head position first)");
        m_panel_draw_in->draw("Draw the menu into the world picture (keeps it in place with Virtual Desktop)");
        m_panel_pointer_laser->draw("The pointer's laser in every menu (as at the main menu)");
        m_panel_distance->draw("Panel distance (m)");
        m_panel_width->draw("Panel width (m)");
        m_panel_height->draw("Panel height against your eyes (m)");
        m_panel_trim_to_box->draw("Show only the menu's box (no black bars above and below)");
        m_panel_visible_height->draw("Part of the menu's height shown (trims more off the top and bottom)");
        m_panel_backdrop->draw("Around the panel");
        m_panel_ambient->draw("Soft light around the panel at the main menu, the intro and loading screens (in the picture's colours)");
        if (m_panel_ambient->value()) {
            m_panel_ambient_strength->draw("Soft light: strength (1: subtle, 0: none, 2: twice as bright)");
        }
        m_panel_see_through->draw("See-through menus (the world shows through the menu)");
        if (m_panel_see_through->value()) {
            m_panel_see_through_darkness->draw("See-through: darkness of the world behind the menu (0.05: it shows at 95%)");
            m_panel_live_world->draw("See-through: the world as it is now (the game keeps drawing it; it follows your head)");
            if (m_panel_live_world->value()) {
                m_panel_world_turns->draw("See-through: the world turns with your head (else it is drawn from where you looked as the menu opened)");
                m_panel_full_frame->draw("See-through: the world at its full size (sharper; more work for the graphics card)");
                m_panel_world_history->draw("See-through: keep the world's own last-frame pictures apart from the menu's (smooth, no grain)");
                if (m_panel_world_history->value()) {
                    m_panel_view_histories->draw("See-through: the menu's 3D pictures keep their own last-frame pictures too (not shiny)");
                    m_panel_history_plans->draw("See-through: use again what was learned for a menu seen before (no relearning)");
                }
            }
            m_panel_previews->draw("See-through: the menu's 3D pictures (you, the planet, the item) over the world");
            if (m_panel_previews->value()) {
                m_panel_depth_cutout->draw("See-through: cut the 3D pictures out by their depth (else by their background's colour)");
                m_panel_preview_glow->draw("See-through: keep the light the 3D pictures cast around them (a glow)");
                m_panel_menu_taa->draw("See-through: every menu with a 3D view drawn as the Data menu is (TAA at full size, not DLSS's size copied up)");
            }
            m_panel_button_backing->draw("See-through: a dark backing under the Inventory's item rows");
            ImGui::TextDisabled("For the menus drawn by the interface alone (Data, Inventory, Pause, Skills, Crew, Research, Missions, "
                                "Powers); the backgrounds and dark edges these menus paint themselves are taken out. The game stops "
                                "drawing the world for these menus; with \"the world as it is now\" it keeps drawing it behind them "
                                "(else the world as it was when the menu opened is shown). Menus that are mostly 3D (the star map, ship "
                                "building, crafting) stay as they are. The dimming of \"Around the panel\" is not added while a menu is "
                                "see-through.");
        }
        m_panel_pointer->draw("Point and click with the controllers");
        auto& vr = VR::get();
        if (vr->m_openxr != nullptr) {
            const auto& xr = vr->m_openxr;
            if (!runtimes::OpenXR::ext_cylinder && m_panel_curved->value()) {
                ImGui::TextDisabled("This headset's runtime has no curved panels: the panel is flat.");
            } else if (xr->panel_cylinder_refused) {
                ImGui::TextDisabled("The runtime refused the curved panel: it is flat until the game restarts.");
            }
            if (xr->panel_refused) {
                ImGui::TextDisabled("The runtime refused the panel: menus are on the old screen until the game restarts.");
            }
            if (xr->ambient_refused) {
                ImGui::TextDisabled("The runtime refused the soft light around the panel: it is off until the game restarts.");
            }
        }
        if (s_pointer_status) {
            ImGui::TextWrapped("%s", s_pointer_status().c_str());
        }
    }

    ImGui::SeparatorText("Virtual screen");
    ImGui::TextWrapped("Flat-screen mode (left grip + left stick click) is shown on this screen%s.",
        m_panel_enabled->value() ? "" : ", and so are menus");
    m_flat_screen_distance->draw("Distance (m)");
    m_flat_screen_width->draw("Width (m)");

    ImGui::Separator();

    if (ImGui::TreeNode("Troubleshooting")) {
        ImGui::TextWrapped("Each switch turns off one of the mod's changes to the game camera. "
                           "If the picture is black, try them one at a time.");
        auto& d = ModSettings::g_debug;
        auto& s = ModSettings::g_internalSettings;
        ImGui::Checkbox("Flat screen mode (no VR camera at all)", &s.forceFlatScreen);
        ImGui::Checkbox("Disable head tracking camera", &d.disableHeadTracking);
        ImGui::Checkbox("Disable per-eye projection (frustum)", &d.disableFrustumOverride);
        ImGui::Checkbox("Disable HUD viewport scaling", &d.disableHudViewport);
        ImGui::Checkbox("Disable window resize to headset resolution", &d.disableWindowResize);
        ImGui::Checkbox("Swap HUD eye offset", &d.swapHudEyes);
        ImGui::Checkbox("Re-apply eye projection every frame", &d.forceFrustumEveryFrame);
        ImGui::Checkbox("Original menu detection (flat screen when no HUD is drawn)", &d.originalMenuDetection);
        ImGui::Checkbox("Never show menus on the flat screen", &d.neverFlatScreenMenus);
        ImGui::Checkbox("Eye projection for other screen-shaped cameras (old test, keep off)", &d.eyeProjectionOtherCameras);
        ImGui::Checkbox("Eye offset for the first-person weapon camera", &d.eyeOffsetFirstPersonCamera);
        ImGui::Separator();
        ImGui::TextWrapped("Ghosting tests (near objects doubled, far objects fine):");
        ImGui::Checkbox("Reset TAA history every frame", &d.taaHistoryReset);
        ImGui::Checkbox("Use the other TAA history slot", &d.taaHistoryInvert);
        ImGui::TextDisabled("Turn these on only after the save has loaded; they reset when the game restarts.");
        ImGui::TreePop();
    }
}

void VRConfig::on_frame() {
    auto& vr = VR::get();

    if (vr->m_openxr != nullptr) {
        auto& xr = vr->m_openxr;
        xr->flat_screen_distance = m_flat_screen_distance->value();
        xr->flat_screen_width = m_flat_screen_width->value();
        xr->panel_enabled = m_panel_enabled->value();
        xr->panel_curved = m_panel_curved->value();
        xr->vd_fix = m_vd_fix->value();
        xr->panel_draw_in = m_panel_draw_in->value();
        xr->panel_distance = m_panel_distance->value();
        xr->panel_width = m_panel_width->value();
        xr->panel_height_offset = m_panel_height->value();
        xr->panel_visible_height = m_panel_visible_height->value();
        xr->panel_trim_to_box = m_panel_trim_to_box->value();
        xr->panel_backdrop = std::clamp(m_panel_backdrop->value(), 0, 3);
        xr->panel_ambient = m_panel_ambient->value();
        xr->panel_ambient_strength = std::clamp(m_panel_ambient_strength->value(), 0.0f, 2.0f);
        xr->panel_pointer_enabled = m_panel_pointer->value();
        xr->panel_see_through = m_panel_see_through->value();
        xr->panel_see_through_darkness = std::clamp(m_panel_see_through_darkness->value(), 0.0f, 0.9f);
        xr->panel_live_world = m_panel_live_world->value();
        xr->panel_previews = m_panel_previews->value();
        xr->panel_depth_cutout = m_panel_depth_cutout->value();
        xr->panel_preview_glow = m_panel_preview_glow->value();
        xr->panel_pointer_laser = m_panel_pointer_laser->value();
        xr->panel_menu_taa = m_panel_menu_taa->value();
        xr->panel_button_backing = m_panel_button_backing->value();
        xr->panel_world_history = m_panel_world_history->value();
        xr->panel_view_histories = m_panel_view_histories->value();
        xr->panel_history_plans = m_panel_history_plans->value();
        xr->panel_full_frame = m_panel_full_frame->value();
        xr->panel_world_turns = m_panel_world_turns->value();
    }
}

void VRConfig::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void VRConfig::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
