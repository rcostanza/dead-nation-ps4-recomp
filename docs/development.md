# Development

How the runner works and how to work on it. Building, tests and packaging are in
[building.md](building.md).

## How it works

The PS4 and the PC are both x86-64, so nothing is recompiled. Dead Nation's `eboot.bin` (Housemarque's
engine, statically linked, with Wwise for audio) is mapped into memory and run directly. What
differs is everything that code calls:

| The game calls | Replaced by |
|---|---|
| PS4 system libraries (kernel, POSIX, threads, files, pad, audio, save data, ...) | C reimplementations in `src/runtime_*.c` |
| GNM GPU command buffers | shadPS4's video core in `gpu/`, GCN shaders translated to Vulkan |

The game image is built from the dump by `run.py` (and by the setup in a release). With an update
(`--update`, `DEADNATION_UPDATE_DIR`; setup's update folder) the image is built from the update's
`eboot.bin`; Dead Nation's update has no `sce_module`, so the modules come from the dump
(`--base`), and the update's files are served over the dump's at run time
(`runtime_file_set_update`, `src/runtime_file.c`): a file in the update replaces the dump's, a
directory lists the entries of both, as a PS4 does with an installed patch.

1. `scripts/prepare.py` strips the SELF wrapper from `eboot.bin` (the dump must be decrypted) and
   writes `out/game/boot.bin`, its report `analysis.json` and the AppContent profile `content.bin`.
2. `scripts/link.py` links the eboot with the two modules Dead Nation ships in `sce_module`, `libc.prx`
   and `libSceFios2.prx`, into `boot-linked.bin`. Symbols match by NID and library identity, never by
   name; `mov rax, fs:[0]` thread-pointer loads are rewritten to GS (glibc owns FS on Linux).
3. On a CPU without an instruction set the game uses (MOVBE, LZCNT, BMI1), `scripts/isa_rewrite.py`
   lists each such instruction for `src/isa_rewrite.c`, which patches in a stub that emulates it.
4. `deadnation` maps the image, resolves every import against the C runtime (by symbol name, from
   `src/import_names.inc`) or the GPU core (by NID), runs each module's initializer and enters the
   game. An import with no implementation becomes a trap that prints
   `STOP: first unsupported PS4 import: <name>` and exits.

The imports Dead Nation's three modules call are implemented. `python run.py check` lists the ones
the runtime cannot resolve; a handful (libSceXml, voice chat, some PSN calls) are left unnamed
because the game never reaches them.

The renderer draws at the game's resolution (1920x1080, or 1280x720 / 2560x1440 / 3840x2160 with `render_resolution=720p` / `1440p` / `4k`,
see [Render resolution](#render-resolution)) and scales the picture to the window in
`post_process.frag`. With `sharpening=1` (the default) that pass draws into an image of the
window's size and FSR 1 RCAS (`rcas.frag`, `host_passes/rcas_pass.cpp`) sharpens it into the frame.
The only overlay is the PS4's system UI (save list, text entry, trophy notices), the Display Settings
panel and the exit prompt (`gpu/shim/deadnation_overlay.cpp`).

## Running from the source tree

Set `DEADNATION_GAME_DIR` to your decrypted dump (or pass `--game DIR` each time):

```bash
python run.py --timeout 20
```

- Without a command, `run.py` prepares `out/game/` if the dump or `scripts/` changed and starts the
  game. The first `STOP:` or `Fault:` line it prints is the next thing to fix. `--timeout` ends the
  run after that many seconds.
- `python run.py check` lists the imports the runtime cannot resolve, by library (full list in
  `out/game/missing-imports.txt`). `python run.py prepare` rebuilds the image only.
- `--guest-arg SWITCH` passes one of the engine's own command-line switches (strings in the eboot:
  `-norumble`, `-nosound`, `-stresstest`, ...).
- Saves go to `user/` (`--user DIR`), settings to `user/settings.ini`. Shader caches go to
  `shaders/` in the project folder (`DEADNATION_GPU_USER_DIR` moves it).
- Every `settings.ini` key has an environment variable that overrides it, named in
  `settings.example.ini`. A variable left set in a shell keeps overriding the panel:
  `DEADNATION_VBLANK_HZ` prints a warning at start for that reason.
- `DEADNATION_VERBOSE=1` logs every routine event (threads, semaphores, memory maps, module loads),
  the GPU library's warnings (else errors only; `DEADNATION_GPU_LOG` still overrides) and the
  statistics report on a normal exit (faults and stops always print it).
  `run.py` and the debug package's `diagnose/start.bat` turn it on; `test.ps1` turns it off.
- Hang dump: once a frame has been shown, a picture held for `DEADNATION_HANG_DUMP` seconds (default 20,
  `0` turns it off) logs every thread's name, CPU time and frames (`Hang:` lines, innermost first).
  The `--timeout` watchdog prints the same list before it ends a run. The dump starts with how far
  the GPU got on the rasterizer and sparse-binding timelines.
- `DEADNATION_AUDIO_LEVELS=1` logs each audio port's peak levels every 3 s (the first three
  minutes); the exit report keeps the session totals either way.
