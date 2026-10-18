# Button remapping (issue #15) — design

Status: design, 2026-10-19. Branch `feature/button-remap`.

## Why

Issue #15: a player installed a texture mod that draws the on-screen button prompts with Switch-style
letters. SwitchWaker maps the controller by **position** (the GameCube layout: the bottom face button,
labelled B on a Switch controller, is the GameCube's A; the right one, labelled A, is the GameCube's
B), so the prompt "A" no longer matches the button labelled A. The player asked for button
remapping. The user (project owner) decided to build a general per-button remapper, not just a
preset swap.

Success: a player can make any physical button do any GameCube button's action from the options
menu, see it take effect at once, keep it across restarts, and never lock themselves out.

## Goals

- **Per-button remapping** of the GameCube's twelve digital actions: A, B, X, Y, Z, L, R, Start,
  D-pad Up, Down, Left, Right.
- **"Press the button you want"** assignment: the player picks an action, the screen says
  "Pulsa el botón para A" (English text in the English variant of the menu strings, as the other rows
  have), the player presses a physical button, and it is assigned.
- **Swap on conflict**: if the pressed button already does another action, the two actions trade
  buttons. There is always exactly one physical input per action and no input with two actions.
- **Live**: applies at once, no restart; works with controllers connected later (hot-plug, Joy-Con
  attach/detach).
- **Persistent**: saved in `settings.ini` with the other settings.
- **Restore defaults** row: one action brings back today's mapping.

## Non-goals

- The sticks (main stick, C stick) are not remappable; their axes stay as they are.
- No per-controller or per-player profiles: one mapping applies to every connected controller.
- No change to the game's on-screen prompts (they stay the GameCube's letters; matching them to the
  player's buttons is exactly what the remap is for).
