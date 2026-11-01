"""Build the Linux AppImage of Astral Chain PC: the setup and launcher window plus the toolchain that builds the game
on the player's machine. It carries no game data and no code derived from the game.

    python3 linux/make_appimage.py [--out build/AstralChainPC-x86_64.AppImage] [--cache DIR ...]
    python3 linux/make_appimage.py --sdk-only      fill tools/ of this checkout instead (for running from source)

Downloads (pinned by SHA-256, kept in build/appimage-dl; --cache names extra folders to look in first):
  python-build-standalone 3.14 (with tkinter), Zig 0.17.0, glslang 16.6.0, NSTool 1.9.2, the Steam Runtime 3
  "sniper" SDK sysroot (SDL3 and the Vulkan loader: the game targets its glibc 2.31), appimagetool and the static
  type-2 AppImage runtime (no libfuse2 needed on the host).

The AppDir:
  AppRun, astral-chain-pc.desktop, astral-chain.png
  python/          the interpreter, tkinter, capstone and lz4
  astral_chain/, third_party/, linux/, launcher/art/, docs/, LICENSE, THIRD_PARTY_NOTICES.md, README.md
  tools/           zig/, glslang/, nstool/, romfs-build, linux-sdk/ (include/SDL3, lib/libSDL3.so.0, lib/libvulkan.so.1)
"""
import argparse
import hashlib
import os
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from astral_chain.setup_game import DOWNLOADS_LINUX  # noqa: E402

