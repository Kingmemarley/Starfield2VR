#pragma once

#include <cstdint>
#include <string>

#include <mods/vr/D3D12Component.hpp>

namespace Scaleform::Gfx
{
    struct Viewport;
}

// (fix177) Build mode as a console (the research's design A, "build console", chosen after fix176).
//
// Since fix175 build mode's menu (WorkshopMenu: ship decorating, outpost building) is drawn over the world like the HUD, in a box of three
// quarters of the eye picture. The menu pins its parts to the edges of whatever box it is given (its onSetSafeRect: GlobalFunc.LockToSafeRect
// on the part of its stage the box shows), and that box is taller than wide in the headset, so its parts sat 32 to 47 degrees from the middle
// of your view: the item card up left, the categories and the list up right, the button hints, the outpost's power and the build warnings
// low down, small and in the lens's blurry edge (the research: scratchpad buildmenu_rs\findings.txt).
//
// Here the menu leaves your view. It is drawn in a fixed 16:9 box as wide as the interface layer (its parts then at the places its stage was
// designed with, known), and its notifications (HUDMessagesMenu: the game's messages, subtitles, mission updates) in a 16:9 box of their own
// below it (the HUD itself, HUDMenu, is not drawn meanwhile: nothing of it would be shown). Just before the game lays its interface layer over
// its picture, the parts are copied out once a frame into one small picture of their own, packed side by side, and both boxes cleared
// there (MenuLayer, D3D12Component::record_console); the headset shows each part as a quad of its own, a crop of that picture (as the
// favorites wheel shows its one picture twice): the item card, the categories and list, the button hints and the outpost's footer low in front
// of your seat, about 0.9 m away, tilted up to your eyes, and fixed in the room (they stay as your head turns, turn with you on stick turns,
// come with you as you walk); the action and choice cards, the build warnings and the notifications small near the middle of your view (with
// your head: they are about what you are aiming at). Text about 0.7 degrees tall (Microsoft's comfortable range). Only for reading: build
// mode has no cursor, its controls stay fix175's and fix176's. Not shown within 0.8 s of an opening: build mode as before for that opening.
namespace BuildConsole
{
    // Logs the setting (the mod's start).
    void Install();
    // (The render side: a movie's viewport, CreationEngineCameraManager.) Build mode's menu, or while it is on the console the notifications'
    // movie or the HUD, put in their fixed boxes of the interface layer (buffer_w x buffer_h, as the game draws it); false: left to the usual
    // placement (the setting off, no headset, the large-text menu, not shown in time this opening...).
    // (movie: the game's movie, to log the part of its stage it shows a few frames into an opening.)
    bool PlaceMovie(void* movie, uint32_t movie_hash, Scaleform::Gfx::Viewport* viewport, int buffer_w, int buffer_h);
    // (MenuLayer, any thread) Build mode placed on the console lately: its cut wanted (cheap, checked first).
    bool CutWanted();
    // (MenuLayer, just before the game lays its interface layer over the picture.) The parts to copy out, where they go in the console's
    // picture, and the boxes to clear, in the layer's pixels; false: none now (not placed lately, or the layer not the size placed for).
    bool CutBoxes(uint32_t layer_w, uint32_t layer_h, vrmod::D3D12Component::ConsoleCut& cut);
    // (MenuLayer) How the cut went (D3D12Component::record_console: 1 done, 0 not this time, -1 refused): refused again and again, build mode
    // stays in the view until the game restarts.
    void NoteCut(int result);
    // (The game's thread, each frame: tracking::Snapshot.) The console placed while build mode is drawn on it, and given to the headset.
    void Update();
    // (MenuClean, on the build menu's own thread after each advance.) The menu looked at (the setting on, in the headset).
    bool LookWanted();
    // (MenuClean) Build mode shown on the console now (placed in its box lately, not back in the view): its centre reticle and corner brackets
    // hidden meanwhile (the right hand aims since fix176), shown again otherwise.
    bool ConsoleShown();
    // (MenuClean) Whether the menu's action or choice card is up now (its timeline between its Open and Hidden frames); known false: could
    // not be read (then the console shows that part always).
    void NoteCentreCards(bool known, bool shown);
    // (MenuClean) Where the menu's list is (BuildPopupContainer_mc's y: about 46 when laid out in a 16:9 box, about -448 in a taller one):
    // laid out for a taller box while on the console, build mode goes back in the view for that opening.
    void NoteMenuLayout(bool known, float list_y);
    // (fix178, MenuClean) The button hints' bar shown (its left and right, stage units; any: one is shown): the hints panel trimmed to it.
    void NoteHintsBar(bool known, bool any, float left, float right);
    // (The viewport hook, every movie, its viewport as given to the game.) Another movie drawn inside the console's boxes while it cuts:
    // logged once each; the game's fader stops the cut while it is drawn.
    void NoteMovie(void* movie, const Scaleform::Gfx::Viewport* viewport);
    std::string Status();
}
