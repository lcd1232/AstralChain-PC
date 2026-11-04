/* Keyboard and mouse, or Xbox / PlayStation controller, button prompts. The game draws every button prompt (in text, on the HUD, in menus) from one
 * 256x512 RGBA8 atlas in ui_core (core.wtp, texture 0): white shapes on transparency with the letter cut out, tinted by
 * the game. When that texture is uploaded, each Switch button icon is replaced by the key or mouse button it is bound
 * to (las/input_bindings.h, the launcher's Controls), drawn in the same style inside the same footprint.
 *
 * Hooks of textures.c (texpatch.h): texpatch_identify finds the atlas by its contents, texpatch_apply repaints its
 * level 0 before upload. AC_PROMPTS=switch keeps the original icons; AC_PROMPTS=xbox or ps redraws only the face
 * buttons, shoulders, triggers and +/- with that controller's labels, by position (as hid_service.h maps XInput: the
 * Switch's right button A is Xbox B / PS circle, its bottom B is Xbox A / PS cross, and so on), and keeps the stick,
 * stick press, D-pad and face button diagram icons, which are the same on every controller. AC_PROMPTS_DUMP=<dir>
 * writes the atlas before and after (atlas-before.rgba, atlas-after.rgba: 256x512 RGBA8) for checking the drawing. */
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn/nvn.h"
#include "nvn/tegra.h"
#include "nvn/vulkan/bc7_cache.h"
#include "input_bindings.h"
#include "texpatch.h"
#include "ps5_glyphs.h"

#define ATLAS_HASH 0x4afe0dd389f44dc9ull
#define ATLAS_W 256
#define ATLAS_H 512
#define SS 4  /* supersampling */

enum Kind { K_KEY, K_LSTICK, K_RSTICK, K_DPAD };
enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8, D_HORZ = 12, D_VERT = 3, D_ALL = 15, D_ROTATE = 16 };
typedef struct { int x0, y0, x1, y1; int kind, arg; } Slot;  /* bounding box of the original icon (inclusive) */
static const Slot slots[] = {
    {9, 8, 48, 32, K_RSTICK, D_HORZ},     {69, 7, 108, 32, K_LSTICK, D_HORZ},   {129, 7, 163, 32, K_LSTICK, D_UP},
    {185, 7, 217, 32, K_LSTICK, D_LEFT},  {4, 52, 52, 76, K_RSTICK, D_ROTATE},  {69, 51, 103, 76, K_RSTICK, D_VERT},
    {125, 51, 159, 76, K_LSTICK, D_DOWN}, {182, 51, 214, 76, K_LSTICK, D_RIGHT}, {9, 95, 43, 120, K_LSTICK, D_VERT},
    {69, 97, 90, 118, K_KEY, IB_MINUS},   {125, 97, 146, 118, K_KEY, IB_B},     {181, 97, 202, 118, K_KEY, IB_A},
    {224, 93, 243, 120, K_KEY, IB_LSTICK}, {224, 137, 243, 164, K_KEY, IB_RSTICK},
    {9, 141, 30, 162, K_KEY, IB_Y},       {69, 141, 90, 162, K_KEY, IB_X},      {124, 142, 147, 161, K_KEY, IB_R},
    {180, 140, 203, 163, K_KEY, IB_ZL},   {8, 184, 31, 207, K_KEY, IB_ZR},      {69, 185, 90, 206, K_KEY, IB_PLUS},
    {124, 184, 147, 208, K_LSTICK, D_ALL}, {8, 228, 31, 252, K_RSTICK, D_ALL},  {68, 230, 91, 249, K_KEY, IB_L},
    {125, 229, 146, 250, K_DPAD, D_ALL},  {181, 229, 202, 250, K_DPAD, D_UP},
    {9, 273, 30, 294, K_DPAD, D_DOWN},    {69, 273, 90, 294, K_DPAD, D_LEFT},   {125, 273, 146, 294, K_DPAD, D_RIGHT},
    {181, 273, 202, 294, K_DPAD, D_VERT}, {9, 317, 30, 338, K_DPAD, D_HORZ},    {69, 317, 90, 338, K_DPAD, D_UP},
    {125, 317, 146, 338, K_DPAD, D_DOWN}, {181, 317, 202, 338, K_DPAD, D_LEFT}, {9, 361, 30, 382, K_DPAD, D_RIGHT},
    /* the four-circle face button diagrams: top X, bottom B, left Y, right A */
    {63, 354, 96, 388, K_KEY, IB_X},      {119, 355, 152, 389, K_KEY, IB_B},    {174, 355, 208, 388, K_KEY, IB_Y},
    {3, 399, 37, 432, K_KEY, IB_A},
};

static uint8_t map[2][IB_ACTIONS];
static int mouse_look = 1;
enum { STYLE_KEYBOARD, STYLE_XBOX, STYLE_PS };
static int style = STYLE_KEYBOARD;

/* AC_PROMPTS and AC_MOUSE_LOOK, read again when the in-game menu changes a setting (game_settings.h in the host runtime);
 * a change of look bumps the generation, and textures.c uploads the atlas again. */
int setting_get(const char *name, char *buf, unsigned size);
int setting_int(const char *name, int otherwise);
unsigned settings_version(void);
static int on = 1;
static uint32_t generation = 1;

