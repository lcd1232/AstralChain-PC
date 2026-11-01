"""Astral Chain PC for Linux and the Steam Deck: setup and launcher window (python3 linux/launcher.py).

The window installs the game from your own dump (prod.keys, the base game and the v1.0.1 update) into an install
folder, once: extraction, translation to C and compilation into a native Linux program, shader preparation. Play then
starts that program directly; nothing is compiled when the game starts. A new version of this launcher (a new AppImage)
rebuilds the game once, keeping the game data and saves.

    launcher.py                 the window
    launcher.py --play          start the game directly when it is installed and built (Steam shortcut); else the window
    launcher.py --install --base B --update U --keys K [--dest D]   install without the window
    launcher.py --selftest      build the window, run the checks it can without game files, and close

Settings live in $XDG_CONFIG_HOME/astral-chain-pc/config.json; the install folder holds game/, build/ and userdata/
(saves in userdata/saves).
"""
import argparse
import json
import locale
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

CODE = Path(__file__).resolve().parent.parent
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home()/'.config')/'astral-chain-pc'/'config.json'
APPIMAGE = os.environ.get('APPIMAGE')


def is_steam_deck():
    try:
        vendor = Path('/sys/devices/virtual/dmi/id/board_vendor').read_text().strip()
        product = Path('/sys/devices/virtual/dmi/id/product_name').read_text().strip()
        return vendor == 'Valve' and product in ('Jupiter', 'Galileo')
    except OSError:
        return os.environ.get('SteamDeck') == '1'


DECK = is_steam_deck()


def use_xft_tk():
    """Load the AppImage's Tk built with Xft (python/lib/tk-xft) before tkinter, if the system has libXft and
    fontconfig: _tkinter then binds to it by its soname. The Tk of python-build-standalone draws only the bitmap
    'fixed' font (no Xft) and, with its static libX11, no themed cursor. Without libXft that Tk stays in use."""
    lib = Path(sys.executable).resolve().parent.parent/'lib'/'tk-xft'/'libtcl9tk9.0.so'
    if 'tkinter' in sys.modules or '_tkinter' in sys.modules or not lib.is_file():
        return False
    import ctypes
    try:
        ctypes.CDLL(str(lib), mode=ctypes.RTLD_GLOBAL)
        return True
    except OSError:
        return False

# ------------------------------------------------------------------------------------------------ text
SYSTEM_LOCALE = (os.environ.get('LANGUAGE') or os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or
                 os.environ.get('LANG') or locale.getlocale()[0] or '').lower()
RU = SYSTEM_LOCALE.startswith('ru')   # (the launcher's language: set_ui_language follows the settings)


def set_ui_language(config):
    global RU
    choice = config['settings'].get('ui_language', 'auto')
    RU = SYSTEM_LOCALE.startswith('ru') if choice == 'auto' else choice == 'ru'


# The game's text language (menus and subtitles), as the Switch's system language: languages this game has
GAME_LANGUAGES = (('en-US', 'English'), ('ja', '日本語'), ('fr', 'Français'), ('de', 'Deutsch'), ('es', 'Español'),
                  ('it', 'Italiano'), ('ru', 'Русский'), ('ko', '한국어'), ('zh-Hant', '繁體中文'), ('zh-Hans', '简体中文'))


def system_game_language():
    lang = SYSTEM_LOCALE.split(':')[0].split('.')[0].replace('-', '_')
    if lang.startswith('zh'):
        return 'zh-Hant' if lang.endswith(('_tw', '_hk', '_mo', 'hant')) else 'zh-Hans'
    for code, _ in GAME_LANGUAGES:
        if lang[:2] == code[:2]:
            return code
    return 'en-US'
