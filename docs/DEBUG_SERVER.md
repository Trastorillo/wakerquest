# Debug server (Switch)

A TCP port on the console for the development machine: the log as it is written, files on the SD card,
controller presses, warps, screenshots, and a new build deployed and restarted without touching the console.
It is off unless the options menu turns it on. There is no password, so use it only on your local network. It is
the same server as SwitchWakerHD's (`docs/debug-server.md` there), with this port's commands.

## Turning it on

In the game's options menu (**Minus**): Depuración > **Servidor de depuración (red)** ("Debug server
(network)"), *Activado*. It takes effect at the next start (quit and start the game again, or `reload` once
the server runs). The menu saves it in `sdmc:/switch/switchwaker/native/user/settings.ini` as
`COS_DEBUG_SERVER=1` (port 6543; a port number there instead uses that port). The `native/env.txt` of
earlier builds is moved into that file at the first start of a newer build (`COS_DEBUG_SERVER=1` in it
becomes the menu setting, so the server starts in that same session) and kept as `env.txt.old`.

Other developer variables (those without a menu row: `COS_TRACE`, `COS_BOOT_STAGE`, `COS_DK_*`, ...) go in
the `[dev]` section at the end of the same file
([switch/native/settings-dev.example.ini](../switch/native/settings-dev.example.ini)); they are read at
start. Over the server:

```sh
scripts/switch/switchwaker_debug.py get native/user/settings.ini build/settings.ini
# edit build/settings.ini: add or change lines after "[dev]"
scripts/switch/switchwaker_debug.py put build/settings.ini native/user/settings.ini
scripts/switch/switchwaker_debug.py reload
```

At start the log says
`[switch] debug server listening on <ip>:6543`. A fixed address for the console (a DHCP reservation in the
router) saves looking it up each time. On the Mac, tell the client where the console is, either with
`export SWITCHWAKER_HOST=<ip>` or with the address on the first line of `build/switch_host.txt` (`build/` is
not in git).

## The client: `scripts/switch/switchwaker_debug.py`

