# Button remapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-button remapping of the GameCube's twelve digital actions from the options menu ("press the button you want", swap on conflict), live, saved in `settings.ini`, with no way to lock oneself out (issue #15).

**Architecture:** A pure, unit-tested map module (actions → physical inputs, swap logic, parsing) under `native/src/pc/features/`; a runtime module that keeps one setting per action, rebuilds each standard controller's Aurora button/axis mapping once per frame when the settings or the connected controllers change; one Aurora patch so a trigger axis (ZL/ZR) can act as a digital button; a new "Controles" tab in `pc_menu.cpp` with remap rows, a capture state that reads SDL directly, and a restore row. Tested on the Mac through an SDL virtual gamepad that presses physical buttons before Aurora's mapping.

**Tech Stack:** C++20, SDL3 gamepads, Aurora (`dolphin/pad.h`, `lib/dolphin/pad/pad.cpp`), ImGui, the repo's `cos_pc_tests` and `COS_SMOKE` harness.

**Spec:** `docs/BUTTON_REMAP_DESIGN.md` (read it first).

## Global Constraints

- Work in the dev worktree `~/Documents/native-port-migrate/build/lanes/dev`, branch `feature/button-remap`. One commit per verified task; never push.
- Mac build: `ninja -C build/native-mac switchwaker` (the `switchwaker` target, or the binary stays stale). Runs: `COS_DISC=~/Documents/windwaker/GZLE01.iso native/tools/run.sh ...`; the run's log is `<run dir>/run.log`.
- After adding an Aurora patch: `rm -rf build/native-mac/_deps/aurora-src build/native-mac/_deps/aurora-subbuild` before rebuilding (same for `build/native-mac-eur`).
- Fork code in its own files; changes to existing files are small and marked (`#if TARGET_PC` in `game/`, comments naming issue #15 elsewhere). Comments in English, in the style of the surrounding code.
- The twelve actions: A, B, X, Y, Z, L, R, Start, D-pad Up, Down, Left, Right. Minus (SDL `BACK`) is never assignable, on every platform.
- Assignable physical inputs, named by the letter printed on a Switch controller: A (East), B (South), X (North), Y (West), L, R (shoulders), ZL, ZR (trigger axes), + (Start), D-pad Up/Down/Left/Right, left/right stick click.
- Default map (today's Pro Controller table): A→B-button (South), B→A-button (East), X→Y-button (West), Y→X-button (North), Z→R, L→ZL, R→ZR, Start→+, D-pad→D-pad. L, left stick click and right stick click are free by default.
- Swap on conflict; assigning to a free input moves the action and frees its old input. Every action always has exactly one input; no input carries two actions.
- `settings.ini` gets a line only for an action that differs from the default; "restore" removes them all. An invalid set (unknown value, duplicate input, missing action after parsing) applies the full default map with one `[cos] button-remap:` log line.
- Applies to controllers whose Aurora type is `PAD_TYPE_SWITCH_PROCON`, `PAD_TYPE_JOYCON_PAIR`, `PAD_TYPE_STANDARD`, `PAD_TYPE_XBOX360`, `PAD_TYPE_XBOXONE`, `PAD_TYPE_PS3`, `PAD_TYPE_PS4`, `PAD_TYPE_PS5`. Others (GameCube adapter, NSO GameCube, single Joy-Con, unknown) keep Aurora's own table. While the map equals the default, every controller keeps Aurora's own table (`PADRestoreDefaultMapping`).
- The options menu keeps reading SDL directly (`readGamepads`); the remap must never change its navigation.
- Capture: listens only after every button is released; cancels after 5 s without a press; Minus cancels.
- Menu text in Spanish with English variants (`T(es, en)`), like the existing rows.

## Review Focus

1. A controller connected after the remap was set (Joy-Con attach, a pad plugged in on the Mac) must get the remap without reopening the menu — Task 2 test `late controller`.
2. A hand-edited `settings.ini` with a duplicate or unknown value must start with the full default map and one log line, never a half-applied map — Task 1 `isValid` tests + Task 2 regress line with `--env COS_PAD_A=b --env COS_PAD_B=b`.
3. A GameCube adapter / NSO GameCube controller must keep its own table after a remap (no lost Z or L/R) — Task 2 test `gc adapter untouched`.
4. The button used to confirm a remap row (or any button held when the capture starts) must not be assigned — Task 3 test `confirm not captured`.
5. While a capture listens, D-pad/A/B must assign, not navigate or close the menu — Task 3 test `capture swallows navigation`.

---

### Task 1: The map (pure logic)

**Files:**
- Create: `native/src/pc/features/pc_button_remap_map.h` (header-only, no SDL/Aurora includes, like `pc_dynres_policy.h`)
- Test: `native/check/pc_tests.cpp` (new section; list it in the file's header comment)

**Interfaces:**
- Produces (namespace `pc::remap`):
  - `enum class Action : uint8_t { A, B, X, Y, Z, L, R, Start, Up, Down, Left, Right, Count };`
  - `enum class Input : uint8_t { A, B, X, Y, L, R, ZL, ZR, Plus, Up, Down, Left, Right, LStick, RStick, Count, None = 0xFF };` (A..Y are the printed Switch letters: A = East, B = South, X = North, Y = West)
  - `using Map = std::array<Input, (size_t)Action::Count>;`
  - `Map defaultMap();`
  - `const char* actionKey(Action);` → `"COS_PAD_A"`, `"COS_PAD_B"`, `"COS_PAD_X"`, `"COS_PAD_Y"`, `"COS_PAD_Z"`, `"COS_PAD_L"`, `"COS_PAD_R"`, `"COS_PAD_START"`, `"COS_PAD_UP"`, `"COS_PAD_DOWN"`, `"COS_PAD_LEFT"`, `"COS_PAD_RIGHT"`
  - `const char* inputValue(Input);` → `"a" "b" "x" "y" "l" "r" "zl" "zr" "plus" "up" "down" "left" "right" "lstick" "rstick"`
  - `bool parseInput(const char* s, Input* out);` (exact lowercase match of `inputValue`; false for `"minus"`, `""`, NULL, anything else)
  - `bool isValid(const Map&);`
  - `struct AssignResult { Map map; bool swapped; Action other; };`
  - `AssignResult assign(const Map& m, Action a, Input in);`

- [ ] **Step 1: Write the failing tests** in `pc_tests.cpp`, using the file's existing check helper:
  - `defaultMap()`: `[A]==Input::B`, `[B]==Input::A`, `[X]==Input::Y`, `[Y]==Input::X`, `[Z]==Input::R`, `[L]==Input::ZL`, `[R]==Input::ZR`, `[Start]==Input::Plus`, D-pad actions on the D-pad inputs; `isValid(defaultMap())`.
  - swap: `assign(defaultMap(), Action::A, Input::A)` → `map[A]==Input::A`, `map[B]==Input::B`, `swapped==true`, `other==Action::B`.
  - free input: `assign(defaultMap(), Action::Z, Input::L)` → `map[Z]==Input::L`, `swapped==false`, no action on `Input::R`, `isValid(map)`.
  - same input: `assign(defaultMap(), Action::A, Input::B)` → map unchanged, `swapped==false`.
  - `isValid` false for a map with `Input::A` twice, and for a map with `Input::None`.
  - `parseInput("zl")` → `Input::ZL`; `parseInput("minus")`, `parseInput("")`, `parseInput("A")`, `parseInput(nullptr)` → false; every `inputValue(i)` parses back to `i`.
  - `actionKey(Action::Start)` is `"COS_PAD_START"`.

- [ ] **Step 2: Run, expect a compile failure** (header missing): `ninja -C build/native-mac cos_pc_tests`

- [ ] **Step 3: Implement `pc_button_remap_map.h`** with the interfaces above. `assign`: if another action holds `in`, it takes `a`'s old input (`swapped=true`, `other`=that action); otherwise `a` just moves.

- [ ] **Step 4: Run** `ninja -C build/native-mac cos_pc_tests && build/native-mac/cos_pc_tests` → prints `ok`.

- [ ] **Step 5: Commit** `Button remap: the action/input map and its swap logic (issue #15)`.

---

### Task 2: Settings, Aurora mapping, hot-plug (no menu yet)

**Files:**
- Create: `native/patches/aurora/0022-pad-trigger-axis-buttons.patch` (Mac and Switch both apply `native/patches/aurora`)
- Create: `native/include/pc/pc_button_remap.h`, `native/src/pc/features/pc_button_remap.cpp`
- Create: `native/src/pc/harness/smokes/pc_button_remap.cpp` (smoke `button-remap`, Mac only: SDL virtual gamepads)
- Modify: `native/include/pc/pc_settings.h` + `native/src/pc/features/pc_settings.cpp` (tab, flag, clear API)
- Modify: the per-frame call site next to `pc_settings_poll_mode()` (game thread) and the init call site next to the other `pc_settings_register` callers; `pc_internal.h`, `pc_smoke.cpp` (`{"button-remap", kInGame}`), `pc_frame.cpp`, `native/check/regress_targets.txt`

**Interfaces:**
- Consumes: Task 1's `pc::remap` API.
- Produces:
  - Aurora: `#define PAD_NATIVE_TRIGGER_LEFT 0x10000u` and `#define PAD_NATIVE_TRIGGER_RIGHT 0x10001u` in `include/dolphin/pad.h`; in `PADRead`'s button-mapping loop such a `nativeButton` is pressed when `SDL_GetGamepadAxis(LEFT_TRIGGER / RIGHT_TRIGGER) > 16000` (instead of `SDL_GetGamepadButton`). Nothing else in Aurora changes.
  - `pc_settings.h`: `PC_SETTING_TAB_CONTROLS = 4`, `PC_SETTING_TABS = 5`; `#define PC_SETTING_REMAP 0x10u` (the menu shows the row as a remap row, Task 3); `int pc_settings_clear(const char* key);` — drops the stored value(s) of a registered key (back to its default), writes the file, notifies if the effective value changed; -1 if unknown or locked.
  - `pc_button_remap.h` (C linkage, game thread):
    - `void pc_button_remap_init(void);` registers the twelve `COS_PAD_*` settings (tab `PC_SETTING_TAB_CONTROLS`, flags `PC_SETTING_REMAP`, choices = the 15 input values, `defaultValue` = the default map's value, no apply fn) and subscribes to each (the subscriber only marks the map dirty).
    - `void pc_button_remap_frame(void);` once per game frame: when dirty, re-reads the twelve keys (`pc_settings_get` + `parseInput`), validates (`isValid`; invalid → default map + one log line per change of the invalid set), and applies; also applies when the controller on any port 0–3 changed (compare `PADGetIndexForPort` and `PADGetVidPid` with the last frame).
    - `int pc_button_remap_assign(int action, int input, int* swappedAction);` computes `assign` on the map in effect, then for each changed action `pc_settings_clear` if it equals the default, else `pc_settings_set`; `*swappedAction` = other action or -1; returns 0, -1 if an action key is locked.
    - `void pc_button_remap_restore_defaults(void);` clears all twelve keys.
    - `int pc_button_remap_current(int action);` the input in effect (as `int` of `pc::remap::Input`).
  - Applying to one port (standard types only, see Global Constraints): call `PADGetButtonMappings(port, &n)` first (it loads Aurora's own file if any), then if the map is the default `PADRestoreDefaultMapping(port)` (check it restores axes too; if not, also restore Aurora's default axes), else `PADSetAllButtonMappings` with one entry per action: native button East/South/North/West, `LEFT/RIGHT_SHOULDER`, `PAD_NATIVE_TRIGGER_LEFT/RIGHT`, `START`, `DPAD_*`, `LEFT/RIGHT_STICK`. Exception for actions L and R: if their input is ZL/ZR the button entry is `PAD_NATIVE_BUTTON_INVALID` and `PADSetAxisMapping` puts `PAD_AXIS_TRIGGER_L/R` on that trigger axis (Aurora's `emulateTriggers` gives the digital bit, the light press keeps working); if their input is a button, the button entry is that button and the axis mapping is `{nativeAxis -1, nativeButton = that button}` (Aurora sets the analog value to full when the digital bit is on). A trigger axis that no longer feeds L/R must not reach `PAD_AXIS_TRIGGER_L/R`.

- [ ] **Step 1: Write the smoke** `pc_button_remap.cpp` (header comment in the style of `pc_dmap.cpp`). Mac only (`#if !defined(__SWITCH__)`; on the Switch it exits 1 with a log line). It attaches an SDL virtual gamepad (`SDL_AttachVirtualJoystick`, `SDL_JOYSTICK_TYPE_GAMEPAD`, vendor 0x057E product 0x2009 so Aurora sees a Pro Controller, all buttons and six axes) after the play scene runs, and reads what the game sees the way `pc_input.cpp` does (`mButtonHold`, and the analog L via the same pad object). Each check presses, waits 3 frames, reads, releases, waits 3 frames. Checks, in order, each logging `[cos] button-remap: <name>: ok|FAIL`:
  - `default`: East → game B; South → game A.
  - `swap`: `pc_button_remap_assign(A, Input::A)` → East → A, South → B; the settings file has `COS_PAD_A=a` and `COS_PAD_B=b`.
  - `trigger as button`: `assign(Start, ZL)` → left trigger axis 32767 → Start held and analog L below 10% of its range.
  - `button as L`: (L got + from that swap) START button → L held and analog L above 60%.
  - `free input`: `assign(Z, Input::L)` → left shoulder → Z; right shoulder → no game button.
  - `late controller`: attach a second virtual Pro Controller now; its East → A on its port.
  - `gc adapter untouched`: a virtual pad with vendor 0x057E product 0x0337 (Aurora: GameCube adapter) is attached at the very start, before any remap, and its `PADGetButtonMappings` copied; after the checks above its mappings still equal that copy.
  - `restore`: `pc_button_remap_restore_defaults()` → no `COS_PAD_` line in the file; East → B.
  - Exit 0 if all ok, else 1.
- [ ] **Step 2: Register it** (`pc_internal.h`, `pc_smoke.cpp`, `pc_frame.cpp`), build, run `native/tools/run.sh button-remap --stage Asoko:0:0:2` → expected FAIL/compile errors (module missing).
- [ ] **Step 3: Write the Aurora patch** (`git diff` format against the pinned source, like `0021-*.patch`), delete `_deps/aurora-src` + `aurora-subbuild`, rebuild.
- [ ] **Step 4: Implement** the settings additions and `pc_button_remap.cpp` per the interfaces; call `pc_button_remap_init` where the other modules register settings, `pc_button_remap_frame` next to `pc_settings_poll_mode`.
- [ ] **Step 5: Run** the smoke → `PASS`; also `native/tools/run.sh button-remap --stage Asoko:0:0:2 --env COS_PAD_A=b --env COS_PAD_B=b` → log shows one `[cos] button-remap:` invalid-set line and the `default` check still passes (the smoke skips the later checks when started with an invalid set and exits 0 after `default`; make that branch explicit in the smoke). Run `cos_pc_tests`, `options-menu` and `camera-invert` regress lines (unchanged results).
- [ ] **Step 6: Add regress lines** (both runs above, with a comment naming issue #15) and **commit** `Button remap: per-action settings applied to the controllers, trigger axes as buttons (issue #15)`.

---

### Task 3: The "Controles" tab and the capture

**Files:**
- Modify: `native/src/pc/features/pc_menu.cpp` (tab names/order, `RowKind::Remap`, `RowKind::RemapRestore`, capture state, labels, the menu smoke's `#expect` support if used)
- Modify: `native/src/pc/harness/smokes/pc_button_remap.cpp` (menu stage)
- Modify: `native/README.md` "Options menu" section (and the player README's options list if it lists tabs): the tab, the rows, Minus never assignable, deleting the `COS_PAD_*` lines restores the defaults.

**Interfaces:**
- Consumes: Task 2's `pc_button_remap_*` and `PC_SETTING_REMAP`, `PC_SETTING_TAB_CONTROLS`.
- Produces: tab order on screen Gráficos, Controles, Rendimiento, Calidad de vida, Depuración (a display-order array; the enum values stay as defined). Tab names `"Controles"` / `"Controls"`. Settings with `PC_SETTING_REMAP` become `RowKind::Remap` rows (label = action: `"A"`, `"B"`, ..., `"Cruceta arriba"`/`"D-pad up"` ...; value = input label: `"botón A"`/`"A button"`, ..., `"ZL"`, `"+"`, `"cruceta arriba"`, `"clic stick izq."`/`"left stick click"`); the last row `RowKind::RemapRestore` `"Restaurar controles por defecto"` / `"Restore default controls"`, value `"A: restaurar"` / `"A: restore"`. The camera rows stay where they are.
- Capture state in `Menu`: `int captureAction = -1; bool captureArmed = false; uint64_t captureStartMs = 0;`. Confirm on a Remap row starts it (row value shows `"Pulsa un botón... (− cancela) 4 s"` / `"Press a button... (− cancels) 4 s"`). While `captureAction >= 0`, `handleInput` does nothing else. Each menu frame: read every gamepad with `PADGetSDLGamepadForIndex` (as `readGamepads` does): until no button and no trigger is down, stay unarmed; armed, BACK → cancel; the first of the assignable buttons or a trigger axis > 16000 → `pc_button_remap_assign`, toast `"Intercambiado con X"` / `"Swapped with X"` when swapped; 5000 ms after `captureStartMs` without a press → cancel. After a capture ends, set `m.waitRelease` so the pressed button does not act on the menu.

- [ ] **Step 1: Extend the smoke** with a menu stage after `restore` (virtual Pro Controller, so SDL's labels are the Switch's: East = "A"): open the menu (ZL+ZR+BACK, the Mac combination), move to the Controles tab (R shoulder), stay on row A, and:
  - `confirm not captured`: press East (confirm), hold it 30 frames, release; press West; release → `pc_button_remap_current(A) == Input::Y` and `X` now holds `Input::B`.
  - `capture swallows navigation`: confirm on row B, release, press D-pad Down → `current(B) == Input::Down`, the menu still open on the same row, `Down` action holds B's old input.
  - `cancel minus`: confirm, release, press BACK → map unchanged, menu still open.
  - `cancel timeout`: confirm, release, wait 330 frames → map unchanged.
  - `restore row`: select the restore row, confirm → default map.
  Expected now: FAIL (no tab).
- [ ] **Step 2: Implement** the tab, rows, capture and restore in `pc_menu.cpp` per the interfaces.
- [ ] **Step 3: Run** the smoke → PASS; `options-menu` regress lines → unchanged; screenshot the tab once (`--shot`) and look at it (labels fit, Spanish text).
- [ ] **Step 4: README** updates; **commit** `Options menu: Controles tab with per-button remapping (issue #15)`.

---

### Task 4: Checkpoint and console

- [ ] **Step 1:** Full Mac regress USA (`native/tools/regress.sh`) and the `button-remap` lines on EUR (`COS_BUILD_DIR=build/native-mac-eur COS_DISC=~/Documents/windwaker/GZLP01/GZLP01.iso`). All match.
- [ ] **Step 2:** Switch build `scripts/switch/build_native.sh --runtime-assets` (Docker running), clean.
- [ ] **Step 3:** Ask the user before touching the console; deploy with `scripts/switch/switchwaker_debug.py deploy` after checking `info` (version, region). The user checks: remap A/B to the printed letters, play a minute (talk, roll, menus, ZL/ZR as L/R), Joy-Con attach/detach, restart (persisted), restore defaults.
- [ ] **Step 4:** Merge only when the user says so; reply on #15 then (fixed on main, close at release).
