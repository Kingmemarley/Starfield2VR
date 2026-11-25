#include "GameSettingsComponent.h"
#include "CreationEngineRendererModule.h"
#include "CreationEngineSettings.h"
#include "CreationEngineConstants.h"
#include "Foveation.h"
#include "EyeShare.h"
#include "GpuTimer.h"
#include "MeleeThrow.h"
#include "RenderSteps.h"
#include "ShadowShare.h"
#include <algorithm>
#include <mods/VR.hpp>
#include <ModSettings.h>
#include <chrono>
#include <cmath>

std::optional<std::string> GameSettingsComponent::on_initialize()
{
    spdlog::info("GameSettingsComponent::on_initialize");
    return Mod::on_initialize();
}

std::optional<std::string> GameSettingsComponent::on_initialize_d3d_thread()
{
    spdlog::info("GameSettingsComponent::on_initialize_d3d_thread");
    return Mod::on_initialize_d3d_thread();
}

// Keep the game's FOV settings matched to the headset. The first-person weapon/arms are drawn
// with their own FOV (fFPGeometryFOV); if it differs from the world FOV, the game scales the
// weapon's projection relative to the world's, which with each eye's off-centre lens puts the
// gun in a different place and size in each eye. Mutar's mod set these once on device reset;
// here they are re-checked every couple of seconds and logged, since the game can reset them.
void GameSettingsComponent::apply_vr_settings(const char* why)
{
    auto vr = VR::get();
    if (!vr->get_runtime()->loaded) {
        return;
    }
    const float fov = vr->get_runtime()->diagonal_fov;
    if (!(fov > 10.0f && fov < 179.0f)) {
        return; // headset FOV not known yet
    }
    static auto settings = CreationEngineSettings::Get();
    using T = CreationEngineSettings::SettingType;
    const float aspect = (float)vr->get_hmd_width() / (float)vr->get_hmd_height();

    // Remember the game's own FOVs (for the low-detail gun switches below), and once list the
    // game's level-of-detail and FOV settings in the log.
    static float game_weapon_fov = -1.0f;
    static float game_world_fov = -1.0f;
    if (game_weapon_fov < 0.0f) {
        const float current = settings->get_setting("fFPGeometryFOV:Camera", T::kINISetting, -1.0f);
        if (current > 10.0f && std::abs(current - fov) > 0.01f) {
            game_weapon_fov = current;
        }
    }
    if (game_world_fov < 0.0f) {
        const float current = settings->get_setting("fFPWorldFOV:Camera", T::kINIPrefSetting, -1.0f);
        if (current > 10.0f && std::abs(current - fov) > 0.01f) {
            game_world_fov = current;
        }
    }
    static bool listed = false;
    if (!listed) {
        listed = true;
        CreationEngineSettings::log_settings_containing({"lod", "fov", "firstperson", "1stperson", "fpgeometry", "fade", "dither", "cull", "nearDist", "farDist", "clip"});
    }

    // The gun is drawn with its own FOV relative to the world's. With both at the headset's FOV
    // it's the right size but the game picks a low-detail model; the game's own weapon FOV gives
    // the detailed model but draws the gun bigger. Using the game's FOV for both is the test for
    // getting both (the view itself is set by the mod each frame, not by these settings).
    float weapon_fov = fov;
    float world_fov = fov;
    if (m_override_weapon_fov->value()) {
        weapon_fov = m_weapon_fov->value();
    } else if (ModSettings::g_debug.gameWeaponFov) {
        weapon_fov = game_weapon_fov > 0.0f ? game_weapon_fov : 85.0f;
        if (ModSettings::g_debug.gameWorldFov) {
            world_fov = game_world_fov > 0.0f ? game_world_fov : 85.0f;
        }
    }

    // Rendering switches (see ModSettings.h).
    {
        struct WantedBool { const char* name; bool value; };
        const WantedBool wanted_bools[] = {
            {"bUseMeshletCulling:Display", ModSettings::g_debug.meshletCulling},
            {"bUseGPUCulling:Display", ModSettings::g_debug.gpuCulling},
            {"bMove1stPersonToOrigin:Gameplay", ModSettings::g_debug.firstPersonAtOrigin},
        };
        for (const auto& w : wanted_bools) {
            auto setting = CreationEngineSettings::get_setting(w.name, T::kINISetting);
            if (setting == nullptr) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    spdlog::info("[Settings] {} was not found", w.name);
                }
                continue;
            }
            const bool current = setting->GetValue<bool>(w.value);
            if (current != w.value) {
                const bool ok = setting->SetValue(w.value);
                spdlog::info("[Settings] {} {} -> {} ({}, {})", w.name, current, w.value, ok ? "set" : "failed", why);
            }
        }
    }

    // The game's GPU mesh-detail selection (test switch for the low-detail gun).
    {
        const bool wanted = ModSettings::g_debug.gpuLodEvaluation;
        static bool reported = false;
        if (!reported) {
            reported = true;
            auto s = CreationEngineSettings::get_setting("bGPULodEvaluation:Display", T::kINISetting);
            spdlog::info("[Settings] bGPULodEvaluation:Display {}", s != nullptr ? (s->GetValue<bool>(true) ? "is on" : "is off") : "was not found");
        }
        const bool current = settings->get_setting("bGPULodEvaluation:Display", T::kINISetting, wanted);
        if (current != wanted) {
            const bool ok = settings->set_setting("bGPULodEvaluation:Display", T::kINISetting, wanted);
            spdlog::info("[Settings] bGPULodEvaluation:Display {} -> {} ({}, {})", current, wanted, ok ? "set" : "NOT FOUND", why);
        }
    }

    // Cheaper drawing (fix138, on by default; the game's values back when off). A tested modlist's StarfieldCustom.ini raises these above
    // the game's own: clouds drawn at full resolution (the game's is half: a quarter of the pixels), grass and rocks casting shadows
    // into the first three shadow cascades (out to about 70 m; the game's: none) and distant rocks into the far ones. Each eye's view
    // draws them again. The game reads these each frame (the clouds' steps are rebuilt by the game itself when theirs changes), so
    // they are changed in memory as the game runs, never in the ini files; the player's own values are kept before the first change.
    {
        struct Cheaper { const char* name; bool is_bool; uint32_t value; bool saved; uint32_t own; bool reported; };
        static Cheaper cheaper[] = {
            {"bHalfResClouds:Display", true, 1, false, 0, false},
            {"uCascadesGrassDistance:Shadows", false, 1, false, 0, false},
            {"uCascadesRocksDistance:Shadows", false, 2, false, 0, false},
            {"bDrawDistantRocksShadows:Shadows", true, 0, false, 0, false},
        };
        const bool on = m_cheaper_drawing->value();
        for (auto& c : cheaper) {
            auto setting = CreationEngineSettings::get_setting(c.name, T::kINISetting);
            if (setting == nullptr || (c.is_bool ? !setting->Is<bool>() : !setting->Is<std::uint32_t>())) {
                if (!c.reported) {
                    c.reported = true;
                    spdlog::info("[Settings] {} was not found, or not of its kind (cheaper drawing)", c.name);
                }
                continue;
            }
            // (A u setting read and written as a uint32_t: the setting's type is checked against its name's first letter.)
            const uint32_t current = c.is_bool ? (setting->GetValue<bool>(false) ? 1u : 0u) : setting->GetValue<std::uint32_t>(0u);
            if (!c.saved) {
                c.saved = true;
                c.own   = current;
                spdlog::info("[Settings] {} is {} (the game's own, kept for when cheaper drawing is off)", c.name, current);
            }
            // (The cascades: shadows in cascade i while i < the value, so cheaper is never more than the game's own, fix138's review.)
            const uint32_t wanted = !on ? c.own : c.is_bool ? c.value : std::min(c.own, c.value);
            if (current != wanted) {
                const bool ok = c.is_bool ? setting->SetValue(wanted != 0) : setting->SetValue(static_cast<std::uint32_t>(wanted));
                spdlog::info("[Settings] {} {} -> {} ({}, cheaper drawing {}, {})", c.name, current, wanted, ok ? "set" : "failed", on ? "on" : "off", why);
            }
        }
    }

    struct Wanted { const char* name; T type; float value; };
    const Wanted wanted[] = {
        {"fWideAspectLimit:Display", T::kINISetting, aspect},
        {"fNarrowAspectLimit:Display", T::kINISetting, aspect},
        {"fFPWorldFOV:Camera", T::kINIPrefSetting, world_fov},
        {"fTPWorldFOV:Camera", T::kINIPrefSetting, fov},
        {"fTPShipFOV:Camera", T::kINIPrefSetting, fov},
        {"fFPGeometryFOV:Camera", T::kINISetting, weapon_fov},
        // Objects look smaller at the headset's wide FOV, so the game picks lower-detail models
        // (the blocky gun). This makes it judge sizes as bigger, so it picks detailed ones again.
        {"fScreenSpaceSizeMultiplier:LOD", T::kINISetting, std::clamp(ModConstants::detailBoost, 0.5f, 8.0f)},
    };

    // Force the most detailed model for every mesh. The game judges the hand-held gun as tiny at
    // the headset's FOV, so it draws its lowest-detail model and dithers it out (see-through
    // patches). Forcing level 0 keeps it solid and detailed; -1 is the game's normal choice.
    {
        const int wanted_lod = ModConstants::forceFullDetail ? 0 : -1;
        const int current = settings->get_setting("iForcedMeshLODIndex:General", T::kINISetting, wanted_lod);
        if (current != wanted_lod) {
            const bool ok = settings->set_setting("iForcedMeshLODIndex:General", T::kINISetting, wanted_lod);
            spdlog::info("[Settings] iForcedMeshLODIndex:General {} -> {} ({}, {})", current, wanted_lod, ok ? "set" : "NOT FOUND", why);
        }
    }

    for (const auto& w : wanted) {
        const float current = settings->get_setting(w.name, w.type, -1.0f);
        if (std::abs(current - w.value) > 0.01f) {
            const bool ok = settings->set_setting(w.name, w.type, w.value);
            spdlog::info("[Settings] {} {} -> {:.2f} ({}, {})", w.name, current, w.value, ok ? "set" : "NOT FOUND", why);
        }
    }
}

