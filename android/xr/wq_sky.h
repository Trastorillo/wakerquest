// Techo abierto (roof_open, 1.2.3): the room's ceiling opens and its walls come down, and over them the sky and
// the sea, a fixed surrounding drawn here (procedurally, no objects), with the game's colours and weather.
#pragma once

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace wqs {

struct Room {
    float centre[2];  // x, z of the room's middle in the space (the stage's origin)
    float floorY;     // the floor's height in the space
    float radius;     // m, the walls' circle around the middle
    float wall;       // m, how high the walls stay once down
};

// The room the headset knows: every event of the session, and every frame (asks the USE_SCENE permission once
// and queries the room while `want`). The sky takes it from here; without it, the circle.
void sceneEvent(XrSession session, const XrEventDataBuffer& ev);
void sceneTick(XrInstance instance, XrSession session, XrSpace space, XrTime time, bool want);

// The XR thread, between xrBeginFrame and xrEndFrame: moves the opening toward `open`, and while anything of it
// shows draws both eyes and fills the layer (views: two). False when there is nothing to show: no layer.
bool layer(XrSession session, XrSpace space, XrTime time, bool srgb, bool open, const Room& room, int hz,
           XrCompositionLayerProjection* out, XrCompositionLayerProjectionView* views);

}  // namespace wqs