- GPU crashes: a lost device prints the driver's fault report (`GPU fault:` lines,
  VK_EXT_device_fault). `DEADNATION_GPU_CHECKPOINTS=1` (NVIDIA) also puts a checkpoint before every
  recorded command and turns on the driver's own diagnostics; the report then names the call site
  and shaders of the last commands each queue reached (`GPU checkpoint` lines, also in a hang dump).
  `DEADNATION_SYNC_DISPATCH=1` submits and waits after every compute dispatch, so a fault names the
  compute shader it happened in (`GPU fault: during the dispatch of compute shader 0x...`) or says
  it was outside every dispatch (a draw or copy). Slow; diagnosis only.
- `Screen: <name>` lines log each menu screen the game shows (Screen_Main_Menu, Screen_Game, ...):
  handy anchors for scripted runs with `DEADNATION_PAD_FILE`.

## Development loop

1. `python run.py --timeout 20` and read the first `STOP:` or `Fault:` line.
2. Implement the function in the matching `src/runtime_*.c` export table (`{"name",function}`),
   following the PS4 contract, with a comment that states it. Return "not available" for services a
   PC does not have (PSN, the camera, the companion app).
3. `bash build.sh`, run again.

When an import shows up as a bare NID, add candidate names to `tools/ps4_names.txt` and run
`python tools/name_imports.py out/game --write`: it hashes every candidate (and every name the
runtime already implements) and appends the matches to `src/import_names.inc`. For a fault in guest
code, `python tools/image_info.py flatten <module> out.bin` writes a module as a flat image to
disassemble at the offset the log names, and `python tools/image_info.py got <offset>` names the
import bound to a GOT slot.

## Game hooks

Everything that depends on the eboot's layout is in `src/runtime_game.c`, keyed by the eboot's
SHA-256 (recorded by `prepare.py` in `analysis.json`): the 1.03 update's eboot and the 1.00 dump's
are known. An unknown eboot runs without the hooks (no intro skip, 30 FPS, 1080p 16:9) and the log
says so. Each address is also checked against the bytes it expects before it is patched.

- **Screens.** The screen manager's push and pop methods (1.03: 0xe21940, 0xe21150) are hooked at
  their entry to record the manager. Its stack of screens is a vector at +0xa8/+0xb0; the top
  screen's vtable slot 2 returns its name. `runtime_game_screen()` reads it at each flip.

## Frame rate

Dead Nation's main loop (1.03: 0xcafe70) measures real time and runs the game's update in fixed steps
of 1/fps (at most 8 a frame), so its speed is real time at any rate. The game sets the rate to 30 at
boot. `frame_rate` (30, 60, 90, 120, 144, 180 or 240) is applied at each flip by
`runtime_game_frame_rate()`, which calls the engine clock's `set_fps` (the global at 1.03 +0x1bf6958,
vtable slot 3; target at +8, step at +0xc). Before its first change the runner checks the clock: a
vtable in the image's data, `set_fps` in its code and a target near 30 or 60; otherwise the game keeps
its 30 and the log says so.

`DeadNationSettings::EffectiveFrameRate()` lowers the rate to what the display refreshes at, unless
VSync is off with tearing allowed. The game calls `sceVideoOutSetFlipRate(1)` (a flip every second
vblank) once at boot; the presenter keeps that only for 30 FPS (`driver.cpp`).