TEXT = {
    'title': ('Astral Chain PC', 'Astral Chain PC'),
    'subtitle': ('Native Linux build from your own dump', 'Нативная сборка для Linux из вашего дампа'),
    'install_folder': ('Install folder', 'Папка установки'),
    'keys': ('prod.keys', 'prod.keys'),
    'base': ('Base game (XCI or NSP)', 'Базовая игра (XCI или NSP)'),
    'update': ('Update v1.0.1 (NSP)', 'Обновление v1.0.1 (NSP)'),
    'browse': ('Browse...', 'Обзор...'),
    'install': ('Install', 'Установить'),
    'play': ('Play', 'Играть'),
    'settings': ('Settings', 'Настройки'),
    'steam': ('Add to Steam', 'Добавить в Steam'),
    'open': ('Open folder', 'Открыть папку'),
    'cancel': ('Cancel', 'Отмена'),
    'close': ('Close', 'Закрыть'),
    'save': ('Save', 'Сохранить'),
    'st_none': ('Not installed. Pick your files and the install folder, then press Install.',
                'Не установлено. Выберите свои файлы и папку установки и нажмите «Установить».'),
    'st_ready': ('Ready to play.', 'Готово к игре.'),
    'st_stale': ('The installed build is out of date (a newer launcher or another game language): Play rebuilds the '
                 'game once (a minute to a few minutes).',
                 'Сборка устарела (новая версия лаунчера или другой язык игры): «Играть» один раз пересоберёт игру '
                 '(от минуты до нескольких минут).'),
    'st_shaders': ('Ready to play (shaders are prepared before the first start).',
                   'Готово к игре (шейдеры подготовятся перед первым запуском).'),
    'st_busy': ('Working...', 'Выполняется...'),
    'st_playing': ('Playing...', 'Идёт игра...'),
    'st_closed': ('The game was closed.', 'Игра закрыта.'),
    'st_failed': ('Stopped: see the log below.', 'Остановлено: подробности в журнале ниже.'),
    'st_installed': ('Installed. Press Play.', 'Установлено. Нажмите «Играть».'),
    'missing': ('Pick all three files first.', 'Сначала выберите все три файла.'),
    'need_glibc': ('This system\'s C library is older than 2.34; the setup tools need 2.34 or newer.',
                   'Библиотека C в системе старше 2.34; инструментам установки нужна 2.34 или новее.'),
    'steam_running': ('Close Steam completely first (Steam > Exit), then press Add to Steam again.',
                      'Сначала полностью закройте Steam (Steam → Выход), затем снова нажмите «Добавить в Steam».'),
    'steam_done': ('Added to Steam as "Astral Chain PC". Start Steam; no Proton is needed for it.',
                   'Добавлено в Steam как «Astral Chain PC». Запустите Steam; Proton для неё не нужен.'),
    'steam_none': ('No Steam user found in ~/.local/share/Steam.', 'Профиль Steam не найден в ~/.local/share/Steam.'),
    'exit_code': ('The game stopped (exit code {code}). The logs are in {folder}.',
                  'Игра остановилась (код {code}). Журналы: {folder}.'),
    'deck': ('Steam Deck detected: handheld settings are the defaults.',
             'Обнаружен Steam Deck: по умолчанию включены настройки для портативного режима.'),
    'log': ('Log', 'Журнал'),
    'auto': ('Automatic', 'Автоматически'),
    'ui_later': ('The launcher\'s language changes when it is opened next time.',
                 'Язык лаунчера сменится при следующем запуске.'),
    'group_launcher': ('Launcher', 'Лаунчер'),
    'group_display': ('Display', 'Экран'),
    'group_performance': ('Performance', 'Производительность'),
    'group_game': ('Game', 'Игра'),
    'group_sound': ('Sound', 'Звук'),
    'group_mouse': ('Mouse and keyboard', 'Мышь и клавиатура'),
    'group_trouble': ('Troubleshooting', 'Устранение неполадок'),
}


def t(key, **kw):
    text = TEXT[key][1 if RU else 0]
    return text.format(**kw) if kw else text


