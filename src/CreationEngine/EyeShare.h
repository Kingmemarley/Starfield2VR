#pragma once

#include <cstdint>
#include <string>

// (fix156) Full stereo's right eye given the left eye's results of the steps that come out the same for both eyes (the option
// "share eye-identical steps", chosen to raise full stereo's frame rate). Full stereo draws the world's view twice in a frame, the left eye's
// first; some of its steps make the same picture for either eye: the right eye's then does the same work again.
// OFF BY DEFAULT (the review, the exe): the render graph places its textures in shared memory pages and gives a texture's range to
// later textures after its last use in the frame, so the left eye's result may be overwritten before the right eye reads it (the same
// texture object is no proof the bytes are still there). Only a step whose textures are all outside that reused memory (their
// entries' +0xA0 is 0) is shared; the log says which are in it (likely all four: then nothing is shared, and sharing would need a
// copy, which costs about what it saves). Those steps (a helper's research of Starfield 1.16.236, below) are skipped in the right eye's view when
// the left eye's own ran earlier in the same frame and every texture the step uses is the very same one as the left eye's (else the
// game's step runs, as before):
// - PrecomputeAtmosphericScatteringTransmittance (the sky's transmittance table: the view's scene's atmosphere, the same for both),
// - GenerateFogMap and BlurTerrainHeightClipmapHorizontal/Vertical (the terrain's fog height map around the camera: the eyes are
//   6.4 cm apart; the blurs keep no state of their own).
// Not shared (the research): the stars (their pass state block is read by later star steps), the terrain's clipmap, MIM and tile
// updates (frame-stamped state, new allocations, CPU uploads), anything temporal or drawn through each eye's own depth.
// Skipping a step's function 7 is as the game's do-nothing steps are: its runner (0x142A5DDE0) does not use its result, and the
// barriers the graph planned around it are made outside it.
namespace EyeShare
{
    // At start: the steps' kinds found (their type tables and functions checked) and hooked through LiveWorld's StepHook.
    void Install();
    // The setting (GameSettingsComponent, each frame).
    void SetOn(bool on);
    // StepHook, before a step of kind `table` runs (any recording thread): true when it is the right eye's step to skip.
    bool Skip(uintptr_t table, void* data, const void* pass_data);
    // StepHook, after it ran: the left eye's textures noted for this frame.
    void After(uintptr_t table, void* data, const void* pass_data);
    // Counts, logged every 30 s (present thread), and one line for the settings panel.
    void Log();
    std::string Status();
}