static int enabled(void) {
    static unsigned version = ~0u;
    static int loaded;
    if (!loaded) { loaded = 1; ib_load(map); }
    if (version == settings_version()) return on;
    version = settings_version();
    char e[32];
    if (!setting_get("AC_PROMPTS", e, sizeof e)) e[0] = 0;
    int was_on = on, was_style = style, was_look = mouse_look;
    on = strcmp(e, "switch") && strcmp(e, "0");
    style = !strcmp(e, "xbox") ? STYLE_XBOX : !strcmp(e, "ps") || !strcmp(e, "playstation") ? STYLE_PS : STYLE_KEYBOARD;
    mouse_look = setting_int("AC_MOUSE_LOOK", 1) != 0;
    if (on != was_on || style != was_style || mouse_look != was_look) generation++;
    return on;
}

/* ---------------------------------------------------------------------------------------------- coverage canvas */
/* A slot is drawn at SS times its size into a coverage canvas (0 empty, 255 the white shape), then box-filtered into
 * the atlas' alpha. Labels are cut out of the shape, as in the original icons. */
typedef struct { int w, h; uint8_t *c; } Canvas;

static void fill_rrect(Canvas *cv, float x0, float y0, float x1, float y1, float r, uint8_t v) {
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++) {
        if (y < 0 || y >= cv->h) continue;
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            if (x < 0 || x >= cv->w) continue;
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0;
            float dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
            if (dx * dx + dy * dy <= r * r) cv->c[y * cv->w + x] = v;
        }
    }
}

static void fill_ellipse(Canvas *cv, float cx, float cy, float rx, float ry, uint8_t v) {
    for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
        for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1) cv->c[y * cv->w + x] = v;
        }
}

/* Triangle pointing in direction d (D_UP...), centred at (cx, cy), half size s. */
static void fill_arrow(Canvas *cv, float cx, float cy, float s, int d, uint8_t v) {
    for (int y = (int)(cy - s) - 1; y <= (int)(cy + s) + 1; y++)
        for (int x = (int)(cx - s) - 1; x <= (int)(cx + s) + 1; x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float u = (x + 0.5f - cx) / s, w = (y + 0.5f - cy) / s;  /* (rotate so the arrow points up) */
            float a, b;
            if (d == D_UP) { a = u; b = w; } else if (d == D_DOWN) { a = u; b = -w; }
            else if (d == D_LEFT) { a = w; b = u; } else { a = w; b = -u; }
            if (b >= -0.8f && b <= 0.6f && fabsf(a) <= (b + 0.8f) * 0.75f) cv->c[y * cv->w + x] = v;
        }
}

/* A thick line from (ax, ay) to (bx, by) with round ends. */
static void fill_line(Canvas *cv, float ax, float ay, float bx, float by, float t, uint8_t v) {
    float r = t / 2, dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int y = (int)(fminf(ay, by) - r) - 1; y <= (int)(fmaxf(ay, by) + r) + 1; y++)
        for (int x = (int)(fminf(ax, bx) - r) - 1; x <= (int)(fmaxf(ax, bx) + r) + 1; x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float k = len2 > 0 ? fminf(fmaxf((px * dx + py * dy) / len2, 0), 1) : 0;
            float ex = px - k * dx, ey = py - k * dy;
            if (ex * ex + ey * ey <= r * r) cv->c[y * cv->w + x] = v;
        }
}

#ifndef __linux__
/* ------------------------------------------------------------------------------------------------- text (GDI) */
/* gdi32 is loaded at run time (the game's link line has no gdi32; app_window.h does the same for StretchDIBits). */
static struct {
    HDC (WINAPI *CreateCompatibleDC)(HDC);
    HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
    HFONT (WINAPI *CreateFontW)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);
    COLORREF (WINAPI *SetTextColor)(HDC, COLORREF);
    int (WINAPI *SetBkMode)(HDC, int);
    BOOL (WINAPI *GetTextExtentPoint32W)(HDC, LPCWSTR, int, LPSIZE);
    BOOL (WINAPI *TextOutW)(HDC, int, int, LPCWSTR, int);
    BOOL (WINAPI *DeleteObject)(HGDIOBJ);
    BOOL (WINAPI *DeleteDC)(HDC);
    BOOL (WINAPI *GdiFlush)(void);
} gdi;

static bool gdi_load(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    HMODULE m = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
#define GDI_FN(name) if (!(*(FARPROC *)&gdi.name = GetProcAddress(m, #name))) return false
    GDI_FN(CreateCompatibleDC); GDI_FN(CreateDIBSection); GDI_FN(SelectObject); GDI_FN(CreateFontW); GDI_FN(SetTextColor);
    GDI_FN(SetBkMode);
    GDI_FN(GetTextExtentPoint32W); GDI_FN(TextOutW); GDI_FN(DeleteObject); GDI_FN(DeleteDC); GDI_FN(GdiFlush);
#undef GDI_FN
    state = 1;
    return true;
}