# ------------------------------------------------------------------------------------------------ settings
# key, group, (English, Russian), kind, default, default on the Steam Deck, environment for the game
OPTIONS = [
    ('ui_language', 'group_launcher', ('Launcher language', 'Язык лаунчера'),
     (('auto', 'auto'), ('en', 'English'), ('ru', 'Русский')), 'auto', 'auto', lambda v: {}),
    ('fullscreen', 'group_display', ('Start in fullscreen (F11 / Alt+Enter switches)', 'Запускать в полноэкранном режиме (F11 / Alt+Enter переключает)'),
     'check', '0', '1', lambda v: {'AC_FULLSCREEN': '1'} if v == '1' else {}),
    ('handheld', 'group_display', ('Handheld mode: render at 1280x720 (instead of 1600x900)', 'Портативный режим: рендер 1280x720 (вместо 1600x900)'),
     'check', '0', '1', lambda v: {'AC_HANDHELD': '1'} if v == '1' else {}),
    ('present', 'group_display', ('Presentation mode', 'Режим вывода'), ('mailbox', 'fifo', 'immediate'), 'mailbox', 'fifo',
     lambda v: {} if v == 'mailbox' else {'LARECOMP_VK_PRESENT': v}),
    ('fps_limit', 'group_performance', ('Frame rate limit (fps)', 'Ограничение частоты кадров (fps)'), ('60', '50', '45', '40', '30'), '60', '60',
     lambda v: {} if v == '60' else {'AC_FPS_LIMIT': v}),
    ('async_submit', 'group_performance', ('Prepare graphics on a separate thread (faster on 4-8 core CPUs)', 'Готовить графику в отдельном потоке (быстрее на 4-8-ядерных CPU)'),
     'check', '0', '1', lambda v: {'AC_ASYNC_SUBMIT': '1'} if v == '1' else {}),
    ('gpu_warm', 'group_performance', ('Compile shaders for this GPU before playing', 'Компилировать шейдеры под эту видеокарту до игры'),
     'check', '1', '1', lambda v: {} if v == '1' else {'AC_GPU_WARM': '0'}),
    ('bc7', 'group_performance', ('Compress textures on the GPU (BC7: less memory traffic; short pauses while areas load)', 'Сжимать текстуры на GPU (BC7: меньше нагрузка на память; короткие паузы при загрузке зон)'),
     'check', '0', '1', lambda v: {'LARECOMP_BC7': '1'} if v == '1' else {}),
    ('game_language', 'group_game', ('Game language (text and subtitles; the game is rebuilt once, about a minute)',
                                     'Язык игры (текст и субтитры; игра один раз пересобирается, около минуты)'),
     (('auto', 'auto'), *GAME_LANGUAGES), 'auto', 'auto',
     lambda v: {'AC_LANGUAGE': system_game_language() if v == 'auto' else v}),
    ('cutscene_fps_30', 'group_game', ('Cutscenes at 30 fps, as on the Switch', 'Ролики в 30 fps, как на Switch'),
     'check', '0', '0', lambda v: {'AC_CUTSCENE_FPS': '30'} if v == '1' else {}),
    ('ignore_focus', 'group_game', ('Keep running while the window is minimized', 'Не ставить на паузу при сворачивании окна'),
     'check', '0', '1', lambda v: {'AC_IGNORE_FOCUS': '1'} if v == '1' else {}),
    ('save_snapshots', 'group_game', ('Back up each save (the newest 30 are kept)', 'Резервная копия при каждом сохранении (хранятся 30 последних)'),
     'check', '1', '1', lambda v: {} if v == '1' else {'AC_SAVE_SNAPSHOTS': '0'}),
    ('audio_resync', 'group_sound', ('Skip sound gaps after a stall to stay in sync', 'Пропускать звук после подвисания, чтобы не отставал'),
     'check', '1', '1', lambda v: {} if v == '1' else {'AC_AUDIO_RESYNC': '0'}),
    ('mouse_look', 'group_mouse', ('Mouse moves the camera (click the window to capture)', 'Мышь управляет камерой (щёлкните по окну, чтобы захватить)'),
     'check', '1', '1', lambda v: {} if v == '1' else {'AC_MOUSE_LOOK': '0'}),
    ('mouse_invert', 'group_mouse', ('Invert mouse up and down', 'Инвертировать мышь по вертикали'),
     'check', '0', '0', lambda v: {'AC_MOUSE_INVERT': '1'} if v == '1' else {}),
    ('prompts', 'group_mouse', ('Button icons', 'Значки кнопок'), ('switch', 'keyboard'), 'keyboard', 'switch',
     lambda v: {'AC_PROMPTS': 'switch'} if v == 'switch' else {}),
    ('gpu_astc', 'group_trouble', ('Decode ASTC textures on the GPU', 'Декодировать ASTC-текстуры на GPU'),
     'check', '1', '1', lambda v: {} if v == '1' else {'LARECOMP_GPU_ASTC': '0'}),
    ('gpl', 'group_trouble', ('Shader libraries (fast pipeline linking)', 'Библиотеки шейдеров (быстрая сборка конвейеров)'),
     'check', '1', '1', lambda v: {} if v == '1' else {'LARECOMP_GPL': '0'}),
    ('pipe_warm', 'group_trouble', ('Build recorded pipelines ahead of their first draw', 'Заранее собирать записанные конвейеры'),
     'check', '1', '1', lambda v: {} if v == '1' else {'LARECOMP_NO_PIPE_WARM': '1'}),
]
SETUP_KEYS = ('gpu_warm', 'gpl', 'game_language')   # these change what the setup builds or compiles


def default_install():
    return str(Path.home()/'Games'/'AstralChainPC')


