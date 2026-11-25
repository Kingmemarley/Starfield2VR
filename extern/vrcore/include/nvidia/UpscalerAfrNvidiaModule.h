// vrcore - per-eye DLSS for alternate-eye rendering (the game draws the left eye on one frame and the right eye on the next).
//
// DLSS keeps its history (the frames it builds each new one from) inside NVIDIA's library, one per Streamline "viewport" id.
// The game uses one id, so with alternating eyes each eye's picture was built on top of the other eye's: ghosting and shimmer
// between the eyes. This module hooks Streamline's interface (sl.interposer.dll's exports and DLSS's set-options function) and
// sends the right eye's frames to a second viewport id, so each eye gets its own DLSS instance and history:
// - slSetConstants / slSetTagForFrame / slEvaluateFeature for a right-eye frame (by the frame token's index, the same index
//   the eyes are assigned by) go to the second id;
// - the game's DLSS options and resource tags (slSetTag, which has no frame) are given to both ids, and resources freed or
//   allocated for the game's id are for the second one too;
// - a history reset the game asks for in one eye is passed on to the other eye's next frame, and both are reset when
//   per-eye DLSS turns on or off.
// The game's motion vectors must then point to the same eye's previous frame, which starfield2vr's "Nvidia DLSS and TAA Fix"
// does (it gives each eye its own previous camera); without it, per-eye DLSS stays off.
#pragma once

#include <memory>

#include "Mod.hpp"

class UpscalerAfrNvidiaModule : public Mod {
public:
    static std::shared_ptr<UpscalerAfrNvidiaModule>& Get() {
        static auto inst = std::make_shared<UpscalerAfrNvidiaModule>();
        return inst;
    }

    // Hooks Streamline as soon as it is loaded (from a short-lived thread if it is not loaded yet).
    UpscalerAfrNvidiaModule();

    [[nodiscard]] std::string_view get_name() const override { return "UpscalerAfr"; }

    std::optional<std::string> on_initialize() override;
    void on_present() override;
    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    // Whether the game gives each eye its own previous camera (motion vectors to the same eye's previous frame).
    static void set_history_fix(bool on);

    // Behind a see-through menu the game draws several views in one frame (the world, then the menu's 3D views: you, the planet,
    // the item), each with its own DLSS step, all with the game's one viewport id: each view's picture would be built on the
    // others'. With views active, the calls of a menu's view go to an id of its own (the game's + 0x10000 x its place), so each
    // view has its own DLSS instance and history; the world keeps the game's id (and its right eye's, per eye).
    // This thread's DLSS calls are for this view: 0 the world's (or the only one), 1..3 a menu's view, -1 not known (set by the
    // game side around the steps that make them: its scene setup, which sets DLSS's constants, and its DLSS step).
    // Returns the view it had (put back after the step: a step may run another view's inline).
    static int set_view(int view);
    // Each view with its own DLSS instance from now on, or no longer (the game side: behind a see-through menu, its views known).
    static void set_views_active(bool on);
    // A menu's views can have their own DLSS instances (Streamline hooked, the game's DLSS options seen, the setting on, not failed).
    static bool views_ready();
    // Full stereo can give each eye its own DLSS instance: the right eye's can be used (as in alternate frames) and the game evaluated
    // DLSS in the last second (not another upscaler).
    static bool eyes_ready();
    // Both eyes' DLSS start over at their next `frames` frames (their history dropped), as at a cut: something very bright that has just
    // appeared (the gravity gloves' glow) shows at full at once instead of being blended in over many frames. Any thread.
    static void request_reset(const char* why, int frames = 1);
    // (fix165) The right eye's instance not needed now (mono, video memory over its budget): let go once (its video memory with it); DLSS
    // makes it again at its first evaluation when two pictures come back. Any thread. (fix173: `menu`: because a menu is shown in mono, from
    // its opening on, not 3 s over the budget first; for the log.)
    static void set_right_unneeded(bool unneeded, bool menu = false);
    // (fix179) The time the Streamline DLSS calls took (all threads, through these hooks) since the last call of this: their number, their
    // total and the longest (microseconds); and, apart, the present thread's own check for instances to let go (its total and longest: mostly
    // its wait for the DLSS lock while a game thread's call holds it). For the menus' slow frames (D3D12Component::note_menu_frame, once a
    // present). Any thread.
    static void take_call_time(uint32_t& calls, int64_t& total_us, int64_t& longest_us, int64_t& check_us, int64_t& check_longest_us);

private:
    const ModToggle::Ptr m_per_eye{ModToggle::create(generate_name("PerEyeDlss"), true)};
    // (Off by default: in fix124's test the game, its setting cleared, dropped TAA behind the menu without starting DLSS: no
    // anti-aliasing there at all. A new name, so the "on" saved by fix124 is not used.)
    const ModToggle::Ptr m_per_view{ModToggle::create(generate_name("PerViewDlssBehindMenus"), false)};
    // (fix153) DLSS 4's transformer model (preset K) for every mode, when the DLSS the game loaded is 310 or newer (the game's own is 3.5:
    // its presets are the old CNN ones; a newer DLSS comes from the NVIDIA app's override or a newer nvngx_dlss.dll).
    // (fix168) Which DLSS model (a choice in F11, starting on K; fix167's DLSS 4.5 cost 3.4 ms an eye against 1.0 for the
    // game's own, full stereo ~29 -> ~24 fps): 0 the game's own presets, 1 the older CNN model (preset E, from 310.6), 2 DLSS 4's
    // transformer (K) in every mode, 3 DLSS 4.5 as NVIDIA recommends (M performance, L ultra performance, K the others). A new name: the
    // old on/off (DlssTransformerModel) is not read.
    const ModCombo::Ptr m_model{ModCombo::create(generate_name("DlssModel"),
        {"The game's own (no change)", "Fastest: the older CNN model (preset E)", "DLSS 4: transformer K (about 1.3 ms more an eye)",
            "DLSS 4.5: NVIDIA's picks (M in performance: about 2.4 ms more an eye)"},
        2)};
};
