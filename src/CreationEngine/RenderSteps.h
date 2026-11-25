#pragma once

#include <string>

// Test build: the steps of the game's frame while a see-through menu is open, to find out whether the game still draws the world
// behind such a menu, and where (for a live world behind menus, as in Skyrim VR, instead of the picture kept from before the menu).
//
// Once a session, when a see-through menu has been open for a moment: the frame's render graph ("Frame") is walked (its steps,
// each a render pass, in order, with their names and kinds, logged), and each kind of step's function 7 (the one the interface
// pass's hook uses: RenderPass::unk_7, the step run) is hooked for two frames. In the first, every step run is logged with the
// pictures it uses (size, format, state); in the second, up to 16 large colour pictures the steps draw (the last ones of the frame,
// and a few earlier) are copied right after their step (vrcore: D3D12Component::record_step_snapshot), then put together into one
// picture, menu_steps.jpg, with each cell's step in the log. Then the hooks are taken out. Nothing is changed in what is drawn.
namespace RenderSteps
{
    // From the render graph hook (CreationEngineRendererModule::RenderGraphStart): before and after each of the frame's graphs.
    void OnGraph(void* graph, const char* name, bool before);
    // (fix173) Its setting (RenderStepsTest, off by default: the walk's ~850 log lines and the picture's save stall the frames as a menu is
    // used). Each frame (GameSettingsComponent).
    void SetOn(bool on);
    std::string Status();
}
