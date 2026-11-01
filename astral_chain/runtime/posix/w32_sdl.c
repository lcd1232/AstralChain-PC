/* Linux host: the Win32 window, input, sound and gamepad subset on SDL3.
 *
 * Windows: an HWND is an SDL window with the class's window procedure. PeekMessageW turns SDL events into the window
 * messages the runtime handles (keys as virtual-key codes, mouse in client coordinates, raw relative mouse motion as
 * WM_INPUT, focus, size, close); DispatchMessageW calls the procedure. Only the thread that created the windows pumps
 * them (app_window.h already keeps to that).
 * Fullscreen: the runtime's borderless-popup switch (WS_POPUP style) becomes SDL desktop fullscreen.
 * Mouse capture (ClipCursor + hidden cursor) becomes SDL relative mouse mode.
 * winmm.dll: waveOut on an SDL audio stream; a buffer is done once the stream has played its last byte.
 * xinput1_4.dll: XInputGetState on SDL gamepads (positions, as XInput: south = A).
 * gdi32.dll: the software frame blit is a no-op (the Vulkan renderer presents); text rendering is unavailable, so
 * keyboard button prompts keep the controller glyphs. */
#include "w32_internal.h"
#include <mmsystem.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>

/* ------------------------------------------------------------------------------------------------ SDL start */
static pthread_mutex_t sdl_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_InitFlags sdl_flags;

int w32_sdl_ensure(unsigned flags) {
 pthread_mutex_lock(&sdl_lock);
 int ok = 1;
 if ((sdl_flags & flags) != flags) {
  if (!sdl_flags) {
   SDL_SetHint(SDL_HINT_APP_NAME, "Astral Chain");
   SDL_SetHint(SDL_HINT_APP_ID, "astral-chain");
   SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
   SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
   SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Astral Chain");
  }
  ok = SDL_InitSubSystem(flags & ~sdl_flags);
  if (ok) sdl_flags |= flags;
  else fprintf(stderr, "[host] SDL_InitSubSystem(0x%x) failed: %s\n", flags, SDL_GetError());
 }
 pthread_mutex_unlock(&sdl_lock);
 return ok;
}

/* ------------------------------------------------------------------------------------------------ windows */
#define WINDOW_MAGIC 0x57494E44u
typedef struct {
 uint32_t magic;
 SDL_Window *sdl;
 WNDPROC proc;
 LONG_PTR style, ex_style, user_data;
 int fullscreen, popup, visible;
} Window;
static struct { wchar_t name[64]; WNDPROC proc; } classes[16];
static Window *windows[16];
static Window *capture_window;
static int raw_mouse_on, cursor_hidden, cursor_clipped, relative_on;
static uint8_t key_down[256];

static Window *window_of(HWND handle) {
 Window *w = handle;
 return w && w->magic == WINDOW_MAGIC ? w : NULL;
}
static Window *window_from_id(SDL_WindowID id) {
 for (int i = 0; i < 16; i++) if (windows[i] && SDL_GetWindowID(windows[i]->sdl) == id) return windows[i];
 return NULL;
}
void *win32_posix_sdl_window(HWND window) { Window *w = window_of(window); return w ? w->sdl : NULL; }

static int class_index(LPCWSTR name) {
 for (int i = 0; i < 16; i++) if (classes[i].proc && !wcscmp(classes[i].name, name)) return i;
 return -1;
}
ATOM RegisterClassW(const WNDCLASSW *window_class) {
 int i = class_index(window_class->lpszClassName);
 if (i >= 0) { SetLastError(1410); return 0; }   /* ERROR_CLASS_ALREADY_EXISTS */
 for (i = 0; i < 16 && classes[i].proc; i++) {}
 if (i == 16 || wcslen(window_class->lpszClassName) >= 64) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
 wcscpy(classes[i].name, window_class->lpszClassName);
 classes[i].proc = window_class->lpfnWndProc;
 return (ATOM)(i + 1);
}

HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y, int width, int height,
                     HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
 (void)parent; (void)menu; (void)instance; (void)parameter;
 int c = class_index(class_name);
 if (c < 0) { SetLastError(1407); return NULL; }   /* ERROR_CANNOT_FIND_WND_CLASS */
 if (!w32_sdl_ensure(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
 int slot = 0;
 while (slot < 16 && windows[slot]) slot++;
 if (slot == 16) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
 char *name = w32_utf8_from_wide(title, -1);
 int popup = (style & WS_POPUP) != 0;
 SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | (popup ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_RESIZABLE);
 SDL_Window *sdl = SDL_CreateWindow(name ? name : "", width > 0 ? width : 1280, height > 0 ? height : 720, flags);
 free(name);
 if (!sdl) {
  fprintf(stderr, "[host] SDL_CreateWindow failed: %s\n", SDL_GetError());
  SetLastError(ERROR_NOT_SUPPORTED);
  return NULL;
 }
 if (x != CW_USEDEFAULT && y != CW_USEDEFAULT && !popup) SDL_SetWindowPosition(sdl, x, y);
 Window *w = calloc(1, sizeof(Window));
 w->magic = WINDOW_MAGIC;
 w->sdl = sdl;
 w->proc = classes[c].proc;
 w->style = style;
 w->ex_style = ex_style;
 w->popup = popup;
 windows[slot] = w;
 if (style & WS_VISIBLE) { SDL_ShowWindow(sdl); w->visible = 1; }
 return w;
}

BOOL DestroyWindow(HWND window) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 for (int i = 0; i < 16; i++) if (windows[i] == w) windows[i] = NULL;
 SDL_DestroyWindow(w->sdl);
 w->magic = 0;
 free(w);
 return TRUE;
}