### Render resolution

The render device (1.03 constructor 0xc87fd0) takes one size, 1920x1080, from its init parameters and
makes every render target that size, the display buffers included. `render_resolution=720p`, `1440p` or `4k`
patches the copy (1.03 0xc88010) to store 1280x720, 2560x1440 or 3840x2160 instead; the size is
`deadnation_render_width/height`, shared with the video out. The shadow maps stay 1024x1024.

Cost (RTX 5070 Ti, gameplay, GPU time a frame): 1080p 3.2 ms, 1440p 4.4 ms, 4K 8.8 ms. Most of
the 4K time is two full-screen passes, the deferred lighting and a 24-tap screen-space pass over
depth whose taps are spaced in screen fractions, so they spread over more texels as the size grows. The game has no PS4 Pro mode of its own.

### Aspect ratios

The same patch widens the size for 21:9 and 32:9 (the height stays, the width grows): the game shows
more of the level to the sides and keeps its menus and HUD 16:9 in the middle.

Narrower shapes (4:3, 16:10) keep the 16:9 width and grow the height (1920x1440 at 4:3 and 1080p).
Left alone, the game keeps its vertical view and the menus' height and cuts the sides. Every camera
(the level's perspective, the menus' and HUD's ortho) is set up by one method (1.03 0xceb3xx) from
its target's size and shape; the builders (perspective 0xce33f0, ortho 0xce3560) hand the
projection to 0xce2cf0, which stores it with the view matrix and everything derived from both.
For the render size, `patch_camera` gives the ortho cameras a 16:9 shape and scales the
projection's y term by (w/h)/(16/9) on its way to 0xce2cf0, and `patch_view_rect` gives the
menus' layout rect (0xcebbd1) 16:9 too. Culling does not use the projection matrix: 0xce2cf0 also
stores the camera's diagonal field of view and aspect (+0x100, +0x10c), and objects are culled by a
frustum rebuilt from them (tan(y/2) = tan(fov/2)/sqrt(1+aspect), tan(x/2) = aspect*tan(y/2)). For
the perspective call, `camera_view` rewrites both so x stays and y grows by the same factor;
without it, objects (not the level) pop in and out as they cross the old 16:9 edge, with their
shadows already drawn. The view the renderer builds each frame (0xddad30) takes its vertical field
of view from the device's 16:9 shape too (tan of half of it loaded at 0xddadd7, some small debris
is culled with it); it is scaled the same way. Together: the level shows the 16:9 width plus more above and below,
and the menus and HUD keep their 16:9 layout, centred. If an eboot lacks any of these sites,
`runtime_game_widen` keeps 16:9 with bars.

## Starting, the title screen and leaving

Dead Nation boots through Screen_Startup_Splash (the logo, then the autosave notice, which waits
for Cross), Screen_Brightness_Calibration (first start only), Screen_SCEPresents, an intro movie and
Screen_Menu_Title ("Press OPTIONS Button") before Screen_Main_Menu. With `skip_intro=1` (the
default), until the main menu first shows (`runtime_pad_intro_screen`, `src/runtime_pad.c`): a
movie is ended after its first frame (`runtime_pad_intro_movie`, AvPlayer), Cross is tapped on the
splash, brightness and SCEPresents screens, and Options on the title screen. A start lands in the
main menu. Later visits (Back from the main menu) are left alone.

