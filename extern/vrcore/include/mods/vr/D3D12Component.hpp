#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <wrl.h>

#include <GraphicsMemory.h>
#include <SpriteBatch.h>

#include "d3d12/ResourceCopier.hpp"
#include "d3d12/TextureContext.hpp"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openvr.h>

#include "runtimes/OpenXR.hpp" // (fix171: the favorites wheel's picture)

class VR;

namespace vrmod {
class D3D12Component {
public:
    vr::EVRCompositorError on_frame(VR* vr);
    void on_post_present(VR* vr);

    void on_reset(VR* vr);
    // (fix165) The card's video memory, sampled once a second on the present thread: what the game uses and the budget Windows gives it
    // (MB), and since when (ms, steady clock) it has been over that budget (0: not over). Over it, Windows moves the game's data out to
    // system memory and frames slow down many times (a pilot's seat at about 10 fps in testing; a helper's research).
    std::atomic<uint32_t> m_mem_used_mb{0}, m_mem_budget_mb{0};
    std::atomic<int64_t>  m_mem_over_since_ms{0};
    // (fix189) One more each time those two are sampled (after both are set): the game side can tell what it made since the last sample
    // (ViewHistory: the world's own pictures kept behind menus, refused near the budget).
    std::atomic<uint32_t> m_mem_samples{0};
    bool memory_over_for(int64_t ms) const;
    void sample_memory();
    // (fix179) The interface's full-screen movie being drawn (the game side: GameFlow::renderMenu, on the game's interface threads; not the
    // cursor's): the last few kept, so the menu panel's lines for a menu's opening and closing can name the menu. Any thread.
    void note_menu_movie(uint32_t hash, std::string_view name);
    // (fix163) A menu closed and its pictures not let go yet (VR holds mono meanwhile, with full stereo to come back to).
    bool menu_release_pending() const { return m_menu_release_pending.load(std::memory_order_relaxed); }
    std::atomic<bool> m_menu_release_pending{false};

    void force_reset() { m_force_reset = true; }

    const auto& get_backbuffer_size() const { return m_backbuffer_size; }

    auto is_initialized() const { return m_openvr.left_eye_tex[0].texture != nullptr; }

    auto& openxr() { return m_openxr; }

    // See-through menus (the menu panel). Starfield draws its menus and HUD (Scaleform) on a picture of their own, see-through
    // where nothing is drawn (premultiplied alpha, in gamma space), and lays it over the world in one pass of its own
    // (CreationRendererPrivate::ScaleformCompositeRenderPass; the Luma HDR mod replaces that pass's shader and documents it:
    // github.com/EndlesslyFlowering/Starfield-Luma). The game side (MenuLayer) gives that picture here from within that pass, on
    // the game's own commands (layer_state: the state it is in there); it is copied, and the panel shows it over the world kept
    // behind the menu instead of the game's whole picture. Any thread. 1: copied; 0: not this time (a new copy is made at most
    // every two seconds); -1: this layer cannot be used.
    int record_menu_layer(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state);
    // No see-through menu any more (the game side, when that changes): the last copy is not shown again.
    void drop_menu_layer();
    // (fix145) The conversation cut out of the game's interface layer (the game side: MenuLayer, on the game's own commands just before it
    // lays the layer over its picture): the part its panel shows copied out (copy, in the layer's pixels), the conversation's whole box
    // cleared in the layer (clear: made see-through, so neither eye's picture has it), the layer left in its state. Any thread. 1 done,
    // 0 not this time, -1 refused (a layer it cannot be done with: logged once).
    int record_conversation(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state,
        const D3D12_BOX& copy, const D3D12_BOX& clear);
    // (fix177) Build mode's console (the game side: BuildConsole, through MenuLayer, on the game's own commands just before the game lays its
    // interface layer over its picture): the parts of the build menu and its notifications copied out of the layer into one picture of their
    // own (each part `from`, in the layer's pixels, to its place `to` in the picture, packed side by side), then the boxes those movies were
    // drawn in cleared in the layer (made see-through: neither eye's picture has them), the layer left in its state. Any thread. 1 done,
    // 0 not this time, -1 refused (a layer it cannot be done with: logged once). snap: this opening's picture saved for the log, a few
    // frames on (the present thread: workshop_cut.png).
    struct ConsoleCut {
        static constexpr int kMax = 12;
        uint32_t size[2]{0, 0}; // the picture (its copy and swapchain): width, height
        int parts{0};
        D3D12_BOX from[kMax]{};
        uint32_t to[kMax][2]{};
        int clears{0};
        D3D12_RECT clear[3]{};
        bool snap{false};
    };
    int record_console(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer, D3D12_RESOURCE_STATES layer_state, const ConsoleCut& cut);

    // Render-steps test (the game side: RenderSteps, once, while a see-through menu is open): a copy of a picture one step of the
    // game's frame drew, made on the game's own commands right after that step (the picture is in `state` there and is left so).
    // Any thread. 1: copied; 0: not (too many already, a kind of picture not kept, not a plain 2D picture); -1: failed.
    int record_step_snapshot(ID3D12GraphicsCommandList* command_list, ID3D12Resource* picture, D3D12_RESOURCE_STATES state, const std::string& label);
    // All the copies are recorded: a few frames on (their frame surely run), they are put together into one picture saved for the
    // log (menu_steps.jpg: a grid, each cell's step in the log) and let go.
    void finish_step_snapshots();