BOOL ShowWindow(HWND window, int command) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 int was = w->visible;
 switch (command) {
 case SW_HIDE: SDL_HideWindow(w->sdl); w->visible = 0; break;
 case SW_MINIMIZE: case SW_SHOWMINIMIZED: case SW_SHOWMINNOACTIVE: SDL_MinimizeWindow(w->sdl); break;
 case SW_MAXIMIZE: SDL_ShowWindow(w->sdl); SDL_MaximizeWindow(w->sdl); w->visible = 1; break;
 case SW_RESTORE: SDL_RestoreWindow(w->sdl); SDL_ShowWindow(w->sdl); w->visible = 1; break;
 default: SDL_ShowWindow(w->sdl); w->visible = 1; break;
 }
 return was;
}
BOOL UpdateWindow(HWND window) { return window_of(window) != NULL; }
BOOL SetWindowTextW(HWND window, LPCWSTR text) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 char *t = w32_utf8_from_wide(text, -1);
 SDL_SetWindowTitle(w->sdl, t ? t : "");
 free(t);
 return TRUE;
}

BOOL GetClientRect(HWND window, LPRECT rect) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 int width = 0, height = 0;
 SDL_GetWindowSize(w->sdl, &width, &height);
 rect->left = rect->top = 0;
 rect->right = width;
 rect->bottom = height;
 return TRUE;
}
BOOL GetWindowRect(HWND window, LPRECT rect) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 int x = 0, y = 0, width = 0, height = 0;
 SDL_GetWindowPosition(w->sdl, &x, &y);
 SDL_GetWindowSize(w->sdl, &width, &height);
 rect->left = x; rect->top = y; rect->right = x + width; rect->bottom = y + height;
 return TRUE;
}
BOOL AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu) { (void)rect; (void)style; (void)menu; return TRUE; }
BOOL IsIconic(HWND window) {
 Window *w = window_of(window);
 return w && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED);
}
BOOL IsZoomed(HWND window) {
 Window *w = window_of(window);
 return w && (SDL_GetWindowFlags(w->sdl) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN));
}
BOOL IsWindowVisible(HWND window) { Window *w = window_of(window); return w && w->visible; }

static void set_fullscreen(Window *w, int on) {
 if (w->fullscreen == on) return;
 SDL_SetWindowFullscreenMode(w->sdl, NULL);   /* borderless desktop fullscreen: no mode change */
 SDL_SetWindowFullscreen(w->sdl, on);
 w->fullscreen = on;
}

LONG_PTR GetWindowLongPtrW(HWND window, int index) {
 Window *w = window_of(window);
 if (!w) return 0;
 return index == GWL_STYLE ? w->style : index == GWL_EXSTYLE ? w->ex_style : index == GWLP_USERDATA ? w->user_data : 0;
}
LONG_PTR SetWindowLongPtrW(HWND window, int index, LONG_PTR value) {
 Window *w = window_of(window);
 if (!w) return 0;
 LONG_PTR old = 0;
 if (index == GWL_STYLE) {
  old = w->style;
  w->style = value;
  if (!w->popup) {
   if ((value & WS_POPUP) && !(old & WS_POPUP)) set_fullscreen(w, 1);
   else if (!(value & WS_POPUP) && (old & WS_POPUP)) set_fullscreen(w, 0);
  }
 } else if (index == GWL_EXSTYLE) { old = w->ex_style; w->ex_style = value; }
 else if (index == GWLP_USERDATA) { old = w->user_data; w->user_data = value; }
 return old;
}
BOOL SetWindowPos(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags) {
 (void)after;
 Window *w = window_of(window);
 if (!w) return FALSE;
 if (w->fullscreen) return TRUE;
 if (!(flags & SWP_NOSIZE) && cx > 0 && cy > 0) SDL_SetWindowSize(w->sdl, cx, cy);
 if (!(flags & SWP_NOMOVE)) SDL_SetWindowPosition(w->sdl, x, y);
 if (flags & SWP_SHOWWINDOW) { SDL_ShowWindow(w->sdl); w->visible = 1; }
 return TRUE;
}
BOOL GetWindowPlacement(HWND window, WINDOWPLACEMENT *placement) {
 RECT r;
 if (!GetWindowRect(window, &r)) return FALSE;
 placement->rcNormalPosition = r;
 placement->showCmd = SW_SHOWNORMAL;
 return TRUE;
}
BOOL SetWindowPlacement(HWND window, const WINDOWPLACEMENT *placement) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 if (w->fullscreen) set_fullscreen(w, 0);
 const RECT *r = &placement->rcNormalPosition;
 if (r->right > r->left && r->bottom > r->top) {
  SDL_SetWindowSize(w->sdl, r->right - r->left, r->bottom - r->top);
  SDL_SetWindowPosition(w->sdl, r->left, r->top);
 }
 return TRUE;
}
HMONITOR MonitorFromWindow(HWND window, DWORD flags) {
 (void)flags;
 Window *w = window_of(window);
 SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w->sdl) : SDL_GetPrimaryDisplay();
 return (HMONITOR)(uintptr_t)(d ? d : 1);
}
BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
 SDL_Rect bounds, usable;
 SDL_DisplayID d = (SDL_DisplayID)(uintptr_t)monitor;
 if (!SDL_GetDisplayBounds(d, &bounds)) return FALSE;
 if (!SDL_GetDisplayUsableBounds(d, &usable)) usable = bounds;
 info->rcMonitor = (RECT){bounds.x, bounds.y, bounds.x + bounds.w, bounds.y + bounds.h};
 info->rcWork = (RECT){usable.x, usable.y, usable.x + usable.w, usable.y + usable.h};
 info->dwFlags = 1;
 return TRUE;
}

/* ---- messages */
static MSG queue[512];
static unsigned queue_head, queue_count;
static RAWINPUT raw_ring[64];
static unsigned raw_next;
static float raw_rest_x, raw_rest_y;

static void post(Window *w, UINT message, WPARAM wparam, LPARAM lparam) {
 if (!w || queue_count == 512) return;
 MSG *m = &queue[(queue_head + queue_count++) % 512];
 m->hwnd = w;
 m->message = message;
 m->wParam = wparam;
 m->lParam = lparam;
 m->time = GetTickCount();
 m->pt.x = m->pt.y = 0;
}
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
 post(window_of(window), message, wparam, lparam);
 return TRUE;
}