On the title screen, Back (Circle, B, Escape) asks "Exit Dead Nation?" (`title_back_exits=1`); F4 or Select+Start
asks anywhere (Start is held back from the game while Select is down). The prompt is drawn by the overlay and takes the input while open; Yes pushes the same
quit event as closing the window. Holding Select (SDL's Back button) for two seconds opens the
Display Settings panel (F10 too): the button's own press is held back while it is down, and a release
before two seconds sends it to the game as a short tap.

## Save data, DLC and trophies

Saves are folders under `<save>/savedata/<user>/CUSA00236/` (`src/runtime_savedata.c`). The game
looks for its save with `sceSaveDataDirNameSearch` and mounts it with `sceSaveDataMount`.

Add-ons are entitlement-only on PS4: the game asks `sceAppContentGetAddcontInfo` about each label it
knows, and an add-on is installed when a folder of that name exists in `<save>/addcont/` or in the
extra DLC folder (`--dlc`, `DEADNATION_DLC_DIR`; shadPS4's DLC folder with its `CUSA00236` folder
works as it is). Nothing is read from those folders.

Trophies come from `sce_sys/trophy/trophy00.trp`; unlocks are kept in `<save>/trophies.dat` and shown
as a notice.

Online features (PSN, co-op online, leaderboards, the companion app) see a signed-out console with no
network: contexts fail with NP SIGNED_OUT or ENETDOWN, which the game handles as offline play.

## Release folder and setup

`deadnation-setup.exe` (`setup/`) runs `prepare/run.py prepare` into `game/` beside it, rewrites the
game's instructions for this CPU if needed, writes `settings.ini` from the template when there is
none, sets its `skip_intro` line from the window's checkbox, and writes `paths.ini` with the game and
save folders and, when chosen, `update=` and `dlc=`. Nothing is written outside the release folder:
shader and pipeline caches go to `shaders/` beside the exe (`cache=` moves them). The command line does the same as the window:
`deadnation-setup.exe --help`.

Started with no arguments, `deadnation.exe` reads `paths.ini` and builds the command line `run.py`
would pass (`src/launcher.c`); without a console its output goes to `log/deadnation.log` in the save
folder. It also watches the game: it starts the same command line as a child (on Windows in a job
that ends it if the launcher is killed, on Linux by `fork` with `PR_SET_PDEATHSIG`), and when the
game ends with an exit code other than 0 it notes the code in the log, copies the log to
`log/crash-<date>-<time>.log` and says so in a message box. `DEADNATION_CRASH_WATCH=0` turns that off.

Preparing the game needs Python 3.10 or newer with its standard library only; releases bundle
python.org's embeddable Python in `prepare/python`. The CPU rewrite also needs `iced-x86`, which only
CPUs missing an instruction set ever use. The icon of both exes is `res/deadnation.ico`
(`tools/make_app_icon.py` builds it from artwork).

## Input

The pad and keyboard layouts are fixed tables in `src/runtime_pad.c` (README "Controls"): gamepads map
by button position, keys by physical position. `deadnation --gamepad` prints the connected pad and its
labels.

Scripted input for automated runs: `DEADNATION_PAD_FILE` (held buttons and stick values, re-read when
the file changes), `DEADNATION_PAD_RECORD` (F9 records the pad) and `DEADNATION_PAD_REPLAY`.

## Layout

| Path | What it is |
|---|---|
| `src/` | Loader (`loader.c`), launcher (`launcher.c`), the runtime (`runtime_*.c`) and the CPU rewrite (`isa_*.c`) |
| `gpu/` | shadPS4's video core (`shadps4/`, changes marked `// deadnation:`), the shim and overlay around it (`shim/`), CMake build |
| `build.sh`, `build_windows.ps1`, `build_linux.sh` | The build (`build.sh`) and the local build, test and boot-check wrappers ([building.md](building.md)) |
| `scripts/` | Building the game image: `prepare.py`, `link.py`, `isa_rewrite.py` |
| `setup/` | `deadnation-setup`, which sets up a release folder from a player's dump |
| `settings/` | `settings.exe` (`settings` on Linux): the game's options outside the game, drawn with `gpu/shim/deadnation_page.h` like the overlay's Display Settings page, saved with the setup's settings.ini helpers |
| `res/` | The exes' icon, Windows resource script and manifests (`libs.manifest`: the DLL folder) |
| `packaging/` | `diagnose/`, the glitch kit the debug package ships |
| `.github/` | CI (`workflows/build.yml`) and its scripts: `ci-deps.sh` (Linux build dependencies), `smoke-test.sh` (a release on fresh distros) |
| `tools/` | `package.py` (the release), `name_imports.py` (names NIDs), `image_info.py` (modules and GOT slots), `make_app_icon.py`, `screenshot.ps1` |
| `tests/` | Python tests (`cd tests; python -m unittest`) and C/C++ tests (`build.sh --test`: its own list and every `*-test` in `gpu/CMakeLists.txt`) |
| `docs/` | This guide and [building.md](building.md) |
