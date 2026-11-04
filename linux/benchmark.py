#!/usr/bin/env python3
"""Repeatable frame-rate benchmark: loads a frozen save and measures the same scene each run.

  python3 linux/benchmark.py --capture-save        freeze the newest save (the game's default slot) for benchmarks
  python3 linux/benchmark.py                       one run: load it, stand still, turn the camera, print the frame rate
  python3 linux/benchmark.py --runs 3 --env AC_CPU_PLACEMENT=0 --env AC_ASYNC_SUBMIT=0
                                                   variants: each --env set is one variant, run --runs times
  python3 linux/benchmark.py --variants "A:" "B:AC_CPU_PLACEMENT=0" --runs 2
                                                   named variants, alternated run by run

The frozen save lives in <install>/benchmark/save-frozen and each run gets a fresh copy (AC_DATA_DIR): the player's
saves are never read or written by a run, and an autosave during one changes nothing for the next. The game runs
windowed (--fullscreen to change), with vibration off and input from a file the script writes (AC_INPUT_FILE):
the player's controller keeps working but should be left alone during a run.

The script finds the screens from the draws per frame in the game's frame-rate log (title ~65, main menu ~95, save
list ~230, dialogs ~340, the level over 800) and presses: ZL+ZR on the title, A on the main menu (Load is selected
when saves exist), A on the save list (the newest save is selected), Left and A on each dialog (the first one's list
ignores Left; the second's "Yes" is on the left). Once the level runs it waits --settle seconds, then measures
--still seconds standing and --turn seconds turning the camera right (full stick), one frame-rate line a second.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import launcher as L  # noqa: E402

BUTTON = {'A': 0x1, 'ZL+ZR': 0x300, 'LEFT': 0x10000, 'RSTICK_RIGHT': 0x400000}
LINE = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms .*?\((\d+) draws')


def save_store(home):
    """The player's save store: the folder with data/ and manifest.bin under userdata/saves."""
    stores = [p.parent for p in (home/'userdata'/'saves').glob('*/*/manifest.bin')]
    if not stores:
        sys.exit(f'No save found under {home}/userdata/saves')
    return max(stores, key=lambda p: max((f.stat().st_mtime for f in (p/'data').iterdir()), default=0))


def capture_save(home):
    store, frozen = save_store(home), home/'benchmark'/'save-frozen'
    if frozen.exists():
        shutil.rmtree(frozen)
    frozen.mkdir(parents=True)
    shutil.copytree(store/'data', frozen/'data')
    shutil.copy2(store/'manifest.bin', frozen/'manifest.bin')
    newest = max((store/'data').iterdir(), key=lambda f: f.stat().st_mtime)
    print(f'Froze {store} into {frozen} (newest file {newest.name}, '
          f'{time.strftime("%Y-%m-%d %H:%M", time.localtime(newest.stat().st_mtime))})')


class Game:
    def __init__(self, config, home, extra, fullscreen, log_dir):
        self.home, self.run_dir = home, home/'benchmark'/'run'
        if self.run_dir.exists():
            shutil.rmtree(self.run_dir)
        (self.run_dir/'save').mkdir(parents=True)
        frozen = home/'benchmark'/'save-frozen'
        shutil.copytree(frozen/'data', self.run_dir/'save'/'data')
        shutil.copy2(frozen/'manifest.bin', self.run_dir/'save'/'manifest.bin')
        # AC_DATA_DIR also moves the code images and the RomFS (runtime: portable bundles): links to the install's
        (self.run_dir/'images').symlink_to(home/'build'/'native-images', target_is_directory=True)
        (self.run_dir/'romfs.bin').symlink_to(home/'game'/'romfs.bin')
        self.pad = self.run_dir/'pad.txt'
        self.pad.write_text('0')
        env = L.game_env(config)
        env.update(AC_DATA_DIR='Z:' + str(self.run_dir).replace('/', '\\'),  # (the save root is a Windows path)
                   AC_INPUT_FILE=str(self.pad), AC_IGNORE_FOCUS='1', AC_VIBRATION='0',
                   AC_FULLSCREEN='1' if fullscreen else '0', LARECOMP_FPS_INTERVAL='1')
        env.update(extra)
        game = home/'build'/'game'
        r0 = game/'round-00'
        self.err_path = log_dir/'stderr.log'
        self.out = open(log_dir/'trace.log', 'w', encoding='utf-8', errors='replace')
        self.err = open(self.err_path, 'w', encoding='utf-8', errors='replace')
        self.process = subprocess.Popen([str(r0/'trace.exe'), str(r0/'relocations.bin')], cwd=game, env=env,
                                        stdout=self.out, stderr=self.err)
        self.reader = open(self.err_path, encoding='utf-8', errors='replace')
        self.lines = []

    def poll(self):
        """New frame-rate lines: (time, frame, fps, max ms, draws per frame)."""
        new = []
        for text in self.reader.readlines():
            m = LINE.search(text)
            if m:
                t, frame, fps, worst, draws = float(m[1]), int(m[2]), float(m[3]), int(m[4]), int(m[5])
                new.append((t, frame, fps, worst, draws / max(fps, 1.0)))
        self.lines += new
        return new

    def hold(self, mask, seconds):
        self.pad.write_text(f'{mask:x}')
        time.sleep(seconds)
        self.pad.write_text('0')

    def alive(self):
        return self.process.poll() is None

    def stop(self):
        if self.alive():
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.out.close()
        self.err.close()


def screen(per_frame):
    if per_frame > 800:
        return 'level'
    for name, low, high in (('title', 40, 80), ('menu', 80, 130), ('saves', 180, 290), ('dialog', 290, 420)):
        if low <= per_frame < high:
            return name
    return None


