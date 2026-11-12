# Dead Nation PC runner

Native Windows and Linux runner for the PS4 version of Dead Nation (CUSA00236: the 1.00 dump plus the
1.03 update, SDK 1.50; Housemarque's own engine, statically linked into eboot.bin, with Wwise audio
in audio.pck and data in stream.dat). Forked from the Alienation runner (same engine family). The game's x86-64 code runs directly; PS4
system calls are reimplemented in C and GNM rendering goes through shadPS4's video core on Vulkan.
See [docs/development.md](docs/development.md) for the pipeline and [docs/building.md](docs/building.md)
for the toolchain and packaging. No game files are in the repository, and nothing assumes where the
decrypted dumps are: set `DEADNATION_GAME_DIR` to the game, `DEADNATION_UPDATE_DIR` to the update and
`DEADNATION_DLC_DIR` to the DLC (or pass `--game`, `--update`, `--dlc` to `run.py`). Tests that need
them skip without them.

## Loop
- Build: `bash build.sh` (Git Bash on Windows, where it puts MSYS2 CLANG64 on PATH itself; or Linux).
- Run: `python run.py --timeout 20`; read the first `STOP:` or `Fault:` line.
- Missing imports: `python run.py check` (full list in `out/game/missing-imports.txt`).
- Tests: `bash build.sh --test` (runtime C tests and every CMake `*-test`), then
  `cd tests && python -m unittest discover -p "test_*.py"`. `build_windows.ps1 -Test -Run` /
  `build_linux.sh --test --run` do the build, both suites and a boot check in one go.
- Scripted input for boot checks past the menus: `DEADNATION_PAD_FILE` (button names, re-read when
  the file changes); `tools/screenshot.ps1` captures the window.

## Conventions
- Names are `deadnation`/`DeadNation`/`DEADNATION_` in code (prose says "Dead Nation"): identifiers, files, env variables (`DEADNATION_*`), log prefixes and
  patch markers in vendored code (`// deadnation:`).
- C runtime imports resolve by symbol name: a function goes in a `src/runtime_*.c` export table
  (`{"name",function}`) and its NID in `src/import_names.inc`. Name NIDs with
  `python tools/name_imports.py out/game --write` (candidates in `tools/ps4_names.txt`).
- `libScePosix` imports are looked up under the `libkernel` key (runtime.c, `canonical_name`).
- POSIX variants return errno; `sce*` variants return SCE codes (0x8002xxxx, errno in the low
  16 bits). See `posix_result` in runtime_mutex.c.
- The loader builds with `-Wall -Wextra -Werror`. Match the surrounding terse C style and comment
  each function with the PS4 contract it follows.
- Guest-visible pointers must stay below 1 TiB (`runtime_low_alloc`).
- Keep only what this game needs: no extra menus or hacks for other games. The renderer
  is shadPS4's with added performance work (draw preparation, memos, async pipelines); behaviour
  changes there need a boot check (`python run.py --timeout 20`).
- Releases are built by `tools/package.py` and must work from the unpacked zip alone: no repo, no
  MSYS2, no Python of the player's own. Never package game files or anything built from a dump.
- Nothing is written outside the project or release folder: shader caches go to `shaders/`
  (`run.py` `shared_cache_dir`, `src/launcher.c`), paths to `paths.ini` beside the exe.
- Python edits on Windows: write files with LF line endings (`write_bytes`, or
  `open(path, 'w', newline='')`): text mode would turn the tree's LF into CRLF.
- The update is served over the dump (`runtime_file_set_update`): a file in the update replaces the
  dump's, directories list both. The update has only eboot.bin (no sce_module): the image is built
  from it, with the dump's modules (`prepare.py`/`link.py --base`).
- No DLC dump has been tried; DLC means sceAppContent reporting add-ons as installed, not serving files.
- Game-specific code lives in `src/runtime_game.c`, keyed by eboot sha256 (01.03 update, 01.00 dump)
  in `known_builds`, every site byte-checked before patching:
  - screens: screen manager push/pop hooked at entry (stub records rdi); stack vector at manager
    +0xa8/+0xb0, top screen's vtable[2] is its name. Logged as `Screen: <name>`.
  - frame clock (01.03 +0x1bf6958): set_fps is vtable slot 3, target at +8, step at +0xc. The game
    sets 30 at boot; game speed is real time at any rate (fixed-step accumulator).
  - render size (01.03 0xc88010, in the render device ctor): patched for 720p/1440p/4k (`deadnation_render_width/height`) and for
    21:9/32:9 (wider, same lines) and 4:3/16:10 (taller, 16:9's width). Taller also needs the
    cameras (`patch_camera`: 16:9 shape for the render-size ortho cameras, the projection's y term
    scaled before 0xce2cf0, and for the perspective the fov/aspect it also passes, `camera_view`, which
    object culling rebuilds its frustum from; plus the renderer view's vertical fov at 0xddadd7;
    without these, objects pop in at the top and bottom) and the menus' view rect (`patch_view_rect`), or the game
    cuts the sides.
- The engine takes its own command-line switches (strings in the eboot: `-norumble`, `-nosound`,
  `-stresstest`, ...); pass them with `python run.py --guest-arg <switch>`.
- `skip_intro=1` (settings.ini, default) is screen-driven (`runtime_pad_intro_screen`,
  src/runtime_pad.c): until Screen_Main_Menu first shows, movies end after their first frame
  (`runtime_pad_intro_movie`, AvPlayer), Cross is tapped on Screen_Startup_Splash,
  Screen_Brightness_Calibration and Screen_SCEPresents, Options on Screen_Menu_Title.
- No HDR and no PS4 Pro mode: the game never imports sceVideoOutGetDeviceCapabilityInfo or
  sceKernelIsNeoMode (`deadnation_hdr` returns 0); 4K is the runner's render-size patch.
- The overlay's pages (Display Settings, exit prompt: F4 or Select+Start) follow the game's menus: near-black backdrop
  with a red glow, heavy white title top left, dark grey bars in a brushed silver frame, blood red
  focus bar, hints right-aligned at the bottom ("MOVE (glyph)"), Roboto Condensed (OFL,
  `gpu/third_party/fonts`, licence shipped in `licenses/`). Never embed the game's own fonts.
  `DEADNATION_PAD_FILE` token `settings=<row>` opens the panel for screenshots.
- The page look lives in `gpu/shim/deadnation_page.h` (palette `Linear` for the overlay's sRGB image,
  `Srgb` for plain windows), shared with `settings/main.cpp`: settings.exe (Linux `settings`),
  SDL3 renderer + ImGui, statically linked on Windows. It writes each change at once with
  `setup::write_setting` into the save folder's settings.ini, else the exe folder's (`--config` overrides).
  A new display option goes in the overlay's rows and settings.exe's both.
- deadnation-setup.exe's window (`setup/main.cpp`, Win32 GDI) draws the same look by hand: the `Srgb`
  palette copied into its `color` namespace, Roboto Condensed embedded as RCDATA in `setup/setup.rc`
  (`AddFontMemResourceEx`). Keep its colours in step with `deadnation_page.h`. The Linux setup has no
  window of its own (terminal, or SDL's system dialogs).