static UINT vk_of_scancode(SDL_Scancode s) {
 if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
 if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
 if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
 if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24) return VK_F13 + (s - SDL_SCANCODE_F13);
 if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
 switch (s) {
 case SDL_SCANCODE_0: return '0';
 case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
 case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
 case SDL_SCANCODE_BACKSPACE: return VK_BACK;
 case SDL_SCANCODE_TAB: return VK_TAB;
 case SDL_SCANCODE_SPACE: return VK_SPACE;
 case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
 case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
 case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
 case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
 case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
 case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
 case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
 case SDL_SCANCODE_GRAVE: return VK_OEM_3;
 case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
 case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
 case SDL_SCANCODE_SLASH: return VK_OEM_2;
 case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;
 case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
 case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
 case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;
 case SDL_SCANCODE_PAUSE: return VK_PAUSE;
 case SDL_SCANCODE_INSERT: return VK_INSERT;
 case SDL_SCANCODE_HOME: return VK_HOME;
 case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
 case SDL_SCANCODE_DELETE: return VK_DELETE;
 case SDL_SCANCODE_END: return VK_END;
 case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
 case SDL_SCANCODE_RIGHT: return VK_RIGHT;
 case SDL_SCANCODE_LEFT: return VK_LEFT;
 case SDL_SCANCODE_DOWN: return VK_DOWN;
 case SDL_SCANCODE_UP: return VK_UP;
 case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
 case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
 case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
 case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
 case SDL_SCANCODE_KP_PLUS: return VK_ADD;
 case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
 case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
 case SDL_SCANCODE_APPLICATION: return VK_APPS;
 case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
 case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
 case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
 case SDL_SCANCODE_LGUI: return VK_LWIN;
 case SDL_SCANCODE_RGUI: return VK_RWIN;
 default: return 0;
 }
}

/* PC/AT set 1 scan code and extended flag (lparam bits 16-24), for the keys that tell left from right */
static unsigned scan_bits(SDL_Scancode s) {
 switch (s) {
 case SDL_SCANCODE_LSHIFT: return 0x2A;
 case SDL_SCANCODE_RSHIFT: return 0x36;
 case SDL_SCANCODE_LCTRL: return 0x1D;
 case SDL_SCANCODE_RCTRL: return 0x1D | 0x100;
 case SDL_SCANCODE_LALT: return 0x38;
 case SDL_SCANCODE_RALT: return 0x38 | 0x100;
 case SDL_SCANCODE_KP_ENTER: return 0x1C | 0x100;
 case SDL_SCANCODE_RETURN: return 0x1C;
 case SDL_SCANCODE_INSERT: case SDL_SCANCODE_DELETE: case SDL_SCANCODE_HOME: case SDL_SCANCODE_END:
 case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
 case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_DIVIDE: return 0x100;
 default: return 0;
 }
}

static WPARAM mouse_keys(void) {
 SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
 WPARAM w = 0;
 if (b & SDL_BUTTON_LMASK) w |= MK_LBUTTON;
 if (b & SDL_BUTTON_RMASK) w |= MK_RBUTTON;
 if (b & SDL_BUTTON_MMASK) w |= MK_MBUTTON;
 if (b & SDL_BUTTON_X1MASK) w |= MK_XBUTTON1;
 if (b & SDL_BUTTON_X2MASK) w |= MK_XBUTTON2;
 SDL_Keymod mod = SDL_GetModState();
 if (mod & SDL_KMOD_SHIFT) w |= MK_SHIFT;
 if (mod & SDL_KMOD_CTRL) w |= MK_CONTROL;
 return w;
}

static void gamepad_event(const SDL_Event *e);