/* Cuts text out of the shape (coverage 0 where the glyphs are), centred in the box, as large as fits. */
static void cut_text(Canvas *cv, const wchar_t *text, float bx0, float by0, float bx1, float by1) {
    if (!gdi_load()) return;
    int n = (int)wcslen(text), bw = (int)(bx1 - bx0), bh = (int)(by1 - by0);
    if (n == 0 || bw < 4 || bh < 4) return;
    HDC dc = gdi.CreateCompatibleDC(NULL);
    if (!dc) return;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cv->w; bi.bmiHeader.biHeight = -cv->h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t *px = NULL;
    HBITMAP bmp = gdi.CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp || !px) { gdi.DeleteDC(dc); return; }
    memset(px, 0, (size_t)cv->w * cv->h * 4);
    HGDIOBJ old_bmp = gdi.SelectObject(dc, bmp);
    /* largest font height whose text fits the box */
    int height = bh, fitted = 0;
    SIZE size = {0};
    HFONT font = NULL;
    for (; height >= 6; height = height * 9 / 10) {
        font = gdi.CreateFontW(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) break;
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.GetTextExtentPoint32W(dc, text, n, &size);
        gdi.SelectObject(dc, old);
        if (size.cx <= bw && size.cy <= bh * 1.45f) { fitted = 1; break; }
        gdi.DeleteObject(font);
        font = NULL;
    }
    if (fitted) {
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.SetBkMode(dc, TRANSPARENT);
        gdi.SetTextColor(dc, RGB(255, 255, 255));
        /* (Segoe UI's cell has room above capitals: centre on the cell, nudged up a little) */
        int tx = (int)(bx0 + (bw - size.cx) / 2.0f), ty = (int)(by0 + (bh - size.cy) / 2.0f + size.cy * 0.02f);
        gdi.TextOutW(dc, tx, ty, text, n);
        gdi.GdiFlush();
        gdi.SelectObject(dc, old);
        for (int i = 0; i < cv->w * cv->h; i++) if (px[i] & 0xFFFFFF) cv->c[i] = 0;
    }
    if (font) gdi.DeleteObject(font);
    gdi.SelectObject(dc, old_bmp);
    gdi.DeleteObject(bmp);
    gdi.DeleteDC(dc);
}
#else
/* --------------------------------------------------------------------------------------------- text (FreeType) */
/* Linux: the system's libfreetype, loaded at run time (nothing to link; without it the keys stay blank), with a bold
 * TrueType face: AC_PROMPT_FONT (the launcher passes fontconfig's), else the usual paths. The same fitting and
 * placement as the GDI version, unantialiased like it. */
#include <dlfcn.h>
#include <wchar.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static struct {
    FT_Error (*Init_FreeType)(FT_Library *);
    FT_Error (*New_Face)(FT_Library, const char *, FT_Long, FT_Face *);
    FT_Error (*Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
    FT_Error (*Load_Char)(FT_Face, FT_ULong, FT_Int32);
    FT_Face face;
} ft;

static bool ft_load(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    void *m = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!m) { printf("[prompts] libfreetype.so.6 not found: key labels left blank\n"); return false; }
    *(void **)&ft.Init_FreeType = dlsym(m, "FT_Init_FreeType");
    *(void **)&ft.New_Face = dlsym(m, "FT_New_Face");
    *(void **)&ft.Set_Pixel_Sizes = dlsym(m, "FT_Set_Pixel_Sizes");
    *(void **)&ft.Load_Char = dlsym(m, "FT_Load_Char");
    FT_Library library;
    if (!ft.Init_FreeType || !ft.New_Face || !ft.Set_Pixel_Sizes || !ft.Load_Char || ft.Init_FreeType(&library)) return false;
    const char *paths[] = {getenv("AC_PROMPT_FONT"),
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        getenv("AC_FONT")};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        if (paths[i] && paths[i][0] && !ft.New_Face(library, paths[i], 0, &ft.face)) {
            printf("[prompts] key labels in %s\n", paths[i]);
            state = 1;
            return true;
        }
    }
    printf("[prompts] no font for the key labels: left blank\n");
    return false;
}

/* Cuts text out of the shape (coverage 0 where the glyphs are), centred in the box, as large as fits. */
static void cut_text(Canvas *cv, const wchar_t *text, float bx0, float by0, float bx1, float by1) {
    if (!ft_load()) return;
    int n = (int)wcslen(text), bw = (int)(bx1 - bx0), bh = (int)(by1 - by0);
    if (n == 0 || bw < 4 || bh < 4) return;
    /* largest font height (em, as GDI's negative height) whose text fits the box */
    int height = bh, fitted = 0, width = 0, ascent = 0, cell = 0;
    for (; height >= 6; height = height * 9 / 10) {
        if (ft.Set_Pixel_Sizes(ft.face, 0, (FT_UInt)height)) return;
        ascent = (int)(ft.face->size->metrics.ascender >> 6);
        cell = ascent + (int)(-ft.face->size->metrics.descender >> 6);
        width = 0;
        for (int i = 0; i < n; i++)
            if (!ft.Load_Char(ft.face, (FT_ULong)text[i], FT_LOAD_DEFAULT)) width += (int)(ft.face->glyph->advance.x >> 6);
        if (width <= bw && cell <= bh * 1.45f) { fitted = 1; break; }
    }
    if (!fitted) return;
    /* (centred on the cell, nudged up a little, as the GDI version) */
    int pen = (int)(bx0 + (bw - width) / 2.0f), baseline = (int)(by0 + (bh - cell) / 2.0f + cell * 0.02f) + ascent;
    for (int i = 0; i < n; i++) {
        if (ft.Load_Char(ft.face, (FT_ULong)text[i], FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)) continue;
        FT_GlyphSlot g = ft.face->glyph;
        const FT_Bitmap *b = &g->bitmap;
        for (unsigned row = 0; row < b->rows; row++) {
            const unsigned char *line = b->buffer + (ptrdiff_t)row * b->pitch;
            for (unsigned col = 0; col < b->width; col++) {
                int on = b->pixel_mode == FT_PIXEL_MODE_MONO ? (line[col >> 3] >> (7 - (col & 7))) & 1 : line[col] >= 128;
                int x = pen + g->bitmap_left + (int)col, y = baseline - g->bitmap_top + (int)row;
                if (on && x >= 0 && y >= 0 && x < cv->w && y < cv->h) cv->c[(size_t)y * cv->w + x] = 0;
            }
        }
        pen += (int)(g->advance.x >> 6);
    }
}
#endif

