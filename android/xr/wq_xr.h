// WakerQuest's headset layer (wq_xr.cpp): the game on a giant screen in the Meta Quest through
// OpenXR, and the Touch controllers as a GameCube controller.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Called once before aurora_initialize. When an OpenXR runtime has a headset, it starts the layer's
// thread and returns true: from then on Aurora draws into the layer's buffers (include/aurora/xr.h)
// while the session runs. Otherwise false, and the game stays in its window. WQ_XR=0 turns it off.
bool wq_xr_start(void);

#ifdef __cplusplus
}
#endif
