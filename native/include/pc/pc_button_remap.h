/*
 * pc_button_remap.h - button remapping (issue #15, docs/BUTTON_REMAP_DESIGN.md): which physical input
 * of a controller drives each of the GameCube pad's twelve digital actions (A, B, X, Y, Z, L, R,
 * Start and the D-pad).
 *
 * One setting per action, COS_PAD_A ... COS_PAD_RIGHT (tab PC_SETTING_TAB_CONTROLS, flag
 * PC_SETTING_REMAP), whose value names the physical input by the letter printed on a Switch
 * controller: a (East), b (South), x (North), y (West), l, r, zl, zr, plus, up, down, left, right,
 * lstick, rstick (native/src/pc/features/pc_button_remap_map.h). The settings file holds a line only
 * for an action that differs from the default (today's mapping: by position on the Mac and Linux, by
 * printed label on the Switch, pc::remap::defaultMap); a set that is not valid (an unknown
 * value, an input used twice) applies the whole default map, with one "[cos] button-remap:" line.
 *
 * The map in effect is written into Aurora's per-controller button and axis mappings of every
 * controller whose type uses the standard (Pro Controller-shaped) table: Pro Controller, Joy-Con
 * pair, Xbox, PlayStation, generic. The GameCube adapter, the NSO GameCube controller, a single
 * Joy-Con and unknown types keep Aurora's own table. While the map is the default every controller
 * gets Aurora's own default table back (PADRestoreDefaultMapping). The options menu reads SDL
 * directly (pc_menu.cpp, readGamepads), so the remap never changes its navigation.
 *
 * All functions run on the game thread.
 */
#ifndef PC_BUTTON_REMAP_H
#define PC_BUTTON_REMAP_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Gamepad;

/* Registers the twelve COS_PAD_* settings and subscribes to them (a change marks the map dirty).
   Once, after the options menu's built-in settings (pc_main.cpp). */
void pc_button_remap_init(void);
/* Once per game frame: when a setting changed, re-reads and validates the twelve values; when they
   changed or the controller on any port 0-3 changed (hot-plug, Joy-Con attach), applies the map to
   the controllers. */
void pc_button_remap_frame(void);
/* Gives `action` (pc::remap::Action) the physical `input` (pc::remap::Input), swapping with the
   action that held it, and saves the changed actions (an action back on its default input loses its
   line). *swappedAction (may be NULL): the action that took the old input, or -1. 0 on success, -1
   if an argument is out of range or a changed action's key is locked by the environment. */
int pc_button_remap_assign(int action, int input, int* swappedAction);
/* Every action back to its default input: the twelve lines leave the settings file. */
void pc_button_remap_restore_defaults(void);
/* The first assignable input (pc::remap::Input, in its order) held on `gamepad`: a button, or ZL/ZR
   when its trigger axis is past 16000, read through the same per-platform table the map is written
   with; -1 when none is (Minus/BACK is never one). The options menu's capture (pc_menu.cpp). */
int pc_button_remap_held_input(struct SDL_Gamepad* gamepad);
/* The input (pc::remap::Input) that drives `action` now; -1 for an action out of range. */
int pc_button_remap_current(int action);

#ifdef __cplusplus
}
#endif

#endif /* PC_BUTTON_REMAP_H */
