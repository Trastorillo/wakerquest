/*
 * pc_vr.h - the diorama in the Meta Quest (native/src/pc/game_hooks/pc_vr_diorama.cpp): the world as
 * a model on a table in the room, through Aurora's view delta (patch 0019) and the XR layer
 * (android/xr/wq_xr.cpp). Android only; elsewhere these do nothing.
 */
#ifndef PC_VR_H
#define PC_VR_H

#ifdef __cplusplus
struct view_class;
struct Vec;

/* The painter's world block (pc_fps60_view_begin/_end, m_Do_graphic.cpp): with the diorama on, the
   draws with the camera's projection are drawn for the headset's eyes. True when it is on (the 60
   fps camera interpolation then stays off). */
bool pc_vr_view_begin(view_class* view);
void pc_vr_view_end(void);
/* The sun, the stars and the sky's clouds (pc_fps60_sky_begin/_end): not in the model. */
void pc_vr_sky_begin(void);
void pc_vr_sky_end(void);

/* The game's culling (mDoLib_clipper, m_Do_lib.h) while the diorama is on: what lies on the table is
   drawn, wherever the game camera looks. 1 culled, 0 drawn; -1: the diorama is off, the camera's test
   applies. m is the game camera's view times the model's matrix, as the clipper takes it. */
/* The options menu's "Realidad virtual" page (pc_menu.cpp): one xr.ini key set (android/xr/wq_xr.h
   wq_xr_set); nothing elsewhere. */
void pc_vr_setting(const char* key, const char* value);
/* One xr.ini key's value in effect, as the file would hold it (into out, size bytes); false elsewhere or for a key
   the layer does not show. */
bool pc_vr_value(const char* key, char* out, int size);
/* xr.ini's Rendimiento preset (perf_preset: "ahorro", "equilibrado", "maximo", "personal"); "" elsewhere. */
const char* pc_vr_perf_preset(void);
/* The internal resolution (lines) xr.ini gives the mode in effect (the window, less as it grows; the giant
   screen in 3D; or the rest), -1 elsewhere; base: the key's own value. And its xr.ini key ("resolution"
   elsewhere). Every frame (pc_menu.cpp syncVrResolution). */
int pc_vr_resolution(int* base);
const char* pc_vr_resolution_key(void);
/* The dynamic resolution in the headset (pc_vr_dynres.cpp, xr.ini dynres; off by default): the lines to
   draw with for the mode's `lines` (base: its xr.ini key's value; a change starts over at the top), fewer
   while the game's frame rate stays low. idle: frames not counted (a menu or the headset's panel open,
   settings.ini's resolution in effect). `lines` as it is when off and elsewhere. Every frame
   (pc_menu.cpp syncVrResolution). */
int pc_vr_dynres_lines(int lines, int base, bool idle);
/* The VR resolution (COS_VR_RESOLUTION, in lines) set and applied at once, also when the row already
   holds it (pc_menu.cpp; a preset's, from the headset layer through pc_vr_panel_frame). */
void pc_vr_apply_resolution(const char* lines);
/* The options menu, every frame (pc_menu.cpp menuFrame): for the headset's laser pointer. */
void pc_vr_menu_state(bool open, float width, float height, bool hovered);
/* The headset's settings panel (android/xr/wq_panel.cpp), every frame (pc_menu.cpp menuFrame): its game
   settings mirrored and its changes applied; returns 1 while it is open (the game waits), | 2 once
   when it asks for the game's full options menu. 0 elsewhere. */
int pc_vr_panel_frame(void);

/* The census (pc_census.cpp, COS_CENSUS): who the clips that follow are for, for the window's line of
   what passes its culling: the actor drawing (fopAc_Draw) or a kind of drawer that is no actor (the
   grass, the trees, the flowers...). Set for a scope, the previous owner put back after it. */
extern const void* pc_vr_clip_actor;
extern const char* pc_vr_clip_kind;
struct PcVrClipOwner {
    const void* prevActor;
    const char* prevKind;
    PcVrClipOwner(const void* actor, const char* kind) : prevActor(pc_vr_clip_actor), prevKind(pc_vr_clip_kind) {
        pc_vr_clip_actor = actor;
        pc_vr_clip_kind = kind;
    }
    ~PcVrClipOwner() {
        pc_vr_clip_actor = prevActor;
        pc_vr_clip_kind = prevKind;
    }
};

/* The characters' draw distance (pc_npc_distance.cpp, xr.ini npc_draw_distance): 1 when fopAc_Draw
   leaves this actor out as culled (an NPC far from Link, outside events and first-person views, with
   hysteresis), 2 when it is left out but nearer than 30 m (its execute goes on), else 0. */
int pc_npc_distance_hidden(void* actor);

/* The vegetation's adaptive draw distance (pc_veg_distance.cpp, xr.ini vegetation_*): a plant at
   (x, y, z) of this radius (game units) drawn at this scale: 1 in full, under 1 shrinking at the edge,
   0 left out (as culled). pc_veg_far_scale: the far plane's factor a packet widens the clipper by (the
   game's, or 1 while plants are cut). pc_veg_scale_mtx: a model(-view) matrix scaled about its origin.
   pc_veg_distance_saturated: 1 at vegetation_min and still overloaded, or the adaptive cut off (the
   dynamic resolution's turn). */
float pc_veg_draw_scale(float x, float y, float z, float radius);
float pc_veg_far_scale(float gameScale);
/* A palm's (Obj_Lpalm) scale: pc_veg_draw_scale's, and its own fixed distance (xr.ini palm_draw_distance)
   always, with the same fade at the edge. */
float pc_veg_palm_scale(float x, float y, float z);
void pc_veg_scale_mtx(float (*mtx)[4], float scale);
int pc_veg_distance_saturated(void);

/* The room's ambience (mode = pantalla, xr.ini ambient; pc_vr_diorama.cpp): the game's own ambient
   actors drawn a second time outside the giant screen (Aurora patch 0034). The actor whose draws are
   taken (nullptr: none); J3DJoint::entryIn hands its packets over while fopAc_Draw draws it, and the
   painter draws them again for the room after the particles (pc_vr_ambient_draw). */
extern const void* pc_vr_ambient_actor;  /* the first of them; nullptr: none (the test's cheap check) */
int pc_vr_ambient_is(const void* actor);  /* 1 for an ambient actor this frame */
void pc_vr_ambient_record(void* matPacket, void* shapePacket);
/* While the painter draws the ambience again, JPAEmitterManager::draw draws only the emitters of this
   user id (the weather's seagulls, then its wind lines), each placed in the room its own way. */
extern int pc_vr_ambient_jpa;
int pc_vr_ambient_jpa_kept(unsigned short userId);
void pc_vr_ambient_draw(void);

class J3DModel;
int pc_vr_clip_sphere(const f32 m[3][4], const Vec* center, f32 radius);
int pc_vr_clip_box(const f32 m[3][4], const Vec* max, const Vec* min);
int pc_vr_clip_model(J3DModel* model); /* the hidden joints' shapes, as clipByBox */
#endif

#endif /* PC_VR_H */
