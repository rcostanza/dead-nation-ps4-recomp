#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""DeadNation on PC: prepare the game image and start the game.

    python run.py            prepare if needed, then start the game
    python run.py check      prepare if needed, then list the PS4 imports the runtime lacks
    python run.py prepare    rebuild the game image only

Options: --game DIR (the decrypted dump; default DEADNATION_GAME_DIR), --update DIR (the decrypted update,
served over the dump; its eboot.bin is the one run; default DEADNATION_UPDATE_DIR), --timeout SECONDS (play only),
--out DIR (the game image, default out/game), --dlc DIR (one more folder of DLC next to the save's
addcont, e.g. shadPS4's addcont; default DEADNATION_DLC_DIR). Folders not given either way come from
paths.ini here (game=, update=, dlc=; git-ignored). Other arguments go to deadnation. Saves land in
user/, shader caches in the folder every copy of the game shares (shared_cache_dir), options in
user/settings.ini, or settings.ini here until that exists (see settings.example.ini).
"""
import argparse
import collections
import os
from pathlib import Path
import subprocess
import sys

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
PORT = Path(__file__).resolve().parent
# The eboot and the modules the linker starts at boot (scripts/link.py, DEFAULT_MODULES);
# the deferred ones are every other .prx in IMAGE_MODULE_DIRS.
IMAGE_INPUTS = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx')
IMAGE_MODULE_DIRS = ('sce_module',)


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def parse_cpu_features(output):
    """Instruction sets `deadnation --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """DEADNATION_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('DEADNATION_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def shared_cache_dir(env=os.environ):
    """Shader and pipeline caches (and the GPU library's logs): shaders/ in the project folder, as a
    set-up copy keeps them next to its exe (src/launcher.c). DEADNATION_GPU_USER_DIR overrides it."""
    if env.get('DEADNATION_GPU_USER_DIR'):
        return Path(env['DEADNATION_GPU_USER_DIR'])
    return PORT / 'shaders'


def run_script(name, *args, check=True):
    command = [sys.executable, str(PORT / 'scripts' / name), *map(str, args)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
    if check and result.returncode:
        sys.exit(result.returncode)
    return result.returncode


def image_is_current(game, out, base=None):
    linked = out / 'boot-linked.bin'
    if not linked.is_file() or not (out / 'content.bin').is_file():
        return False
    source = out / 'source.txt'
    if not source.is_file() or source.read_text(encoding='utf-8') != str(game):
        return False
    built = linked.stat().st_mtime
    sources = [root / name for root in (game, base) if root for name in IMAGE_INPUTS]
    sources += list((PORT / 'scripts').glob('*.py'))
    sources += [path for root in (game, base) if root for folder in IMAGE_MODULE_DIRS for path in (root / folder).glob('*.prx')]
    return all(not path.is_file() or path.stat().st_mtime < built for path in sources)


def prepare(game, out, force=False, base=None):
    """The image from `game` (the update when there is one); `base`, the dump under an update, gives
    the modules the update does not ship (Dead Nation's update holds only eboot.bin)."""
    if not force and image_is_current(game, out, base):
        print(f'Game image is current: {out / "boot-linked.bin"}', flush=True)
        return
    out.mkdir(parents=True, exist_ok=True)
    base_args = ['--base', base] if base else []
    run_script('prepare.py', game, '--out', out, *base_args)
    run_script('link.py', game, '--out', out, *base_args)
    (out / 'source.txt').write_text(str(game), encoding='utf-8')


def find_exe():
    exe = Path(os.environ['DEADNATION_EXE']) if os.environ.get('DEADNATION_EXE') else PORT / 'out' / ('deadnation' + EXE_SUFFIX)
    if not exe.is_file():
        fail(f'{exe} not found: build it with build.sh (MSYS2 CLANG64 shell on Windows).')
    # Development builds use the MSYS2 CLANG64 DLLs (SDL3, zlib, ...); a release has them in libs/.
    if WINDOWS and not (exe.parent / 'libs' / 'SDL3.dll').is_file():
        clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            os.environ['PATH'] = f'{clang64}{os.pathsep}{os.environ.get("PATH", "")}'
    return exe


def check(exe, out):
    """--check-imports, summarized by library; the full list goes to out/missing-imports.txt."""
    result = subprocess.run([str(exe), str(out / 'boot-linked.bin'), '--check-imports'], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    missing = [line[len('MISSING: '):] for line in result.stdout.splitlines() if line.startswith('MISSING: ')]
    (out / 'missing-imports.txt').write_text(''.join(line + '\n' for line in missing), encoding='utf-8')
    by_library = collections.defaultdict(list)
    for line in missing:
        key, _, name = line.partition(' (')
        nid, _, library = key.partition('#')
        by_library[library].append(nid if name.startswith('name not resolved') else name.rstrip(')'))
    for library, names in sorted(by_library.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        shown = ', '.join(names[:6]) + (', ...' if len(names) > 6 else '')
        print(f'{len(names):4} {library:28} {shown}')
    print(f'{len(missing)} imports missing; full list: {out / "missing-imports.txt"}')
    return 1 if missing else 0


def play(exe, game, out, timeout, user, extra, dlc=None, update=None):
    env = os.environ
    features = subprocess.run([str(exe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True)
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        return 20
    isa_args = []
    if isa_rewrite_wanted(parse_cpu_features(features.stdout), env):
        if run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out, check=False) == 0:
            isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
    # As the release's launcher: the save folder's settings.ini, else the one next to this script.
    env.setdefault('DEADNATION_CONFIG', str(user / 'settings.ini'))
    env.setdefault('DEADNATION_CONFIG_DEFAULT', str(PORT / 'settings.ini'))
    command = [str(exe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--app0', str(game), '--user', str(user), '--timeout', str(timeout), *isa_args,
               *(['--update', str(update)] if update else []), *(['--dlc', str(dlc)] if dlc else []), *extra]
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    try:
        return process.wait()
    except KeyboardInterrupt:
        process.terminate()
        return 130


def local_paths(path=PORT / 'paths.ini'):
    """game=, update= and dlc= of the developer's own paths.ini beside run.py (git-ignored)."""
    paths = {}
    if path.is_file():
        for line in path.read_text(encoding='utf-8').splitlines():
            key, sep, value = line.partition('=')
            if sep and not key.strip().startswith(('#', ';')) and value.strip():
                paths[key.strip().lower()] = value.strip()
    return paths


def main():
    local = local_paths()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', nargs='?', choices=('play', 'check', 'prepare'), default='play')
    p.add_argument('--game', type=Path, default=os.environ.get('DEADNATION_GAME_DIR') or local.get('game'))
    p.add_argument('--timeout', type=int, default=int(os.environ.get('DEADNATION_TIMEOUT', '0')))
    p.add_argument('--user', type=Path, default=PORT / 'user', help='saves, settings and the log (default: user/)')
    p.add_argument('--out', type=Path, default=PORT / 'out' / 'game', help='the game image (default: out/game)')
    p.add_argument('--update', type=Path, default=os.environ.get('DEADNATION_UPDATE_DIR') or local.get('update'),
                   help='the decrypted update (patch) folder, served over the dump')
    p.add_argument('--dlc', type=Path, default=os.environ.get('DEADNATION_DLC_DIR') or local.get('dlc'),
                   help="one more folder of DLC, next to the save's addcont (e.g. shadPS4's addcont)")
    a, extra = p.parse_known_args()
    if a.game is None:
        fail('Pass the decrypted game dump with --game DIR, DEADNATION_GAME_DIR or game= in paths.ini.')
    game = a.game.resolve()
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (pass --game or set DEADNATION_GAME_DIR).')
    update = a.update.resolve() if a.update else None
    if update and not (update / 'eboot.bin').is_file():
        fail(f'No eboot.bin in the update folder {update}.')
    out = a.out.resolve()
    # The update's eboot.bin and modules make the image; the base dump serves what it lacks.
    prepare(update or game, out, force=a.command == 'prepare', base=game if update else None)
    if a.command == 'prepare':
        return 0
    exe = find_exe()
    # The GPU library's folder (shader caches, logs) reaches every run of the exe through the environment,
    # the --cpu-features one too: the shared cache, never the save folder, so test boots on a copy
    # of the save do not write to ./user.
    user = a.user.resolve()
    user.mkdir(parents=True, exist_ok=True)
    os.environ['DEADNATION_GPU_USER_DIR'] = str(shared_cache_dir())
    # Development runs log every routine event; installed copies only faults, stops and errors.
    os.environ.setdefault('DEADNATION_VERBOSE', '1')
    if a.command == 'check':
        return check(exe, out)
    return play(exe, game, out, a.timeout, user, extra, a.dlc.resolve() if a.dlc else None, update)


if __name__ == '__main__':
    sys.exit(main())