static void translate(const SDL_Event *e) {
 switch (e->type) {
 case SDL_EVENT_QUIT:
  for (int i = 0; i < 16; i++) if (windows[i] && !windows[i]->popup) { post(windows[i], WM_CLOSE, 0, 0); break; }
  break;
 case SDL_EVENT_WINDOW_CLOSE_REQUESTED: post(window_from_id(e->window.windowID), WM_CLOSE, 0, 0); break;
 case SDL_EVENT_WINDOW_FOCUS_GAINED: post(window_from_id(e->window.windowID), WM_ACTIVATEAPP, 1, 0); break;
 case SDL_EVENT_WINDOW_FOCUS_LOST: {
  Window *w = window_from_id(e->window.windowID);
  memset(key_down, 0, sizeof key_down);
  post(w, WM_ACTIVATEAPP, 0, 0);
  break;
 }
 case SDL_EVENT_WINDOW_MINIMIZED: post(window_from_id(e->window.windowID), WM_SIZE, SIZE_MINIMIZED, 0); break;
 case SDL_EVENT_WINDOW_RESTORED: case SDL_EVENT_WINDOW_MAXIMIZED: case SDL_EVENT_WINDOW_RESIZED: {
  Window *w = window_from_id(e->window.windowID);
  if (!w) break;
  int width = 0, height = 0;
  SDL_GetWindowSize(w->sdl, &width, &height);
  post(w, WM_SIZE, e->type == SDL_EVENT_WINDOW_MAXIMIZED ? SIZE_MAXIMIZED : SIZE_RESTORED, MAKELONG(width, height));
  break;
 }
 case SDL_EVENT_WINDOW_MOVED: post(window_from_id(e->window.windowID), WM_MOVE, 0, MAKELONG(e->window.data1, e->window.data2)); break;
 case SDL_EVENT_WINDOW_EXPOSED: post(window_from_id(e->window.windowID), WM_PAINT, 0, 0); break;
 case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
  Window *w = window_from_id(e->key.windowID);
  UINT vk = vk_of_scancode(e->key.scancode);
  if (!w || !vk) break;
  int down = e->type == SDL_EVENT_KEY_DOWN;
  key_down[vk] = (uint8_t)down;
  if (vk == VK_SHIFT) key_down[e->key.scancode == SDL_SCANCODE_LSHIFT ? VK_LSHIFT : VK_RSHIFT] = (uint8_t)down;
  if (vk == VK_CONTROL) key_down[e->key.scancode == SDL_SCANCODE_LCTRL ? VK_LCONTROL : VK_RCONTROL] = (uint8_t)down;
  if (vk == VK_MENU) key_down[e->key.scancode == SDL_SCANCODE_LALT ? VK_LMENU : VK_RMENU] = (uint8_t)down;
  int alt = (e->key.mod & SDL_KMOD_ALT) != 0;
  int system = alt || vk == VK_F10 || vk == VK_MENU;
  UINT message = down ? (system ? WM_SYSKEYDOWN : WM_KEYDOWN) : (system ? WM_SYSKEYUP : WM_KEYUP);
  LPARAM lparam = 1 | ((LPARAM)scan_bits(e->key.scancode) << 16) | ((LPARAM)alt << 29) |
                  ((LPARAM)(e->key.repeat || !down) << 30) | ((LPARAM)!down << 31);
  post(w, message, vk, lparam);
  break;
 }
 case SDL_EVENT_MOUSE_MOTION: {
  Window *w = window_from_id(e->motion.windowID);
  if (!w) break;
  if (relative_on && raw_mouse_on) {
   RAWINPUT *raw = &raw_ring[raw_next++ % 64];
   memset(raw, 0, sizeof *raw);
   raw->header.dwType = RIM_TYPEMOUSE;
   raw->header.dwSize = sizeof *raw;
   raw_rest_x += e->motion.xrel;
   raw_rest_y += e->motion.yrel;
   raw->data.mouse.lLastX = (LONG)raw_rest_x;
   raw->data.mouse.lLastY = (LONG)raw_rest_y;
   raw_rest_x -= (float)raw->data.mouse.lLastX;
   raw_rest_y -= (float)raw->data.mouse.lLastY;
   raw->data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
   if (raw->data.mouse.lLastX || raw->data.mouse.lLastY) post(w, WM_INPUT, 0, (LPARAM)(uintptr_t)raw);
  }
  post(w, WM_MOUSEMOVE, mouse_keys(), MAKELONG((int)e->motion.x, (int)e->motion.y));
  break;
 }
 case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
  Window *w = window_from_id(e->button.windowID);
  if (!w) break;
  int down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
  LPARAM at = MAKELONG((int)e->button.x, (int)e->button.y);
  WPARAM keys = mouse_keys();
  switch (e->button.button) {
  case SDL_BUTTON_LEFT: post(w, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, keys, at); key_down[VK_LBUTTON] = (uint8_t)down; break;
  case SDL_BUTTON_RIGHT: post(w, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, keys, at); key_down[VK_RBUTTON] = (uint8_t)down; break;
  case SDL_BUTTON_MIDDLE: post(w, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, keys, at); key_down[VK_MBUTTON] = (uint8_t)down; break;
  case SDL_BUTTON_X1: post(w, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, keys | ((WPARAM)XBUTTON1 << 16), at); break;
  case SDL_BUTTON_X2: post(w, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, keys | ((WPARAM)XBUTTON2 << 16), at); break;
  }
  break;
 }
 case SDL_EVENT_MOUSE_WHEEL: {
  Window *w = window_from_id(e->wheel.windowID);
  if (!w) break;
  float sign = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
  int x = 0, y = 0;
  SDL_GetWindowPosition(w->sdl, &x, &y);
  LPARAM at = MAKELONG(x + (int)e->wheel.mouse_x, y + (int)e->wheel.mouse_y);
  if (e->wheel.y != 0) post(w, WM_MOUSEWHEEL, mouse_keys() | ((WPARAM)(WORD)(short)(e->wheel.y * sign * WHEEL_DELTA) << 16), at);
  if (e->wheel.x != 0) post(w, WM_MOUSEHWHEEL, mouse_keys() | ((WPARAM)(WORD)(short)(e->wheel.x * sign * WHEEL_DELTA) << 16), at);
  break;
 }
 case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: gamepad_event(e); break;
 default: break;
 }
}

static void pump(void) {
 if (!(sdl_flags & SDL_INIT_VIDEO)) return;
 SDL_Event e;
 int budget = 1024;
 while (queue_count < 480 && budget-- > 0 && SDL_PollEvent(&e)) translate(&e);
}

BOOL PeekMessageW(LPMSG message, HWND window, UINT first, UINT last, UINT remove) {
 (void)window; (void)first; (void)last;
 if (!queue_count) pump();
 if (!queue_count) return FALSE;
 *message = queue[queue_head];
 if (remove & PM_REMOVE) { queue_head = (queue_head + 1) % 512; queue_count--; }
 return TRUE;
}
BOOL TranslateMessage(const MSG *message) { (void)message; return FALSE; }
LRESULT DispatchMessageW(const MSG *message) {
 Window *w = window_of(message->hwnd);
 if (!w || !w->proc) return 0;
 return w->proc(message->hwnd, message->message, message->wParam, message->lParam);
}
LRESULT DefWindowProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
 (void)window; (void)wparam; (void)lparam;
 if (message == WM_CLOSE) DestroyWindow(window);
 return 0;
}

int w32_sdl_message_wait(DWORD milliseconds) {
 if (queue_count) return 1;
 if (!(sdl_flags & SDL_INIT_VIDEO)) { Sleep(milliseconds); return 0; }
 return SDL_WaitEventTimeout(NULL, (Sint32)(milliseconds > 0x7fffffff ? 0x7fffffff : milliseconds)) ? 1 : 0;
}

HDC BeginPaint(HWND window, PAINTSTRUCT *paint) { memset(paint, 0, sizeof(*paint)); return (HDC)window; }
BOOL EndPaint(HWND window, const PAINTSTRUCT *paint) { (void)window; (void)paint; return TRUE; }
BOOL InvalidateRect(HWND window, const RECT *rect, BOOL erase) { (void)rect; (void)erase; post(window_of(window), WM_PAINT, 0, 0); return TRUE; }