void GameSettingsComponent::on_frame()
{
    // (Foveated shading's settings handed over each frame: cheap, two numbers.)
    Foveation::SetOptions(m_foveation->value(), m_foveation_ring->value());
    ShadowShare::SetOn(m_shadow_share->value());
    EyeShare::SetOn(m_eye_share->value());
    EyeShare::Log();
    GpuTimer::SetOn(m_gpu_timer->value());
    MeleeThrow::SetAimAtPeople(m_throw_aim->value());
    RenderSteps::SetOn(m_render_steps_test->value()); // (fix173)
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(2)) {
        last = now;
        apply_vr_settings("periodic check");
    }
}


void GameSettingsComponent::on_device_reset()
{
    spdlog::info("GameSettingsComponent::on_device_reset");
    apply_vr_settings("device reset");
}

void GameSettingsComponent::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }
    ImGui::Separator();
    m_foveation->draw("Foveated shading (what the lens does not show shaded coarsely, its outer ring at half detail)");
    m_foveation_ring->draw("Foveated shading: the ring starts at (share of the way to the lens edge)");
    m_shadow_share->draw("Full stereo: the right eye uses the left eye's sun shadows (faster)");
    m_eye_share->draw("Full stereo: the right eye reuses the left eye's sky table, fog map and terrain blur (test only: their memory is reused by the game)");
    ImGui::TextUnformatted(EyeShare::Status().c_str());
    m_gpu_timer->draw("GPU timer (one frame in 31 measured, written to the log)");
    m_throw_aim->draw("Throwing: lock onto the person your hand points at (a buzz), the throw flies at them");
    m_render_steps_test->draw("Test: the render steps of one frame behind the first see-through menu (log, menu_steps.jpg; stalls that menu a moment)");
    if (m_cheaper_drawing->draw("Cheaper drawing (clouds at half resolution, grass and rock shadows near you only)")) {
        apply_vr_settings("switched");
    }
    auto creation_engine_settings = CreationEngineSettings::Get();
    {
        m_override_weapon_fov->draw("Override Weapon Fov");
        if (m_override_weapon_fov->value()) {
            ImGui::SameLine();
            if(m_weapon_fov->draw("Weapon Fov")) {
                creation_engine_settings->set_setting("fFPGeometryFOV:Camera", CreationEngineSettings::SettingType::kINISetting, m_weapon_fov->value());
            }
        }
    }

