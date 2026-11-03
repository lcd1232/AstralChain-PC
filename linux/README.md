# Linux and Steam Deck

A native Linux build of Astral Chain PC, without Proton or Wine. As on Windows, the setup builds the game once from
**your own** dump (base game, the v1.0.1 update and your console's `prod.keys`); after that, **Play** starts the
built program directly. Nothing is compiled when the game starts.

The setup and launcher window comes as an AppImage (`AstralChainPC-x86_64.AppImage`, about 160 MB). It carries its own
Python, the Zig compiler, glslang, NSTool and SDL3, and no game data.

## Requirements

- x86-64 CPU with AVX2 (the Steam Deck qualifies)
- Vulkan 1.3 GPU and driver (Mesa RADV, ANV or the NVIDIA driver)
- Linux 6.7 or newer (CPU-written memory is tracked with `userfaultfd`; older kernels work, but slower). SteamOS 3.7
  and later qualify
- glibc 2.34 or newer for the setup tools (the game itself needs 2.29)
- About 35 GB free during setup, 20 GB after

## Installing

1. Make the AppImage executable and start it (on the Steam Deck: in Desktop Mode).
   ```
   chmod +x AstralChainPC-x86_64.AppImage
   ./AstralChainPC-x86_64.AppImage
   ```
2. Pick the install folder (default `~/Games/AstralChainPC`), your `prod.keys`, the base game (XCI or NSP) and the
   v1.0.1 update (NSP), then press **Install**. Extraction, translation, compilation and shader preparation take
   about 10-30 minutes, once.
3. Press **Play**.

Without a window: `./AstralChainPC-x86_64.AppImage --install --keys prod.keys --base base.nsp --update update.nsp
--dest ~/Games/AstralChainPC`.

A newer AppImage rebuilds the game once at the next **Play** (a few minutes); the game data, settings and saves stay.

The install folder holds `game/` (extracted data), `build/` (the program, shader and pipeline caches) and
`userdata/` (saves in `userdata/saves`, backups next to them). Do not share it: it contains the game.

## Steam Deck

- **Add to Steam** (with Steam closed, the game installed) creates a non-Steam shortcut with artwork that starts the
  game itself: `play.sh` in the install folder, with the launcher's settings (rewritten whenever they are saved), and
  glslang copied next to it. The AppImage is not needed to play from Steam and can be removed. In Game Mode the game
  then starts from the library like any other; Proton is not used. A new launcher version still rebuilds the game:
  open it and press Play once.
- On the Deck the launcher defaults to: fullscreen, handheld mode (rendering at 1280x720 instead of 1600x900),
  graphics prepared on a separate thread, BC7 texture compression on the GPU (less memory traffic on the shared
  memory), Vsync (FIFO) and the controller's button icons.
- **Frame rate.** Settings > Frame rate limit offers 60, 50, 45, 40 and 30 fps. The game's step follows the measured
  frame time, so it runs at the right speed at any rate. For the smoothest 40 fps, set the limit to 40 and the
  display's refresh rate to 40 Hz in the Steam quick access menu (Performance).
- The Deck's controls arrive through Steam Input as a standard gamepad.

The Deck values are starting points, not measurements: frame rates on the Deck have not been measured yet. Reports
(with `build/game/round-00/trace.log`) are welcome.

## Settings

The launcher's Settings set the same variables as the Windows launcher (`AC_FULLSCREEN`, `AC_HANDHELD`,
`LARECOMP_VK_PRESENT`, `AC_ASYNC_SUBMIT`, `LARECOMP_BC7`, ...), plus:

| Variable | Effect |
| --- | --- |
| `AC_FPS_LIMIT=<20..59>` | paces frames on a grid of that rate instead of 60 Hz |

They live in `~/.config/astral-chain-pc/config.json`. Mesa's and NVIDIA's shader caches are kept with the game
(`build/game/cache/driver`), so the setup's GPU warm-up and play share them.

## How the Linux build works

The runtime keeps its Windows code. `astral_chain/runtime/posix/` holds a `<windows.h>` and the other headers it
includes, implemented on POSIX and SDL3:

| Windows | Linux |
| --- | --- |
| threads, events, waits, SRW locks | pthreads, one waiter list per object |
| `VirtualAlloc` / `VirtualFree` / `VirtualProtect` / `VirtualQuery` | `mmap` reservations, `mprotect`, `MADV_DONTNEED`, a state byte per guest page |
| `GetWriteWatch` | `userfaultfd` asynchronous write-protect and `PAGEMAP_SCAN` (Linux 6.7; kernel writes are tracked too) |
| placeholders, `MapViewOfFile3` | `memfd` sections mapped with `MAP_FIXED` |
| vectored exception handlers | `SIGSEGV` / `SIGBUS` handlers on an alternate stack, with an `EXCEPTION_RECORD` and `CONTEXT` |
| window, messages, raw mouse | SDL3 window; SDL events become `WM_*` messages; capture is relative mouse mode |
| `waveOut` | SDL3 audio stream; the position counts what was played, not what the device buffered |
| XInput | SDL3 gamepads |
| Vulkan Win32 surface | `SDL_Vulkan_CreateSurface` (Wayland or X11) |
| `C:\` paths | any drive letter maps to `/` (the save code requires one) |

`astral_chain/host.py` holds what differs in the build: the target `x86_64-linux-gnu.2.31` (the Steam Runtime 3
"sniper" baseline), the install folder (`AC_HOME`) apart from the code, and the link inputs (the layer's objects,
SDL3 found beside the program through `$ORIGIN`, the system's Vulkan loader). The game's own source changes are
three: the Vulkan surface (`device.c`), `AC_FPS_LIMIT` and the sampling profiler's image range (`nvn_hle.h`).

## Building the AppImage

```
python3 linux/make_appimage.py            # build/AstralChainPC-x86_64.AppImage
python3 linux/make_appimage.py --sdk-only # fill tools/ to run linux/launcher.py from this checkout
```

Downloads are pinned by SHA-256: python-build-standalone 3.14, Zig 0.17.0, glslang 16.6.0, NSTool 1.9.2, the Steam
Runtime "sniper" SDK sysroot (SDL3 3.4 and the Vulkan loader; 1.2 GB, only two libraries and the SDL headers are
kept), appimagetool and the static type-2 runtime.

## Credits

The Linux approach follows [TotK Recomp](https://github.com/valentderah/totk-pc-recomp)'s Linux port (AppImage
setup, Steam Runtime baseline, `userfaultfd` write tracking). Its write-watch code was adapted from
`runtime/platform/linux.c` (MIT).