/* ---- cursor, capture */
static void update_relative(void) {
 int want = cursor_clipped && cursor_hidden;
 if (want == relative_on) return;
 for (int i = 0; i < 16; i++) if (windows[i] && !windows[i]->popup) SDL_SetWindowRelativeMouseMode(windows[i]->sdl, want);
 relative_on = want;
 raw_rest_x = raw_rest_y = 0;
}
HCURSOR LoadCursorW(HINSTANCE instance, LPCWSTR name) { (void)instance; (void)name; return (HCURSOR)(uintptr_t)1; }
HCURSOR SetCursor(HCURSOR cursor) {
 HCURSOR old = cursor_hidden ? NULL : (HCURSOR)(uintptr_t)1;
 cursor_hidden = cursor == NULL;
 if (cursor_hidden) SDL_HideCursor(); else SDL_ShowCursor();
 update_relative();
 return old;
}
BOOL GetCursorPos(LPPOINT point) {
 float x = 0, y = 0;
 SDL_GetGlobalMouseState(&x, &y);
 point->x = (LONG)x;
 point->y = (LONG)y;
 return TRUE;
}
BOOL SetCursorPos(int x, int y) {
 if (!cursor_clipped) { cursor_hidden = 0; SDL_ShowCursor(); update_relative(); }
 if (!relative_on) SDL_WarpMouseGlobal((float)x, (float)y);
 return TRUE;
}
BOOL ClipCursor(const RECT *rect) {
 cursor_clipped = rect != NULL;
 for (int i = 0; i < 16; i++) if (windows[i] && !windows[i]->popup) SDL_SetWindowMouseGrab(windows[i]->sdl, cursor_clipped);
 update_relative();
 return TRUE;
}
HWND SetCapture(HWND window) {
 Window *old = capture_window;
 capture_window = window_of(window);
 SDL_CaptureMouse(capture_window != NULL);
 return old;
}
BOOL ReleaseCapture(void) { capture_window = NULL; SDL_CaptureMouse(false); return TRUE; }
HWND GetCapture(void) { return capture_window; }
HWND GetForegroundWindow(void) {
 for (int i = 0; i < 16; i++) if (windows[i] && (SDL_GetWindowFlags(windows[i]->sdl) & SDL_WINDOW_INPUT_FOCUS)) return windows[i];
 return NULL;
}
int MapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count) {
 int dx = 0, dy = 0, x, y;
 Window *a = window_of(from), *b = window_of(to);
 if (a) { SDL_GetWindowPosition(a->sdl, &x, &y); dx += x; dy += y; }
 if (b) { SDL_GetWindowPosition(b->sdl, &x, &y); dx -= x; dy -= y; }
 for (UINT i = 0; i < count; i++) { points[i].x += dx; points[i].y += dy; }
 return MAKELONG(dx, dy);
}
BOOL ScreenToClient(HWND window, LPPOINT point) {
 Window *w = window_of(window);
 if (!w) return FALSE;
 int x, y;
 SDL_GetWindowPosition(w->sdl, &x, &y);
 point->x -= x;
 point->y -= y;
 return TRUE;
}
SHORT GetKeyState(int key) {
 if (key < 0 || key > 255) return 0;
 SHORT state = key_down[key] ? (SHORT)0x8000 : 0;
 SDL_Keymod mod = (sdl_flags & SDL_INIT_VIDEO) ? SDL_GetModState() : 0;
 if (key == VK_CAPITAL && (mod & SDL_KMOD_CAPS)) state |= 1;
 if (key == VK_NUMLOCK && (mod & SDL_KMOD_NUM)) state |= 1;
 if (key == VK_SCROLL && (mod & SDL_KMOD_SCROLL)) state |= 1;
 return state;
}
SHORT GetAsyncKeyState(int key) { return GetKeyState(key) & (SHORT)0x8000; }
UINT MapVirtualKeyW(UINT code, UINT type) {
 if (type != MAPVK_VK_TO_CHAR) return 0;
 if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return code;
 for (int s = 0; s < SDL_SCANCODE_COUNT; s++) if (vk_of_scancode((SDL_Scancode)s) == code) {
  SDL_Keycode k = (sdl_flags & SDL_INIT_VIDEO) ? SDL_GetKeyFromScancode((SDL_Scancode)s, SDL_KMOD_NONE, false) : 0;
  return k > 0 && k < 0x10000 ? (UINT)(k >= 'a' && k <= 'z' ? k - 32 : k) : 0;
 }
 return 0;
}
BOOL RegisterRawInputDevices(const RAWINPUTDEVICE *devices, UINT count, UINT size) {
 (void)size;
 for (UINT i = 0; i < count; i++) if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2) raw_mouse_on = !(devices[i].dwFlags & RIDEV_REMOVE);
 return TRUE;
}
UINT GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, UINT *size, UINT header_size) {
 (void)header_size;
 if (command != RID_INPUT || !input) return (UINT)-1;
 if (!data) { *size = sizeof(RAWINPUT); return 0; }
 if (*size < sizeof(RAWINPUT)) return (UINT)-1;
 memcpy(data, input, sizeof(RAWINPUT));
 return sizeof(RAWINPUT);
}

int MessageBoxW(HWND window, LPCWSTR text, LPCWSTR caption, UINT type) {
 char *t = w32_utf8_from_wide(text, -1), *c = w32_utf8_from_wide(caption, -1);
 int r = MessageBoxA(window, t ? t : "", c ? c : "", type);
 free(t); free(c);
 return r;
}
int MessageBoxA(HWND window, LPCSTR text, LPCSTR caption, UINT type) {
 Window *w = window_of(window);
 fprintf(stderr, "%s: %s\n", caption ? caption : "", text ? text : "");
 if (w32_sdl_ensure(SDL_INIT_VIDEO))
  SDL_ShowSimpleMessageBox((type & 0xF0) == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR :
                           (type & 0xF0) == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION,
                           caption ? caption : "", text ? text : "", w ? w->sdl : NULL);
 return 1;   /* IDOK */
}

