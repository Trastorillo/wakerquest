// Reprojection with depth (rama reproyeccion, xr.ini reprojection): the main picture of the window, the diorama and
// the immersive mode (the eyes' projection layer) drawn again at every headset frame for where the eyes are now.
//
// Aurora draws both eyes at 30 fps for the head pose of the frame's tag; the compositor corrects the head's turn
// since then, but not the eyes' move (they move as the neck turns), so what is near jumps at 30 Hz. With
// reprojection on, Aurora also hands over each eye's depth (Aurora patch 0042, AuroraXrFrame.depth: the inverse of
// the distance along the eye's view axis, metres of the room, 0 = nothing drawn); this module keeps the eyes'
// colour and that depth of the newest frame (copied when the frame is copied, so Aurora's buffers go back to it as
// before) and at every headset frame draws each eye as a grid over the picture, its vertices taken from the frame's
// eye (its pose and frustum) to the space and into the eye where it is now, the colour sampled where it was. What
// has no depth goes to infinity (turned only, as the compositor would).
#pragma once

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>

namespace wqr {

// The EGL calls the module needs (wq_xr.cpp's), once its context is current.
struct Egl {
    EGLDisplay display;
    PFNEGLCREATEIMAGEKHRPROC createImage;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC nativeClientBuffer;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture;
};
void init(const Egl& egl);

// Every headset frame (runFrame, after the settings are read): xr.ini's reprojection. 0 off: as without this
// module (Aurora draws no depth; whatever the module had is freed). 1: the eyes' pictures for where the eyes are
// now, keeping the orientation and frustum they were drawn with (the compositor turns them, as it does today).
// 2: the eyes' pose and frustum of now altogether. Aurora is told (aurora_xr_set_eye_depth) when it changes.
// refine (xr.ini reprojection_refine): each pixel's colour found along its line of sight in the depth (a few
// fixed-point steps in the fragment shader), not only the grid's interpolation; 0: the grid's alone; 2: as 1, the
// pixels whose steps did not converge painted magenta (a diagnosis).
void configure(XrSession session, int mode, int refine);
int mode();

// copyNewestFrame, a diorama frame with its eyes' depth, after its fences, with the copy program and its uniforms
// in use and the frame's buffer bound: binds the eyes' copy (both eyes, eyeW x height each) as the framebuffer
// with a viewport as large as the whole buffer (bufferW x height), so one draw of the copy fills it with the
// buffer's first two eyes exactly as the screen's swapchain gets them. False: nothing bound (no capture).
bool beginColor(uint32_t bufferW, uint32_t height, uint32_t eyeW, bool srgb);
// Then the frame's depth buffer (AuroraXrFrame.depth, width x height: both eyes), copied texel for texel.
void captureDepth(AHardwareBuffer* depth, uint32_t width, uint32_t height);
// Once the frame is shown (copyNewestFrame's copy done): whether the eyes' colour and depth above are this
// frame's (tag), drawn for `drawn` (each eye's pose; nullptr: not known) with `drawnFov`, kept with them; else
// none (the frame is shown as before, without reprojection).
// window: the frame was drawn through the window with its mask (the window mode, a special scene; not the immersive
// mode), the window's pose in the space (+z toward the player) and half size then; nullptr otherwise.
void commit(bool captured, uint32_t tag, const XrPosef drawn[2], const XrFovf& drawnFov, const XrPosef* window,
            float windowHalfW, float windowHalfH);

// runFrame, the eyes' projection layer of the frame of tag `tag`: the eyes drawn again for this frame's eyes,
// from the capture's own poses and frustum, into the module's swapchain, and views filled with it (pose, frustum,
// sub-image). False: not done (no capture of that frame, the views not located...): the caller fills the views as
// before.
// For a frame committed with its window (commit): what is outside the window's mask is taken on its plane (as the
// frame had it), and the mask is done again for the eye now with the window where it stands now: `window`, its pose
// and half size (metres), or nullptr for the frame's own.
bool render(XrSession session, XrSpace space, XrTime time, uint32_t tag, bool srgb, const XrPosef* window,
            float windowHalfW, float windowHalfH, XrCompositionLayerProjectionView views[2]);

}  // namespace wqr
