#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

// Full stereo (fix134, a test: setting "Full stereo", off by default): the game draws both eyes in every frame, the right eye as a
// second view of the world in the same frame as the left.
//
// Starfield's renderer draws several cameras a frame (the frame's view list: the world's view first, then a shown menu's 3D views;
// each view gets its own part of the frame, RenderViewSubGraph, with its own culling and shadows, while the world's scene, its
// geometry, lighting and the game's work are done once). The menus make their 3D views at runtime: a camera (an NiCamera, registered
// in the cameras' table) and a camera view (a row in the camera views' table) with a scene and that camera (0x142552B20). This does
// the same once for the right eye: a camera of its own (so its previous-frame camera, kept per camera, is its own: motion vectors and
// DLSS right) and a view of the world's scene with it, its settings copied from the world's view; and adds that view right after the
// world's to the frame's list (0x140840C4F, before the list is handed to the renderer) in the frames VR plays as full stereo
// (VR::is_native_frame), with the camera put at the right eye just then (the world's camera, already updated for the frame, moved by
// the eyes' distance). The world's camera is the left eye in those frames. Aboard a ship in space (fix135) the game draws the same
// scene through its galaxy camera, with its own lens and far plane (the space view, +0x3d4): the right eye's view then copies that
// view's settings (and its shadow settings), and its camera is the galaxy camera moved by the eyes'
// distance along its own axes.
//
// The renderer refuses DLSS for a frame with more than one view (0x14298CC32: two views and DLSS make the upscale a plain copy); with
// these two views it is let through (each view upscaled by DLSS, each with its own instance: vrcore's right-eye instance for the
// right). Each view's last step writes the frame's one picture: LiveWorld copies the left eye's out as it is drawn, the right eye's is
// the game's picture, the interface laid on it by the game (D3D12Component lays it on the left eye's too).
namespace StereoView
{
    // Checks the game's code (Starfield 1.16.236) and hooks the list and the DLSS check. Once, at start.
    void Install();
    // The game's thread, at each frame's start (after the world's camera was given its local transform): the camera and view made
    // when wanted, the view's settings kept as the world's, and whether full stereo can be played now (VR::m_native_possible).
    void OnFrameStart();
    // The eyes' offset this frame, right eye minus left, in the world camera's parent's space (as its local transform is), from
    // CreationEngineCameraManager::UpdateWorldCamera (the engine frame it is for: the camera is placed only with that frame's).
    void SetEyeDelta(const glm::vec3& right_minus_left, uint32_t frame);
    // A render graph run (before: starting; else ended): the right eye's history list's cut flag cleared after the frame (fix136).
    void OnGraph(const char* name, bool before);
    // The view is the right eye's (a frame's view list).
    bool IsEyeView(uint32_t handle);
    // The right eye's view was in a frame's list in the last moments.
    bool Active();
    // (fix140) How far apart the eyes were last placed, in the drawn camera's units (0: not yet).
    float EyesApart();
    // The right eye's camera (null until made).
    void* Camera();
    std::string Status();
}