/* ------------------------------------------------------------------------------------------------ Vulkan */
const char *const *win32_posix_vulkan_instance_extensions(uint32_t *count) {
 if (!w32_sdl_ensure(SDL_INIT_VIDEO)) { *count = 0; return NULL; }
 Uint32 n = 0;
 const char *const *names = SDL_Vulkan_GetInstanceExtensions(&n);
 *count = n;
 return names;
}
int win32_posix_vulkan_surface(HWND window, struct VkInstance_T *instance, uint64_t *surface) {
 Window *w = window_of(window);
 if (!w) return 0;
 VkSurfaceKHR s = 0;
 if (!SDL_Vulkan_CreateSurface(w->sdl, (VkInstance)instance, NULL, &s)) {
  fprintf(stderr, "[host] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
  return 0;
 }
 *surface = (uint64_t)s;
 return 1;
}

/* ------------------------------------------------------------------------------------------------ gamepads */
static SDL_Gamepad *pads[4];
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;

static void pads_refresh_locked(void) {
 for (int i = 0; i < 4; i++) if (pads[i] && !SDL_GamepadConnected(pads[i])) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
 int n = 0;
 SDL_JoystickID *ids = SDL_GetGamepads(&n);
 for (int k = 0; ids && k < n; k++) {
  int known = 0;
  for (int i = 0; i < 4; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == ids[k]) known = 1;
  if (known) continue;
  for (int i = 0; i < 4; i++) if (!pads[i]) {
   pads[i] = SDL_OpenGamepad(ids[k]);
   if (pads[i]) fprintf(stderr, "[host] gamepad %d: %s\n", i, SDL_GetGamepadName(pads[i]));
   break;
  }
 }
 SDL_free(ids);
}
static void gamepad_event(const SDL_Event *e) {
 (void)e;
 pthread_mutex_lock(&pad_lock);
 pads_refresh_locked();
 pthread_mutex_unlock(&pad_lock);
}

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVibration;

static SHORT flip(Sint16 v) { return v == -32768 ? 32767 : (SHORT)-v; }

static DWORD WINAPI XInputGetState(DWORD index, XState *state) {
 if (index >= 4 || !state) return ERROR_INVALID_PARAMETER;
 if (!w32_sdl_ensure(SDL_INIT_GAMEPAD)) return 1167;   /* ERROR_DEVICE_NOT_CONNECTED */
 static uint64_t last_refresh;
 pthread_mutex_lock(&pad_lock);
 if (!(sdl_flags & SDL_INIT_VIDEO)) SDL_UpdateGamepads();   /* no window pumping events yet */
 uint64_t now = GetTickCount64();
 if (now - last_refresh > 1000) { last_refresh = now; pads_refresh_locked(); }
 SDL_Gamepad *g = pads[index];
 if (!g) { pthread_mutex_unlock(&pad_lock); return 1167; }
 static const struct { SDL_GamepadButton b; WORD bit; } map[] = {
  {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},
  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008}, {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
  {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
  {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
  {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000}, {SDL_GAMEPAD_BUTTON_WEST, 0x4000},
  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
 XPad p = {0};
 for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GetGamepadButton(g, map[i].b)) p.wButtons |= map[i].bit;
 p.bLeftTrigger = (BYTE)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
 p.bRightTrigger = (BYTE)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
 p.sThumbLX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
 p.sThumbLY = flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
 p.sThumbRX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
 p.sThumbRY = flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
 pthread_mutex_unlock(&pad_lock);
 static DWORD packets[4];
 static XPad previous[4];
 if (memcmp(&previous[index], &p, sizeof p)) { packets[index]++; previous[index] = p; }
 state->dwPacketNumber = packets[index];
 state->Gamepad = p;
 return ERROR_SUCCESS;
}
static DWORD WINAPI XInputSetState(DWORD index, XVibration *vibration) {
 if (index >= 4 || !vibration) return ERROR_INVALID_PARAMETER;
 pthread_mutex_lock(&pad_lock);
 SDL_Gamepad *g = pads[index];
 if (g) SDL_RumbleGamepad(g, vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed, 1000);
 pthread_mutex_unlock(&pad_lock);
 return g ? ERROR_SUCCESS : 1167;
}

/* ------------------------------------------------------------------------------------------------ waveOut */
#define WAVE_MAGIC 0x57415645u
typedef struct {
 uint32_t magic;
 SDL_AudioStream *stream;
 WAVEFORMATEX format;
 DWORD_PTR callback, instance;
 DWORD flags;
 pthread_mutex_t lock;
 struct { LPWAVEHDR header; uint64_t end; } queue[256];
 unsigned head, count;
 uint64_t put, position_base;
 /* Played bytes: the device takes whole buffers from the stream ahead of playing them, so what left the stream is
  * not what was heard. At each take (the stream's get callback) the bytes taken so far and the time are recorded;
  * the played position is that count minus one device buffer, advanced by the clock since, never past what was
  * taken and never backwards. waveOut's position and its buffer completions follow it. */
 uint64_t taken, played, lag;
 double taken_at, paused_at;
 int paused;
 pthread_t poller;
 volatile int stop;
} WaveDevice;

static double wave_now(void) { return (double)SDL_GetTicksNS() / 1e9; }

static void wave_notify(WaveDevice *d, UINT message, LPWAVEHDR header) {
 DWORD type = d->flags & 0x70000u;
 if (type == CALLBACK_FUNCTION && d->callback)
  ((void (*)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))d->callback)((HWAVEOUT)d, message, d->instance, (DWORD_PTR)header, 0);
 else if (type == CALLBACK_EVENT && d->callback) SetEvent((HANDLE)d->callback);
}

static uint64_t wave_taken_locked(WaveDevice *d) {
 int queued = SDL_GetAudioStreamQueued(d->stream);
 return queued >= 0 && (uint64_t)queued <= d->put ? d->put - (uint64_t)queued : d->put;
}

static uint64_t wave_consumed_locked(WaveDevice *d) {
 uint64_t taken = wave_taken_locked(d);
 if (taken > d->taken) { d->taken = taken; d->taken_at = wave_now(); }   /* (a take the callback did not see) */
 double now = d->paused ? d->paused_at : wave_now();
 double since = now > d->taken_at ? now - d->taken_at : 0;
 uint64_t estimate = d->taken > d->lag ? d->taken - d->lag : 0;
 estimate += (uint64_t)(since * d->format.nAvgBytesPerSec) / d->format.nBlockAlign * d->format.nBlockAlign;
 if (estimate > d->taken) estimate = d->taken;
 if (estimate > d->played) d->played = estimate;
 return d->played;
}

static void SDLCALL wave_take(void *user, SDL_AudioStream *stream, int additional, int total) {
 (void)stream; (void)total;
 WaveDevice *d = user;
 pthread_mutex_lock(&d->lock);
 uint64_t before = wave_taken_locked(d);
 uint64_t after = before + (uint64_t)additional;
 if (after > d->put) after = d->put;
 wave_consumed_locked(d);   /* (the played position up to now, before the new take moves the anchor) */
 d->taken = after;
 d->taken_at = wave_now();
 pthread_mutex_unlock(&d->lock);
}

/* Done buffers, oldest first; called with the lock held, notifies after releasing it. */
static void wave_complete(WaveDevice *d, int all) {
 LPWAVEHDR done[256];
 unsigned n = 0;
 pthread_mutex_lock(&d->lock);
 uint64_t consumed = all ? UINT64_MAX : wave_consumed_locked(d);
 while (d->count && d->queue[d->head].end <= consumed) {
  LPWAVEHDR h = d->queue[d->head].header;
  h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
  done[n++] = h;
  d->head = (d->head + 1) % 256;
  d->count--;
 }
 pthread_mutex_unlock(&d->lock);
 for (unsigned i = 0; i < n; i++) wave_notify(d, WOM_DONE, done[i]);
}

static void *wave_poll(void *argument) {
 WaveDevice *d = argument;
 while (!d->stop) {
  struct timespec t = {0, 2000000};
  nanosleep(&t, NULL);
  wave_complete(d, 0);
 }
 return NULL;
}

static WaveDevice *wave_of(HWAVEOUT out) {
 WaveDevice *d = out;
 return d && d->magic == WAVE_MAGIC ? d : NULL;
}

MMRESULT waveOutOpen(LPHWAVEOUT out, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
 (void)device;
 if (!format || format->wFormatTag != WAVE_FORMAT_PCM || format->wBitsPerSample != 16) return WAVERR_BADFORMAT;
 if (!w32_sdl_ensure(SDL_INIT_AUDIO)) return MMSYSERR_NODRIVER;
 SDL_AudioSpec spec = {SDL_AUDIO_S16LE, format->nChannels, (int)format->nSamplesPerSec};
 SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
 if (!stream) { fprintf(stderr, "[host] audio device: %s\n", SDL_GetError()); return MMSYSERR_NODRIVER; }
 WaveDevice *d = calloc(1, sizeof(WaveDevice));
 d->magic = WAVE_MAGIC;
 d->stream = stream;
 d->format = *format;
 d->callback = callback;
 d->instance = instance;
 d->flags = flags;
 pthread_mutex_init(&d->lock, NULL);
 /* one device buffer, in this stream's bytes */
 SDL_AudioSpec device_spec;
 int frames = 0;
 if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device_spec, &frames) || frames <= 0) frames = 1024;
 double scale = device_spec.freq > 0 ? (double)format->nSamplesPerSec / device_spec.freq : 1.0;
 d->lag = (uint64_t)(frames * scale) * format->nBlockAlign;
 d->taken_at = wave_now();
 SDL_SetAudioStreamGetCallback(stream, wave_take, d);
 pthread_create(&d->poller, NULL, wave_poll, d);
 SDL_ResumeAudioStreamDevice(stream);   /* an opened waveOut device plays (until paused) */
 *out = (HWAVEOUT)d;
 wave_notify(d, WOM_OPEN, NULL);
 return MMSYSERR_NOERROR;
}

