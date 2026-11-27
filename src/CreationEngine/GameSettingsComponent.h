#pragma once
#include <Mod.hpp>

class GameSettingsComponent : public Mod
{
public:

    inline static std::shared_ptr<GameSettingsComponent>& Get()
    {
        static std::shared_ptr<GameSettingsComponent> instance{ std::make_shared<GameSettingsComponent>() };
        return instance;
    }

    [[nodiscard]] inline std::string_view get_name() const override { return "StarFieldSettings"; }

    std::optional<std::string> on_initialize() override;
    void                       on_draw_advanced_ui(int part) override; // (fix196: throwing, drawing for performance)
    void                       on_draw_debug_ui() override;            // (fix196: the drawing tests)
    void                       on_config_load(const utility::Config& cfg, bool set_default) override;
    void                       on_config_save(utility::Config& cfg) override;
    void                       on_frame() override;
    void                       on_device_reset() override;
    std::optional<std::string> on_initialize_d3d_thread() override;

private:
    void apply_vr_settings(const char* why);
    const ModSlider::Ptr m_weapon_fov{ ModSlider::create(generate_name("WeaponFov"), 10.0, 179.0, 120.0) };
    const ModToggle::Ptr m_override_weapon_fov{ ModToggle::create(generate_name("OverrideWeaponFov"), false) };
    // Cheaper drawing (fix138): clouds at half resolution, grass and rock shadows in the nearer cascades only (apply_vr_settings).
    const ModToggle::Ptr m_cheaper_drawing{ ModToggle::create(generate_name("CheaperDrawing"), true) };
    // Foveated shading (fix139): what each eye's lens does not show at 4x4, its outer ring at 2x2 (Foveation.h); where the ring starts.
    const ModToggle::Ptr m_foveation{ ModToggle::create(generate_name("FoveatedShading"), true) };
    const ModSlider::Ptr m_foveation_ring{ ModSlider::create(generate_name("FoveatedShadingRing"), 0.6f, 1.2f, 0.9f) };
    // (fix140) Full stereo's right eye lit by the left eye's sun shadows (ShadowShare.h); one frame in 30 timed on the GPU (GpuTimer.h).
    const ModToggle::Ptr m_shadow_share{ ModToggle::create(generate_name("ShareSunShadows"), true) };
    // (fix156) Full stereo's right eye given the left eye's results of its eye-identical steps (EyeShare.h).
    const ModToggle::Ptr m_eye_share{ ModToggle::create(generate_name("ShareEyeStepsV2"), false) }; // (off: the review; EyeShare.h)
    const ModToggle::Ptr m_gpu_timer{ ModToggle::create(generate_name("GpuTimer"), true) };
    // (fix141) A thrown melee weapon flies at the person you point at (MeleeThrow).
    const ModToggle::Ptr m_throw_aim{ ModToggle::create(generate_name("ThrowAimAtPeople"), true) };
    // (fix173) The render steps test (RenderSteps.h): off unless asked for (it stalled the frames as menus were used).
    const ModToggle::Ptr m_render_steps_test{ ModToggle::create(generate_name("RenderStepsTest"), false) };
    ValueList m_options{ *m_weapon_fov, *m_override_weapon_fov, *m_cheaper_drawing, *m_foveation, *m_foveation_ring, *m_shadow_share, *m_eye_share, *m_gpu_timer, *m_throw_aim,
        *m_render_steps_test };
};