/* ------------------------------------------------------------------------------------------------------- glyphs */
static const wchar_t *key_label(unsigned vk, wchar_t buf[8]) {
    switch (vk) {
    case 0x08: return L"Bksp"; case 0x09: return L"Tab"; case 0x0D: return L"Enter"; case 0x10: return L"Shift";
    case 0x11: return L"Ctrl"; case 0x12: return L"Alt"; case 0x13: return L"Pause"; case 0x14: return L"Caps";
    case 0x1B: return L"Esc"; case 0x20: return L"Space"; case 0x21: return L"PgUp"; case 0x22: return L"PgDn";
    case 0x23: return L"End"; case 0x24: return L"Home"; case 0x2D: return L"Ins"; case 0x2E: return L"Del";
    case 0xA0: case 0xA1: return L"Shift"; case 0xA2: case 0xA3: return L"Ctrl"; case 0xA4: case 0xA5: return L"Alt";
    case 0x6A: return L"*"; case 0x6B: return L"+"; case 0x6D: return L"-"; case 0x6E: return L"."; case 0x6F: return L"/";
    }
    if (vk >= 0x70 && vk <= 0x87) { swprintf(buf, 8, L"F%u", vk - 0x6F); return buf; }
    if (vk >= 0x60 && vk <= 0x69) { swprintf(buf, 8, L"N%u", vk - 0x60); return buf; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { buf[0] = (wchar_t)vk; buf[1] = 0; return buf; }
    UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;  /* (punctuation, on this keyboard layout) */
    if (ch >= 0x21) { buf[0] = (wchar_t)ch; buf[1] = 0; return buf; }
    swprintf(buf, 8, L"%u", vk);
    return buf;
}

/* A mouse: rounded body, buttons split at the top; `which` is filled solid, the rest of the body outlined. dirs adds
 * movement arrows beside it (camera prompts): left and right on either side, up and down stacked on its right, all four
 * as a small cross on its right. */
static void draw_mouse(Canvas *cv, float x0, float y0, float x1, float y1, unsigned which, int dirs) {
    float w = x1 - x0, h = y1 - y0;
    /* width of the mouse and its arrows, in mouse heights */
    float aw = dirs == D_HORZ ? 0.66f + 2 * 0.42f : dirs == D_VERT ? 0.66f + 0.5f : dirs ? 0.66f + 0.95f : 0.66f;
    float mh = fminf(h * 0.96f, w / aw), mw = mh * 0.66f, arrow = mh * 0.17f;
    float left = x0 + (w - mh * aw) / 2 + (dirs == D_HORZ ? mh * 0.42f : 0);  /* the mouse's left edge */
    float cx = left + mw / 2, cy = y0 + h / 2;
    float mx0 = cx - mw / 2, my0 = cy - mh / 2, mx1 = cx + mw / 2, my1 = cy + mh / 2;
    float line = fmaxf(mw * 0.13f, SS * 1.3f), split = my0 + mh * 0.42f;
    fill_rrect(cv, mx0, my0, mx1, my1, mw * 0.48f, 255);
    fill_rrect(cv, mx0 + line, my0 + line, mx1 - line, my1 - line, mw * 0.48f - line, 0);  /* (outline) */
    fill_rrect(cv, mx0, split - line / 2, mx1, split + line / 2, 0, 255);                    /* button line */
    fill_rrect(cv, cx - line / 2, my0, cx + line / 2, split, 0, 255);                        /* between buttons */
    if (which == IB_VK_LBUTTON || which == IB_VK_RBUTTON) {
        float hx0 = which == IB_VK_LBUTTON ? mx0 : cx, hx1 = which == IB_VK_LBUTTON ? cx : mx1;
        for (int y = (int)my0; y < (int)split; y++)
            for (int x = (int)hx0; x < (int)hx1; x++) {
                if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
                float dx = (x + 0.5f - cx) / (mw / 2), dy = (y + 0.5f - (my0 + mw * 0.48f)) / (mw * 0.48f);
                if (y + 0.5f > my0 + mw * 0.48f || dx * dx + dy * dy <= 1.0f) cv->c[y * cv->w + x] = 255;
            }
    } else if (which == IB_VK_MBUTTON || which == IB_VK_WHEEL_UP || which == IB_VK_WHEEL_DOWN) {
        fill_rrect(cv, cx - mw * 0.16f, my0 + mh * 0.10f, cx + mw * 0.16f, split - line, mw * 0.16f, 255);  /* wheel */
        if (which != IB_VK_MBUTTON) fill_arrow(cv, cx, which == IB_VK_WHEEL_UP ? my0 + mh * 0.66f : my0 + mh * 0.72f,
                                               mw * 0.24f, which == IB_VK_WHEEL_UP ? D_UP : D_DOWN, 255);
    } else if (which == IB_VK_XBUTTON1 || which == IB_VK_XBUTTON2) {  /* side buttons: a tab on the left, 4 / 5 */
        float ty = which == IB_VK_XBUTTON1 ? my0 + mh * 0.64f : my0 + mh * 0.47f;
        fill_rrect(cv, mx0 - line * 1.2f, ty, mx0 + line * 2.6f, ty + mh * 0.15f, line * 0.6f, 255);
    }
    if (dirs == D_HORZ) {
        fill_arrow(cv, mx0 - mh * 0.22f, cy, arrow, D_LEFT, 255);
        fill_arrow(cv, mx1 + mh * 0.22f, cy, arrow, D_RIGHT, 255);
    } else if (dirs == D_VERT) {
        fill_arrow(cv, mx1 + mh * 0.27f, cy - mh * 0.2f, arrow, D_UP, 255);
        fill_arrow(cv, mx1 + mh * 0.27f, cy + mh * 0.2f, arrow, D_DOWN, 255);
    } else if (dirs) {
        float ax = mx1 + mh * 0.5f, d = mh * 0.27f, a = arrow * 0.9f;
        fill_arrow(cv, ax, cy - d, a, D_UP, 255);
        fill_arrow(cv, ax, cy + d, a, D_DOWN, 255);
        fill_arrow(cv, ax - d, cy, a, D_LEFT, 255);
        fill_arrow(cv, ax + d, cy, a, D_RIGHT, 255);
    }
}

/* One key cap filling the box: the arrow keys get an arrow, others their name. */
static void draw_keycap(Canvas *cv, float x0, float y0, float x1, float y1, unsigned vk) {
    if (!vk) return;  /* (unbound: left empty) */
    if (ib_is_mouse(vk)) { draw_mouse(cv, x0, y0, x1, y1, vk, 0); return; }
    float w = x1 - x0, h = y1 - y0, r = fminf(w, h) * 0.22f;
    fill_rrect(cv, x0, y0, x1, y1, r, 255);
    int arrow = vk == 0x26 ? D_UP : vk == 0x28 ? D_DOWN : vk == 0x25 ? D_LEFT : vk == 0x27 ? D_RIGHT : 0;
    if (arrow) { fill_arrow(cv, x0 + w / 2, y0 + h / 2, fminf(w, h) * 0.3f, arrow, 0); return; }
    wchar_t buf[8];
    float pad = fminf(w, h) * 0.14f;
    cut_text(cv, key_label(vk, buf), x0 + pad, y0 + pad, x1 - pad, y1 - pad);
}

/* The binding shown for an action: the main one, else the alternative (as the README and the launcher say; a mouse
 * alternative preferred over a keyboard main one made L and R show the mouse side buttons instead of Q and E). */
static unsigned shown(int action) {
    return map[0][action] ? map[0][action] : map[1][action];
}

/* Several key caps side by side (two directions), or a four-key cluster (all directions). */
static void draw_directions(Canvas *cv, float x0, float y0, float x1, float y1, const int act[4] /* up down left right */,
                            int dirs) {
    float w = x1 - x0, h = y1 - y0;
    int single = dirs == D_UP ? 0 : dirs == D_DOWN ? 1 : dirs == D_LEFT ? 2 : dirs == D_RIGHT ? 3 : -1;
    if (single >= 0) {  /* a square cap in the middle */
        float s = fminf(w, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, shown(act[single]));
        return;
    }
    if (dirs == D_HORZ || dirs == D_VERT) {  /* two caps side by side, a small gap */
        int a = dirs == D_HORZ ? 2 : 0;
        float g = SS * 1.5f, s = fminf((w - g) / 2, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - g / 2 - s, cy - s / 2, cx - g / 2, cy + s / 2, shown(act[a]));
        draw_keycap(cv, cx + g / 2, cy - s / 2, cx + g / 2 + s, cy + s / 2, shown(act[a + 1]));
        return;
    }
    /* four caps: up above, left / down / right below (labels only when they are big enough to read) */
    float g = SS * 1.0f, s = fminf((w - 2 * g) / 3, (h - g) / 2), cx = x0 + w / 2, cy = y0 + h / 2;
    float top = cy - g / 2 - s, bottom = cy + g / 2;
    bool labels = s >= SS * 9;
    float xs[3] = {cx - s * 1.5f - g, cx - s / 2, cx + s / 2 + g};
    struct { int a; float x, y; } caps[4] = {{0, xs[1], top}, {2, xs[0], bottom}, {1, xs[1], bottom}, {3, xs[2], bottom}};
    for (int i = 0; i < 4; i++) {
        if (labels) draw_keycap(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, shown(act[caps[i].a]));
        else fill_rrect(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, s * 0.22f, 255);
    }
}

/* ------------------------------------------------------------------------------------------- controller icons */
/* A face button: a disc with the label (Xbox letter, PlayStation symbol) cut out. pos: 0 right, 1 bottom, 2 top,
 * 3 left (the Switch's A, B, X, Y). */
static void draw_face(Canvas *cv, float cx, float cy, float r, int pos) {
    fill_ellipse(cv, cx, cy, r, r, 255);
    if (style == STYLE_XBOX) {
        static const wchar_t *const letters[4] = {L"B", L"A", L"Y", L"X"};
        float k = r * 0.62f;
        cut_text(cv, letters[pos], cx - k, cy - k, cx + k, cy + k);
        return;
    }
    float t = r * 0.2f, k = r * 0.48f;
    switch (pos) {
    case 0:  /* circle */
        fill_ellipse(cv, cx, cy, k + t / 2, k + t / 2, 0);
        fill_ellipse(cv, cx, cy, k - t / 2, k - t / 2, 255);
        break;
    case 1:  /* cross */
        k *= 0.9f;
        fill_line(cv, cx - k, cy - k, cx + k, cy + k, t, 0);
        fill_line(cv, cx - k, cy + k, cx + k, cy - k, t, 0);
        break;
    case 2: {  /* triangle */
        float ax = cx, ay = cy - k * 1.05f, bx = cx - k * 1.05f, by = cy + k * 0.75f, ex = cx + k * 1.05f;
        fill_line(cv, ax, ay, bx, by, t, 0);
        fill_line(cv, bx, by, ex, by, t, 0);
        fill_line(cv, ex, by, ax, ay, t, 0);
        break;
    }
    default:  /* square */
        k *= 0.82f;
        fill_rrect(cv, cx - k - t / 2, cy - k - t / 2, cx + k + t / 2, cy + k + t / 2, 0, 0);
        fill_rrect(cv, cx - k + t / 2, cy - k + t / 2, cx + k - t / 2, cy + k - t / 2, 0, 255);
        break;
    }
}

/* Three bars (menu, options) cut out of a shape, centred at (cx, cy), half width k. */
static void cut_bars(Canvas *cv, float cx, float cy, float k) {
    float t = k * 0.28f;
    for (int i = -1; i <= 1; i++) fill_line(cv, cx - k, cy + i * k * 0.62f, cx + k, cy + i * k * 0.62f, t, 0);
}

/* A PlayStation glyph (ps5_glyphs.h: Zacksly's "PS5 Button Icons and Controls", CC BY 3.0), its visible part fitted
 * into the canvas at its own proportions, bilinear. */
static void blit_glyph(Canvas *cv, int g) {
    const unsigned char *a = ps5_glyphs[g];
    enum { N = PS5_GLYPH_SIZE };
    int bx0 = N, by0 = N, bx1 = -1, by1 = -1;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            if (a[y * N + x] > 8) { if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
    if (bx1 < 0) return;
    float gw = (float)(bx1 - bx0 + 1), gh = (float)(by1 - by0 + 1);
    float k = fminf(cv->w / gw, cv->h / gh), ox = (cv->w - gw * k) / 2, oy = (cv->h - gh * k) / 2;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (x + 0.5f - ox) / k + bx0 - 0.5f, v = (y + 0.5f - oy) / k + by0 - 0.5f;
            if (u < bx0 - 1 || v < by0 - 1 || u > bx1 + 1 || v > by1 + 1) continue;
            int iu = (int)floorf(u), iv = (int)floorf(v);
            float fu = u - iu, fv = v - iv, sum = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int xx = iu + i, yy = iv + j;
                    float w = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
                    if (xx >= 0 && yy >= 0 && xx < N && yy < N) sum += w * a[yy * N + xx];
                }
            cv->c[y * cv->w + x] = (uint8_t)(sum + 0.5f);
        }
}

