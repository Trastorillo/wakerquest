// WakerQuest's headset layer (wq_xr.cpp): the game on a giant screen in the Meta Quest through
// OpenXR, or as a diorama on a table in the room, and the Touch controllers as a GameCube controller.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once before aurora_initialize. When an OpenXR runtime has a headset, it starts the layer's
// thread and returns true: from then on Aurora draws into the layer's buffers (include/aurora/xr.h)
// while the session runs. Otherwise false, and the game stays in its window. WQ_XR=0 turns it off.
bool wq_xr_start(void);

// The diorama (native/src/pc/game_hooks/pc_vr_diorama.cpp), any thread.
typedef struct WqXrHead {
    uint32_t tag;   // names this pose for aurora_xr_set_frame_info; 0 = none yet
    float pos[3];   // the eyes' midpoint in the LOCAL space (metres)
    float rot[4];   // the head's orientation, a quaternion (x, y, z, w)
    float ipd;      // the distance between the eyes (metres)
    // the frustum both eyes' pictures are drawn with, as tangents from each eye (left and down < 0)
    float tanLeft, tanRight, tanDown, tanUp;
    // the room's floor in the LOCAL space (metres, y; the STAGE space's origin), when the runtime knows it
    bool floorValid;
    float floorY;
} WqXrHead;

typedef struct WqXrTable {
    bool diorama;    // xr.ini mode = maqueta (and the session runs); false for the window
    bool sphere;     // the bubble (shape = esfera): a sphere of diameter `width`; else the table (a box)
    float width;     // the bubble's diameter, or the table's metres across (x) and deep (z)
    float depth;
    float span;      // metres of the game's world across the table's width
    float distance;  // the table's middle in front of the eyes at the start (metres)
    float height;    // its top below the eyes (metres)
    float fade;      // the edges' fade (metres)
    bool followYaw;  // the world turns with the game camera (else it keeps its first heading)
    float x;         // metres to the right of straight ahead (moved with the right grip)
    float yaw;       // radians the world is turned by the right grip
    bool autoScreen; // the game's first-person views (telescope, aiming...) on the giant screen
    bool cineScreen; // and its cutscenes
    bool birdsFree;  // the flying birds drawn around the room, outside the bubble
    float birdRange; // how far: radii of the bubble from its middle
    bool ambient;        // mode = pantalla, ambient 1: the game's ambience also outside the screen
    float ambientRange;  // metres from the screen's edge where it has faded out
    float ambientFocus;  // the game's units that lie on the screen's plane (stereo_focus)
    bool ambientTestLink;  // the test's actor: Link (else the nearest seagull)
    bool ambientMask;      // hidden where the screen is (false: over it too)
    float ambientMove[4];  // the screen's ambience moved: metres in the room (x, y, z), radians about the
                           // vertical through the screen's middle
    float ambientScale;    // and sized about the screen's middle
    bool ambientWorld;     // anchored in the room around Link (else the game camera's view)
    // the giant screen in the room (LOCAL space): its middle, its turn about the vertical (radians,
    // positive to the left) and its width (metres)
    float screenX, screenY, screenDistance, screenYaw, screenWidth;
    // the window (mode = ventana): the world at its own scale seen through a rectangle in the room, the
    // game camera's eye in front of it looking through it, each eye looking through it from where it is
    bool window;
    float windowWidth, windowHeight;  // the rectangle (metres; 16:9)
    float windowX, windowY;           // its middle: to the right of straight ahead, above eye height
    float windowDistance;             // in front of the eyes at the start
    float windowYaw;                  // radians, turned about the vertical (positive to the left)
    float windowScale;                // the game's metres per metre of the room (1 = real size)
    float windowDepth;                // the game camera's eye this far in front of the window (metres)
    float windowPopout;               // what may come out toward the player (metres; 0 = nothing)
    float windowFade;                 // the edges' fade (metres)
    // the special scenes (special_scenes): some cutscenes (the Helmaroc King on Outset) through the
    // window in any mode, at the world's own size, the camera this deep and this much coming out
    bool specialScenes;
    float windowDepthSpecial, windowPopoutSpecial;
    // in the bubble the special scene closes in on its subject (specialZoom of the span) unless
    // specialWindow (the window, as above)
    bool specialWindow;
    float specialZoom;
    float talkZoom;            // cutscenes and dialogues: the bubble's span times this (0 = off)
    float seeThroughRadius;    // Link seen through what is in front of him: the game's metres (0 = off)
    float windowDrawDistance;  // the window: the game's metres drawn at scale 1 (the horizon)
    float npcDrawDistance;     // the characters drawn within the game's metres from Link, every mode (0 = all)
    // the dynamic resolution (xr.ini dynres, pc_vr_dynres.cpp): on; the fps under which it goes down
    // (3 s) and that it must hold to go up (10 s), for 30 fps; the lowest fraction of the mode's lines
    bool dynres;
    float dynresLowFps, dynresHighFps, dynresMin;
} WqXrTable;

