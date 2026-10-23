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
} WqXrHead;

typedef struct WqXrTable {
    bool diorama;    // xr.ini mode = maqueta (and the session runs)
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
} WqXrTable;

// The latest head pose and frustum (false while the session does not run).
bool wq_xr_head(WqXrHead* out);
void wq_xr_table(WqXrTable* out);
// The options menu's "Realidad virtual" page (pc_menu.cpp): sets one xr.ini key (its line replaced,
// or added) and has the layer read the file again. Any thread.
void wq_xr_set(const char* key, const char* value);
// The diorama (pc_vr_diorama.cpp), every frame: the game camera's heading in the room (radians, 0 =
// straight ahead, positive to the right), so the left stick can be turned to the player's view.
// rotate: the player controls Link freely (no options menu, pause menu or event): only then is the
// stick turned. dialogue: a message box is open (shown on its own panel above the model).
void wq_xr_set_camera_heading(float heading, bool rotate, bool dialogue);
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