MMRESULT waveOutClose(HWAVEOUT out) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 pthread_mutex_lock(&d->lock);
 unsigned pending = d->count;
 pthread_mutex_unlock(&d->lock);
 if (pending) return WAVERR_STILLPLAYING;
 d->stop = 1;
 pthread_join(d->poller, NULL);
 SDL_DestroyAudioStream(d->stream);
 wave_notify(d, WOM_CLOSE, NULL);
 d->magic = 0;
 pthread_mutex_destroy(&d->lock);
 free(d);
 return MMSYSERR_NOERROR;
}

MMRESULT waveOutPrepareHeader(HWAVEOUT out, LPWAVEHDR header, UINT size) {
 if (!wave_of(out)) return MMSYSERR_INVALHANDLE;
 if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
 header->dwFlags |= WHDR_PREPARED;
 return MMSYSERR_NOERROR;
}
MMRESULT waveOutUnprepareHeader(HWAVEOUT out, LPWAVEHDR header, UINT size) {
 if (!wave_of(out)) return MMSYSERR_INVALHANDLE;
 if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
 if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
 header->dwFlags &= ~WHDR_PREPARED;
 return MMSYSERR_NOERROR;
}
MMRESULT waveOutWrite(HWAVEOUT out, LPWAVEHDR header, UINT size) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
 if (!(header->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
 pthread_mutex_lock(&d->lock);
 if (d->count == 256) { pthread_mutex_unlock(&d->lock); return MMSYSERR_NOMEM; }
 header->dwFlags = (header->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
 if (!SDL_PutAudioStreamData(d->stream, header->lpData, (int)header->dwBufferLength)) {
  header->dwFlags &= ~WHDR_INQUEUE;
  pthread_mutex_unlock(&d->lock);
  return MMSYSERR_ERROR;
 }
 d->put += header->dwBufferLength;
 d->queue[(d->head + d->count) % 256].header = header;
 d->queue[(d->head + d->count) % 256].end = d->put;
 d->count++;
 pthread_mutex_unlock(&d->lock);
 return MMSYSERR_NOERROR;
}
MMRESULT waveOutPause(HWAVEOUT out) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 pthread_mutex_lock(&d->lock);
 if (!d->paused) { wave_consumed_locked(d); d->paused = 1; d->paused_at = wave_now(); }
 pthread_mutex_unlock(&d->lock);
 return SDL_PauseAudioStreamDevice(d->stream) ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
}
MMRESULT waveOutRestart(HWAVEOUT out) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 pthread_mutex_lock(&d->lock);
 if (d->paused) { d->taken_at += wave_now() - d->paused_at; d->paused = 0; }
 pthread_mutex_unlock(&d->lock);
 return SDL_ResumeAudioStreamDevice(d->stream) ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
}
MMRESULT waveOutReset(HWAVEOUT out) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 pthread_mutex_lock(&d->lock);
 SDL_ClearAudioStream(d->stream);
 d->taken = d->played = d->position_base = d->put;
 d->taken_at = wave_now();
 pthread_mutex_unlock(&d->lock);
 wave_complete(d, 1);
 return MMSYSERR_NOERROR;
}
MMRESULT waveOutGetPosition(HWAVEOUT out, LPMMTIME time, UINT size) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 if (!time || size < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
 pthread_mutex_lock(&d->lock);
 uint64_t consumed = wave_consumed_locked(d);
 uint64_t bytes = consumed > d->position_base ? consumed - d->position_base : 0;
 pthread_mutex_unlock(&d->lock);
 if (time->wType == TIME_SAMPLES) time->u.sample = (DWORD)(bytes / d->format.nBlockAlign);
 else if (time->wType == TIME_MS) time->u.ms = (DWORD)(bytes * 1000 / d->format.nAvgBytesPerSec);
 else { time->wType = TIME_BYTES; time->u.cb = (DWORD)bytes; }
 return MMSYSERR_NOERROR;
}
MMRESULT waveOutSetVolume(HWAVEOUT out, DWORD volume) {
 WaveDevice *d = wave_of(out);
 if (!d) return MMSYSERR_INVALHANDLE;
 SDL_SetAudioStreamGain(d->stream, (float)(volume & 0xFFFF) / 65535.0f);
 return MMSYSERR_NOERROR;
}
MMRESULT timeBeginPeriod(UINT period) { (void)period; return TIMERR_NOERROR; }
MMRESULT timeEndPeriod(UINT period) { (void)period; return TIMERR_NOERROR; }