SNIPER = '3.0.20261021.266025'
PINS = {
    'python': ('https://github.com/astral-sh/python-build-standalone/releases/download/20261026/'
               'cpython-3.14.10%2B20261026-x86_64-unknown-linux-gnu-install_only.tar.gz',
               '1b38024924eb7f4cf0cab45d7b871d1f1991354190fccd2625ac7529a43786e2'),
    'sniper': (f'https://repo.steampowered.com/steamrt-images-sniper/snapshots/{SNIPER}/'
               'com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz',
               '8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf'),
    'appimagetool': ('https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage',
                     '793fca0ce4bcc309dc65d3f66669f58e3755643e4dadbe03932e7b436128494f'),
    'runtime': ('https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64',
                '617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46'),
    **{name: (url, digest) for name, (url, digest, _) in DOWNLOADS_LINUX.items()},
}
WHEELS = ('capstone==5.0.9', 'lz4==4.4.5')
CODE_ITEMS = ('astral_chain', 'third_party', 'linux', 'docs', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(name, caches, folder):
    url, digest = PINS[name]
    leaf = url.rsplit('/', 1)[1]
    for place in [*caches, folder]:
        for candidate in (place/leaf, place/leaf.replace('%2B', '+').replace('%2C', ',')):
            if candidate.is_file() and sha256(candidate) == digest:
                return candidate
    folder.mkdir(parents=True, exist_ok=True)
    target = folder/leaf
    print(f'== downloading {name}: {url}', flush=True)
    with urllib.request.urlopen(url, timeout=300) as response, open(str(target) + '.part', 'wb') as out:
        shutil.copyfileobj(response, out)
    if sha256(str(target) + '.part') != digest:
        raise SystemExit(f'{name}: SHA-256 mismatch for {url}')
    os.replace(str(target) + '.part', target)
    return target


def unpack(archive, destination, prefix='', members=None):
    """Files of a zip or tar under prefix (prefix stripped) into destination; members filters names (with prefix)."""
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()

    def place(name, data_open, mode, link=None):
        if not name.startswith(prefix) or (members and not members(name)):
            return
        rel = name[len(prefix):].lstrip('/')
        if not rel:
            return
        target = (destination/rel).resolve()
        if root not in target.parents:
            raise SystemExit(f'unexpected path {name} in {archive.name}')
        target.parent.mkdir(parents=True, exist_ok=True)
        if link is not None:
            if target.exists() or target.is_symlink():
                target.unlink()
            os.symlink(link, target)
            return
        with data_open() as source, open(target, 'wb') as out:
            shutil.copyfileobj(source, out)
        target.chmod(mode | stat.S_IRUSR | stat.S_IWUSR)

    if archive.name.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            for m in z.infolist():
                if not m.is_dir():
                    place(m.filename, lambda m=m: z.open(m), 0o755)
    else:
        with tarfile.open(archive) as t:
            for m in t:
                if m.isfile():
                    place(m.name, lambda m=m: t.extractfile(m), m.mode & 0o777)
                elif m.issym():
                    place(m.name, None, 0, link=m.linkname)


def make_sdk(sniper_tar, sdk):
    """SDL3 headers and library, and the Vulkan loader, from the sniper sysroot; their licenses beside them."""
    shutil.rmtree(sdk, ignore_errors=True)
    staging = sdk.parent/'sniper-staging'
    shutil.rmtree(staging, ignore_errors=True)
    wanted = ('usr/include/SDL3/', 'usr/lib/x86_64-linux-gnu/libSDL3.so', 'usr/lib/x86_64-linux-gnu/libvulkan.so',
              'usr/share/doc/libsdl3-0/copyright', 'usr/share/doc/libvulkan1/copyright')
    print('== unpacking SDL3 and the Vulkan loader from the Steam Runtime sysroot', flush=True)
    unpack(sniper_tar, staging, members=lambda n: n.lstrip('./').startswith(wanted))
    usr = staging/'usr'
    (sdk/'include').mkdir(parents=True)
    shutil.copytree(usr/'include'/'SDL3', sdk/'include'/'SDL3')
    (sdk/'lib').mkdir()
    for lib in ('libSDL3.so.0', 'libvulkan.so.1'):
        shutil.copyfile((usr/'lib'/'x86_64-linux-gnu'/lib).resolve(), sdk/'lib'/lib)
    (sdk/'licenses').mkdir()
    for package in ('libsdl3-0', 'libvulkan1'):
        source = usr/'share'/'doc'/package/'copyright'
        if source.is_file():
            shutil.copyfile(source, sdk/'licenses'/f'{package}.copyright')
    shutil.rmtree(staging, ignore_errors=True)


def make_tools(tools, caches, dl, sniper=True):
    tools.mkdir(parents=True, exist_ok=True)
    for name, (url, _, prefix) in DOWNLOADS_LINUX.items():
        if not (tools/name).is_dir():
            unpack(fetch(name, caches, dl), tools/(name + '.partial'), prefix)
            os.replace(tools/(name + '.partial'), tools/name)
    zig = tools/'zig'/'zig'
    if not (tools/'romfs-build').is_file():
        subprocess.run([str(zig), 'cc', '-O2', '-target', 'x86_64-linux-gnu.2.31', '-I', str(ROOT/'third_party'/'LASRecomp'/'runtime'),
                        str(ROOT/'astral_chain'/'runtime'/'romfs_tool.c'),
                        str(ROOT/'third_party'/'LASRecomp'/'runtime'/'romfs_build.c'), '-o', str(tools/'romfs-build')], check=True)
    if sniper and not (tools/'linux-sdk'/'lib'/'libSDL3.so.0').is_file():
        make_sdk(fetch('sniper', caches, dl), tools/'linux-sdk')


def icon_png(ico, out):
    """The largest PNG image inside the launcher's .ico."""
    data = ico.read_bytes()
    count = struct.unpack_from('<H', data, 4)[0]
    best = None
    for i in range(count):
        w, h, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        blob = data[offset:offset + size]
        if blob[:8] == b'\x89PNG\r\n\x1a\n' and (best is None or (w or 256) > best[0]):
            best = (w or 256, blob)
    if best:
        out.write_bytes(best[1])
        return True
    return False


APPRUN = '''#!/bin/sh
# Astral Chain PC: the setup and launcher window (python3 linux/launcher.py), with its own Python and toolchain.
HERE="$(dirname "$(readlink -f "$0")")"
unset PYTHONHOME PYTHONPATH
exec "$HERE/python/bin/python3" "$HERE/linux/launcher.py" "$@"
'''
DESKTOP = '''[Desktop Entry]
Type=Application
Name=Astral Chain PC
Comment=Native Linux build of Astral Chain from your own dump
Exec=AppRun
Icon=astral-chain
Categories=Game;
Terminal=false
'''


def make_appdir(appdir, caches, dl):
    shutil.rmtree(appdir, ignore_errors=True)
    appdir.mkdir(parents=True)
    print('== Python', flush=True)
    unpack(fetch('python', caches, dl), appdir/'python', 'python/')
    py = appdir/'python'/'bin'/'python3'
    subprocess.run([str(py), '-m', 'pip', 'install', '--only-binary=:all:', '--no-cache-dir', '--disable-pip-version-check',
                    '-q', *WHEELS], check=True)
    for junk in ('lib/python3.14/test', 'lib/python3.14/idlelib', 'lib/python3.14/ensurepip', 'share/man'):
        shutil.rmtree(appdir/'python'/junk, ignore_errors=True)
    print('== code', flush=True)
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc', '.git*')
    for item in CODE_ITEMS:
        source = ROOT/item
        if source.is_dir():
            shutil.copytree(source, appdir/item, ignore=ignore)
        elif source.is_file():
            shutil.copyfile(source, appdir/item)
    shutil.copytree(ROOT/'launcher'/'art', appdir/'launcher'/'art')
    print('== tools', flush=True)
    make_tools(appdir/'tools', caches, dl)
    (appdir/'AppRun').write_text(APPRUN)
    (appdir/'AppRun').chmod(0o755)
    (appdir/'astral-chain-pc.desktop').write_text(DESKTOP)
    if not icon_png(ROOT/'launcher'/'icon.ico', appdir/'astral-chain.png'):
        shutil.copyfile(ROOT/'launcher'/'art'/'1-visor.jpg', appdir/'astral-chain.png')
    shutil.copyfile(appdir/'astral-chain.png', appdir/'linux'/'astral-chain.png')
    shutil.copyfile(appdir/'astral-chain.png', appdir/'.DirIcon')
    # no game data or anything derived from it
    for path in appdir.rglob('*'):
        if path.suffix.lower() in ('.nsp', '.xci', '.nca', '.keys') or path.name in ('romfs.bin', 'main.npdm'):
            raise SystemExit(f'refusing to package {path}')


def pack(appdir, out, caches, dl):
    tool = fetch('appimagetool', caches, dl)
    runtime = fetch('runtime', caches, dl)
    tool.chmod(0o755)
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1')
    subprocess.run([str(tool), '--no-appstream', '--runtime-file', str(runtime), str(appdir), str(out)], check=True, env=env)
    out.chmod(0o755)
    (out.parent/(out.name + '.sha256')).write_text(f'{sha256(out)}  {out.name}\n')
    print(f'== {out} ({out.stat().st_size / 2**20:.0f} MB)')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=ROOT/'build'/'AstralChainPC-x86_64.AppImage')
    parser.add_argument('--cache', type=Path, action='append', default=[], help='folder with downloads to reuse')
    parser.add_argument('--sdk-only', action='store_true', help='fill tools/ of this checkout instead')
    parser.add_argument('--appdir-only', action='store_true', help='stop after making the AppDir')
    args = parser.parse_args()
    dl = ROOT/'build'/'appimage-dl'
    if args.sdk_only:
        make_tools(ROOT/'tools', args.cache, dl)
        print(f'== tools ready in {ROOT/"tools"}')
        return
    appdir = ROOT/'build'/'appimage'/'AstralChainPC.AppDir'
    make_appdir(appdir, args.cache, dl)
    if not args.appdir_only:
        pack(appdir, args.out.resolve(), args.cache, dl)


if __name__ == '__main__':
    main()