// The latest head pose and frustum (false while the session does not run).
bool wq_xr_head(WqXrHead* out);
void wq_xr_table(WqXrTable* out);
// The game, with each frame drawn for head pose `tag` (before aurora_xr_set_frame_info): whether it is a
// window's frame (the window mode's, or a special scene's: the HUD over the window) and the frustum it
// drew both eyes with when not the head's (tangents left, right, down, up, as WqXrHead's; the window's
// fitted one), or NULL for the head's. The XR layer composites the frame with the same.
void wq_xr_set_frame_view(uint32_t tag, bool window, const float* tangents);
// The options menu's "Realidad virtual" page (pc_menu.cpp): sets one xr.ini key (its line replaced,
// or added) and has the layer read the file again. Any thread.
void wq_xr_set(const char* key, const char* value);
// The Rendimiento preset in effect (xr.ini's perf_preset: "ahorro", "equilibrado", "maximo" or
// "personal"), or "" without the headset layer. Any thread.
const char* wq_xr_perf_preset(void);
// One xr.ini key's value in effect, as the file would hold it (the options menu's rows mirror it); false
// without the headset layer or for a key it does not show. Any thread.
bool wq_xr_value(const char* key, char* out, int size);
// The internal resolution (lines) of the mode in effect: window_resolution in the window (fewer lines
// as the window grows past 1.6 m, down to 648), stereo_resolution on the giant screen in 3D, resolution
// otherwise; -1 without the headset layer. base: the xr.ini key's own value. And that key (the options
// menu's Resolucion row sets it). The game thread, every frame.
int wq_xr_resolution(int* base);
const char* wq_xr_resolution_key(void);
// The diorama (pc_vr_diorama.cpp), every frame: the game camera's heading in the room (radians, 0 =
// straight ahead, positive to the right), so the left stick can be turned to the player's view.
// rotate: the player controls Link freely (no options menu, pause menu or event): only then is the
// stick turned. dialogue: a message box is open (shown on its own panel above the model).
void wq_xr_set_camera_heading(float heading, bool rotate, bool dialogue);
// The game, every frame (pc_vr_diorama.cpp): an event (a cutscene) runs; the giant screen's 3D is
// stronger meanwhile (cine_3d).
void wq_xr_set_cinematic(bool cinematic);
// The game, every frame (pc_vr_diorama.cpp): a special scene plays, drawn through the window whatever the
// mode (its frames are window frames: the HUD over the window, the room around it).
void wq_xr_set_special(bool special);
// The game, every frame (pc_vr_diorama.cpp): the bubble closes in on who talks (a cutscene or a
// dialogue, talk_zoom): the game's 2D on a larger panel beside it meanwhile (talk_screen).
void wq_xr_set_talk(bool talk);
// The options menu (pc_menu.cpp), every frame: open, ImGui's display size (the pointer's coordinates)
// and whether the pointer is over something it can press.
void wq_xr_menu_state(bool open, float width, float height, bool hovered);

// The headset's settings panel (wq_panel.cpp) and the game's settings (pc_settings), on the game thread
// every frame (pc_vr_panel_frame): the game keys the panel shows (NULL-terminated), the value of each,
// the changes made on the panel to apply, and its state: 1 open (the game waits), 2 the game's full
// options menu was asked for (once).
const char* const* wq_xr_panel_game_keys(void);
void wq_xr_panel_game_value(const char* key, const char* value);
bool wq_xr_panel_take_change(char* key, int keySize, char* value, int valueSize);
int wq_xr_panel_flags(void);

#ifdef __cplusplus
}
#endif