/* The controller icon of an action, filling the canvas; false keeps the original icon. */
static bool draw_pad(Canvas *cv, const Slot *sl) {
    if (sl->kind != K_KEY || sl >= &slots[sizeof slots / sizeof slots[0] - 4]) return false;  /* (the diagrams) */
    if (style == STYLE_PS) {
        int g = sl->arg == IB_A ? PS5_CIRCLE : sl->arg == IB_B ? PS5_CROSS : sl->arg == IB_X ? PS5_TRIANGLE
              : sl->arg == IB_Y ? PS5_SQUARE : sl->arg == IB_L ? PS5_L1 : sl->arg == IB_R ? PS5_R1
              : sl->arg == IB_ZL ? PS5_L2 : sl->arg == IB_ZR ? PS5_R2 : sl->arg == IB_PLUS ? PS5_OPTIONS
              : sl->arg == IB_MINUS ? PS5_CREATE : -1;
        if (g < 0) return false;
        blit_glyph(cv, g);
        return true;
    }
    float w = (float)cv->w, h = (float)cv->h, s = fminf(w, h), cx = w / 2, cy = h / 2;
    bool xbox = style == STYLE_XBOX;
    switch (sl->arg) {
    case IB_A: case IB_B: case IB_X: case IB_Y:
        draw_face(cv, cx, cy, s * 0.49f, sl->arg == IB_A ? 0 : sl->arg == IB_B ? 1 : sl->arg == IB_X ? 2 : 3);
        return true;
    case IB_L: case IB_R: case IB_ZL: case IB_ZR: {
        static const wchar_t *const names[2][4] = {{L"L1", L"R1", L"L2", L"R2"}, {L"LB", L"RB", L"LT", L"RT"}};
        int i = sl->arg - IB_L;
        bool trigger = i >= 2;
        float bw = fminf(w, s * 1.5f), bh = trigger ? s : s * 0.8f;
        float x0 = cx - bw / 2, y0 = cy - bh / 2, x1 = cx + bw / 2, y1 = cy + bh / 2;
        /* (bumpers a flat pill, triggers a taller cap rounded on top) */
        fill_rrect(cv, x0, y0, x1, y1, trigger ? bh * 0.42f : bh * 0.45f, 255);
        if (trigger) fill_rrect(cv, x0, cy, x1, y1, bh * 0.14f, 255);
        float p = bh * 0.16f;
        cut_text(cv, names[xbox][i], x0 + p, y0 + p, x1 - p, y1 - p);
        return true;
    }
    case IB_PLUS: case IB_MINUS: {
        bool plus = sl->arg == IB_PLUS;
        if (xbox) {  /* Menu (three bars) and View (two overlapping windows) on a disc */
            float r = s * 0.49f, k = r * 0.46f, t = r * 0.16f;
            fill_ellipse(cv, cx, cy, r, r, 255);
            if (plus) { cut_bars(cv, cx, cy, k); return true; }
            float a = k * 0.55f, d = k * 0.38f;  /* back window: outline; front window: solid with a gap around it */
            fill_rrect(cv, cx - d - a - t / 2, cy - d - a - t / 2, cx - d + a + t / 2, cy - d + a + t / 2, 0, 0);
            fill_rrect(cv, cx - d - a + t / 2, cy - d - a + t / 2, cx - d + a - t / 2, cy - d + a - t / 2, 0, 255);
            fill_rrect(cv, cx + d - a - t, cy + d - a - t, cx + d + a + t, cy + d + a + t, 0, 255);
            fill_rrect(cv, cx + d - a, cy + d - a, cx + d + a, cy + d + a, 0, 0);
            return true;
        }
        /* PlayStation Options (three bars) and Create (three short rays) on a disc, as the face buttons */
        float r = s * 0.49f, k = r * 0.46f, t = r * 0.16f;
        fill_ellipse(cv, cx, cy, r, r, 255);
        if (plus) { cut_bars(cv, cx, cy, k); return true; }
        /* (rays from a point below the centre, starting away from it: a burst, not an arrow) */
        float ox = cx, oy = cy + k * 0.9f;
        for (int i = -1; i <= 1; i++) {
            float dx = sinf(i * 0.7f), dy = -cosf(i * 0.7f);
            fill_line(cv, ox + dx * k * 0.6f, oy + dy * k * 0.6f, ox + dx * k * 1.75f, oy + dy * k * 1.75f, t, 0);
        }
        return true;
    }
    }
    return false;
}

