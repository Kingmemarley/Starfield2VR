#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// DLSS kept on behind a see-through menu, each of the frame's views with its own DLSS instance.
//
// With a menu that has 3D views shown, the game turns DLSS off for the whole frame (0x140840A10, while the setting
// bTempMenusCanDisableUpscalers:Display is set): all of the frame's views share one DLSS viewport id (one per root render graph),
// so each view's picture would be built on the others'. Here the game's DLSS calls are told which view they are for (vrcore's
// UpscalerAfrNvidiaModule then gives each menu's view an id of its own): the view's scene setup (SceneSetupRenderPass, function 6,
// which sets DLSS's constants) and its DLSS step (DLSSUpscaleRenderPass, function 7, which tags DLSS's pictures and evaluates)
// read their view's handle (the view's record +0x24; the DLSS step finds the record as the game does, its graph data +0x108
// indexed by +0x140) and find it among the frame's views (LiveWorld's, the world's first). Only once every view's scene setup has
// been seen with a handle of the frame's views is the setting cleared (DLSS kept on); set again as soon as the world is no longer
// kept drawn, a handle is not found, or the views' instances fail.
namespace DlssViews
{
    // The two steps' kinds hooked (found by their types' names). Once, at start.
    void Install();
    // Each frame (LiveWorld, the render thread): the frame's views (their graphs and handles, in their order) and the world's handle.
    void SetViews(const std::vector<std::pair<void*, uint32_t>>& views, uint32_t world);
    // Each frame as it begins (CRBeginFrame, the render thread): the world kept drawn behind a see-through menu or not.
    void OnFrame(bool world_kept);
    std::string Status();
}
