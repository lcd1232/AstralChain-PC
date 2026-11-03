"""What differs between a Windows and a Linux build of the game: target, file names, folders, link inputs.

On Linux the runtime keeps its Windows code: runtime/posix/ holds a <windows.h> that implements the Win32 subset it
uses on POSIX and SDL3 (w32_*.c), and the game links those objects, SDL3 and the Vulkan loader. Binaries target
glibc 2.31 (the Steam Runtime 3 "sniper" baseline), so a build runs on SteamOS and any distribution at least as new.

Folders:
  CODE   this checkout (or the mounted AppImage): astral_chain/, third_party/; read-only is fine
  HOME   where the game is installed: game/, build/, userdata/ (AC_HOME, default CODE)
  TOOLS  zig, glslang, nstool, the RomFS packer and, on Linux, the SDK (AC_TOOLS, default CODE/tools)
The Linux SDK folder (TOOLS/linux-sdk) holds include/SDL3/*.h, lib/libSDL3.so.0 and lib/libvulkan.so.1 (link input
only: the game loads the system's Vulkan loader by its soname).
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

LINUX = sys.platform.startswith('linux')
CODE = Path(__file__).resolve().parent.parent
HOME = Path(os.environ['AC_HOME']).expanduser().resolve() if os.environ.get('AC_HOME') else CODE
TOOLS = Path(os.environ['AC_TOOLS']).expanduser().resolve() if os.environ.get('AC_TOOLS') else CODE/'tools'
RUNTIME = CODE/'astral_chain'/'runtime'
POSIX = RUNTIME/'posix'
SDK = TOOLS/'linux-sdk'

TARGET = 'x86_64-linux-gnu.2.31' if LINUX else 'x86_64-windows-gnu'
EXE = '' if LINUX else '.exe'

ZIG = TOOLS/'zig'/('zig' + EXE)
GLSLANG = TOOLS/'glslang'/'bin'/('glslang' + EXE)
NSTOOL = TOOLS/'nstool'/('nstool' + EXE)
ROMFS_TOOL = TOOLS/('romfs-build' + EXE)

SHIM_SOURCES = ('w32_core.c', 'w32_files.c', 'w32_memory.c', 'w32_sdl.c')


def target():
    return ['-target', TARGET]


def shim_include():
    """Compile flags that make <windows.h> (and the other Windows headers) resolve to the POSIX subset."""
    return ['-I', str(POSIX)] if LINUX else []


def freetype_include():
    """FreeType's headers (TOOLS/linux-sdk/freetype) for the button icons' key labels (las/button_prompts.c, which loads
    the system's libfreetype at run time); in a folder of their own, so the NVN layer's SDL3 shim stays the only SDL3."""
    return ['-isystem', str(SDK/'freetype')] if LINUX else []


def vulkan_library():
    """Link input for the Vulkan loader on Linux (the SDK's libvulkan.so.1)."""
    return SDK/'lib'/'libvulkan.so.1'


def _digest(paths, extra=''):
    h = hashlib.sha256(extra.encode())
    for p in paths:
        h.update(p.name.encode())
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def shim_objects(compiler, cache, main=False):
    """Objects of the Win32 subset (Linux only), cached by sources, headers and compiler."""
    if not LINUX:
        return []
    compiler = Path(compiler).resolve()
    sources = [POSIX/name for name in SHIM_SOURCES] + ([POSIX/'w32_main.c'] if main else [])
    headers = sorted(POSIX.glob('*.h'))
    flags = [*target(), '-O2', '-mcpu=x86_64_v3', '-fno-strict-aliasing', '-I', str(POSIX), '-isystem', str(SDK/'include')]
    key = _digest([*sources, *headers, compiler], json.dumps(flags))[:24]
    folder = Path(cache).resolve()/f'w32-{key}'
    objects = [folder/(s.stem + '.o') for s in sources]
    if all(o.is_file() for o in objects):
        return objects
    folder.mkdir(parents=True, exist_ok=True)
    for source, obj in zip(sources, objects):
        result = subprocess.run([str(compiler), 'cc', *flags, '-c', str(source), '-o', str(obj) + '.partial'],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'{source.name} failed to compile:\n{result.stdout}{result.stderr}')
        os.replace(str(obj) + '.partial', obj)
    return objects


def link_flags(compiler, cache, output_dir, main=False):
    """Libraries and options for linking a program into output_dir. Linux: the Win32 subset objects, SDL3 (copied
    beside the program, found through $ORIGIN) and the C libraries; Windows: the system import libraries."""
    if not LINUX:
        return ['-lbcrypt', '-luser32']
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    sdl = SDK/'lib'/'libSDL3.so.0'
    if not sdl.is_file():
        raise RuntimeError(f'SDL3 not found at {sdl} (the Linux SDK folder is incomplete)')
    beside = output_dir/'libSDL3.so.0'
    if not beside.is_file() or beside.stat().st_size != sdl.stat().st_size:
        shutil.copyfile(sdl, beside)
    return [*(str(o) for o in shim_objects(compiler, cache, main)), str(sdl), '-lpthread', '-ldl', '-lm',
            '-Wl,-rpath,$ORIGIN', '-Wl,-z,noexecstack']


def main_flags():
    """The game's main() runs on a 64 MB stack: on Windows through the linker, on Linux on a thread (w32_main.c)."""
    return ['-Dmain=w32_program_main'] if LINUX else ['-Wl,--stack,67108864']


def kill_tree(process):
    if LINUX:
        process.kill()
    else:
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)


def windows_path(path):
    """A path as the runtime's Windows code expects it. Linux: the save code wants a drive letter, so a POSIX path
    becomes Z:\\... (the Win32 subset maps any drive letter to the root, as Wine does)."""
    text = str(path)
    if LINUX and text.startswith('/'):
        return 'Z:' + text.replace('/', '\\')
    return text
