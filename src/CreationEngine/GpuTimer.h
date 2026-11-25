#pragma once

#include <cstdint>
#include <string>

// What each of the game's render steps costs on the graphics card (fix140), for the log: one frame in 30 timed, each step between two
// GPU timestamps on its own command list (D3D12 timestamp queries, on the main kind of list only), read back a few frames later and
// summed over 10 s by view (the world's or left eye's, full stereo's right eye's, a menu's, the frame's own) and by kind of step.
//
// The steps are the game's render passes, each recorded through its type table's function 7 (LiveWorld's StepHook goes round every
// kind: LiveWorld::HookAllSteps walks the frame's graphs). The timestamps are recorded right before and after the step's own commands;
// steps can run side by side on the GPU, so each one's time is at most what it adds, and the view's span (its first step's start to its
// last step's end) is the surest number. The step's list is read as the game keeps it (never made by this: the game makes one when a
// step first asks). Results are resolved at a present, on the game's queue, for frames presented before it, and read once done.
namespace GpuTimer
{
    void SetOn(bool on);
    // Each of the frame's top-level graphs as it starts or ends (the renderer module): the timed frame begins at CRBeginFrame.
    void OnGraph(void* graph, const char* name, bool before);
    // (LiveWorld's StepHook) A frame being timed now.
    bool Timing();
    // Around a step's own work: Begin gives its pair (or -1: not timed), End closes it.
    int  Begin(void* data);
    void End(int pair, void* pass, void* data);
    // At each present (its own thread): the timed frames resolved, read back, and logged every 10 s.
    void OnPresent();
    std::string Status();
}