def load_config():
    try:
        config = json.loads(CONFIG.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        config = {}
    config.setdefault('install', default_install())
    settings = config.setdefault('settings', {})
    for key, _, _, _, default, deck_default, _ in OPTIONS:
        settings.setdefault(key, deck_default if DECK else default)
    return config


def save_config(config):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG.with_suffix('.tmp')
    tmp.write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
    os.replace(tmp, CONFIG)


def option_env(config, keys=None):
    env = {}
    for key, _, _, _, _, _, make in OPTIONS:
        if keys is None or key in keys:
            env.update(make(config['settings'].get(key, '')))
    return env


def base_env(config):
    """Every process the launcher starts: the install folder, the Python path and the shader caches. Mesa and NVIDIA
    keep their shader caches with the game, so the setup's GPU warm-up and play share them and nothing evicts them."""
    home = Path(config['install']).expanduser()
    cache = home/'build'/'game'/'cache'
    env = dict(os.environ)
    env.update(AC_HOME=str(home), PYTHONPATH=os.pathsep.join(filter(None, [str(CODE), os.environ.get('PYTHONPATH')])),
               PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8',
               MESA_SHADER_CACHE_DIR=str(cache/'driver'), MESA_SHADER_CACHE_MAX_SIZE='4G',
               __GL_SHADER_DISK_CACHE_PATH=str(cache/'driver'), __GL_SHADER_DISK_CACHE_SIZE='4294967296',
               __GL_SHADER_DISK_CACHE_SKIP_CLEANUP='1')
    env.update(option_env(config, SETUP_KEYS))
    return env


def glibc_ok():
    try:
        major, minor = (int(x) for x in os.confstr('CS_GNU_LIBC_VERSION').split()[1].split('.')[:2])
        return (major, minor) >= (2, 34)
    except (ValueError, OSError, IndexError):
        return True


def status(config):
    """setup_game --status: {"installed", "build", "shaders"}."""
    try:
        out = subprocess.run([sys.executable, '-m', 'astral_chain.setup_game', '--status'], env=base_env(config),
                             cwd=CODE, capture_output=True, text=True, timeout=60).stdout
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError, subprocess.SubprocessError):
        return {'installed': False, 'build': 'missing', 'shaders': 'never'}


def game_env(config):
    """run_guarded.ps1's variables (plus the build's run-env.txt), the launcher's and the chosen settings."""
    home = Path(config['install']).expanduser()
    env = base_env(config)
    env.update(AC_NVN_HLE_LINKED='vulkan', AC_PARALLEL='1', AC_GLSLANG=str(CODE/'tools'/'glslang'/'bin'/'glslang'))
    run_env = home/'build'/'game'/'run-env.txt'
    if run_env.is_file():
        for line in run_env.read_text(encoding='utf-8').splitlines():
            if '=' in line:
                name, value = line.split('=', 1)
                env[name.strip()] = value.strip()
    env.update(option_env(config))
    if not env.get('AC_FONT') and (font := system_font()):
        env['AC_FONT'] = font
    if not env.get('AC_PROMPT_FONT') and (font := system_font(':bold')):
        env['AC_PROMPT_FONT'] = font
    return env


