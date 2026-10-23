/*
 * pc_vr.h - the diorama in the Meta Quest (native/src/pc/game_hooks/pc_vr_diorama.cpp): the world as
 * a model on a table in the room, through Aurora's view delta (patch 0019) and the XR layer
 * (android/xr/wq_xr.cpp). Android only; elsewhere these do nothing.
 */
#ifndef PC_VR_H
#define PC_VR_H

#ifdef __cplusplus
struct view_class;

/* The painter's world block (pc_fps60_view_begin/_end, m_Do_graphic.cpp): with the diorama on, the
   draws with the camera's projection are drawn for the headset's eyes. True when it is on (the 60
   fps camera interpolation then stays off). */
bool pc_vr_view_begin(view_class* view);
void pc_vr_view_end(void);
/* The sun, the stars and the sky's clouds (pc_fps60_sky_begin/_end): not in the model. */
void pc_vr_sky_begin(void);
void pc_vr_sky_end(void);
#endif

#endif /* PC_VR_H */
