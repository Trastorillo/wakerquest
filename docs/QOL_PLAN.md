# Quality-of-life options (BetterWW features)

Plan written 2026-10-12 from `main` ba8d0a3 for issue #4 ("Quality-of-life options (BetterWW
features)"; the scope is the checklist in the maintainer's comment). Line numbers are from that commit.

BetterWW and wwrando patch `main.dol` and the RELs at fixed addresses, so a patched disc cannot run
here (SwitchWaker compiles the game from the decompilation and refuses a changed `main.dol`). Each
patch is redone in the game's C++ instead, behind a toggle of a new options-menu tab
**"Calidad de vida"** ("Quality of life").

## References

- BetterWW, [github.com/WideBoner/betterww](https://github.com/WideBoner/betterww) at `4501481`
  (2025-07-21): `randomizer.py` (which toggle applies what), `tweaks.py`, `asm/patches/*.asm`,
  `asm/patches/custom_funcs.asm`.
- wwrando, [github.com/LagoLunatic/wwrando](https://github.com/LagoLunatic/wwrando) at `9775811`
  (2026-07-20), the randomizer BetterWW is built on: the same tweaks, some newer.
- Both MIT, copyright LagoLunatic (BetterWW's additions by WideBoner); credited in
  `THIRD_PARTY.md`. No code, asset or text is copied: the asm patches were read to find what each
  one changes, and the same change is written in the decompiled function it patches.

Addresses were mapped to functions with the decomp's `config/GZLE01/symbols.txt` (main.dol) and, for
REL offsets (file offsets in BetterWW), the REL's section table plus `config/GZLE01/rels/<rel>/
symbols.txt`; the split assembly (`build/GZLE01/asm`, `build/GZLE01/<rel>/asm`) shows the patched
instruction, matched to the source line below.

## Rules

- Every option is a `settings.ini` key (`COS_QOL_*`), **off by default**: a fresh install plays the
  original game. Off, every changed line takes its original branch (the original code stays in the
  `#else` of a `#if TARGET_PC` hunk, or the new condition is `pc_qol(...) || <original>` /
  `pc_qol(...) ? <new> : <original>`), so the off state is the original behaviour.
- All of them switch live (the menu's apply function sets the value the game reads); none needs a
  restart. A change applies from the next time the game reads it (e.g. the grappling hook's next
  throw, the next text box).
- `native/include/pc/pc_qol.h` / `native/src/pc/features/pc_qol.cpp`: `pc_qol(PcQol)` returns the
  option's value (0 = off). Hunks in `game/` are one-line calls or conditions, marked with a comment
  naming the option, the BetterWW/wwrando patch and the original value.
- Nothing here writes the save beyond what the game itself writes (see "Saves").

## The items

| # | Item (checklist) | Key | BetterWW / wwrando | Decomp change |
|---|---|---|---|---|
| 1 | Instant text boxes + hold B to skip text | `COS_QOL_INSTANT_TEXT` | `tweaks.make_all_text_instant` (every BMG message: initial draw type 1; wait commands `1A 07 00 00 07`, `..04` and `..03` removed, `..04` kept in messages 7726 and 2488) + `b_button_skips_text.asm` (7 B-trigger reads become B-held) | `fopMsgM_msgGet_c::getMesgEntry` / `fopMsgM_itemMsgGet_c::getMesgEntry` (`f_op_msg_mng.cpp:1505`, `1571`): the entry's `mDrawType` is 1. `fopMsgM_msgDataProc_c::stringSet` (`f_op_msg_mng.cpp:3873-3920`, the only parser of those codes): tags UNK03/UNK04/WAIT are skipped as if removed (UNK04 kept for 7726, 2488). B held instead of triggered in `dMsg_CHECK_TRIG_AB()` at the five patched uses (`d_msg.cpp`: `dMsg_stopProc` 2374, `dMsg_selectProc` 2431, `dMsg_continueProc` 2747, `dMsg_closewaitProc` 2923, `dMsg_finishProc` 2962) and `dMesg_outwaitProc` 2532 / `dMesg_closewaitProc` 2575 (`d_mesg.cpp`) |
| 2 | Faster rolling / movement speed | `COS_QOL_FAST_ROLL` | `tweaks.increase_player_movement_speeds`: roll speed `walk * 6/17 + 20` instead of `walk * 1.5 + 0.5` (`daPy_HIO_roll_c0::m` +0x18, +0x1C); its crawl part is item 5 | `daPy_lk_c::procFrontRoll_init` (`d_a_player_main.cpp:6837`, `6844`): the two roll constants |
| 3 | Faster block moving | `COS_QOL_FAST_BLOCKS` | `tweaks.increase_block_moving_animation`: push/pull animation 1.0 -> 1.4 (`daPy_HIO_pushpull_c0::m` +0x8, +0x10); every block type's push/pull time 20 -> 12 frames (`daObjMovebox::Act_c::M_attr` m04, m0A) | `procPushMove_init` / `procPullMove_init` (`d_a_player_pushpull.inc:129`, `172`); `daObjMovebox::Act_c::mode_wait` (`d_a_obj_movebox.cpp:1602-1606`). **Not** `dProcPushMove_init` (`d_a_player_dproc.inc:1507`, the event push of doors): the shared constant made Forsaken Fortress's big door softlock in wwrando (`fix_forsaken_fortress_door_softlock` patches the room's collision); here the door push keeps the original speed and no stage data is touched |
| 4 | Faster grappling hook | `COS_QOL_FAST_GRAPPLE` | `tweaks.increase_grapple_animation_speed` | `new_himo2_move` (`d_a_himo2.cpp`): throw speed 20 -> 40 (1094), first-person extension 40 -> 20 frames (982), third-person 20 -> 10 (1051), fall onto the target 10 -> 20 (1364), wrap speed constant 17 -> 25 (1367), wrap counter +1 -> +6 (1371) |
| 5 | Faster crawling, climbing and NPC chat zoom | `COS_QOL_FAST_CLIMB` | `tweaks.increase_misc_animations` (climb start 0.8 -> 1.6, ladder/vine climb 1.2 -> 1.6, start 1.0 -> 1.6, end 0.9 -> 1.4, sidle 1.6 -> 2.0, talk camera 20 -> 10 frames) + the crawl part of `increase_player_movement_speeds` (crawl 3.0 -> 6.0) | `d_a_player_hang.inc:604` (`mWallCatch` +0x1C), `d_a_player_ladder.inc:19` (+0x24), `:93` and `d_a_player_climb.inc:213` (+0x4), `d_a_player_ladder.inc:175` (+0xC), `d_a_player_whide.inc:27` (`mWall` +0x14), `d_a_player_crawl.inc:34` (`mCrouch` +0x3C); `dCamera_c::talktoCamera` (`d_camera.cpp:4012`) |
| 6 | Faster Ballad of Gales (no warp cutscene) | `COS_QOL_FAST_BALLAD` | `ballad.asm` (BetterWW): no wait for the boat to rise 5000 units before the warp, no warp music | `daShip_c::procTactWarp` (`d_a_ship.cpp`, the `m03F4 + 5000.0f` check), `daShip_c::procTactWarp_init` (`fopAcM_seStartCurrent(this, 0x186E, 0)`). The arrival (falling out of the cyclone) is kept, as in BetterWW (wwrando's `remove_ballad_of_gales_warp_in_cutscene` changes 49 rooms' spawn data and is commented out there) |
| 7 | Remove the title / ending videos | `COS_QOL_SKIP_VIDEOS` | `tweaks.remove_title_and_ending_videos`: `title_loop.thp` and `end_st_epilogue.thp` replaced by a blank video | `dScnTitle_Execute` (`d_s_title.cpp:73`, `86`): the movie counts as finished at once, so the title scene goes back to the opening and the ending scene goes on as when the epilogue ends. BetterWW's same toggle also applies `skipintro.asm` (skips the opening legend), `misc_rando_features.asm` and `fix_vanilla_bugs.asm`: randomizer changes outside this item, **not** done |
| 8 | Reveal the full sea chart | `COS_QOL_REVEAL_CHART` | `reveal_sea_chart.asm`: `dSv_player_map_c::init` sets bit 0 of all 49 sectors on a new game | See "Saves": the chart menus (`d_menu_fmap.cpp`, `d_menu_fmap2.cpp`) read every sector as charted while the option is on; the save is not written |
| 9 | Tingle chests without the Tingle Tuner | `COS_QOL_TINGLE_CHESTS` | `tingle_chests_without_tuner.asm` (BetterWW: no GBA link checks, any bomb, Tingle Bomb or not; dungeon map and minimap show the chests with the compass) | `daAgbsw0_c::ExeSubT` (`d_a_agbsw0.cpp:1153-1157`); `dMenu_Dmap_c::treasureSet` (`d_menu_dmap.cpp`, the current floor's `tboxNo != 0xF && != 0x10`, the one check the patch removes), `dMap_c::drawPointGc` (`d_map.cpp:3319`) |
| 10 | Swift Sail / Brisk Sail | `COS_QOL_SAIL` (`0`, `swift`, `brisk`) | `swift_sail.asm`, `brisk_sail.asm`, `tweaks.make_sail_behave_like_swift_sail` / `_brisk_sail`: wind always behind the boat (rounded to 45 degrees) while sailing; every 55.0 and 80.0 of `d_a_ship.rel` doubled (sailing and initial speed); A-stop deceleration 1.0/0.1 -> 2.0/0.2 (brisk 3.0/0.2); idle deceleration 1.0/0.05 -> 2.0/0.1 (brisk 4.0/0.2); R cruise 10 -> 30; `decrementShipSpeed` 0.1/0.015 -> 0.2/0.03; `firstDecrementShipSpeed` 5.0/1.0 -> 10.0/2.0 | `daShip_c` (`d_a_ship.cpp`): `execute` (4353, after `mDoAud_setShipSailState(1)`: `dKyw_tact_wind_set`), `setSelfMove`, `procWait`, `decrementShipSpeed`, `firstDecrementShipSpeed`, and the `55.0f` / `80.0f` literals (one pooled constant each in the REL: 945, 1148, 1657, 1661-1662, 2274, 2605, 2646, 2648, 3808, 4101, 4275). The new sail textures, icon and the item name "Swift Sail" are BetterWW assets: not done (the sail looks and is named as in the original) |
| 11 | Turn while swinging or grappling | `COS_QOL_SWING_TURN` | `swing_turn.asm` + `custom_funcs.asm` `turn_while_swinging` (BetterWW): in `procRopeSwing`, while swinging, the main stick's X turns the player by `64 * x` units a frame (none under 25 %) | `daPy_lk_c::procRopeSwing` (`d_a_player_rope.inc`, the last `else` before `f30_3 > 0.6f`); the grappling hook's swing is the same procedure |
| 12 | No song replays | `COS_QOL_NO_SONG_REPLAY` | `song_no_replay.asm` (BetterWW): `procTactPlay` stops waiting for the "You conducted..." box to close and for the conducting animation to end | `daPy_lk_c::procTactPlay` (`d_a_player_tact.inc:605`, `666`) |

## Saves

- Item 8: BetterWW sets the 49 "charted" bits (`mFmapBits[i]` bit 0, `isSaveArriveGrid`) in
  `dSv_player_map_c::init`, i.e. only in new games, and they are then saved for good. Here the option
  does not write anything: while it is on, the two sea chart menus read every sector as charted;
  off, they show the save's own bits again. Sectors the player really reaches are still recorded as
  in the original. The other reader of those bits, the fishmen (`d_a_npc_so.cpp:625`, `1195`: a
  fishman offers to draw the sector only if it is not charted yet), keeps reading the save, so they
  still draw their sectors (with BetterWW they would not, since the bits are set).
- Item 10: the wind direction follows the boat through `dKyw_tact_wind_set`, the function the Wind's
  Requiem uses: like the song, it stores the direction in the save's wind fields (`dComIfGs_setWindX/Y`),
  which every save keeps anyway. Turning the option off leaves the last wind until the song changes it.
- No other item touches save data.

## Notes and choices

- Item 1's B: BetterWW turns 7 reads of "B triggered" into "B held"; the other B checks (number
  input, `dMsg_outnowProc`, the instant-finish check with `mDrawType == 0`) are unchanged, as there.
- Item 5's crawl constant belongs to wwrando's "movement speeds" tweak; the checklist (and BetterWW's
  label) names crawling with climbing, so it is under item 5.
- Item 7: the THP actor is still created (as with the blank video); its first frame may be drawn.
- Item 9: wwrando's newer version also lets any explosion hit (not only a bomb actor); BetterWW's
  (bombs, Tingle Bombs too) is followed. Only the current floor's check in `treasureSet` is removed,
  as in both patches; the floors above and below still hide Tingle chests.
- Item 10 doubles every use of the two pooled constants, as the REL patch does (the tornado pull,
  the turn rate's and the effects' speed scale, a demo's target speed), so the boat handles as with
  BetterWW. One use is left out: `setWaveAngle`'s `static cXyz local_left(80.0f, ...)`, initialised
  once on first use (a live option cannot change it) and paired with `local_right(-80.0f, ...)`, a
  different constant the patch leaves at -80: doubling only one side there is a side effect of the
  patch, not a sail behaviour.

## Verification (Mac)

Per item, a targeted run with the option off and on (`--env COS_QOL_...=1`), measured from the log
(`COS_TRACE=qol`: `native/src/pc/harness/smokes/pc_qol_probe.cpp` prints the player's procedure,
position and speed, the boat's speed, the message box status and the talk camera's timer) or shots;
the regression (`native/tools/regress.sh`) at checkpoints and at the end with every option off and
with every option on; the Switch NRO built. Console tests are left to the user.
