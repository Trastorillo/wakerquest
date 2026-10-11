# The European disc (GZLP01): plan

Issue #5. Status: **plan**, 2026-10-12, branch `feature/pal-disc`. The USA disc (GZLE01 revision 0)
stays the reference build: every change below keeps the USA build's output and checks unchanged.

## What is already there

- The decompilation covers the PAL version: `VERSION_PAL` (3) and `VERSION_SELECT(...)` branches in
  131 files of `game/`, and the decomp's `config/GZLP01` (`config.yml`, `symbols.txt`, `splits.txt`,
  the SHA-1 of main.dol and of each REL).
- `COS_VERSION` and `COS_GAME_ID` in `native/cmake/GameConfig.cmake`.
- Runtime assets (docs/RUNTIME_ASSETS.md): the binary holds no game data, the arrays come from the
  player's disc at start-up, so an EUR binary can be built from stubs as the USA one is.
- Aurora keeps memory card files per region (`EUR/Card A/` for a `GZLP` disc), so USA and EUR saves
  never mix.
- `cos_switch_system_language` (Switch) and `COS_LANG` / `LC_ALL` / `LANG` (Mac) already pick the
  options menu's language.

## Decisions

| Topic | Decision |
|---|---|
| Builds | The game version is a compile-time constant (`VERSION`), so EUR is a second build: `-DCOS_VERSION=3` (the `COS_GAME_ID` follows: `GZLP01`). Mac: its own build directory (e.g. `build/native-mac-eur`). Switch: `switchwaker-eur.nro` (NACP name "SwitchWaker (EUR)"), in the same `sdmc:/switch/switchwaker/` folder as `switchwaker.nro`: settings, pipeline caches and logs are shared (the logs are named after the NRO), saves are separate (Aurora's region folders). |
| Disc file | `sdmc:/switch/switchwaker/GZLP01.iso` for the EUR NRO (`GZLE01.iso` for the USA one), so both discs can sit side by side. Revision 0, uncompressed, as for USA. |
| Disc ID in code | One place: the build defines `COS_GAME_ID_STR` ("GZLE01" / "GZLP01") for the harness; the disc checks, messages, the default path on the Switch and the smoke tests use it instead of the literal. |
| Language | PAL has 5 languages: English, German, French, Spanish, Italian (`dComIfGs_getPalLanguage()` 0-4). A new setting `COS_GAME_LANGUAGE` (EUR build only): `auto` (default) = the console's system language on the Switch, the OS locale on the Mac (`COS_LANG`, `LC_ALL`, `LANG`, then the macOS preferred languages); any language other than the five = English. Explicit choices `en`/`de`/`fr`/`es`/`it`. Takes effect at the next start (the game loads its message archives once, in the logo scene), so it is `PC_SETTING_RESTART`. The game's own choice (the language byte the memory card's save carries, else `OSGetLanguage`) is replaced by this setting at the same point (`d_s_logo.cpp` phase_1), under `TARGET_PC`. The menu tab: a new "Juego / Game" tab is out of scope; the row goes in the first tab (Gráficos) to keep the change small (the QoL lane may add a game tab later; then it moves there). |
| 50/60 Hz | Always 60 Hz (EuRGB60), never the PAL 50 Hz mode: under `TARGET_PC` the PAL build selects the EuRGB60 render mode at `mDoMch_Create` (what the game does after the player says yes) and the logo scene never shows the refresh-rate prompt (`checkProgSelect`). The 50 Hz render mode (`VI_TVMODE_PAL_INT`, a 534-line XFB) is never used. |
| USA build | Unchanged: every new branch is `#if VERSION == VERSION_PAL` (game code) or reads the disc ID macro; the USA build's checks (regress.sh) must still pass. |

## Steps (one commit each, after its check)

1. **This plan.**
2. **Build configuration.** `GameConfig.cmake`: `COS_GAME_ID` derived from `COS_VERSION` (an explicit
   value still wins), `COS_GAME_ID_STR` defined for the harness. `gen_assets.sh --version GZLP01`,
   `gen_asset_stubs.py --version`, `check_runtime_assets.py --version`. `pc_check.cpp`'s static
   assert accepts USA or PAL. Then make the EUR Mac build compile (`VERSION=3`): whatever the PAL
   branches need that the port never compiled (SDK names, harness glue, layout checks).
   Check: USA build unchanged (configure + build), EUR build links.
3. **Disc check.** `pc_disc.cpp`, `pc_main.cpp`, `pc_assets.cpp`: the expected ID from
   `COS_GAME_ID_STR`; messages (English and Spanish on the Switch) naming the disc and the path
   (`GZLP01.iso` / `GZLE01.iso`); `cos_switch.h`'s default disc path per build.
   Check: EUR binary with the PAL disc boots to the title; with the USA disc it exits 14 with the
   message, and the other way round.
4. **Runtime assets for GZLP01.** Stubs from `config/GZLP01`; `COS_SMOKE=assets` +
   `check_runtime_assets.py` against dtk's split of the PAL disc (`build/GZLP01/bin/assets`):
   every array identical. `disc_manifest.py`: the GZLP01 revision 0 hashes (image and main.dol).
5. **60 Hz, no prompt.** As decided above. Check: no prompt at boot, EuRGB60 render mode logged.
6. **Language.** The setting, system language detection, the override. Check: boot in each of the
   five languages (title, file select, a dialogue), screenshots.
7. **Review of the PC fixes against PAL.** Every `TARGET_PC` change in a file that has PAL branches
   (about 60 files): is the fix also applied on the PAL side of each `VERSION_SELECT` / `#if`? The
   crash fixes (B-series), the 60 fps interpolation (pc_fps60_*), camera shake / heat haze options,
   the tool watch, widescreen (HUD offsets), the movie player (`VIGetTvFormat` with EuRGB60).
   Hardcoded GZLE01 offsets in native/ (comments only so far). Fix what differs.
8. **Regression tools.** `regress.sh`, `run.sh`, `disc_manifest.py`, the sweeps (event, item, save,
   tool, boot, room, actor): pick the disc ID from the build (`COS_BUILD_DIR`'s `COS_GAME_ID`) or the
   disc, PAL message/save names (`EUR/Card A/01-GZLP-...gci`), targets that do not hold on PAL marked.
   Check: `regress.sh` passes for USA (unchanged) and for EUR.
9. **Switch build.** `build_native.sh --eur` (stubs from config/GZLP01, `switchwaker-eur.nro`, its own
   build directory); `package_switch.sh`, `make_sd.sh`, `guard_nro.py` for both. Check: the NRO builds.
10. **Docs.** README, INSTALL.md (which disc, file name, which NRO), docs/SWITCH_BUILD.md,
    native/README.md, this file's status.

## Verification (Mac)

- EUR build with the PAL disc: title screen, file select, Outset, the sea, a dungeon, events and
  cutscenes; each of the five languages.
- The event / item / save / tool sweeps adapted for PAL where practical.
- Full regression (`regress.sh`) at checkpoints and at the end, for USA and EUR.
- USA build: its full regression passes, unchanged.

## Pending after this branch (needs the user)

- Console tests of `switchwaker-eur.nro` (boot, languages, 60 Hz, saves next to USA saves).
- Release packaging: two NROs in one release, and the HOME-menu forwarder (it starts
  `switchwaker.nro`; an EUR forwarder or a choice is a separate decision).

## Risks

- PAL-only code paths the port never compiled may reference things the port replaced or stubbed
  (memory card language file `load2`/`LoadSync2`, movie player TV format).
- Data offsets: fixes keyed to GZLE01 addresses would silently miss on PAL; the review (step 7)
  looks for them.
- PAL text is longer in some languages: widescreen HUD offsets were tuned on USA layouts.