def system_font(style=''):
    """A TrueType sans face from fontconfig, or None (the game then tries the usual font paths itself): regular for the
    game's system font (nn::pl shared font: the Switch's own is not in the dump), bold for the key labels on the
    button icons."""
    try:
        out = subprocess.run(['fc-match', '-f', '%{file}|%{fontformat}', f'sans-serif{style}:fontformat=TrueType'],
                             capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    path, _, kind = out.partition('|')
    return path if kind == 'TrueType' and path.lower().endswith(('.ttf', '.ttc')) and Path(path).is_file() else None


def run_game(config, log=print):
    """Start the built game and wait for it. Returns its exit code."""
    home = Path(config['install']).expanduser()
    run = home/'build'/'game'
    round0 = run/'round-00'
    exe = round0/'trace.exe'
    env = game_env(config)
    for name in ('AC_FULLSCREEN', 'AC_HANDHELD', 'AC_FPS_LIMIT', 'AC_ASYNC_SUBMIT', 'LARECOMP_VK_PRESENT'):
        if name in env:
            log(f'{name}={env[name]}')
    with open(round0/'trace.log', 'w', encoding='utf-8', errors='replace') as out, \
            open(round0/'stderr.log', 'w', encoding='utf-8', errors='replace') as err:
        process = subprocess.Popen([str(exe), str(round0/'relocations.bin')], cwd=run, env=env, stdout=out, stderr=err)
        try:
            return process.wait()
        except KeyboardInterrupt:
            process.send_signal(signal.SIGTERM)
            return process.wait()


def setup_command(*args):
    return [sys.executable, '-u', '-m', 'astral_chain.setup_game', *args]


# ------------------------------------------------------------------------------------------------ Steam shortcut
def steam_running():
    for proc in Path('/proc').iterdir():
        if proc.name.isdigit():
            try:
                if (proc/'comm').read_text().strip() == 'steam':
                    return True
            except OSError:
                pass
    return False


def add_to_steam():
    """A non-Steam shortcut to this AppImage (or launcher) with --play; artwork from launcher/art. Steam must be closed
    (it rewrites shortcuts.vdf on exit). Returns the number of Steam users updated."""
    import struct
    import zlib
    steam = Path.home()/'.local/share/Steam'
    users = [p for p in (steam/'userdata').glob('*') if p.is_dir() and p.name.isdigit() and p.name != '0']
    if not users:
        return 0
    exe = f'"{APPIMAGE}"' if APPIMAGE else f'"{sys.executable}"'
    options = '--play' if APPIMAGE else f'"{Path(__file__).resolve()}" --play'
    name = 'Astral Chain PC'
    for user in users:
        config = user/'config'
        config.mkdir(parents=True, exist_ok=True)
        shortcuts = config/'shortcuts.vdf'
        root = vdf_parse(shortcuts.read_bytes()) if shortcuts.exists() else {'shortcuts': (0, {})}
        entries = root.setdefault('shortcuts', (0, {}))[1]
        entry = next((v for _, v in entries.values() if isinstance(v, dict) and v.get('AppName', (1, ''))[1] == name), None)
        appid = zlib.crc32((exe + name).encode()) | 0x80000000
        if entry is None:
            entry = {}
            entries[str(max([int(k) for k in entries if k.isdigit()], default=-1) + 1)] = (0, entry)
        icon = CODE/'linux'/'astral-chain.png'
        for key, value in {'AppName': name, 'Exe': exe, 'StartDir': f'"{Path(exe.strip(chr(34))).parent}"',
                           'icon': str(icon) if icon.is_file() else '', 'ShortcutPath': '', 'LaunchOptions': options}.items():
            entry[key] = (1, value)
        for key, value in {'appid': appid, 'IsHidden': 0, 'AllowDesktopConfig': 1, 'AllowOverlay': 1, 'OpenVR': 0,
                           'Devkit': 0}.items():
            entry[key] = (2, struct.pack('<I', value))
        entry.setdefault('LastPlayTime', (2, struct.pack('<I', 0)))
        entry.setdefault('tags', (0, {}))
        grid = config/'grid'
        grid.mkdir(exist_ok=True)
        art = CODE/'launcher'/'art'
        for source, suffix in (('1-visor.jpg', 'p'), ('2-ark-city.jpg', ''), ('5-panorama.jpg', '_hero')):
            if (art/source).is_file():
                shutil.copyfile(art/source, grid/f'{appid}{suffix}.jpg')
        if shortcuts.exists():
            shutil.copy2(shortcuts, shortcuts.with_name('shortcuts.vdf.backup-astral-chain'))
        tmp = shortcuts.with_suffix('.vdf.tmp')
        tmp.write_bytes(vdf_encode(root))
        os.replace(tmp, shortcuts)
    return len(users)


def vdf_parse(data):
    pos = 0

    def string():
        nonlocal pos
        end = data.index(0, pos)
        value = data[pos:end].decode('utf-8', errors='surrogateescape')
        pos = end + 1
        return value

    def obj():
        nonlocal pos
        result = {}
        while True:
            kind = data[pos]
            pos += 1
            if kind == 8:
                return result
            key = string()
            if kind == 0:
                value = obj()
            elif kind == 1:
                value = string()
            elif kind in (2, 3, 7):
                size = 8 if kind == 7 else 4
                value = data[pos:pos + size]
                pos += size
            else:
                raise ValueError(f'unsupported VDF type {kind}')
            result[key] = (kind, value)
    return obj()


def vdf_encode(obj):
    out = bytearray()
    for key, (kind, value) in obj.items():
        out += bytes([kind]) + key.encode('utf-8', errors='surrogateescape') + b'\0'
        if kind == 0:
            out += vdf_encode(value)
        elif kind == 1:
            out += value.encode('utf-8', errors='surrogateescape') + b'\0'
        else:
            out += value
    return bytes(out + b'\x08')


# ------------------------------------------------------------------------------------------------ window
class App:
    def __init__(self, root, config):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.root, self.config = root, config
        self.events = queue.Queue()
        self.busy = False
        self.process = None
        root.title(t('title'))
        # Tk leaves a window's cursor unset (inherited from the root window); under XWayland the root window's cursor
        # is empty, so the pointer vanished over everything but text fields. Toplevels, file and message dialogs
        # and combobox lists get the arrow; their children inherit it.
        root.configure(cursor='left_ptr')
        for cls in ('Toplevel', 'TkFDialog', 'TkChooseDir', 'Dialog', 'ComboboxPopdown'):  # (a combobox's list is a toplevel)
            root.option_add(f'*{cls}.cursor', 'left_ptr')
        root.minsize(760, 0)
        style = ttk.Style(root)
        if 'clam' in style.theme_names():
            style.theme_use('clam')
        scale = 1.25 if DECK else 1.0
        base_font = ('Sans', int(11 * scale))
        root.option_add('*Font', base_font)
        style.configure('.', font=base_font)
        style.configure('Big.TButton', font=('Sans', int(14 * scale), 'bold'), padding=(24, 10))
        style.configure('TButton', padding=(10, 6))

        outer = ttk.Frame(root, padding=14)
        outer.pack(fill='both', expand=True)
        # a font option on the label: the option database's *Font would override the style's
        ttk.Label(outer, text=t('title'), font=('Sans', int(20 * scale), 'bold')).pack(anchor='w')
        ttk.Label(outer, text=t('subtitle') + (f'  ·  {t("deck")}' if DECK else '')).pack(anchor='w', pady=(0, 10))

        form = ttk.Frame(outer)
        form.pack(fill='x')
        form.columnconfigure(1, weight=1)
        self.vars = {}
        rows = [('install', t('install_folder'), True), ('keys', t('keys'), False), ('base', t('base'), False),
                ('update', t('update'), False)]
        for row, (key, label, folder) in enumerate(rows):
            ttk.Label(form, text=label).grid(row=row, column=0, sticky='w', padx=(0, 10), pady=3)
            var = tk.StringVar(value=config.get(key, ''))
            self.vars[key] = var
            ttk.Entry(form, textvariable=var).grid(row=row, column=1, sticky='ew', pady=3)
            ttk.Button(form, text=t('browse'), command=lambda k=key, f=folder: self.browse(k, f)).grid(
                row=row, column=2, padx=(8, 0), pady=3)

        buttons = ttk.Frame(outer)
        buttons.pack(fill='x', pady=12)
        self.play_button = ttk.Button(buttons, text=t('play'), style='Big.TButton', command=self.on_play)
        self.play_button.pack(side='left')
        self.install_button = ttk.Button(buttons, text=t('install'), command=self.on_install)
        self.install_button.pack(side='left', padx=(12, 0))
        ttk.Button(buttons, text=t('settings'), command=self.on_settings).pack(side='left', padx=(12, 0))
        ttk.Button(buttons, text=t('steam'), command=self.on_steam).pack(side='left', padx=(12, 0))
        ttk.Button(buttons, text=t('open'), command=self.on_open).pack(side='left', padx=(12, 0))
        self.cancel_button = ttk.Button(buttons, text=t('cancel'), command=self.on_cancel, state='disabled')
        self.cancel_button.pack(side='right')

        self.status = tk.StringVar(value=t('st_busy'))
        ttk.Label(outer, textvariable=self.status, wraplength=900).pack(anchor='w')
        self.progress = ttk.Progressbar(outer, mode='indeterminate')
        self.progress.pack(fill='x', pady=(6, 8))
        # the log appears with the first line written to it (Install, a failed start), not before
        self.log_label = ttk.Label(outer, text=t('log'))
        self.logframe = logframe = ttk.Frame(outer)
        self.log = tk.Text(logframe, height=12, wrap='word', state='disabled', font=('Monospace', int(10 * scale)))
        scroll = ttk.Scrollbar(logframe, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.poll_id = root.after(100, self.poll)
        self.refresh()

    # ---- helpers
    def write(self, text):
        if not self.logframe.winfo_manager():
            self.log_label.pack(anchor='w')
            self.logframe.pack(fill='both', expand=True)
            self.root.minsize(760, 560)
        self.log.configure(state='normal')
        self.log.insert('end', text if text.endswith('\n') else text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def remember(self):
        for key, var in self.vars.items():
            self.config[key] = var.get().strip()
        save_config(self.config)

    def browse(self, key, folder):
        from tkinter import filedialog
        current = self.vars[key].get()
        start = str(Path(current).expanduser().parent) if current else str(Path.home())
        if folder:
            chosen = filedialog.askdirectory(initialdir=current or str(Path.home()), mustexist=False)
        else:
            types = [('prod.keys', '*.keys')] if key == 'keys' else [('Switch', '*.xci *.nsp *.XCI *.NSP')]
            chosen = filedialog.askopenfilename(initialdir=start, filetypes=types + [('*', '*')])
        if chosen:
            self.vars[key].set(chosen)
            self.remember()
            if key == 'install':
                self.refresh()

    def set_busy(self, busy, text=None):
        self.busy = busy
        state = 'disabled' if busy else 'normal'
        self.play_button.configure(state=state)
        self.install_button.configure(state=state)
        self.cancel_button.configure(state='normal' if busy else 'disabled')
        if busy:
            self.progress.start(12)
        else:
            self.progress.stop()
        if text:
            self.status.set(text)

    def refresh(self):
        self.remember()

        def work():
            self.events.put(('status', status(self.config)))
        threading.Thread(target=work, daemon=True).start()

    def show_status(self, state):
        self.state = state
        if not state.get('installed'):
            self.status.set(t('st_none'))
            self.play_button.configure(state='disabled')
        elif state.get('build') != 'current':
            self.status.set(t('st_stale') if state.get('build') == 'stale' else t('st_none'))
            self.play_button.configure(state='normal' if state.get('build') == 'stale' else 'disabled')
        else:
            self.status.set(t('st_ready') if state.get('shaders') == 'compiled' else t('st_shaders'))
            self.play_button.configure(state='normal')

    def stream(self, command, env, then=None):
        """Run a command in the background, lines into the log; then(code) on the window thread at its end."""
        def work():
            try:
                self.process = subprocess.Popen(command, cwd=CODE, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                text=True, errors='replace', bufsize=1, start_new_session=True)
                for line in self.process.stdout:
                    self.events.put(('line', line.rstrip('\n')))
                code = self.process.wait()
            except OSError as error:
                self.events.put(('line', str(error)))
                code = 1
            self.process = None
            self.events.put(('done', (then, code)))
        threading.Thread(target=work, daemon=True).start()

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == 'line':
                    self.write(value)
                elif kind == 'status':
                    if not self.busy:
                        self.show_status(value)
                elif kind == 'done':
                    then, code = value
                    if then:
                        then(code)
        except queue.Empty:
            pass
        self.poll_id = self.root.after(100, self.poll)

    # ---- actions
    def on_install(self):
        self.remember()
        missing = [k for k in ('keys', 'base', 'update') if not Path(self.config.get(k, '')).expanduser().is_file()]
        if missing:
            self.status.set(t('missing'))
            return
        if not glibc_ok():
            self.status.set(t('need_glibc'))
            return
        Path(self.config['install']).expanduser().mkdir(parents=True, exist_ok=True)
        self.set_busy(True, t('st_busy'))
        command = setup_command('--keys', self.config['keys'], '--base', self.config['base'], '--update', self.config['update'])
        self.stream(command, base_env(self.config), self.after_install)

    def after_install(self, code):
        self.set_busy(False, t('st_installed') if code == 0 else t('st_failed'))
        self.refresh()

    def on_play(self):
        self.remember()
        state = getattr(self, 'state', {})
        self.set_busy(True, t('st_busy'))
        if state.get('build') != 'current' or state.get('shaders') != 'compiled':
            # a newer launcher (rebuild once) or shaders not prepared yet: the setup does what is missing, then play
            self.stream(setup_command('--rebuild-if-stale'), base_env(self.config), self.start_game)
        else:
            self.start_game(0)

    def start_game(self, code):
        if code:
            self.set_busy(False, t('st_failed'))
            return
        self.status.set(t('st_playing'))
        self.root.iconify() if not DECK else None

        def work():
            code = run_game(self.config, log=lambda line: self.events.put(('line', line)))
            self.events.put(('done', (self.after_game, code)))
        threading.Thread(target=work, daemon=True).start()

    def after_game(self, code):
        self.root.deiconify()
        folder = Path(self.config['install']).expanduser()/'build'/'game'/'round-00'
        if code == 0:
            self.status.set(t('st_closed'))
            # shaders the game met in play are prepared now, so the next start has them
            self.stream(setup_command('--rebuild-if-stale'), base_env(self.config),
                        lambda c: (self.set_busy(False, t('st_closed')), self.refresh()))
        else:
            self.set_busy(False, t('exit_code', code=code, folder=folder))
            for name in ('stderr.log', 'trace.log'):
                path = folder/name
                if path.is_file():
                    tail = path.read_text(encoding='utf-8', errors='replace').splitlines()[-12:]
                    self.write(f'--- {name}')
                    for line in tail:
                        self.write(line)
            self.refresh()

    def on_cancel(self):
        process = self.process
        if process:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except OSError:
                pass

    def on_open(self):
        folder = Path(self.config['install']).expanduser()
        folder.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(['xdg-open', str(folder)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def on_steam(self):
        from tkinter import messagebox
        if steam_running():
            messagebox.showwarning(t('title'), t('steam_running'))
            return
        try:
            count = add_to_steam()
        except (OSError, ValueError) as error:
            messagebox.showerror(t('title'), str(error))
            return
        messagebox.showinfo(t('title'), t('steam_done') if count else t('steam_none'))

    def on_settings(self):
        tk, ttk = self.tk, self.ttk
        dialog = tk.Toplevel(self.root)
        dialog.title(t('settings'))
        dialog.transient(self.root)
        frame = ttk.Frame(dialog, padding=14)
        frame.pack(fill='both', expand=True)
        values = {}
        row = 0
        group = None
        for key, grp, labels, kind, _, _, _ in OPTIONS:
            if grp != group:
                group = grp
                ttk.Label(frame, text=t(grp), font=('Sans', 12, 'bold')).grid(row=row, column=0, columnspan=2, sticky='w', pady=(10, 2))
                row += 1
            label = labels[1 if RU else 0]
            var = tk.StringVar(value=self.config['settings'].get(key, ''))
            values[key] = var
            if kind == 'check':
                ttk.Checkbutton(frame, text=label, variable=var, onvalue='1', offvalue='0').grid(row=row, column=0, columnspan=2, sticky='w')
            elif isinstance(kind[0], tuple):  # (value, shown name) pairs: the box shows names, the setting keeps values
                names = {value: t('auto') if name == 'auto' else name for value, name in kind}
                shown = tk.StringVar(value=names.get(var.get(), var.get()))
                ttk.Label(frame, text=label).grid(row=row, column=0, sticky='w')
                box = ttk.Combobox(frame, textvariable=shown, values=list(names.values()), state='readonly', width=16)
                box.grid(row=row, column=1, sticky='w', padx=8)
                box.bind('<<ComboboxSelected>>', lambda _e, var=var, shown=shown, names=names:
                         var.set(next(v for v, n in names.items() if n == shown.get())))
            else:
                ttk.Label(frame, text=label).grid(row=row, column=0, sticky='w')
                ttk.Combobox(frame, textvariable=var, values=list(kind), state='readonly', width=12).grid(row=row, column=1, sticky='w', padx=8)
            row += 1

        def save():
            ui_before = self.config['settings'].get('ui_language')
            for key, var in values.items():
                self.config['settings'][key] = var.get()
            save_config(self.config)
            dialog.destroy()
            if self.config['settings'].get('ui_language') != ui_before:
                if self.busy:
                    self.write(t('ui_later'))
                else:
                    self.rebuild()
            else:
                self.refresh()  # (the game language changes what the build is)
        bar = ttk.Frame(frame)
        bar.grid(row=row, column=0, columnspan=2, sticky='e', pady=(14, 0))
        ttk.Button(bar, text=t('save'), command=save).pack(side='right')
        ttk.Button(bar, text=t('close'), command=dialog.destroy).pack(side='right', padx=8)

    def on_close(self):
        self.on_cancel()
        self.root.destroy()

    def rebuild(self):
        """The window again in the launcher's newly chosen language."""
        self.root.after_cancel(self.poll_id)
        set_ui_language(self.config)
        for child in self.root.winfo_children():
            child.destroy()
        App(self.root, self.config)


def selftest():
    """No game files needed: the window builds, the setup answers, the toolchain is complete."""
    xft = use_xft_tk()
    import tkinter as tk
    import tkinter.font
    config = load_config()
    set_ui_language(config)
    root = tk.Tk()
    app = App(root, config)
    root.update()
    app.on_settings()
    root.update()
    problems = []
    for path in (CODE/'tools'/'zig'/'zig', CODE/'tools'/'glslang'/'bin'/'glslang', CODE/'tools'/'nstool'/'nstool',
                 CODE/'tools'/'romfs-build', CODE/'tools'/'linux-sdk'/'lib'/'libSDL3.so.0',
                 CODE/'tools'/'linux-sdk'/'lib'/'libvulkan.so.1', CODE/'tools'/'linux-sdk'/'include'/'SDL3'/'SDL.h'):
        if not path.exists():
            problems.append(f'missing {path}')
    state = status(config)
    font = tkinter.font.Font(family='Sans', size=11).actual('family')
    if xft and font == 'fixed':
        problems.append('Xft Tk loaded but fonts are not found')
    root.destroy()
    print(json.dumps({'status': state, 'deck': DECK, 'xft': xft, 'font': font, 'problems': problems}))
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--play', action='store_true')
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--base')
    parser.add_argument('--update')
    parser.add_argument('--keys')
    parser.add_argument('--dest')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    config = load_config()
    set_ui_language(config)
    if args.dest:
        config['install'] = str(Path(args.dest).expanduser().resolve())
        save_config(config)
    if args.selftest:
        sys.exit(selftest())
    if args.install:
        for key in ('base', 'update', 'keys'):
            if getattr(args, key):
                config[key] = str(Path(getattr(args, key)).expanduser().resolve())
        save_config(config)
        sys.exit(subprocess.run(setup_command('--keys', config['keys'], '--base', config['base'], '--update', config['update']),
                                cwd=CODE, env=base_env(config)).returncode)
    if args.play:
        state = status(config)
        if state.get('installed') and state.get('build') == 'current':
            code = run_game(config)
            subprocess.run(setup_command('--rebuild-if-stale'), cwd=CODE, env=base_env(config),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            sys.exit(code)
    use_xft_tk()
    try:
        import tkinter as tk
    except ImportError:
        sys.exit('tkinter is missing: use the AppImage, or install python3-tk.')
    root = tk.Tk()
    App(root, config)
    root.mainloop()


if __name__ == '__main__':
    main()