#ifdef _DEBUG
    {
        //"fNarrowAspectLimit:Display"
        static float wideAspectLimit = creation_engine_settings->get_setting("fWideAspectLimit:Display", CreationEngineSettings::SettingType::kINISetting, 1.5f);
        if (ImGui::SliderFloat("fWideAspectLimit:Display", &wideAspectLimit, 0.8f, 1.5f)) {
            creation_engine_settings->set_setting("fWideAspectLimit:Display", CreationEngineSettings::SettingType::kINISetting, wideAspectLimit);
        }
    }
    {
        static int iSizeW = creation_engine_settings->get_setting("iSize W:Display", CreationEngineSettings::SettingType::kINIPrefSetting, 1920);
        if(ImGui::InputInt("iSize W:Display", &iSizeW)) {
            creation_engine_settings->set_setting("iSize W:Display", CreationEngineSettings::SettingType::kINIPrefSetting, iSizeW);
        }
    }
    {
        static bool scan = false;
        if(ImGui::Checkbox("Scan Options", &scan)) {
            if(scan) {
                CreationEngineSettings::get_setting("test", CreationEngineSettings::SettingType::kGameSetting);
                CreationEngineSettings::get_setting("test", CreationEngineSettings::SettingType::kRendererQualitySetting);
                CreationEngineSettings::get_setting("test", CreationEngineSettings::SettingType::kRendererPerfSetting);
                CreationEngineSettings::get_setting("test", CreationEngineSettings::SettingType::kINISetting);
                CreationEngineSettings::get_setting("test", CreationEngineSettings::SettingType::kINIPrefSetting);
            }
        }
    }
#endif
}

void GameSettingsComponent::on_config_load(const utility::Config& cfg, bool set_default) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_default);
    }
    if(m_override_weapon_fov->value()) {
        CreationEngineSettings::Get()->set_setting("fFPGeometryFOV:Camera", CreationEngineSettings::SettingType::kINISetting, m_weapon_fov->value());
    }
}

void GameSettingsComponent::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
