#pragma once

#include <cstdint>
#include <string>

#include <d3d12.h>

namespace Scaleform::Gfx
{
    struct Viewport;
}

// (fix145) A conversation on a panel of its own, fixed in the room in front of the person you talk to (after fix144 the text box
// still moved with the head slightly; the conversation is meant to stay locked in front of the npc).
//
// fix144 moved the conversation's movie, in the game's picture, to where a point of the room was seen: only that point stayed put; the
// movie is an upright rectangle in the picture, so the rest of it (the answers, up to 0.4 m off that point) turned and tilted with the
// head, a few cm (fix145's research), and its width was capped at the picture's.
//
// Here the conversation (Starfield's DialogueMenu, with Enhanced Dialogue Interface's text) is drawn in a fixed 16:9 box in the game's
// interface layer (PlaceMovie); just before the game lays that layer over its picture, its part with what is said and the answers is
// copied out and the whole box cleared there (MenuLayer, D3D12Component::record_conversation), and the copy is shown by the headset as
// a panel of its own in the room (an OpenXR quad layer: OpenXR::set_conversation), redrawn by the headset at its own rate wherever you
// look, sharp, the same in both eyes. As Skyrim VR puts its dialogue beside the speaker (fDialogueOffsetX): the panel goes 0.45 m from
// your eyes on the way to the person speaking (MenuTopicManager's speaker; else the person nearest your view), a little below their
// face, facing you, and stays there; it is moved (eased) only when they move a lot or are well off it for a while, or you move. Its
// answers are touched with a finger as before (the panel's own plane), or pointed at.
namespace ConversationPanel
{
    // Finds the game's conversation manager (the mod's start).
    void Install();
    // (The game's thread, each frame: tracking::Snapshot.) The panel placed while a conversation is drawn, and given to the headset.
    void Update();
    // (The render side: the conversation movie's viewport, CreationEngineCameraManager.) The movie put in its fixed box in the interface
    // layer (buffer_w x buffer_h, as the game draws it); false: not on its panel now (the setting off, no headset, the cut refused).
    bool PlaceMovie(Scaleform::Gfx::Viewport* viewport, int buffer_w, int buffer_h);
    // (MenuLayer, any thread) A conversation placed on its panel lately: its cut wanted (cheap, checked first).
    bool CutWanted();
    // (MenuLayer, just before the game lays its interface layer over the picture.) The part to copy out and the box to clear, in the
    // layer's pixels; false: none now (no conversation placed lately, or the layer not the size it was placed for).
    bool CutBoxes(uint32_t layer_w, uint32_t layer_h, D3D12_BOX& copy, D3D12_BOX& clear);
    // (MenuLayer) How the cut went (D3D12Component::record_conversation: 1 done, 0 not this time, -1 refused): refused again and again,
    // the conversation is left in the picture (fix144's way) until the game restarts.
    void NoteCut(int result);
    // (fix146, MenuPointer) The conversation movie's box, centred in a picture of this size (placed for it lately): its width and height (the
    // game gives the movie's cursor from the box's corner); false: not on its panel now.
    bool MovieBox(int picture_w, int picture_h, int& width, int& height);
    std::string Status();
}