    // The world as it is now behind a see-through menu, and that menu's 3D pictures (the game side: LiveWorld, which keeps the game
    // drawing the world behind these menus): a picture the last steps of one of the game's views drew, copied on the game's own
    // commands right after that step (the picture is in `state` there and is left so). kind 0: the world's (the eye of `frame`);
    // 1..3: the menu's 3D pictures, in their order. frame: the index of the frame being drawn (which eye, and which present shows
    // it). Any thread. 1: copied; 0: not (not a kind of picture kept, not a plain 2D picture); -1: failed.
    // (fix147) eye_sized: full stereo's left eye (kind 0 with no see-through menu wanting it): only a picture of the eye image's size
    // is taken (the game's finished picture, as the right eye's); one of another size is refused and leaves its slot as it is. A slot
    // of another size is made again at the eye image's size at once (not after the half-second wait between remakes).
    int record_live_picture(ID3D12GraphicsCommandList* command_list, ID3D12Resource* picture, D3D12_RESOURCE_STATES state, int kind, uint32_t frame,
        bool eye_sized = false);
    // One of the menu's 3D pictures' depth (kind 1..3, as record_live_picture's), copied on the game's own commands right after a step
    // that copied from it (it is in the copy-source state there, and is left so: no change of the game's picture's state). The panel
    // then cuts the 3D picture out by its depth rather than by its background's colour. Any thread. 1: copied; 0: not (not a depth
    // picture kept); -1: failed.
    int record_live_depth(ID3D12GraphicsCommandList* command_list, ID3D12Resource* depth, int kind, uint32_t frame);

private:
    void setup();
    void setup_sprite_batch_pso(DXGI_FORMAT output_format);
    // (dest: where in dst, the whole of it when null; source: the part of src drawn, the whole of it when null. batch: the sprite
    // batch drawing it, the plain one when null; color: given to
    // its pixel shader, white for the plain one.)
    void render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state, const RECT* dest = nullptr,
        DirectX::DX12::SpriteBatch* batch = nullptr, const float* color = nullptr, const RECT* source = nullptr);