static void draw_slot(Canvas *cv, const Slot *sl) {
    float x0 = 0, y0 = 0, x1 = (float)cv->w, y1 = (float)cv->h;
    static const int lstick[4] = {IB_LUP, IB_LDOWN, IB_LLEFT, IB_LRIGHT}, rstick[4] = {IB_RUP, IB_RDOWN, IB_RLEFT, IB_RRIGHT},
                     dpad[4] = {IB_DUP, IB_DDOWN, IB_DLEFT, IB_DRIGHT};
    switch (sl->kind) {
    case K_KEY: {
        unsigned vk = shown(sl->arg);
        if (!vk) return;
        float s = fminf(x1, y1), w = x1;
        /* (a key name longer than a letter gets a wider cap where the icon was wider) */
        if (ib_is_mouse(vk) || (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) w = s;
        float cx = x1 / 2, cy = y1 / 2;
        draw_keycap(cv, cx - w / 2, cy - s / 2, cx + w / 2, cy + s / 2, vk);
        return;
    }
    case K_RSTICK:
        if (mouse_look) { draw_mouse(cv, x0, y0, x1, y1, 0, sl->arg == D_ALL ? 0 : sl->arg); return; }
        if (sl->arg == D_ROTATE) { draw_directions(cv, x0, y0, x1, y1, rstick, D_ALL); return; }
        draw_directions(cv, x0, y0, x1, y1, rstick, sl->arg);
        return;
    case K_LSTICK: draw_directions(cv, x0, y0, x1, y1, lstick, sl->arg); return;
    case K_DPAD: draw_directions(cv, x0, y0, x1, y1, dpad, sl->arg); return;
    }
}

/* ------------------------------------------------------------------------------------------------ the backplate */
/* The atlas holds each icon twice: alpha is the white icon (letters cut out), the colour channels its backplate, the
 * same shape filled and grown by ~2.5 px, which the game draws first in a dark tint: the outline around the icon and
 * the colour seen through its cut-out letters. Writing white over the whole slot made that plate the slot's rectangle
 * (square dark corners behind round PlayStation and Xbox buttons). Here the plate is made from the new icon: its shape
 * with the holes filled, grown with a soft edge, written over the slot and a margin (the original's plate reached
 * past the slot). */
enum { PLATE_MARGIN = 4 };
static void write_plate(uint8_t *data, uint32_t row, const Slot *sl) {
    int x0 = sl->x0 - PLATE_MARGIN, y0 = sl->y0 - PLATE_MARGIN, x1 = sl->x1 + PLATE_MARGIN, y1 = sl->y1 + PLATE_MARGIN;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= ATLAS_W) x1 = ATLAS_W - 1;
    if (y1 >= ATLAS_H) y1 = ATLAS_H - 1;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    uint8_t *solid = calloc((size_t)w * h, 1), *outside = calloc((size_t)w * h, 1);
    int *stack = malloc(sizeof(int) * (size_t)w * h * 4);
    if (!solid || !outside || !stack) { free(solid); free(outside); free(stack); return; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) solid[y * w + x] = data[(size_t)(y0 + y) * row + (size_t)(x0 + x) * 4 + 3] >= 32;
    /* holes: whatever the region's border cannot reach without crossing the shape */
    int n = 0;
    for (int x = 0; x < w; x++) { stack[n++] = x; stack[n++] = (h - 1) * w + x; }
    for (int y = 0; y < h; y++) { stack[n++] = y * w; stack[n++] = y * w + w - 1; }
    while (n) {
        int i = stack[--n];
        if (outside[i] || solid[i]) continue;
        outside[i] = 1;
        int x = i % w, y = i / w;
        if (x > 0) stack[n++] = i - 1;
        if (x < w - 1) stack[n++] = i + 1;
        if (y > 0) stack[n++] = i - w;
        if (y < h - 1) stack[n++] = i + w;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float best = 9;
            for (int j = -3; j <= 3; j++)
                for (int i = -3; i <= 3; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h || outside[yy * w + xx]) continue;
                    float d = sqrtf((float)(i * i + j * j));
                    if (d < best) best = d;
                }
            float v = best <= 1.0f ? 255.0f : 255.0f * (3.0f - best) / 2.0f;
            uint8_t *p = data + (size_t)(y0 + y) * row + (size_t)(x0 + x) * 4;
            p[0] = p[1] = p[2] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    free(solid); free(outside); free(stack);
}