/* ------------------------------------------------------------------------------------------------ GDI */
HDC GetDC(HWND window) { return (HDC)window; }
int ReleaseDC(HWND window, HDC dc) { (void)window; (void)dc; return 1; }
HDC CreateCompatibleDC(HDC dc) { (void)dc; return NULL; }
BOOL DeleteDC(HDC dc) { (void)dc; return TRUE; }
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset) {
 (void)dc; (void)info; (void)usage; (void)section; (void)offset;
 if (bits) *bits = NULL;
 return NULL;
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) { (void)dc; (void)object; return NULL; }
BOOL DeleteObject(HGDIOBJ object) { (void)object; return TRUE; }
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality, DWORD pitch,
                  LPCWSTR face) {
 (void)height; (void)width; (void)escapement; (void)orientation; (void)weight; (void)italic; (void)underline;
 (void)strike; (void)charset; (void)out_precision; (void)clip_precision; (void)quality; (void)pitch; (void)face;
 return NULL;
}
COLORREF SetTextColor(HDC dc, COLORREF color) { (void)dc; (void)color; return 0; }
int SetBkMode(HDC dc, int mode) { (void)dc; (void)mode; return 0; }
BOOL TextOutW(HDC dc, int x, int y, LPCWSTR text, int count) { (void)dc; (void)x; (void)y; (void)text; (void)count; return FALSE; }
BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size) {
 (void)dc; (void)text; (void)count;
 size->cx = size->cy = 0;
 return FALSE;
}
BOOL GdiFlush(void) { return TRUE; }
static int WINAPI StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits,
                                const BITMAPINFO *info, UINT usage, DWORD rop) {
 (void)dc; (void)x; (void)y; (void)w; (void)sx; (void)sy; (void)sw; (void)sh; (void)bits; (void)info; (void)usage; (void)rop;
 return h;
}

/* ------------------------------------------------------------------------------------------------ built-in modules */
int w32_builtin_module(const char *name) {
 return !strcmp(name, "winmm.dll") || !strcmp(name, "xinput1_4.dll") || !strcmp(name, "xinput1_3.dll") ||
        !strcmp(name, "xinput9_1_0.dll") || !strcmp(name, "gdi32.dll") || !strcmp(name, "user32.dll");
}

FARPROC w32_builtin_proc(const char *module, const char *name) {
#define P(fn) if (!strcmp(name, #fn)) return (FARPROC)(void *)fn
 if (!strcmp(module, "winmm.dll")) {
  P(waveOutOpen); P(waveOutClose); P(waveOutPrepareHeader); P(waveOutUnprepareHeader); P(waveOutWrite);
  P(waveOutPause); P(waveOutRestart); P(waveOutReset); P(waveOutGetPosition); P(waveOutSetVolume);
  P(timeBeginPeriod); P(timeEndPeriod);
 } else if (!strncmp(module, "xinput", 6)) {
  P(XInputGetState); P(XInputSetState);
 } else if (!strcmp(module, "gdi32.dll")) {
  /* only the blit: without the text calls, button_prompts.c keeps the controller glyphs */
  P(StretchDIBits);
 }
#undef P
 return NULL;
}
