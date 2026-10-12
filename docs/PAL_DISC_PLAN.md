# The European disc (GZLP01): plan

Issue #5. Status: **implemented and checked on the Mac**, 2026-10-12, branch `feature/pal-disc`
(written first as the plan; what was built and how it was checked: [Implementation](#implementation-2026-10-12)).
Console tests pending. The USA disc (GZLE01 revision 0)
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
| Builds | The game version is a compile-time constant (`VERSION`), so EUR is a second build: `-DCOS_VERSION=3` (the `COS_GAME_ID` follows: `GZLP01`). Mac: its own build directory (e.g. `build/native-mac-eur`). Switch: `switchwaker-eur.nro` (NACP name "SwitchWaker EUR") in its own folder, `sdmc:/switch/switchwaker-eur/` (revised 2026-10-12, see [SD card layout](#sd-card-layout-2026-10-12): hbmenu lists one NRO per folder), reading everything else from `sdmc:/switch/switchwaker/` as `switchwaker.nro` does: settings and pipeline caches are shared, logs are named and pruned per NRO, saves are separate (Aurora's region folders). |
| Disc file | `sdmc:/switch/switchwaker/GZLP01.iso` for the EUR NRO (`GZLE01.iso` for the USA one), so both discs can sit side by side. Revision 0, uncompressed, as for USA. |
| Disc ID in code | One place: the build defines `COS_GAME_ID_STR` ("GZLE01" / "GZLP01") for the harness; the disc checks, messages, the default path on the Switch and the smoke tests use it instead of the literal. |
| Language | PAL has 5 languages: English, German, French, Spanish, Italian (`dComIfGs_getPalLanguage()` 0-4). The console's language takes the place of the GameCube's IPL language (`OSGetLanguage`, through the SDK's SRAM): the Switch's system language, the Mac's preferred locale (`SDL_GetPreferredLocales`); any language other than the five = English, as on the GameCube. A new setting `COS_GAME_LANGUAGE` (EUR build only, `PC_SETTING_RESTART`: the game loads its message archives once, in the logo scene): `auto` (default) keeps the GameCube's order, the language the memory card's save remembers (the PAL file select can change it), else the console's; `en`/`de`/`fr`/`es`/`it` force one, at the logo scene and wherever the game restores a save's language. The row is the last of the first tab (Gráficos: no "Game" tab yet; the menu scripts count rows from the top). |
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

## Implementation (2026-10-12)

| Part | As built |
|---|---|
| Build | `GameConfig.cmake` derives `COS_GAME_ID` from `COS_VERSION` and defines `COS_GAME_ID_STR`; `native/include/pc/pc_game_id.h` names the disc, its region and image. `gen_assets.sh`/`gen_asset_stubs.py`/`check_runtime_assets.py --version GZLP01`. GZLP01's `config.yml` has no module hashes: the stubs take them from the decomp's `build.sha1` (main.dol and the 7 RELs with arrays, checked against the decompressed files of the disc). 167 arrays (163 + the four extra language message tables). |
| Lost PAL code | The zeldaret convergence (step G4) had replaced `d_file_select`, `d_menu_window` and `d_a_bst` with zeldaret's units, which have no PAL branches (45, 48 and 15 PAL conditionals in snrubrm's; 5, 1 and 0 in zeldaret's): the PAL file select's language panes, the menu window's PAL textures and Gohdan's PAL cutscene timings. For VERSION_PAL each converged unit includes the pre-convergence unit (snrubrm's code with our host hunks) from `game/src/pal/`; the USA build compiles the converged code unchanged. Accessor names added to `d_file_select.h` and `d_menu_collect.h`. |
| Host fixes, PAL only | The title's archive name written into a string literal (SIGBUS at the opening); three PAL strings longer than their buffers (Spanish/Italian shot count, German " Ritterwappen", the name entry's "O.K."/"FINE" in a 4-byte "END" string). Found by booting and by a review of every PAL `strcpy`/`strcat`/`sprintf` of a localized literal. |
| Review of the PC fixes | Every `TARGET_PC` block nested in a version conditional: only the three units above had fixes on the USA side alone (handled by `game/src/pal/`). Heaps: every PAL heap size goes through the same host multiplier. 60 fps, widescreen and meter hooks: no message IDs, addresses or file names; the 16:9 meter values are the same in both versions. The profile list (`MSG3`'s place) follows the version. |
| 60 Hz | EuRGB60 render mode at `mDoMch_Create`, the logo scene's question skipped. |
| Tools | `run.sh`/`regress.sh` pass the build's ID to `disc_manifest.py --verify --game` (GZLP01 image and main.dol SHA-1 added); the sweeps follow `COS_BUILD_DIR`; `run.sh` runs the EUR build in English unless `COS_GAME_LANGUAGE` is given; msg-sweep checks all five language archives; the save smoke knows PAL's language word and header strings. |
| Switch | `build_native.sh --eur [--build-dir]`, `docker/build.sh` and `make_sd.sh` follow the disc's ID, `push.sh native-eur`, `guard_nro.py --version GZLP01`, `package_switch.sh --with-eur`. Logs pruned per NRO name. |

Checked on the Mac (EUR build, `-DCOS_VERSION=3`, runtime assets, the Redump-verified GZLP01 image):

- `COS_SMOKE=assets`: 167 of 167 arrays identical to dtk's split of the PAL disc.
- Disc checks: EUR + PAL disc boots; EUR + USA disc and USA + PAL disc exit 14 naming the other build.
- Boot: Nintendo and Dolby logos, opening, title, no 50/60 Hz question even with B held.
- Languages: file select, name entry and a cutscene's text in each of the five (forced); auto with
  an empty card follows the system (es-ES -> Spanish); auto with an English save -> English.
- Stages with 60 fps, camera shake and heat haze on: Outset (with its arrival cutscene), Windfall,
  Dragon Roost Cavern, Forbidden Woods.
- Sweeps: tool 18/18; save 7/7 situations saved and loaded back equal; item 213 ok + 1 expected
  failure (0x16, the same as USA's), 41 not given; event sweep on sea, M_NewD2, Asoko, Omori and
  kindan: 507 results (174 played to their end), no fault signature; the fairy events' crash in
  `setDemoTextureAnime` (gone with flag 0x2D01) happens on the USA build too.
- Switch: `switchwaker-eur.nro` (17,615,328 bytes) and, from the same branch, `switchwaker.nro`
  (17,598,944 bytes) build; `guard_nro.py` finds none of the disc's arrays in either.
- Full regression (`regress.sh`, 82 checks: build, static checks and 76 game runs) passes for both
  builds, EUR and the unchanged USA reference.

## Verification (Mac)

- EUR build with the PAL disc: title screen, file select, Outset, the sea, a dungeon, events and
  cutscenes; each of the five languages.
- The event / item / save / tool sweeps adapted for PAL where practical.
- Full regression (`regress.sh`) at checkpoints and at the end, for USA and EUR.
- USA build: its full regression passes, unchanged.

## Pending after this branch (needs the user)

- Console tests of `switchwaker-eur.nro`: boot with `GZLP01.iso` on the card, the language from the
  console's system language and from the menu row, 60 Hz, a save (`EUR/Card A`) next to USA saves,
  the logs (`switchwaker-eur_*.log`).
- ~~Release packaging~~: resolved 2026-10-12, two downloads ([SD card layout](#sd-card-layout-2026-10-12)).
  CI building the EUR NRO is still open.
- ~~The HOME-menu forwarder starts `switchwaker.nro` only~~: resolved 2026-10-12 as a known limitation:
  the European NRO starts from hbmenu (in title mode); an EUR forwarder (another title ID) or a chooser
  can come later.
- ~~The debug server's `deploy` uploads `switchwaker.nro` only~~: resolved 2026-10-12, `deploy --eur`.
- An HD texture pack built from the USA disc does not cover PAL's localized textures.

## SD card layout (2026-10-12)

Found on the console: hbmenu shows one NRO per folder, so `switchwaker-eur.nro` next to `switchwaker.nro`
in `sdmc:/switch/switchwaker/` never appeared ("SwitchWaker EUR" was missing). The NRO's paths are fixed
at build time (`COS_SWITCH_NRO_DIR` /switch/switchwaker for the bundled caches, `COS_SWITCH_ROOT`
/switch/switchwaker/native, the disc `COS_SWITCH_DEFAULT_DISC`); nothing depends on where the NRO itself
is (argv[0] is only logged; the log names, the `.elf` name and the crash report use `COS_SWITCH_NRO_NAME`;
the debug server's root is fixed; `reload` restarts the forwarder's program). So:

```
sdmc:/switch/switchwaker/        switchwaker.nro (USA), GZLE01.iso, GZLP01.iso, initial_pipeline_cache.db,
                                 initial_dksh_cache.bin, native/ (saves, settings, caches, logs): shared
sdmc:/switch/switchwaker-eur/    switchwaker-eur.nro ("SwitchWaker EUR")
```

- Release: two self-contained downloads, the player picks one: `SwitchWaker-<tag>-switch.zip` (USA) and
  `SwitchWaker-<tag>-switch-eur.zip` (`switch/switchwaker-eur/switchwaker-eur.nro` and the caches in
  `switch/switchwaker/`). The caches are built from the same pipeline database and are byte-identical for
  both builds; `package_switch.sh --with-eur` refuses to package them if they differ, so both zips can be
  extracted onto one card.
- `make_sd.sh`, `push.sh native-eur` and the debug client's `deploy --eur` put the European NRO in its folder
  and the caches in `switch/switchwaker/`.
- The wrong program for the disc on the card (SwitchWaker EUR with only `GZLE01.iso`, SwitchWaker with only
  `GZLP01.iso`, or the other region's disc under this build's file name): the console's message names the
  program to start ("SwitchWaker" / "SwitchWaker EUR") and the end of its download's name, in English and
  Spanish (`pc_disc.cpp`).
- Players who copied `switchwaker-eur.nro` into `switch/switchwaker/` move it to `switch/switchwaker-eur/`
  (INSTALL.md, "Updating").

## Risks

- PAL-only code paths the port never compiled may reference things the port replaced or stubbed
  (memory card language file `load2`/`LoadSync2`, movie player TV format).
- Data offsets: fixes keyed to GZLE01 addresses would silently miss on PAL; the review (step 7)
  looks for them.
- PAL text is longer in some languages: widescreen HUD offsets were tuned on USA layouts.
