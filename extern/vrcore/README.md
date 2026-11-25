# vrcore

Open replacement for the `vrframework` submodule that starfield2vr was built against. That
repository is no longer publicly available, so this folder provides the same headers and API
from public, MIT-licensed code.

## Where the code comes from

| Part | Source |
|---|---|
| D3D12 present hook, Windows message hook, window filter | REFramework (praydog, MIT), mostly unchanged |
| OpenXR / OpenVR runtimes, D3D12 eye submission, d3d12 helpers | REFramework (praydog, MIT), adapted |
| `FunctionHook` (safetyhook based) | REFramework (praydog, MIT) |
| `Framework`, `Mod`, `Mods`, `VR`, `VRConfig`, dxgi proxy | Written for vrcore, structured after REFramework |
| NVIDIA AFR upscaler module, shader debug overlay | Placeholders (no-ops) |

UEVR is not used: its current license is not open source.

## What changed compared to REFramework's VR mod

* Alternate-frame rendering is driven by the engine's frame index. The game integration calls
  `on_wait_rendering`, `on_begin_rendering` and `update_hmd_state` with Starfield's frame index;
  even frames render the left eye, odd frames the right eye.
* The eye written at present time follows the index of the frame being presented
  (`m_presenter_frame_count`), not a present counter.
* OpenXR copies are skipped while the game backbuffer and the swapchain sizes differ (the
  game window is resized to the headset resolution asynchronously).
* Separate left/right face-button actions and thumbrest touch actions for the virtual gamepad.
* A head-locked virtual screen (OpenXR quad layer) is used for menus and flat-screen mode.

## Not implemented yet

* Per-eye DLSS/Streamline state (`UpscalerAfrNvidiaModule` is a placeholder).
* Default SteamVR binding files for the OpenVR build; bind controllers in SteamVR's binding UI.

## Build

vrcore is added by the parent project with `add_subdirectory(extern/vrcore)` and fetches its
dependencies (spdlog, glm, imgui, kananlib, bddisasm, safetyhook, OpenXR SDK, DirectXTK12,
OpenVR headers) at configure time, pinned to the commits REFramework uses.

## License

MIT. See `LICENSE` and `LICENSE.REFramework`.
