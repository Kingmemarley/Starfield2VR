#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

struct ID3D12GraphicsCommandList;

// The world's own last-frame pictures, kept apart from the menu's views' (LiveWorld: the world kept drawn behind a see-through menu).
//
// With a full-screen menu open the game draws all of the frame's views (the world first, then the menu's 3D views: you, the planet,
// the item, the menu's room) with one set of working pictures, the ones a view keeps from one frame to the next among them (its
// anti-aliasing history, its previous depth, its light's history, its exposure...). The world then finds in them what the menu's
// views drew last, decides nothing matches and starts over each frame: its grainy effects are never smoothed, its edges flicker.
//
// Here the frame's steps are watched for a few frames (which step uses which picture, in which state, in the frame's order:
// ViewHistoryPlan), and from then on, for each of the world's histories, a copy of what the world left in it is saved right after
// the world's last use of it, and put back right after the last use of it by anything else, before the world reads it again. The
// menu's views see what they did before (the world's pictures of that frame). In stereo each eye has its own copies.
namespace ViewHistory
{
    enum StepGroup : uint8_t
    {
        kWorldStep = 0,
        kMenuStep  = 1,
        kFrameStep = 2,
    };

    struct StepPlace
    {
        int32_t     order{ 0 };
        uint8_t     group{ kFrameStep };
        bool        shared{ false }; // found twice in the frame (in two places): never a place to save or put back
        bool        blind{ false };  // its kind could not be watched: its uses are not seen
        bool        fresh{ false };  // not there at the walk before (a new object: many of the views' steps are, each frame)
        uint8_t     view{ 0xff };    // its view's place among the frame's views (0: the world's; 0xff: the frame's own steps)
        std::string name{};
    };

    struct Places
    {
        std::unordered_map<void*, StepPlace> steps{};
        uint64_t signature{ 0xcbf29ce484222325ull };       // the frame's views (the same views in the same places: the same signature)
        uint64_t frame_signature{ 0xcbf29ce484222325ull }; // ...and the "Frame" graph's own steps outside the views
        int                                  counts[3]{};
        bool                                 complete{ true }; // all of the frame's steps found (the walk was not cut short)
        bool                                 every_view{ true };  // each of the menu's views keeps its own histories too (else only the world)
        bool                                 reuse_plans{ true }; // plans made before for the same views used again
        uint32_t                             planned{ 0xffffffff }; // the views (bits by place) whose histories are kept: those whose picture is shown
    };

    using ListOf = ID3D12GraphicsCommandList* (*)(void* graph_data);

    // A frame begins (its "Frame" graph, the render thread), with the world kept drawn behind a see-through menu and its steps'
    // places (null: not kept this frame, the setting off or no world).
    void BeginFrame(uint32_t frame, bool mono, std::shared_ptr<const Places> places);
    // Each frame while no world is kept drawn behind a menu (the render thread): stopped, and the copies let go a few seconds after.
    void Idle();
    // Watching the steps (learning or keeping): AfterStep is wanted.
    bool Busy();
    // Keeping the world's histories apart (in stereo, each eye's: its previous camera is then its own too).
    bool Keeping();
    // Learning, planning or keeping: the world's histories are to be its own (MenuCamera clears the frozen camera's cut flag then; else
    // the game throws them away each frame, as before, rather than show what the menu's views left in them).
    bool Engaged();
    // The frame's steps wanted found again now (learning starts: from a fresh list of them).
    bool TakeWalkRequest();
    // Learning (the frame's steps watched): the frame's steps found again each frame, and its own graphs around it walked as they run
    // (EdgeSteps).
    bool Learning();
    // One of the frame's own graphs about to run (which 0: "CREndFrame", after the frame; 1: "CRBeginFrame", before the next one), its
    // steps as they are now: they may be new objects each frame. Their uses are the frame's own, at its end; never a place to save or
    // put back (a plan used again later would not find them).
    void EdgeSteps(int which, std::unordered_map<void*, StepPlace> steps, bool complete);
    // A step about to run (LiveWorld's hook, before the game's code): its textures with no picture attached yet, as bits by item (the
    // step attaches those itself as it runs, and the game may change their state then: never a place to save or put back). Only read
    // when it matters (learning, or a step with something to do).
    uint32_t EmptyBefore(void* pass, void* pass_data);
    // A step done (its kind hooked by LiveWorld; on the thread that records it). taa: the game's anti-aliasing step (its view's
    // persistent pictures logged). The step's command list is asked for only when needed. empty_before: as EmptyBefore gave.
    void AfterStep(void* pass, void* graph_data, void* pass_data, bool taa, ListOf list_of, uint32_t empty_before);
    std::string Status();
}