| Command | What it does |
|---|---|
| `info` | version, game (`game GZLE01 (USA, build switchwaker)`, `game GZLP01 (Europe, build switchwaker-eur)` for the European build, also installed as `switchwaker.nro`), frame, stage and room, memory, applet type, address |
| `log [--all] [--save F] [--grep RE] [--seconds N]` | the log as it is written; `--all` starts with the last 2 MiB |
| `deploy [NRO] [--eur] [--force] [--no-reload] [--nro-only]` | uploads `build/switch-native/switchwaker.nro` and the `initial_pipeline_cache.db` and `initial_dksh_cache.bin` built next to it, checks each CRC32, restarts the game, waits until it answers again. `--eur` (or an NRO named `*-eur.nro`): `build/switch-native-eur/switchwaker-eur.nro`, uploaded as `switchwaker.nro` as well (one region per SD card; the forwarder starts it, so it reloads like the USA one). Refused when `info`'s `game` line names the other region (the NRO would replace one that matches the console's disc), unless `--force`; a build without that line only gets a note. The guard against SwitchWakerHD's server (same port) applies to both |
| `shot [OUT.png] [--game]` | PNG of the next frame, with the FPS panel and the options menu drawn over it; `--game` gives the game's picture alone |
| `press A [B ...] [ms]` | buttons pressed together (`A+B` is the same); 120 ms by default; replies once released |
| `hold X`, `release [X]` | held until released (`release` alone lets go of everything) |
| `stick L\|R x y [ms]` | stick override, -1..1, y up; `stick L 0 0` lets go |
| `warps`, `warp N`, `warp STAGE [ROOM] [POINT] [LAYER]` | the options menu's travel list, by number or by name; LAYER forces the stage's actor layer (default: the game's choice; e.g. `warp Pjavdou 0 1 8` = Jabun's cave with Jabun, on the boat); applied once a file is being played |
| `event NAME` | orders an event of the current stage's event list (as the Mac's event sweep: the player orders it, every frame until an event runs, 240 frames at most; the log says `event NAME running` or `did not start`). Jabun's cutscene (issue #3): Navegar in the menu, `warp Pjavdou 0 1 8`, then `event getperl_jab` |
| `give ITEM [ITEM ...]` | items through the game's own item-get functions (`execItemGet`, as a chest or a shop gives them: inventory slot, "got it" bit, song bits, bomb count), applied once a file is being played, so the save stays consistent and the game's save screen keeps them. Names: `grapple` (Grappling Hook, 0x25: what "Garfio más rápido" changes), `hookshot` (the Hookshot, 0x2F, a different item), `baton` (Wind Waker), `ballad` (Ballad of Gales), `requiem` (Wind's Requiem), `bombs` (the bomb bag: 30 bombs, room for 30; with bombs already, 30 more instead of setting a bigger bag back to 30), `sail`, `telescope`, `bow`, `map`, `compass` (the current dungeon's), or an item number (`0x25`, `37`). One unknown name refuses the whole command. The log says what the game's own check reports for each item. A new item still has to be put on a button in the item menu |
| `raw time HOUR` | the time of day (0-24, decimals allowed, e.g. `17.5`), the save's time that the game copies to the sky every frame, set once a file is being played. The sun's glare shows from 8 to 18 |
| `raw weather clear` | the sea's random weather back to sunny: a jump in time (`time`) starts a new weather pattern, and every pattern starts cloudy or rainy, which hides the sun's glare. The next roll comes about 12 game hours later |
| `raw take map\|compass ...` | the current dungeon's map and/or compass taken away again (the save bits `give map`/`give compass` set) |
| `audio` | what the game's audio plays, as one line (`native/src/pc/features/pc_audio_state.cpp`): the registered sound effects (`id:state`), the SE tracks still sounding (`t<n>=<id>/<active><command>`; `ORPHAN` with active 1 would be a track with nothing left to stop it), the BGM ids, the takt mute factors, and two counters: `dac-repeat` climbs if the audio thread stalls (the same audio block replayed), `dsp-sync` stops then. For a sound that does not stop: run it twice a few seconds apart and keep the output |
| `get`, `put`, `ls`, `rm`, `mkdir` | SD card files; paths are relative to `sdmc:/switch/switchwaker` unless they start with `/` |
| `logs`, `lastlog [LOCAL]` | the session logs in `native/logs/`; download the newest one. The running session's file cannot be opened while the game writes it, so `lastlog` then takes the text the console keeps in memory (`logtext`, the last 2 MiB, from the first line) |
| `crashes [--fetch DIR]` | Atmosphère's crash reports (`/atmosphere/crash_reports`) |
| `wait`, `quit`, `reload`, `ping`, `help`, `raw CMD ...` | `raw logtext` prints the kept log text |
| `crash` | a deliberate data abort (on the server's thread): checks the crash report in the log and that the process ends a few seconds later |

Button names: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LS RS`. The presses go into the gamepad shim
(`switch/aurora/sdl3_shim/sdl3_shim_gamepad.c`), as if they came from the controller, so they reach the
options menu too: `press MINUS` opens it.

### Testing the QoL options on your own save

The items the "Calidad de vida" options need, without playing to them (`give` uses the game's item-get, so
saving in the game keeps them):

- Garfio más rápido (the Grappling Hook): `give grapple`, put it on a button, then
  `warp M_NewD2 15 18` (Dragon Roost Cavern, room 15): a grapple post (`Kui`) about 850 units straight ahead,
  400 up. Throw at it with the option off and on.
- Balada del vendaval rápida: `give baton ballad`, the baton on a button, on the boat at sea (your own boat, or
  Depuración > "Navegar" in the menu: on the boat at Windfall), then conduct the Ballad of Gales and pick a
  destination. With the option on the warp starts at once (no rise into the cyclone, no warp music).
- Cofres de Tingle sin Tingle Tuner: `give bombs`, then `warp M_NewD2 15 18` again: the Tingle statue mark of
  that room's Tingle Statue chest (switch 0x6f) is on the floor about 1,480 units straight ahead (100 lower).
  A normal bomb on it with the option on starts the chest's appear cutscene (checked on the Mac:
  `DEFAULT_TREASURE_APPEAR`, the chest next to the mark); off, nothing happens. The other four Tingle Statue
  chests: `warp kindan 14 0` (Forbidden Woods, the mark ~1,270 units from the spawn), `M_Dai` room 9, `Siren`
  room 6, `kaze` room 3 (marks at the `agbTBOX` actors of those rooms).

### Known issue: the boat's wave sound stuck after a warp

On the console (dev 0.3.1-32, 2026-10-12) the boat's wave / sailing sound once kept playing after a warp
from the options menu (Viajar to `sea 11 0`), after sailing with "Vela" switched between 0, swift and
brisk from the menu. Not reproduced on the Mac: sailing at speed or stopped, Vela 0, swift and brisk (also
switched from the menu while sailing), then Viajar or Navegar to Windfall and on to an interior
(`COS_TRACE=qol`, `COS_QOL_PROBE_SE`, `COS_QOL_PROBE_WARP`): the boat's sounds end with the boat at the
scene change, nothing is left registered in the interior, and the audio thread keeps mixing. At Windfall
the docked boat itself plays its idle wave (0x3006) while the player is near it, as in the original. If it
happens again: `audio` twice a few seconds apart (on the island, then in an interior), and the log.

A typical loop:

```
scripts/switch/build_native.sh && scripts/switch/switchwaker_debug.py deploy
scripts/switch/switchwaker_debug.py log --save build/session.log &
scripts/switch/switchwaker_debug.py warp 2 && sleep 10 && scripts/switch/switchwaker_debug.py shot
```

`reload` restarts the application (`appletRestartProgram`: the GPU profile is restored and the log written
first, as at an exit), so it only works when the game was started from the HOME-menu forwarder. Started from
hbmenu, use `quit`, then start it again. A game that crashed takes the server down with it: start it again
from the console, then fetch `lastlog` (the crashed session is then the second newest, see `logs`) and
`crashes`.

## Session logs

Since the debug server, the log follows SwitchWakerHD's pattern: one file per session,
`native/logs/switchwaker_<date>_<time>.log` (the console clock at start), and only the 10 most recent
sessions are kept, so the logs cannot fill the SD card. The `switchwaker.log` / `switchwaker.prev.log` of
earlier builds move into `logs/` at the first start.

## Protocol

Plain TCP, one command per line: `name arg arg ...`. An argument with spaces goes in double quotes. Every
reply is `ok <n>\n` or `err <n>\n` followed by n bytes (text, or a file's contents).
`put <path> <size>\n` is followed by the file's bytes and replies `crc32 <hex> size <n>`. The file is
written to `<path>.part` and then renamed, so a broken upload never replaces a good file. `log` replies `ok 0`,
then streams the log's text until the client closes the connection. At most 4 connections are open at once,
each with its own thread.

## Code

- `switch/native/source/debug_server.{h,cpp}`: the server. It knows nothing about the game, and it is the
  same file as SwitchWakerHD's (`runtime/src/platform/`): keep the two copies the same. Its stand-in for
  testing on the Mac is `scripts/switch/debug_server_host_test.cpp`.
- `switch/native/source/cos_debug.cpp`: this port's commands (`info`, `warps`, `warp`, `event`, `give`,
  `audio`, `shot`, `reload`, `quit`, `crash`), the start from the settings file (`cos_switch_debug_start`, called by `cos_switch_start` after
  `pc_settings_load_early`), and the input
  bridge.
- The hooks:
  - `cos_switch.cpp` `queueBytes`: the log's text, kept from the first line (`keep_log`, let go by `drop_log`
    when the server is off).
  - `sdl3_shim_gamepad.c`: the presses and sticks.
  - `native/src/pc/harness/pc_shot.cpp` (`pc_debug_shot_*`): the screenshot at the next frame end.
  - `native/src/pc/features/pc_menu.cpp` (`pc_debug_warp*`, `pc_debug_event`, `pc_debug_give`,
    `pc_debug_audio_*`, `pc_debug_status`): the warp, the event, the items, the audio line and the game's
    state, all on the game thread. On the Mac, `COS_TRACE=qol` with `COS_QOL_PROBE_GIVE=<frame>:<name>,...`
    (`pc_qol_probe.cpp`) runs the same `give` queue.
- `scripts/switch/switchwaker_debug.py`: the client, the same as SwitchWakerHD's `tools/switch/wwhd_debug.py`
  except for its configuration block and this port's game commands (`event`, `give`, `audio`, `crash`).