    // Sprite batches drawing into an 8-bit picture with this mod's own pixel shaders (compiled when first needed, with the
    // system's shader compiler, d3dcompiler_47.dll): a menu's 3D picture with its even background taken out (keyed: the colour of
    // its top corners), and a menu's interface with a dark backing under its faint parts. Null when they could not be made.
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_keyed_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_laser_batch{}; // (the gun's laser sight's: reads depths into a small picture, LaserSight)
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_backing_batch{};
    // A menu's 3D picture cut out by its depth (record_live_depth): its pixel shader reads two pictures, the colour (t0) and the
    // depth (t1), side by side in one table of its own root signature (the sprite batch binds the table's start: the colour's view,
    // the depth's right after it). Null when it could not be made.
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_depth_keyed_batch{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_depth_keyed_root{};
    // (Present thread.) The views of each kind's and parity's colour and depth copies, in pairs; and the copies they are of (held:
    // not let go while a view of them is in the table).
    std::unique_ptr<DirectX::DescriptorHeap> m_depth_pairs{};
    std::array<std::array<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, Microsoft::WRL::ComPtr<ID3D12Resource>>, 2>, 4> m_depth_pairs_of{};
    // Draws a menu's 3D picture cut out by its depth (the pair of views at the table's start) into dst (in dst_state, left so).
    void render_pair_to_rtv(ID3D12GraphicsCommandList* command_list, D3D12_GPU_DESCRIPTOR_HANDLE pair, uint32_t width, uint32_t height,
        const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES dst_state, const RECT& dest, const float* color);
    // The menu drawn into the live world's eye pictures (OpenXR::menu_draw_params): its own shaders; its picture's views (made again
    // when the panel's picture is made anew).
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_panel_ray_batch{};
    d3d12::TextureContext m_panel_ray_source{};
    ID3D12Resource* m_panel_ray_source_of{nullptr};
    bool draw_menu_into(ID3D12GraphicsCommandList* list, d3d12::TextureContext& eye, VR* vr, bool mono, bool left);
    // (fix164) The soft light round the panel (present_ambient): its three passes' own pixel shaders (null when they could not be made: the
    // light stays off). The game's picture's shown rows made small (into 64x36, linear light), that kept smoothly over time (two pictures
    // in turn), and the light's own image drawn from it (with the menu-drawn-in batch's vertex shader: the numbers in its "transform").
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ambient_down_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ambient_smooth_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ambient_light_batch{};
    bool m_custom_batches_tried{false};
    void ensure_custom_batches();

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    d3d12::TextureContext m_backbuffer_copy{};
    d3d12::TextureContext m_converted_eye_tex{};
    std::array<d3d12::ResourceCopier, 3> m_generic_copiers{};

    std::unique_ptr<DirectX::DX12::SpriteBatch> m_sprite_batch{};

    // Mimicking what OpenXR does.
    struct OpenVR {
        d3d12::TextureContext& get_left() {
            auto& ctx = this->left_eye_tex[this->texture_counter % left_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& get_right() {
            auto& ctx = this->right_eye_tex[this->texture_counter % right_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& acquire_left() {
            auto& ctx = get_left();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        d3d12::TextureContext& acquire_right() {
            auto& ctx = get_right();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        void copy_left(ID3D12Resource* src) {
            auto& ctx = this->acquire_left();
            ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src) {
            auto& ctx = this->acquire_right();
            ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        std::array<d3d12::TextureContext, 3> left_eye_tex{};
        std::array<d3d12::TextureContext, 3> right_eye_tex{};
        uint32_t texture_counter{0};
    } m_openvr;

    struct OpenXR {
        void initialize(XrSessionCreateInfo& session_info);
        // width/height of 0 = the runtime's recommended size
        std::optional<std::string> create_swapchains(uint32_t width = 0, uint32_t height = 0);
        void destroy_swapchains();
        // Into the eye image swapchain_idx (resized to the source's size first); the source is in src_state before and after.
        // True when the image was filled and handed back. frustum: the one the picture was drawn with (picture_frustum), submitted with
        // it (fix138).
        // (fix148) beam: the gun laser's beam drawn into the image after the copy (LaserSight::draw_beam): the picture's frame, and for
        // full stereo's right eye its camera's distance from the left eye's along the camera's right (the game's units).
        struct Beam {
            bool on{false};
            uint32_t frame{0};
            bool from_left{false};
            float eyes_apart{0.0f};
        };
        bool copy(uint32_t swapchain_idx, ID3D12Resource* src, const Vector4f& frustum, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
            const Beam& beam = Beam{});
        void wait_for_all_copies() {
            std::scoped_lock _{this->mtx};

            for (auto& ctx : this->contexts) {
                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }
            }
        }

        // Gun sight image: made once, the first time it is shown. A setup during which the game stopped last time keeps it
        // off (blocked) until reticle_retry.
        void ensure_reticle();
        void destroy_reticle();
        bool reticle_is_blocked();
        void reticle_retry();
        std::vector<std::unique_ptr<d3d12::TextureContext>> reticle_contexts{};
        bool reticle_failed{false};
        std::atomic<bool> reticle_blocked{false}; // (read by the menu without the lock)
        bool reticle_marker_checked{false};

        // The menu panel's own swapchain (OpenXR::panel_swapchain), made the size of the game's picture the first time it is
        // needed and again when that size changes, filled with the picture like the eye images (a copy, nothing drawn).
        bool copy_panel(ID3D12Resource* src);
        void destroy_panel();
        // (fix145) The conversation's panel's own swapchain (OpenXR::conv_swapchain), the size of the part cut out (record_conversation),
        // filled with it each frame it came (a copy, nothing drawn).
        bool copy_conversation(ID3D12Resource* src);
        void destroy_conversation();
        // (fix164) The soft light's own swapchain behind the panel (OpenXR::ambient_swapchain), the size of its image (present_ambient), made the
        // first time it is needed, filled with it each frame it is drawn (a copy, as the panel's).
        bool copy_ambient(ID3D12Resource* src);
        void destroy_ambient();
        // (fix171) The favorites wheel's own swapchain (OpenXR::wheel_swapchain), made the size of its picture the first time one comes,
        // filled with each new picture from an upload buffer of its own (the gun sight's way: the pixels made on the processor by the game
        // side, copied in; nothing drawn on the GPU), on commands of its own. Let go a while after the wheel was last open (present_wheel).
        bool copy_wheel(const runtimes::OpenXR::WheelPicture& picture);
        void destroy_wheel();
        // (fix177) Build mode's console's own swapchain (OpenXR::console_swapchain), the size of its picture (record_console), filled with it
        // each frame it came (a copy, nothing drawn, as the conversation's). Let go a while after build mode (present_console).
        bool copy_console(ID3D12Resource* src);
        void destroy_console();
        std::vector<XrSwapchainImageD3D12KHR> wheel_textures{};
        d3d12::CommandContext wheel_commands{};
        bool wheel_commands_ready{false};
        Microsoft::WRL::ComPtr<ID3D12Resource> wheel_upload{};
        uint8_t* wheel_upload_mapped{nullptr};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT wheel_footprint{};
        bool wheel_bgra{false};
        std::array<uint32_t, 2> wheel_resolution{};
        uint64_t wheel_uploaded_serial{0}; // (the picture its last copy was of)
        std::chrono::steady_clock::time_point wheel_failed_time{};
        std::chrono::steady_clock::time_point wheel_last_wanted{};

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
            bool     wait_owed{false}; // (fix189) an image acquired whose wait failed: waited for and given back at the next copy's start
        };

        std::vector<SwapchainContext> contexts{};
        SwapchainContext panel_context{};
        std::array<uint32_t, 2> panel_resolution{};
        std::array<uint32_t, 2> panel_failed_resolution{}; // could not be made at this size: tried again after a few seconds
        std::chrono::steady_clock::time_point panel_failed_time{};
        SwapchainContext conv_context{};
        std::array<uint32_t, 2> conv_resolution{};
        std::array<uint32_t, 2> conv_failed_resolution{};
        std::chrono::steady_clock::time_point conv_failed_time{};
        SwapchainContext console_context{}; // (fix177)
        std::array<uint32_t, 2> console_resolution{};
        std::array<uint32_t, 2> console_failed_resolution{};
        std::chrono::steady_clock::time_point console_failed_time{};
        SwapchainContext ambient_context{}; // (fix164)
        std::array<uint32_t, 2> ambient_resolution{};
        std::chrono::steady_clock::time_point ambient_failed_time{}; // (could not be made: tried again a few seconds on)
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
        std::array<uint32_t, 2> failed_resolution{}; // swapchains of this size could not be made: tried again after a few seconds,
        std::chrono::steady_clock::time_point failed_time{}; // not every frame
    } m_openxr;

    // The world behind a menu's panel: the last few world pictures (both eyes', in m_openvr's textures, unused with OpenXR:
    // slot i is left_eye_tex[i] and right_eye_tex[i]) with the views they were submitted with. When a menu opens, the one
    // before the newest is shown (the newest may already have the menu in it); nothing is written while the panel is up.
    struct WorldHistory {
        int slot{0};                                    // being written
        std::array<uint8_t, 3> written{};               // bit 0 left, bit 1 right (both: a mono picture)
        std::array<bool, 3> mono{};                     // one picture for both eyes (in the left texture)
        std::array<uint64_t, 3> order{};                // when it was completed (0: not a whole picture)
        std::array<std::array<XrPosef, 2>, 3> pose{};
        std::array<std::array<XrFovf, 2>, 3> fov{};
        uint64_t next_order{1};
        int frozen{-1};                                 // the slot shown behind the panel
        bool panel_was_up{false};
        bool world_allowed_seen{false};                 // (fix192) a world allowed behind the panel at some time since it came up
        bool load_seen{false};                          // (fix192) a loading screen on the panel at some time since it came up
        void clear() {
            written = {};
            mono = {};
            order = {};
            frozen = -1;
        }
    } m_world_history{};
    void write_world_history(ID3D12Resource* eye_texture, bool left, bool mono);
    // (fix138) The frustum a picture was drawn with: mono's (one picture for both eyes) or eye's own (0 left, 1 right).
    static Vector4f picture_frustum(bool mono, int eye);

    // A copy the game's frames wrote into, let go: kept until the GPU is surely done with it (copy_done).
    struct RetiredCopy {
        ComPtr<ID3D12Resource> resource{};
        std::chrono::steady_clock::time_point at{};
        uint32_t presents{0}; // (VR::m_presents_done when it was let go)
    };
    static uint32_t presents_now();
    static bool copy_done(const RetiredCopy& r, std::chrono::steady_clock::time_point now);
    // (fix181) Pictures let go without the present thread waiting for the graphics card: their objects (the picture, its views, its own
    // command list) kept here with the fences of every command list that may still use them (its own, the eye images', the panel's, the
    // headset copies'), and freed once the graphics card has passed them all (checked once a present, never waited for), or after 10 s.
    // (fix180's test: the left eye's image let go 1 s into the Data menu waited 490 ms for every copy queued, the game held up with it.)
    // (fix181, the review) The point to pass: a fence of the mod's own signalled on the command queue as it is let go, after everything
    // given to the queue so far (every command list of the mod's runs on it, in order: its own, the eye images', the panel's, the headset
    // copies', the world-history copies'). The lists' own fences are kept too, only for when that signal could not be given.
    struct RetiredGpu {
        std::vector<Microsoft::WRL::ComPtr<IUnknown>> objects{};
        std::vector<std::unique_ptr<DirectX::DescriptorHeap>> heaps{};
        std::vector<HANDLE> events{};
        std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Fence>, UINT64>> fences{};
        UINT64 queue_value{0}; // (m_retire_fence's value signalled for it; 0: not signalled)
        std::chrono::steady_clock::time_point at{};
        const char* what{""};
        bool told_late{false}; // (logged once: not passed in 10 s)
    };
    std::vector<RetiredGpu> m_retired_gpu{};
    Microsoft::WRL::ComPtr<ID3D12Fence> m_retire_fence{};
    UINT64 m_retire_value{0};
    int m_retired_logs{0};
    // A command list's fence as it is now (all it was given so far): for when the queue's signal could not be given.
    static void note_fence(RetiredGpu& r, d3d12::CommandContext& c);
    // The picture's objects moved into r (its own command list's fence noted): it is left empty, as after reset(), with no wait.
    static void retire_texture(RetiredGpu& r, d3d12::TextureContext& t);
    // The headset copies' fences (likewise only for when the queue's signal could not be given).
    void note_copy_fences(RetiredGpu& r);
    void retire(RetiredGpu&& r);
    bool retired_passed(const RetiredGpu& r) const;
    // Those the graphics card has passed, freed: once a present (never waited for). drain: the graphics card waited for first (up to 5 s:
    // before the drawing tools the retired lists used are made again, setup and on_reset).
    void free_retired(bool drain = false);

    // The game's interface layer (record_menu_layer) and the panel's see-through picture made from it (compose_menu_layer).
    struct MenuLayer {
        std::mutex mtx{};
        // Copies of the layer, one per size and format it comes in (the game draws a menu's layer the size of the menu's box,
        // 1996x1123, and its first frame the picture's: each kept, none made again and again), kept in the common state.
        struct Slot {
            ComPtr<ID3D12Resource> copy{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};    // (a typed one)
            uint32_t width{0};
            uint32_t height{0};
            std::chrono::steady_clock::time_point used{};
            uint64_t copies{0};                      // (a new one is shown from its second copy on: its first has surely run)
        };
        std::array<Slot, 3> slots{};
        int current{-1};                           // the slot last copied into
        uint64_t serial{0};                        // copies made (a new one each frame while a menu is open)
        std::chrono::steady_clock::time_point at{};
        // (fix192) The game frame the current slot's last copy was made in (the render frame's index; the frame presented has the same
        // count): the copy belongs to the picture shown in that frame or the next, whatever the milliseconds between.
        uint32_t frame{0};
        // (fix192) The layer's height as the game drew it in each of the last four frames (by the frame's count, modulo 4: the game may
        // record a frame or two ahead of the one presented), copied or not. The game lays its layer over its picture at the same size: the
        // eye picture's whole height means the frame was kept full size (FullFrame), the menu drawn in its top rows, in the game's own
        // picture too; the menu's box height means the game's own centred 16:9 box.
        struct Seen {
            uint32_t frame{0};
            uint32_t height{0};
            bool has{false};
        };
        std::array<Seen, 4> seen{};
        std::vector<RetiredCopy> retired{}; // (in use on the GPU a moment)
        std::chrono::steady_clock::time_point made_at{};
        uint32_t logged_other_size[2]{};
        bool refused_logged{false};
        // (Present thread.)
        std::array<d3d12::TextureContext, 3> source{}; // views of each slot's copy, made again when it changes
        std::array<ID3D12Resource*, 3> source_of{};
        d3d12::TextureContext composed{};          // the panel's picture: the layer over a see-through dark, 8-bit, the eye image's size
        uint32_t composed_size[2]{};
        bool     tall_logged{false}; // (the layer last seen taller than the menu's box: logged when that changes)
        bool was_used{false};                      // the last panel picture was see-through
        std::chrono::steady_clock::time_point used_since{}; // (since when)
        unsigned shown_serial{0};                            // (ModSettings::menuShownSerial last seen: a menu shown anew)
        std::chrono::steady_clock::time_point shown_since{}; // (since when)
        int previews_shown{0};                     // the menu's 3D pictures laid under the last see-through picture
        int snapshots{0};                          // pictures saved for the log (the first few menus)
        bool panel_snapped{false};                 // (this menu's)
        int mode_logs{0};                          // (this panel's changes between see-through and opaque, logged)
        // (fix173) Why the last compose_menu_layer made no see-through picture (for the log: the layer's copy's age and size, the eye's), and
        // the last one that made one (its picture is shown again for a few frames when one fails, not the game's raw picture: the flash).
        const char* fail_reason{nullptr};
        int64_t fail_age_ms{0};
        uint32_t fail_layer[2]{};
        uint32_t fail_eye[2]{};
        std::chrono::steady_clock::time_point good_at{};
        uint32_t good_presents{0};
        bool good_valid{false};
        uint64_t held{0}, fallbacks{0};            // (frames: the last see-through picture shown again; the game's whole picture shown)
        std::chrono::steady_clock::time_point fallback_logged_at{};
        int fallback_logs{0};
        bool holding{false};
        // (fix192) The last check of the layer's copy: how many game frames it is behind the frame shown (negative: a later frame's,
        // recorded while this one was presented), for the log.
        int32_t fail_behind{0};
        // (fix192) When this present's work began (on_frame): the mod's own work before the copy's check is timed from it, for the log.
        std::chrono::steady_clock::time_point present_started{};
        // (fix192) Since when the game shows a see-through menu (the panel set see-through): until the first see-through picture made from
        // the layer after it, an empty see-through picture (the panel's dark alone) for at most half a second, not the game's own picture;
        // and only a picture of this spell (or made just before it) shown again.
        std::chrono::steady_clock::time_point wanted_since{};
        bool wanted_was{false};
        // (fix192) Since when the see-through picture is possible (that, and a world to show behind the panel): the empty picture's half
        // second counted from here.
        std::chrono::steady_clock::time_point empty_since{};
        bool sees_was{false};
        uint64_t empty{0};                         // (this menu's frames shown with the empty see-through picture)
        // (fix192) This menu's checks of the layer's copy, for the line at the panel going down: pictures made from it; of them, with a copy
        // over 60 ms old (each refused before fix192, the game's own picture shown in its place after 6 frames), and with a copy older than
        // the frame before (used while the game still shows a see-through menu); refused; the oldest used; the most frames behind; the
        // mod's own work on the present thread before the check (its waits for the graphics card among it).
        struct CopyStats {
            uint64_t made{0}, over_60{0}, older_used{0}, refused{0};
            int64_t oldest_ms{0};
            int32_t most_behind{0};
            double before_sum_ms{0.0}, before_max_ms{0.0};
            uint64_t before_n{0};
        } stats{};
        int stats_logs{0};
        // (fix192) A run of frames with the game's own picture on the panel while the game shows a see-through menu: when it began, its
        // frames, those shown with the menu's box from the top rows (the frame kept full size), the oldest copy then; logged at its start
        // and end.
        bool raw_run{false};
        std::chrono::steady_clock::time_point raw_since{};
        uint64_t raw_frames{0}, raw_top{0};
        int64_t raw_oldest_ms{0};
        int raw_logs{0};
    } m_menu_layer{};
    // (fix192) The layer as the game drew it in `frame` (or the one before): its height, if seen. (Under the layer's lock.)
    bool menu_layer_height_of(uint32_t frame, uint32_t& height);
    // (fix192) The panel's empty see-through picture: its dark alone, the eye image's size (no interface, no 3D pictures).
    bool compose_menu_empty(VR* vr, ID3D12Resource* eye_texture);
    // (fix192) The panel's see-through picture made (or made again) at the eye image's size, its last commands done first.
    bool ensure_menu_composed(ID3D12Device* device, const D3D12_RESOURCE_DESC& eye_desc);
    // (fix145) The conversation cut out of the interface layer (record_conversation), and its panel's image filled from it (the present
    // thread: present_conversation).
    struct ConversationCut {
        std::mutex mtx{};
        ComPtr<ID3D12Resource> copy{};  // the part shown on its panel (8-bit, the layer's colours as stored), in the common state
        ComPtr<ID3D12DescriptorHeap> rtv_heap{}; // a view of the layer, to clear the conversation's box in it
        uint32_t copy_size[2]{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint64_t copies{0};                         // (shown from its second on: its first has surely run)
        std::chrono::steady_clock::time_point at{}; // the last cut recorded
        std::chrono::steady_clock::time_point made_at{};
        std::vector<RetiredCopy> retired{};
        bool refused_logged{false};
        int logs{0};
    } m_conversation{};
    void present_conversation(VR* vr);
    // (fix177) Build mode's console: its picture (record_console's parts, packed), and its swapchain filled from it (the present thread:
    // present_console). Its picture and swapchain let go 20 s after the last cut (about 8 MB each: the game is short of video memory).
    struct ConsoleCopy {
        std::mutex mtx{};
        ComPtr<ID3D12Resource> copy{};          // the picture (8-bit, the layer's colours as stored), in the common state
        ComPtr<ID3D12DescriptorHeap> rtv_heap{}; // a view of the layer, to clear the boxes in it
        uint32_t copy_size[2]{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint64_t copies{0};                         // (shown from its second on: its first has surely run)
        uint64_t total_copies{0};
        std::chrono::steady_clock::time_point at{}; // the last cut recorded
        std::chrono::steady_clock::time_point made_at{};
        std::vector<RetiredCopy> retired{};
        bool refused_logged{false};
        int logs{0};
        bool snap_wanted{false};   // (this opening's picture to be saved, from its third copy on)
        int snaps{0};
    } m_console{};
    // (shown: not with the menu panel up; else only let go when old.)
    void present_console(VR* vr, bool shown);
    // (fix171) The favorites wheel's picture (the game side's newest, OpenXR::set_wheel_picture) copied into its swapchain when a new one
    // came while the wheel is open (the present thread, as the eye images were just filled); nothing while it is closed.
    void present_wheel(VR* vr);

    // (fix164) The soft light round the panel while nothing is behind it (OpenXR::panel_ambient), all on the graphics card, nothing read
    // back: each frame the panel's shown rows of the game's picture made small (now: 64x36, linear light), that kept smoothly over time
    // (kept[current]: out = kept + (now - kept) x (1 - e^(-dt/tau)), tau 0.4 s, 0.12 s at a cut; started anew each time the light comes
    // on), and the light's image drawn from it (light: 512x256, 8-bit, its colours stored as sRGB, as the eye images are), copied into its
    // own swapchain. The present thread. Its pictures rest in the common state; its views: [0] now, [1] kept[0], [2] now, [3] kept[1] (a
    // pair of views for each way round: what is now and what is kept), [4] the game's picture, or its shown rows' copy (band) when the
    // picture cannot be read by a shader (written each frame, its last commands done first).
    struct AmbientLight {
        d3d12::CommandContext commands{};
        ComPtr<ID3D12Resource> now{};
        std::array<ComPtr<ID3D12Resource>, 2> kept{};
        ComPtr<ID3D12Resource> light{};
        // (The game's picture's shown rows copied, when it cannot be read by a shader itself: the swap chain's own picture.)
        ComPtr<ID3D12Resource> band{};
        uint32_t band_size[2]{};
        DXGI_FORMAT band_format{DXGI_FORMAT_UNKNOWN};
        std::unique_ptr<DirectX::DescriptorHeap> views{};   // (shader-visible)
        std::unique_ptr<DirectX::DescriptorHeap> targets{}; // (render target views: now, kept[0], kept[1], light)
        bool made{false};
        bool failed{false};    // (could not be made: off until the next reset)
        int  current{0};       // the kept picture last written
        bool history{false};   // the kept pictures hold the light of the frames before (else started anew from this frame's)
        bool was_on{false};    // (shown last frame)
        std::chrono::steady_clock::time_point last_on_at{}; // (fix164, the review: when it was last shown; a gap of a frame or two is no restart)
        std::chrono::steady_clock::time_point on_since{}; // (it came on: faded in over 0.4 s from then)
        std::chrono::steady_clock::time_point last_at{};  // (the last frame drawn: dt)
        int  logs{0};
        bool refused_logged{false};
        bool loading_snapped{false}; // (a loading screen's picture saved for the log, once a session)
        bool was_loading{false};     // (a loading screen last frame, and since when one has been)
        std::chrono::steady_clock::time_point loading_since{};
    } m_ambient{};
    // Draws its image for this frame from the panel's picture (source: the game's whole picture, in the present state, left so) and copies
    // it into its swapchain: true when the image is there (OpenXR::ambient_ready).
    bool present_ambient(VR* vr, ID3D12Resource* source, ID3D12CommandQueue* queue);
    // One of its passes into a picture of its own (in the render target state), the sprite over all of it: srv the view of what it reads
    // (srv_size its size), source the part of that read (all of it when null), color and numbers (the "transform", or none) its shader's.
    void draw_ambient_pass(ID3D12GraphicsCommandList* list, DirectX::DX12::SpriteBatch* batch, D3D12_GPU_DESCRIPTOR_HANDLE srv,
        DirectX::XMUINT2 srv_size, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, const RECT* source, const float* color,
        const float* numbers);
    // Its pictures and views let go (its last commands and copies done first): made again when next needed.
    void release_ambient();

    // (frame: the frame being presented: the menu's 3D pictures of that frame, or the one before, are laid under the interface.)
    bool compose_menu_layer(VR* vr, ID3D12Resource* eye_texture, ID3D12CommandQueue* queue, uint32_t frame);

    // The render-steps test's copies (record_step_snapshot) and their picture (process_step_snapshots, the present thread).
    struct StepSnapshots {
        std::mutex mtx{};
        struct Shot {
            ComPtr<ID3D12Resource> copy{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; // (a typed one)
            uint32_t width{0};
            uint32_t height{0};
            std::string label{};
        };
        std::vector<Shot> shots{};
        uint64_t bytes{0};
        bool finished{false};  // all recorded
        int frames_since{0};   // frames presented since
        bool done{false};      // saved (or given up): nothing more is kept
    } m_steps{};
    void process_step_snapshots(ID3D12CommandQueue* queue);

    // The live world and the menu's 3D pictures (record_live_picture), and what the panel and the eye images make of them.
    struct LivePictures {
        std::mutex mtx{};
        struct Slot {
            ComPtr<ID3D12Resource> copy{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; // (a typed one)
            uint32_t width{0};
            uint32_t height{0};
            uint32_t frame{0};  // the frame its last copy was made in
            bool has{false};    // a copy was made in it since it was made
            uint64_t copies{0};
            std::chrono::steady_clock::time_point made_at{};   // (made or tried last: not again and again)
            std::chrono::steady_clock::time_point copied_at{}; // (its last copy recorded)
            std::chrono::steady_clock::time_point failed_at{}; // (fix147) the last try to make it that failed
        };
        // [kind][the frame's parity]: a frame's copy is not written over (by the frame after next) before that frame is shown.
        std::array<std::array<Slot, 2>, 4> slots{};
        // The menu's 3D pictures' depth (record_live_depth), likewise: its copy (the depth picture's own format, a depth picture too;
        // kept in the pixel-shader-resource state between copies), the format its view reads it as.
        struct DepthSlot {
            ComPtr<ID3D12Resource> copy{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};      // (the depth picture's)
            DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN}; // (its depth read as a number)
            uint32_t width{0};
            uint32_t height{0};
            uint32_t frame{0};
            bool has{false};
            uint64_t copies{0};
            std::chrono::steady_clock::time_point made_at{};
        };
        std::array<std::array<DepthSlot, 2>, 4> depth{};
        std::array<bool, 4> depth_logged{};
        uint64_t depth_cut{0}, color_cut{0}; // (the 3D pictures laid under the interface: cut out by depth, by colour)
        std::vector<RetiredCopy> retired{};
        RetiredCopy last_copy{}; // (when the game's frames last copied into these)
        // (fix163) ...into the menu's own (its 3D pictures and their depth): full stereo copies its left eye into the world's every frame, so
        // last_copy never grew old in full stereo and the menu's pictures (about 240 MB) were never let go after a menu (stereo
        // glitches after unpausing: full stereo at half its speed, video memory over its budget; a helper's research).
        RetiredCopy last_menu_copy{};
        std::array<bool, 4> logged{};
        // (fix147) Full stereo's left eye (record_live_picture's eye_sized): pictures of another size than the eye image's refused, and
        // its slots made at the eye image's size (for the log, the first few times).
        uint64_t eye_size_refused{0};
        int eye_size_refused_logs{0};
        int eye_slot_logs{0};
        // (Present thread.) Views of each slot's copy, made again when it changes.
        std::array<std::array<d3d12::TextureContext, 2>, 4> source{};
        std::array<std::array<ID3D12Resource*, 2>, 4> source_of{};
        // The world's picture at the eye image's size, 8-bit (kept in the present state): [0] the left eye's (and mono's), [1] the
        // right eye's. They keep the last pair shown (shown again when a frame's world does not come).
        std::array<d3d12::TextureContext, 2> eye{};
        uint32_t eye_size[2]{};
        uint32_t eye_frame[2]{};       // the frame whose world each eye image has (eye_has: one drawn since it was made)
        bool eye_has[2]{};
        bool eye_menu[2]{};            // the menu was drawn into each eye image (draw_menu_into)
        uint32_t left_frame{0};        // the frame whose world went into the left eye image (left_ok)
        bool left_ok{false};
        bool was_live{false};          // the last panel frame showed the live world
        uint64_t live_pairs{0};
        uint64_t held_frames{0};       // frames the last pair was shown again
        bool last_good_valid{false};   // a live pair was shown with this panel (the frame and whether it was mono)
        uint32_t last_good_frame{0};
        uint32_t last_good_left{0};    // (its left eye's frame: the frame before, or the same one in mono)
        bool last_good_mono{false};
        int flips_logged{0};           // (this panel's changes between the live world and the world as it was, logged)
        std::chrono::steady_clock::time_point down_at{}; // the panel went down (what is kept is let go a few seconds on)
        uint32_t down_presents{0}; // (VR::m_presents_done then)
        bool previews_logged{false}; // (this menu: its 3D pictures saved, for the log)
        int preview_menus{0};
        bool live_snapped{false};      // (the first live world's eye image saved for the log)
        bool before_snapped{false};    // (the world as it was just before a menu opened, saved once for the log: to compare with it)
        bool turned_snapped{false};    // (the live world with your head turned well away from the menu, saved once for the log)
        uint64_t world_this{0}, world_previous{0}, world_missing{0}; // the live world shown: this frame's, the frame before's, none (this menu)
    } m_live{};
    // The copy of a kind made in `frame` (with previous: or the frame before), its views made: null if none.
    d3d12::TextureContext* live_source(int kind, uint32_t frame, bool previous, uint32_t* width = nullptr, uint32_t* height = nullptr,
        uint32_t* used_frame = nullptr);
    // The world of `frame` into its eye image (both, mono), at the eye image's size: false if there is none.
    bool present_live_world(VR* vr, uint32_t frame, bool mono, bool left);
    // Full stereo (fix134): the left eye's picture, made each frame from the world's view (copied as its last step drew it, kind 0 of
    // the live pictures) with the game's interface layer over it (its copy, MenuLayer); kept in the present state, eye-sized, 8-bit.
    struct NativeStereo {
        d3d12::TextureContext left{};
        uint32_t size[2]{};
        uint64_t shown{0}, missing{0}, with_hud{0}, shown_at_stats{0};
        uint64_t one_picture{0};          // (frames shown with one picture for both eyes)
        uint64_t previous_left{0};        // (fix147) frames shown with the left eye's last picture again, with its own pose
        // (fix147) What the left eye's image holds: the picture of kept_frame, made at kept_at, drawn with the left eye at kept_pose (its
        // frame's kept pose, as it was submitted with). Shown again, as AER shows an eye's last picture, when a frame's own left picture
        // is not there. (Not valid once the image is drawn into again, made anew or let go.)
        bool kept_valid{false};
        uint32_t kept_frame{0};
        std::chrono::steady_clock::time_point kept_at{};
        XrPosef kept_pose{};
        std::chrono::steady_clock::time_point last_at{}; // (the last full stereo frame: its copies let go a few seconds after)
        bool logged{false};
        int missing_logs{0};
        bool snapped{false};   // (the first pair saved once, for the log: native_left.jpg and native_right.jpg)
        std::chrono::steady_clock::time_point first_at{};
        std::chrono::steady_clock::time_point stats_at{};
    } m_native{};
    // AER (fix136): the pose each eye's last picture was drawn with (submitted with it while it is shown again), and counts for the log.
    struct AerEyes {
        XrPosef pose[2]{};
        bool pose_valid[2]{};
        uint64_t two_pictures{0}, one_picture{0};
        std::chrono::steady_clock::time_point stats_at{};
    } m_aer{};
    // The left eye of a full stereo frame into its eye image (true), or nothing when that frame's world picture is not there.
    bool present_native_left(uint32_t frame);
    // (fix147) A full stereo frame whose own left picture is not there: the left eye's last picture into its eye image again, submitted
    // with the pose it was drawn with (the right eye's with this frame's), as AER does; only while that picture is recent (3 frames,
    // 100 ms) and both poses are known. False: not (the caller shows the game's picture to both eyes from the head's middle).
    bool present_native_previous_left(VR* vr, uint32_t frame);
    // A few seconds after the panel went down: the live world's copies and eye images let go (their video memory).
    // (keep_world: the world's copies kept, full stereo's left eye uses them.)
    void release_live_pictures(bool keep_world = false);
    // Behind a menu, for the log (fix127: the lag seen in testing): the frames' times between presents, and the video memory the game
    // and the mod use against the budget Windows gives it (the adapter found once, through the game's swap chain).
    struct MenuPerformance {
        std::chrono::steady_clock::time_point last{};
        std::chrono::steady_clock::time_point window_start{};
        uint32_t frames{0};
        double sum_ms{0.0};
        double worst_ms{0.0};
        uint32_t over_50{0};
        uint64_t peak_used{0};
        uint32_t menu_frames{0};
        double menu_sum_ms{0.0};
        double menu_worst_ms{0.0};
        uint32_t menu_over_50{0};
        uint32_t stalls{0};      // (gaps of 2 s or more: left out of the average)
        uint32_t menu_stalls{0};
        uint32_t menu_saving{0}; // (frames a picture was saved for the log in: left out)
        bool saving{false};
        int logs{0};
        // (The first 3 s after the menu opened, fix131: the frames over 40 ms, when after the opening and how long.)
        std::chrono::steady_clock::time_point opened{};
        std::vector<std::pair<int, int>> slow{};
        uint32_t opening_frames{0};
        bool opening_logged{false};
    } m_menu_perf{};
    std::chrono::steady_clock::time_point m_present_at{}; // (the last present's time: note_menu_frame)
    // (fix179) The 3 s after a menu closed, frame by frame as its first 3 s (the game makes DLSS again as it closes): the frames over 40 ms,
    // when after the closing and how long; the menu's movies, as it closed.
    struct MenuClose {
        std::chrono::steady_clock::time_point closed{}; // (none: no closing watched now)
        std::chrono::steady_clock::time_point last{};
        std::vector<std::pair<int, int>> slow{};
        uint32_t frames{0};
        std::string menus{};
    } m_menu_close{};
    // (fix179) Where the time between two presents went (note_menu_frame takes it every present; a slow frame of a menu's opening or
    // closing logs it): the mod's own work on the present thread (all of it: the present hook's, the menus', ImGui's), the game's own
    // present call (it waits when the graphics card is behind), the headset's frame wait and frame end, the wait for the game's next
    // picture; and Streamline's DLSS calls on all threads (their number, total and longest). Microseconds.
    struct FrameTimes {
        int64_t from_us{0}, to_us{0};
        int64_t own_us{0}, present_us{0}, xr_wait_us{0}, xr_end_us{0}, game_us{0};
        uint32_t presents{0};
        uint32_t dlss_calls{0};
        int64_t dlss_us{0}, dlss_longest_us{0};
        int64_t check_us{0}, check_longest_us{0}; // (the present thread's DLSS let-go check: mostly its wait for the DLSS lock)
        int64_t gpu_wait_us{0}; // (fix181) the mod's command lists waited for (the graphics card not done with them yet), any thread
    };
    struct FrameSplit {
        int64_t note_us{0};
        uint64_t xr_wait_us{0}, xr_end_us{0};
        uint32_t enters{0};
        int window_details{0}; // (detail lines in this opening's or closing's 3 s: at most 12)
        int details{0};        // (in the session: at most 400)
        int close_logs{0};     // (the closing lines, apart from the menus' own lines' 200: at most 200 in the session; the review)
    } m_split{};
    FrameTimes take_frame_times(std::chrono::steady_clock::time_point now);
    void log_slow_frame(const std::string& when, int at_ms, int took_ms, bool saving, const FrameTimes& t);
    // (fix179) The interface's full-screen movies drawn lately (note_menu_movie), and the ones drawn since a time, for the lines' names.
    struct MenuMovie {
        uint32_t hash{0};
        std::string name{};
        int64_t last_ms{0};
    };
    std::mutex m_movies_mtx{};
    std::array<MenuMovie, 8> m_movies{};
    std::atomic<uint32_t> m_movie_hash{0};
    std::atomic<int64_t> m_movie_ms{0};
    std::string menu_movies_since(int64_t since_ms);
    Microsoft::WRL::ComPtr<IUnknown> m_adapter{}; // (an IDXGIAdapter3)
    bool m_adapter_tried{false};
    bool video_memory(uint64_t& used, uint64_t& budget);
    void note_menu_frame(bool panel_up);

    uint32_t m_backbuffer_size[2]{};
    bool m_backbuffer_is_8bit{false};
    bool m_force_reset{false};
};
} // namespace vrmod