/* ---------------------------------------------------------------------------------------------------- the hooks */
int texpatch_identify(const void *texture) {
    const NvnTexture *t = texture;
    if (t->format != 0x25 || t->layout.width != ATLAS_W || t->layout.height != ATLAS_H || !t->addr) return -1;
    enabled();
    uint64_t total = tegra_total_size(&t->layout);
    if (total != (uint64_t)ATLAS_W * ATLAS_H * 4) return -1;
    /* (a texture of the same size that is not the atlas is asked about at every check: hash it again only when a
     *  sample of its bytes changed) */
    static struct { const void *t; uint64_t sample; } seen[16];
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    uint64_t sample = 0;
    for (uint64_t i = 0; i < total; i += total / 16) { uint64_t v; memcpy(&v, mem + i, 8); sample = (sample ^ v) * 0x100000001B3ull; }
    unsigned k = (unsigned)(((uintptr_t)t >> 4) % 16);
    if (seen[k].t == t && seen[k].sample == sample) return -1;
    if (bc7_hash_full(mem, total) != ATLAS_HASH) { seen[k].t = t; seen[k].sample = sample; return -1; }
    printf("[prompts] button icon atlas found: %s prompts\n", !on ? "Switch" :
           style == STYLE_XBOX ? "Xbox" : style == STYLE_PS ? "PlayStation" : "keyboard and mouse");
    return 0;
}