def reach_level(game, timeout):
    """Presses through the menus; returns once the level has rendered for 3 s."""
    start, acted, level = time.time(), 0.0, 0
    while time.time() - start < timeout:
        if not game.alive():
            sys.exit(f'The game ended (exit code {game.process.returncode}); see {game.err_path}')
        time.sleep(0.25)
        for line in game.poll():
            level = level + 1 if screen(line[4]) == 'level' else 0
        if level >= 3:
            return
        recent = [screen(line[4]) for line in game.lines[-2:]]
        if len(recent) < 2 or recent[0] != recent[1] or recent[0] in (None, 'level') or time.time() - acted < 3:
            continue
        where = recent[0]
        print(f'  {time.time() - start:5.1f} s: {where}', flush=True)
        if where == 'title':
            game.hold(BUTTON['ZL+ZR'], 2.0)
        elif where in ('menu', 'saves'):
            game.hold(BUTTON['A'], 0.3)
        else:
            game.hold(BUTTON['LEFT'], 0.4)
            time.sleep(0.8)
            game.hold(BUTTON['A'], 0.3)
        acted = time.time()
    sys.exit(f'The level did not load in {timeout} s; see {game.err_path}')


def measure(game, seconds, mask=0):
    game.poll()
    if mask:
        game.pad.write_text(f'{mask:x}')
    first = len(game.lines)
    time.sleep(seconds)
    game.pad.write_text('0')
    game.poll()
    lines = game.lines[first:]
    if len(lines) < 2:
        return None
    frames = lines[-1][1] - lines[0][1]
    elapsed = lines[-1][0] - lines[0][0]
    return {'fps': frames / elapsed if elapsed > 0 else 0.0, 'min_fps': min(x[2] for x in lines[1:]),
            'worst_ms': max(x[3] for x in lines[1:]), 'draws': sum(x[4] for x in lines[1:]) / (len(lines) - 1)}


def one_run(config, home, extra, args, log_dir):
    game = Game(config, home, extra, args.fullscreen, log_dir)
    try:
        reach_level(game, args.timeout)
        time.sleep(args.settle)
        still = measure(game, args.still)
        turn = measure(game, args.turn, BUTTON['RSTICK_RIGHT']) if args.turn > 0 else None
        return still, turn
    finally:
        game.stop()


def parse_env(items):
    out = {}
    for item in items:
        if item:
            name, _, value = item.partition('=')
            out[name.strip()] = value.strip()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--install', help='the install folder (default: the launcher\'s)')
    ap.add_argument('--capture-save', action='store_true', help='freeze the newest save for benchmarks and exit')
    ap.add_argument('--runs', type=int, default=1, help='runs of each variant (default 1)')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game (one variant)')
    ap.add_argument('--variants', nargs='+', help='"label:NAME=VALUE,NAME=VALUE" each; runs alternate between them')
    ap.add_argument('--settle', type=float, default=8, help='seconds after the level appears (default 8)')
    ap.add_argument('--still', type=float, default=20, help='seconds standing still (default 20)')
    ap.add_argument('--turn', type=float, default=15, help='seconds turning the camera (default 15; 0: none)')
    ap.add_argument('--timeout', type=float, default=240, help='seconds to reach the level (default 240)')
    ap.add_argument('--fullscreen', action='store_true')
    ap.add_argument('--json', help='write the results here too')
    args = ap.parse_args()

    config = L.load_config()
    if args.install:
        config['install'] = args.install
    home = Path(config['install']).expanduser()
    if args.capture_save:
        capture_save(home)
        return
    if not (home/'benchmark'/'save-frozen'/'manifest.bin').is_file():
        sys.exit('No frozen save: run with --capture-save first (it freezes the newest save).')
    for proc in Path('/proc').iterdir():
        try:
            if proc.name.isdigit() and (proc/'comm').read_text().strip() == 'trace.exe':
                sys.exit('The game is running: close it first.')
        except OSError:
            pass

    if args.variants:
        variants = []
        for spec in args.variants:
            label, _, rest = spec.partition(':')
            variants.append((label or 'default', parse_env(rest.split(','))))
    else:
        variants = [(' '.join(args.env) or 'default', parse_env(args.env))]
    stamp = time.strftime('%Y%m%d-%H%M%S')
    results = []
    for run in range(args.runs):
        for label, extra in variants:
            log_dir = home/'benchmark'/'logs'/f'{stamp}-{len(results) + 1:02d}'
            log_dir.mkdir(parents=True, exist_ok=True)
            print(f'Run {len(results) + 1}: {label}', flush=True)
            still, turn = one_run(config, home, extra, args, log_dir)
            for part, r in (('still', still), ('turn', turn)):
                if r:
                    print(f'  {part:5}: {r["fps"]:5.1f} fps (lowest second {r["min_fps"]:4.1f}, worst frame '
                          f'{r["worst_ms"]} ms, {r["draws"]:.0f} draws a frame)', flush=True)
            results.append({'label': label, 'env': extra, 'still': still, 'turn': turn, 'log': str(log_dir)})
    if len(results) > 1:
        print('\nSummary (mean of the runs):')
        for label, _ in variants:
            mine = [r for r in results if r['label'] == label]
            parts = []
            for part in ('still', 'turn'):
                values = [r[part]['fps'] for r in mine if r[part]]
                if values:
                    parts.append(f'{part} {sum(values) / len(values):5.1f} fps ({", ".join(f"{v:.1f}" for v in values)})')
            print(f'  {label:30} ' + '   '.join(parts))
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