- No remapping of the options menu's own navigation.
- No keyboard remapping on the Mac (Aurora's keyboard bindings stay as they are).

## Anti-goals (would make it a failure even if it "works")

- A player ending up unable to open the options menu or to undo a remap.
- A capture that assigns the button the player used to confirm the row (the A that opened the
  "press a button" prompt must not become the assignment).
- Any change to the default behaviour for players who never open the remap rows.

## Lock-out protection (decided with the user)

1. **Minus is never assignable** (and is not a GameCube action): it always opens the options menu on
   the Switch. Pressing Minus during a capture cancels it.
2. **The options menu reads the controller itself** (`pc_menu.cpp`, `readGamepads`: SDL gamepad
   buttons by their printed labels, D-pad/stick, L/R for tabs), not through the game's PAD mapping.
   This must stay so: the remap changes only what the game sees.
3. **"Restaurar controles por defecto"** row in the controls section.
4. **Safe capture**: the prompt waits until every button is released before it listens; it cancels
   itself after about 5 seconds without a press; Minus cancels.
5. **Outside the game**: the mapping lives in its own `settings.ini` lines; deleting them restores
   the defaults. Documented in the README's options section.

Swap-on-conflict means no "broken" state exists: at worst an inconvenient one, fixable from the menu.

## Physical inputs that can be assigned

On the Switch (all controllers appear to SDL as a Pro Controller through the libnx shim):
A, B, X, Y (by printed label), L, R, ZL, ZR, Plus, D-pad Up/Down/Left/Right, left and right stick
clicks. Minus is excluded. On the Mac/Linux: the same SDL gamepad buttons and the two trigger axes,
with BACK (Minus/Select) excluded on every platform for simplicity.

ZL and ZR are analog **axes** in SDL (LEFT_TRIGGER/RIGHT_TRIGGER), not buttons, and today they feed
the GameCube's analog L and R. The capture must accept them, and the mapping must handle:
- a trigger axis assigned to a digital action (e.g. ZL → A): pressed past a threshold counts as the
  button;
- a digital button assigned to L or R (e.g. ZL ↔ L swapped away to a face button): pressed means the
  digital L/R bit **and** a full analog value, so the game's light-press logic sees a full press.

Buttons that today give no GameCube action (physical L, L3, R3 on the Pro Controller table) are
**free**: assigning an action to a free button moves the action there and leaves its old button free
(no swap, since nothing is displaced). Every action still has exactly one input. *(Builder-check call:
nothing in the repo reads those buttons today; the debug server reads HID directly.)*

How this is achieved inside Aurora (its `PADButtonMapping`/`PADAxisMapping` per controller, or a
filter in our own layer) is left to the builder, with the constraint below.

## Which controllers (builder-check call)

Aurora keeps one default table per controller type (`__PADSetDefaultMapping` in `pad.cpp`): the
GameCube adapter has L/R on MISC3/MISC4, the NSO GameCube controller has Z on BACK. The remap is
stored by **physical position in the Pro Controller layout** (South/East/West/North, shoulders,
triggers, Plus, D-pad, stick clicks), shown with the letters printed on a Switch controller, and
applies to every controller whose Aurora type uses the standard Pro Controller-shaped table (on the
Switch: all of them, the shim reports a Pro Controller; on the Mac: Pro Controller, Joy-Cons, Xbox,
PlayStation, generic). The GameCube adapter and the NSO GameCube controller keep their own tables
untouched (they already match the GameCube's prompts). "Restore defaults" means each controller's own
Aurora table.

## Defaults

Today's mapping (Aurora's Pro Controller table, positional) is the default and what "restore"
returns to: South (B label) → A, East (A label) → B, West (Y label) → X, North (X label) → Y,
R → Z, ZL → L (analog), ZR → R (analog), Plus → Start, D-pad → D-pad. The builder confirms this
against the running Switch build (the Pro Controller table in `aurora-src/lib/dolphin/pad/pad.cpp`
plus anything `cos_switch.cpp` sets) before treating it as the default.

*Builder's note:* that positional table is the Mac's and Linux's default. On the Switch the libnx SDL
shim maps the face buttons by printed label (HidNpadButton_A to SDL's south button), so the same
Aurora table gives each GameCube face button the Switch button with the same letter there: the
Switch's default is label identity (A → A, B → B, X → X, Y → Y), the rest as above
(`pc::remap::defaultMapFor`).

## Menu (where and how it looks)

- A new tab **"Controles"** in the options menu, after Gráficos (or, if a new tab is awkward, a block
  at the top of an existing tab: builder's call, but the rows stay together). The existing camera
  options (invert X/Y, camera shake) may move there; builder's call, keeping their keys.
- One row per action: `A ........ botón B` style, the right side naming the physical button the way
  it is printed on a Switch controller (A, B, X, Y, L, R, ZL, ZR, +, cruceta ↑, L3...).
- A (confirm) on a row opens the capture: the row shows "Pulsa un botón... (− cancela)" with the
  seconds left; on a press, the row (and the swapped row, if any) update; a short note says
  "Intercambiado con X" when a swap happened.
- Last row: "Restaurar controles por defecto" (asks nothing; it is cheap to redo).

## Storage

`settings.ini` lines, one per action, e.g. `COS_PAD_A=b`, `COS_PAD_B=a`, ... (names and value
spelling are the builder's call; values name the physical input, not an SDL enum number, so the file
is readable and stable across SDL versions). Missing lines mean the default for that action; any
invalid or duplicate set falls back to the full default mapping with a log line (never a half-applied
map). Lines are written only for actions that differ from the default, so untouched players' files
gain nothing. These keys are not environment-locked in practice on the Switch; on the Mac they follow
the existing env-var rule of `pc_settings.h`.

Aurora's own controller-mapping files (`PADSerializeMappings`, `*.controller` in the user dir) are
not used for this; if Aurora loads such a file at start it must not override ours (the builder
checks).

## Code placement

Fork code in its own module (`native/src/pc/features/pc_button_remap.cpp` + header in
`native/include/pc/`), small marked hooks elsewhere; the menu rows in `pc_menu.cpp` next to the
others. Aurora is patched only if its API cannot express the mapping (then as a numbered patch under
`switch/native/aurora/patches` / `native/patches/aurora`, as the others).

## Tests

The existing input scripts (`COS_INPUT`, `PADSetVirtualStatus`) inject GameCube buttons after the
mapping, so they cannot test it. *(Builder-check call:)* the automated test is **Mac-only**, through an
SDL virtual gamepad (`SDL_AttachVirtualJoystick`) that presses physical buttons before Aurora's
mapping, added as a small test-only input path; the Switch side (libnx shim, triggers) is verified by
hand on the console.

- A smoke (`COS_SMOKE=button-remap` or an `#expect` script in the existing `options-menu` smoke):
  drive the menu to remap one action (including a swap), check the game receives the new button
  (e.g. the remapped button opens the pause menu, or a driven PAD read), check the settings file
  lines, restore defaults and check again. Must fail without the feature's mapping code.
- Unit-level check of the swap/validation logic if it is separable (cos_pc_tests).
- Console check by the user: remap A/B to match a Switch-letter layout, play a minute (talk, roll,
  menus), restart and confirm it persisted; restore defaults.

## Left to the builder

- Exact Aurora mechanism (button/axis mapping API vs. our own filter) for the trigger cases.
- Setting key names and value spelling.
- Whether the camera rows move into the new tab.
- The trigger threshold for an axis used as a button.