uint32_t texpatch_generation(void) { enabled(); return generation; }

static void dump(const uint8_t *data, uint32_t row, const char *name) {
    const char *dir = getenv("AC_PROMPTS_DUMP");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (uint32_t y = 0; y < ATLAS_H; y++) fwrite(data + (size_t)y * row, 4, ATLAS_W, f);
    fclose(f);
}

void texpatch_apply(int id, uint32_t level, uint8_t *data, uint32_t w, uint32_t h, uint32_t row) {
    if (id != 0 || level != 0 || w != ATLAS_W || h != ATLAS_H || !enabled()) return;  /* (Switch: as it is) */
    dump(data, row, "atlas-before.rgba");
    for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        const Slot *sl = &slots[i];
        int sw = sl->x1 - sl->x0 + 1, sh = sl->y1 - sl->y0 + 1;
        Canvas cv = {sw * SS, sh * SS, calloc((size_t)sw * sh, SS * SS)};
        if (!cv.c) return;
        if (style == STYLE_KEYBOARD) draw_slot(&cv, sl);
        else if (!draw_pad(&cv, sl)) { free(cv.c); continue; }
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                unsigned sum = 0;
                for (int j = 0; j < SS; j++)
                    for (int i2 = 0; i2 < SS; i2++) sum += cv.c[(y * SS + j) * cv.w + x * SS + i2];
                uint8_t *p = data + (size_t)(sl->y0 + y) * row + (size_t)(sl->x0 + x) * 4;
                p[3] = (uint8_t)(sum / (SS * SS));
            }
        free(cv.c);
        write_plate(data, row, sl);
    }
    dump(data, row, "atlas-after.rgba");
}
