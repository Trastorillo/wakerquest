# Installing SwitchWaker on your Switch

SwitchWaker runs The Wind Waker (GameCube) natively on a Switch with custom firmware. **Nothing of the
game comes with it**: the download holds the program only, and it reads everything of the game (models,
stages, sounds, and the data the program needs) from your own disc image on the SD card, every time it
starts.

## 1. What you need

| | |
|---|---|
| **The game** | Your own disc of **The Wind Waker**, **USA** (`GZLE01`) or **Europe** (`GZLP01`), **revision 0**, as an **uncompressed `.iso`** (1.4 GB). RVZ, GCZ, CISO or NKit images must be converted first: in Dolphin, right-click the game → *Convert File...* → ISO |
| **A Switch** | Running Atmosphère with the Homebrew Menu, and about 1.5 GB free on the SD card |

How to dump your disc is outside this guide: [Dolphin's guide](https://dolphin-emu.org/docs/guides/ripping-games/)
covers it.

## 2. Download

From the project's **Releases** page, download `SwitchWaker-<version>-switch.zip` (and check it against its
`.sha256` if you like). It holds:

```
switch/switchwaker/
├── switchwaker.nro                the program, for the USA disc
├── switchwaker-eur.nro            the program, for the European disc
├── initial_pipeline_cache.db      the list of graphics pipelines the game uses
└── initial_dksh_cache.bin         their shaders, compiled for the Switch's GPU
```

The game's version is fixed when the program is built, so there is one program per disc: use
`switchwaker.nro` with the USA disc and `switchwaker-eur.nro` with the European one. You can keep both
(or delete the one you do not need).

## 3. Copy it to the SD card

1. Copy the zip's `switch` folder to the root of the SD card, so the program ends up at
   `sdmc:/switch/switchwaker/switchwaker.nro`.
2. Copy your disc image into that same folder, named after its disc ID: **`GZLE01.iso`** for the USA
   disc (`sdmc:/switch/switchwaker/GZLE01.iso`), **`GZLP01.iso`** for the European one
   (`sdmc:/switch/switchwaker/GZLP01.iso`).

You can take the card out, or connect the Switch by USB with hekate (*Tools → USB Tools → SD Card*). The
SD card must be FAT32 or exFAT; a 1.4 GB file is fine on both.

## 4. Play

- Start the Homebrew Menu **in title mode**: hold **R** while starting any installed game, then pick
  **SwitchWaker** (USA disc) or **SwitchWaker EUR** (European disc). From the album (applet mode) the
  game does not get enough memory.
- Optional: a HOME-screen icon that always starts it the right way (you build it with your own console
  keys): [switch/forwarder/INSTALL.md](switch/forwarder/INSTALL.md).
- The shaders come prebuilt: the first start takes a few seconds, like every later one.
- **Minus (−)** opens the options menu (in Spanish or English, following the console's language):
  graphics, performance, controls, debug. Settings are saved on the card.
- **European disc, language**: the game starts in the console's language when it is English, German,
  French, Spanish or Italian (any other: English); once you have a save, in the language it was saved
  in, as on the GameCube (the file select screen can change it). The options menu's *Game language*
  (last row of the first tab) can fix one of the five instead; it takes effect at the next start.
- The European version always runs at 60 Hz: there is no 50/60 Hz question.
- If the disc is missing, compressed or another version, the console says so and the program closes.

## Updating

Download the new release and copy its `switch` folder over the old one (`switchwaker.nro`,
`switchwaker-eur.nro`, `initial_pipeline_cache.db` and `initial_dksh_cache.bin`). Your disc, saves
(`native/user/USA/Card A`, `native/user/EUR/Card A` for the European disc) and settings stay.

The OpenGL renderer (`switchwaker_gl.nro`, "SwitchWaker (GL)") was removed on 2026-10-09: deko3d is the
only renderer. Optional, to free the space its files take on the card, delete what it left behind:

- the folder `sdmc:/switch/switchwaker_gl/`, if you built and installed the GL NRO;
- in `sdmc:/switch/switchwaker/native/user/cache/`: `dawn_cache.db` (and `dawn_cache.db-journal`),
  `mesa_shader_cache.bin`, `mesa_shader_cache.idx` and `mesa_shader_cache.use`; keep the rest of the
  folder;
- in `sdmc:/switch/switchwaker/native/logs/`: the `switchwaker_gl_*.log` files.

## Problems

| | |
|---|---|
| The console says the disc is missing or not the right one | `sdmc:/switch/switchwaker/GZLE01.iso` must be the USA disc and `GZLP01.iso` the European one, revision 0, as an uncompressed ISO (see "What you need"), and you must start the program for that disc (*SwitchWaker* for USA, *SwitchWaker EUR* for Europe): the message names the right one |
| The game closes at once or says it is out of memory | Start it in title mode (hold R), not from the album |
| It fails to start after an update, or keeps stuttering | Delete `sdmc:/switch/switchwaker/native/user/cache/` (the local caches; they are rebuilt) |
| Something else | The session logs in `sdmc:/switch/switchwaker/native/logs/` (the newest file is the last run) say what happened; include them in a report (they contain no game data) |

## Building it yourself (from source)

For developers, or to run a version that has no release yet. Everything is built in containers on your
computer.

| | |
|---|---|
| **A computer** | Windows 10/11 (with WSL), macOS or Linux; 8 GB of RAM or more, 20 GB of free disk space |
| **On the computer** | [Docker Desktop](https://www.docker.com/products/docker-desktop/) (or Docker / Podman on Linux), git, Python 3, and the source (a release's source zip or a clone) |

**Windows**: run everything below inside WSL (Ubuntu): install WSL with `wsl --install` in an
administrator PowerShell, turn on Docker Desktop's *Use the WSL 2 based engine* and its integration with
your Ubuntu, then open Ubuntu and work there. Keep the source inside the WSL file system (for example
`~/switchwaker`), not under `/mnt/c`: builds there are much slower.

Open a terminal in the source folder:

```sh
scripts/switch/make_sd.sh --disc /path/to/GZLE01.iso
```

It checks the disc first, then builds everything in Docker. **The first build takes an hour or more**
(it downloads and compiles the graphics libraries once); later builds take minutes. If the computer runs
out of memory, add `--jobs 2`. At the end `build/sd/switch/switchwaker/` holds the three files above plus a
copy of your disc as `GZLE01.iso` (with the European disc: `switchwaker-eur.nro` and `GZLP01.iso`;
`--no-disc` leaves the disc out, for an update): copy the **contents** of
`build/sd/` to the root of the SD card. A build made this way contains code generated from your disc: it is
for your own console only, do not share it. (The published download is built without the disc:
`scripts/switch/build_native.sh --runtime-assets`, [docs/RUNTIME_ASSETS.md](docs/RUNTIME_ASSETS.md).)

| Building problem | |
|---|---|
| `this is not a disc the port is built for` | The image is not GZLE01 or GZLP01 revision 0, or is compressed: use the USA or European disc, converted to an uncompressed ISO |
| Docker errors | Docker Desktop must be running (on Windows, with its WSL integration on) |
