// The in-game menu (menu.h): Dear ImGui (third_party/imgui) drawn over the game image in backend_present (device.c).
//
// The Vulkan set-up and the open / input-block / pad navigation structure follow the TotK PC port's runtime/ui/menu.cpp,
// itself after LASRecomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp, MIT, Copyright (c)
// 2026 Peiikai): dynamic rendering into the swapchain image, the back end's own descriptor pool, vertex buffers rotated
// over more frames than are in flight.
//
// Tabs of rows over game_settings.h (each row a setting, AC_* / LARECOMP_*), in the TotK port's look: L / R or Q / E
// switch the tabs. Text in English or Russian (AC_LANGUAGE), in the system font (AC_FONT; without it ImGui's own,
// English only).
#include <atomic>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner, SetFocusID, SetNavCursorVisible
#include "backends/imgui_impl_vulkan.h"
#include "menu.h"

extern "C" {
// game_settings.h (host runtime)
int setting_get(const char *name, char *buf, unsigned size);
int setting_fixed(const char *name);
void setting_set(const char *name, const char *value);
int settings_save(void);
}

// ---- open state and input (window thread in, render thread out)
static std::atomic<int> g_open;
static std::atomic<int> g_reset;   // opened since the last frame: the cursor goes to the first row
static std::atomic<int> g_failed;  // the Vulkan back end could not start: the menu stays closed
static std::atomic<uint64_t> g_pad_buttons;
static std::atomic<int32_t> g_pad_lx, g_pad_ly;

extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
static void set_open(int open) {
    if (g_failed.load() && open) return;
    if (g_open.exchange(open) != open) {
        if (open) g_reset.store(1);
        printf("[menu] %s\n", open ? "open" : "closed");
        fflush(stdout);
    }
}
extern "C" void menu_toggle(void) { set_open(!menu_is_open()); }

enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static std::mutex ev_lock;
static Ev ev_q[EV_MAX];
static int ev_n;
static void push(Ev e) {
    if (!menu_is_open()) return;
    std::lock_guard<std::mutex> l(ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
}

static ImGuiKey key_of(unsigned vk) {
    switch (vk) {
    case 0x26: return ImGuiKey_UpArrow;
    case 0x28: return ImGuiKey_DownArrow;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x0D: return ImGuiKey_Enter;
    case 0x20: return ImGuiKey_Space;
    case 0x1B: return ImGuiKey_Escape;
    case 0x09: return ImGuiKey_Tab;
    case 0x08: return ImGuiKey_Backspace;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x10: case 0xA0: case 0xA1: return ImGuiKey_LeftShift;
    case 'Q': return ImGuiKey_Q;
    case 'E': return ImGuiKey_E;
    }
    return ImGuiKey_None;
}
extern "C" void menu_key(unsigned vk, int down) {
    ImGuiKey k = key_of(vk);
    if (k != ImGuiKey_None) push({EV_KEY, (int)k, down, 0, 0});
}
// (as fractions of the client area: the swapchain may have more pixels than the window has units)
extern "C" void menu_mouse_move(float x, float y, float cw, float ch) {
    if (cw > 0 && ch > 0) push({EV_POS, 0, 0, x / cw, y / ch});
}
extern "C" void menu_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_mouse_wheel(float steps) { push({EV_WHEEL, 0, 0, 0, steps}); }

// Switch buttons (hid_service.h): A 0 (right), B 1 (bottom), X 2 (top), Y 3 (left), L 6, R 7, Plus 10, Minus 11,
// D-pad left 12, up 13, right 14, down 15.
enum { PAD_PLUS = 1 << 10, PAD_MINUS = 1 << 11 };
extern "C" int menu_pad_gate(uint64_t buttons, int32_t lx, int32_t ly) {
    static int combo_was, held_after;
    g_pad_buttons.store(buttons, std::memory_order_relaxed);
    g_pad_lx.store(lx, std::memory_order_relaxed);
    g_pad_ly.store(ly, std::memory_order_relaxed);
    int combo = (buttons & (PAD_PLUS | PAD_MINUS)) == (PAD_PLUS | PAD_MINUS);
    if (combo && !combo_was) menu_toggle();
    combo_was = combo;
    if (menu_is_open()) { held_after = 1; return 1; }
    // (closed: what was held for the menu reaches the game only once let go)
    if (held_after && !buttons && abs(lx) < 8000 && abs(ly) < 8000) held_after = 0;
    return held_after;
}

static int g_step;   // left / right this frame (D-pad, stick, arrow keys): -1 / +1 to the row with the cursor

static void drain_events(ImGuiIO &io, float w, float h) {
    Ev q[EV_MAX];
    int n;
    {
        std::lock_guard<std::mutex> l(ev_lock);
        n = ev_n;
        memcpy(q, ev_q, sizeof(Ev) * n);
        ev_n = 0;
    }
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY:
            io.AddKeyEvent((ImGuiKey)e.a, e.b != 0);
            if (e.b && e.a == ImGuiKey_LeftArrow) g_step = -1;
            if (e.b && e.a == ImGuiKey_RightArrow) g_step = 1;
            break;
        case EV_POS: io.AddMousePosEvent(e.x * w, e.y * h); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(0, e.y); break;
        }
    }
}

// The pad as ImGui's gamepad keys, Xbox positions (the bottom button activates, the right one goes back); the left
// stick moves as the D-pad does, past half way, kept until its axis falls under 0.35.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {1, ImGuiKey_GamepadFaceDown}, {0, ImGuiKey_GamepadFaceRight}, {2, ImGuiKey_GamepadFaceUp},
    {3, ImGuiKey_GamepadFaceLeft}, {6, ImGuiKey_GamepadL1}, {7, ImGuiKey_GamepadR1},
    {12, ImGuiKey_GamepadDpadLeft}, {13, ImGuiKey_GamepadDpadUp}, {14, ImGuiKey_GamepadDpadRight},
    {15, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;
static int stick_dir = -1;
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;  // y up
    float along = stick_dir == 12 ? -x : stick_dir == 14 ? x : stick_dir == 13 ? y : stick_dir == 15 ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? 12 : 14) : (y > 0 ? 13 : 15);
}
static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b = g_pad_buttons.load(std::memory_order_relaxed);
    stick_dir = stick_to_dpad(g_pad_lx.load(std::memory_order_relaxed), g_pad_ly.load(std::memory_order_relaxed));
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (reset) { pad_prev = b; return; }  // (held when the menu opened: counts once let go and pressed again)
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    // (left / right change the row's value; ImGui's own handling of them only moves between items)
    if (b & ~pad_prev & (1ull << 12)) g_step = -1;
    if (b & ~pad_prev & (1ull << 14)) g_step = 1;
    pad_prev = b;
}

// ---- the pages. Rows in the look of the TotK PC port's runtime/ui/option_rows.cpp (MIT; adapted there from
// SavageWindRecomp's runtime/ui/option_rows.mm, https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai): rows on the left, the selected (controller) or hovered (mouse) row's description on the
// right; A or a click steps a row (toggle, next value), left and right step it; a row marks its default value, says
// "after restart" when the setting is read at the start, and is greyed out when an environment variable fixes it.
enum Kind { CHECK, CHOICE, NUMBER, FULLSCREEN, NOTE };
struct Row {
    Kind kind;
    const char *key;
    const char *en, *ru;            // label (NOTE: the text)
    const char *desc_en, *desc_ru;  // the description pane
    const char *def;                // value when the setting is not set
    bool live;                      // applies at once (else after a restart)
    const char *values;             // CHOICE: "value|English|Russian;..."; NUMBER: "low;high;step;suffix"; CHECK: the on value
    bool presence;                  // CHECK read by the setting's presence (LARECOMP_NO_PIPE_WARM): on = not set
};
static const Row display_rows[] = {
    {FULLSCREEN, "AC_FULLSCREEN", "Fullscreen", "Полноэкранный режим",
     "Fills the screen with the game; F11 or Alt+Enter switch it too. The game also starts this way.",
     "Игра на весь экран; F11 или Alt+Enter тоже переключают. Игра и запускается так же.", "0", true},
    {CHOICE, "AC_FPS_LIMIT", "Frame rate limit", "Ограничение частоты кадров",
     "The highest frame rate. Lower limits give steadier frames on slow machines (40 on the Steam Deck); the game "
     "runs at its normal speed at any of them.",
     "Наибольшая частота кадров. Ниже — ровнее на слабых машинах (40 на Steam Deck); скорость игры от этого не "
     "меняется.", "60", true,
     "60|60 fps|60 fps;50|50 fps|50 fps;45|45 fps|45 fps;40|40 fps|40 fps;30|30 fps|30 fps"},
    {CHECK, "AC_HANDHELD", "Handheld mode", "Портативный режим",
     "The game thinks it runs out of the dock and renders at 1280x720 instead of 1600x900: faster on weak GPUs.",
     "Игра считает, что Switch вынут из док-станции, и рисует в 1280x720 вместо 1600x900: быстрее на слабых "
     "видеокартах.", "0", false},
    {CHOICE, "LARECOMP_VK_PRESENT", "Presentation", "Режим вывода",
     "Mailbox: no tearing and little delay. FIFO: classic V-Sync. Immediate: no waiting, may tear.",
     "Mailbox: без разрывов и с малой задержкой. FIFO: обычная вертикальная синхронизация. Immediate: без ожидания, "
     "возможны разрывы.", "mailbox", false,
     "mailbox|Mailbox|Mailbox;fifo|FIFO (V-Sync)|FIFO (V-Sync);immediate|Immediate|Immediate"},
};
static const Row game_rows[] = {
    {CHECK, "AC_CUTSCENE_FPS", "Cutscenes at 30 fps", "Ролики в 30 fps",
     "Cutscenes run at 30 fps, as on the Switch. Off: at the game's frame rate.",
     "Ролики на движке идут в 30 fps, как на Switch. Выкл: с частотой кадров игры.", "0", true, "30"},
    {CHECK, "AC_IGNORE_FOCUS", "Keep running when minimized", "Не ставить на паузу при сворачивании",
     "Off: the game pauses while its window is minimized, as the Switch does in the HOME menu.",
     "Выкл: игра встаёт на паузу, пока окно свёрнуто, как Switch в меню HOME.", "0", true},
    {CHECK, "AC_SAVE_SNAPSHOTS", "Back up each save", "Резервная копия сохранений",
     "A copy of the save folder after each save; the newest 30 are kept, next to the saves.",
     "Копия папки сохранений после каждого сохранения; хранятся 30 последних, рядом с сохранениями.", "1", true},
};
static const Row sound_rows[] = {
    {CHECK, "AC_AUDIO_RESYNC", "Catch up after a stall", "Догонять звук после подвисания",
     "After a stall (loading, a shader compiled) the sound skips the gap instead of staying behind the picture.",
     "После подвисания (загрузка, компиляция шейдера) звук пропускает паузу, а не отстаёт от картинки.", "1", true},
};
static const Row controls_rows[] = {
    {CHOICE, "AC_PROMPTS", "Button icons", "Значки кнопок",
     "The button prompts in the game. Xbox also suits the Steam Controller; the Xbox and PlayStation icons follow "
     "the button positions.",
     "Значки кнопок в подсказках игры. Xbox подходит и для Steam Controller; значки Xbox и PlayStation следуют "
     "расположению кнопок.", "keyboard", true,
     "keyboard|Keyboard and mouse|Клавиатура и мышь;xbox|Xbox / Steam|Xbox / Steam;ps|PlayStation|PlayStation;"
     "switch|Switch|Switch"},
    {CHECK, "AC_MOUSE_LOOK", "Mouse moves the camera", "Мышь управляет камерой",
     "A click in the window captures the mouse; Alt lets it go while held.",
     "Щелчок по окну захватывает мышь; пока зажат Alt, она свободна.", "1", true},
    {NUMBER, "AC_MOUSE_SENS", "Mouse sensitivity", "Чувствительность мыши",
     "How fast the camera follows the mouse.", "Как быстро камера следует за мышью.", "100", true, "10;500;10; %"},
    {CHECK, "AC_MOUSE_INVERT", "Invert mouse up and down", "Инвертировать мышь по вертикали",
     "Moving the mouse forward looks down.", "Мышь от себя — камера смотрит вниз.", "0", true},
    {NOTE, NULL, "Keys are set in the launcher (Controls).", "Клавиши настраиваются в лаунчере (Управление)."},
};
static const Row advanced_rows[] = {
    {CHECK, "AC_ASYNC_SUBMIT", "Graphics on a separate thread", "Графика в отдельном потоке",
     "Prepares the graphics on a thread of its own, while the game goes on with the next frame.",
     "Готовит графику в отдельном потоке, пока игра готовит следующий кадр.", "1", false},
    {CHECK, "LARECOMP_BC7", "Compress textures on the GPU", "Сжимать текстуры на GPU",
     "BC7: a quarter of the video memory of the textures. Prepared at setup (the launcher's option): no extra "
     "pauses, on by default; else converted on the GPU, with short pauses while areas load.",
     "BC7: текстуры занимают вчетверо меньше видеопамяти. Подготовленные при установке (пункт в лаунчере) — без "
     "лишних пауз, включено по умолчанию; иначе сжимаются на видеокарте, с короткими паузами при загрузке зон.",
     "0", false},
    {CHECK, "LARECOMP_GPU_ASTC", "Decode ASTC on the GPU", "Декодировать ASTC на GPU",
     "Off: the textures are decoded on the CPU (slower; for a GPU that draws them wrong).",
     "Выкл: текстуры декодируются на процессоре (медленнее; для видеокарт, которые рисуют их неправильно).", "1", false},
    {CHECK, "LARECOMP_NO_PIPE_WARM", "Build pipelines ahead", "Заранее собирать конвейеры",
     "Builds recorded pipelines before their first draw: fewer hitches the first time an effect shows.",
     "Собирает записанные конвейеры до первого использования: меньше рывков, когда эффект появляется впервые.",
     NULL, false, NULL, true},
};
struct Page { const char *en, *ru; const Row *rows; int n; };
#define PAGE(en, ru, r) {en, ru, r, (int)(sizeof r / sizeof r[0])}
static const Page pages[] = {
    PAGE("Screen", "Экран", display_rows), PAGE("Game", "Игра", game_rows), PAGE("Sound", "Звук", sound_rows),
    PAGE("Controls", "Управление", controls_rows), PAGE("Advanced", "Дополнительно", advanced_rows),
};
enum { PAGES = sizeof pages / sizeof pages[0] };

static bool g_ru;
static bool g_dirty;          // changed since the last save
static int g_quit_armed;      // "Quit game" pressed once: the next press quits
static int g_tab, g_tab_request = -1;
static bool g_focus_first;    // the page's first row takes the cursor (opening, switching tabs)
static int g_focus_page = -1; // ... of this page, when it is next drawn (a new tab shows a frame later)

static const char *tr(const char *en, const char *ru) { return g_ru ? ru : en; }

// The n-th "value|English|Russian" of a choice list; false past the end.
static bool choice_at(const char *list, int n, char *value, char *name, size_t size) {
    const char *p = list;
    for (int i = 0; i < n && p; i++) { p = strchr(p, ';'); if (p) p++; }
    if (!p || !*p) return false;
    size_t len = strcspn(p, ";");
    char item[160];
    snprintf(item, sizeof item, "%.*s", (int)len, p);
    char *a = strchr(item, '|'), *b = a ? strchr(a + 1, '|') : NULL;
    if (!a || !b) return false;
    *a = *b = 0;
    snprintf(value, size, "%s", item);
    snprintf(name, size, "%s", g_ru ? b + 1 : a + 1);
    return true;
}

// BC7 is on by default once its textures were prepared at setup (bc7_gpu.c; the run folder is the working directory).
static const char *row_default(const Row &r) {
    if (r.key && !strcmp(r.key, "LARECOMP_BC7")) {
        static int prepared = -1;
        if (prepared < 0) {
            FILE *f = fopen("cache/bc7/complete.txt", "rb");
            prepared = f != NULL;
            if (f) fclose(f);
        }
        return prepared ? "1" : "0";
    }
    return r.def;
}

static void current(const Row &r, char *v, size_t size) {
    if (r.presence) { char x[64]; snprintf(v, size, "%s", setting_get(r.key, x, sizeof x) ? "0" : "1"); return; }
    const char *def = row_default(r);
    if (!setting_get(r.key, v, (unsigned)size) || !v[0]) snprintf(v, size, "%s", def ? def : "");
}

static void change(const Row &r, const char *value) {
    if (r.presence) setting_set(r.key, !strcmp(value, "1") ? NULL : "1");
    else setting_set(r.key, value);
    g_dirty = true;
}

// ---- the row widget
static const ImU32 kGold = IM_COL32(236, 194, 84, 255);
static const ImU32 kDim = IM_COL32(150, 150, 140, 255);
static const ImU32 kGrey = IM_COL32(110, 110, 102, 255);
static float g_scale = 1;
static char g_hover[512], g_focus[512], g_first[512], g_shown[512];   // description texts

static void desc_set(char *d, const Row &r, bool off) {
    int n = snprintf(d, 512, "%s", tr(r.desc_en, r.desc_ru));
    if (n < 0 || n >= 512) return;
    if (off) snprintf(d + n, 512 - n, "\n\n%s %s", tr("Set by the environment variable", "Задано переменной окружения"), r.key);
    else if (!r.live) snprintf(d + n, 512 - n, "\n\n%s", tr("Applies after the game is restarted.", "Вступит в силу после перезапуска игры."));
}

struct RowView { const char *value; bool is_default, arrows; };

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right)
// or 0; always 0 for a row the environment fixes.
static int row_widget(const Row &r, const RowView &v, bool off) {
    ImGui::PushID(r.key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (g_focus_first) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollY(0);
        g_focus_first = false;
    }
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    if (foc && !off) {
        // (the left stick arrives as the D-pad: pad_events)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || g_step < 0) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) || g_step > 0) step = 1;
        g_step = 0;
        static const ImGuiKey steps[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow};
        // (left and right belong to the row: navigation would move sideways into the description pane)
        for (ImGuiKey k : steps) ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (foc) desc_set(g_focus, r, off);
    if (hov) desc_set(g_hover, r, off);
    if (!g_first[0]) desc_set(g_first, r, off);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float rad = 9 * g_scale, pad = 16 * g_scale, small = fs * 0.72f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), rad);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                hov || foc ? (off ? IM_COL32(236, 194, 84, 120) : kGold) : IM_COL32(255, 255, 255, off ? 18 : 38), rad, 0,
                hov || foc ? 2.0f : 1.0f);
    float ty = p.y + fs * 0.25f, ny = p.y + fs * 1.3f;
    float right = p.x + w - pad, value_left = right;
    if (v.arrows && v.value[0]) {   // < value >, drawn; a click on an arrow steps that way
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(v.value).x - gap - aw;
        ImU32 ac = off ? kGrey : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x;
            if (mx >= lx - gap && mx <= lx + aw + gap) step = -1;
            else if (mx >= rx - gap && mx <= rx + aw + gap) step = 1;
        }
        right = rx - gap;
        value_left = lx;
    }
    if (v.value[0]) {
        float vx = right - ImGui::CalcTextSize(v.value).x;
        dl->AddText(ImVec2(vx, ty), off ? kGrey : v.is_default ? IM_COL32(205, 205, 195, 255) : IM_COL32(255, 255, 255, 255), v.value);
        if (!v.arrows) value_left = vx;
    }
    if (v.is_default) {
        const char *t = tr("default", "по умолчанию");
        float tw = font->CalcTextSizeA(small, FLT_MAX, 0, t).x;
        dl->AddText(font, small, ImVec2(right - tw, ny), kDim, t);
    }
    ImVec4 clip(p.x, p.y, value_left - fs * 0.6f, p.y + h);
    dl->AddText(font, fs, ImVec2(p.x + pad, ty), off ? kGrey : IM_COL32(240, 236, 224, 255), tr(r.en, r.ru), NULL, 0, &clip);
    const char *note = off ? tr("set by the environment", "задано переменной окружения")
                     : !r.live ? tr("after restart", "после перезапуска") : NULL;
    if (note) dl->AddText(font, small, ImVec2(p.x + pad, ny), off ? IM_COL32(200, 150, 90, 255) : kDim, note);
    ImGui::PopID();
    return off ? 0 : step;
}

// A row of the table: its value shown, a step applied.
static void draw_row(const Row &r) {
    if (r.kind == NOTE) {
        ImGui::Dummy(ImVec2(0, 4 * g_scale));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr(r.en, r.ru));
        ImGui::PopTextWrapPos();
        return;
    }
    bool off = setting_fixed(r.key) != 0;
    char v[64], shown[96] = "";
    current(r, v, sizeof v);
    RowView view = {shown, false, false};
    switch (r.kind) {
    case CHECK: {
        bool on = r.values ? !strcmp(v, r.values) : atoi(v) != 0;
        const char *def = row_default(r);
        bool def_on = r.presence || (r.values ? def && !strcmp(def, r.values) : def && atoi(def) != 0);
        snprintf(shown, sizeof shown, "%s", on ? tr("On", "Вкл") : tr("Off", "Выкл"));
        view.is_default = on == def_on;
        if (row_widget(r, view, off)) change(r, on ? "0" : r.values ? r.values : "1");
        break;
    }
    case FULLSCREEN: {
        int on = host_menu_fullscreen(-1);
        snprintf(shown, sizeof shown, "%s", on ? tr("On", "Вкл") : tr("Off", "Выкл"));
        if (row_widget(r, view, off)) { host_menu_fullscreen(!on); change(r, on ? "0" : "1"); }
        break;
    }
    case CHOICE: {
        char value[64], name[64];
        int n = 0, cur = -1;
        while (choice_at(r.values, n, value, name, sizeof value)) {
            if (!strcmp(value, v)) { cur = n; snprintf(shown, sizeof shown, "%s", name); }
            n++;
        }
        if (cur < 0) snprintf(shown, sizeof shown, "%s", v);
        view.arrows = true;
        view.is_default = r.def && !strcmp(v, r.def);
        int step = row_widget(r, view, off);
        if (!step || !n) break;
        int nv = cur < 0 ? 0 : step == 2 ? (cur + 1) % n : cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
        if (nv != cur && choice_at(r.values, nv, value, name, sizeof value)) change(r, value);
        break;
    }
    case NUMBER: {
        int lo = 0, hi = 0, st = 1;
        sscanf(r.values, "%d;%d;%d", &lo, &hi, &st);
        const char *suffix = strrchr(r.values, ';');
        snprintf(shown, sizeof shown, "%s%s", v, suffix ? suffix + 1 : "");
        view.arrows = true;
        view.is_default = r.def && atoi(v) == atoi(r.def);
        int step = row_widget(r, view, off);
        if (!step) break;
        int x = atoi(v) + (step == 2 ? 1 : step) * st;
        if (step == 2 && x > hi) x = lo;
        x = x < lo ? lo : x > hi ? hi : x;
        char out[16];
        snprintf(out, sizeof out, "%d", x);
        if (x != atoi(v)) change(r, out);
        break;
    }
    case NOTE: break;
    }
}

static void page(const Page &pg, float bottom) {
    g_hover[0] = g_focus[0] = g_first[0] = 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= bottom;
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.58f, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 5 * g_scale));
    for (int i = 0; i < pg.n; i++) draw_row(pg.rows[i]);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // the hovered row (mouse) wins, else the selected one (controller), else the last shown, else the first row
    if (g_hover[0]) memcpy(g_shown, g_hover, sizeof g_shown);
    else if (g_focus[0]) memcpy(g_shown, g_focus, sizeof g_shown);
    else if (!g_shown[0]) memcpy(g_shown, g_first, sizeof g_shown);
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), da = ImGui::GetContentRegionAvail();
    da.y -= bottom;
    float gap = 24 * g_scale;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + da.y),
                                        IM_COL32(236, 194, 84, 110), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, da.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_shown);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---- ImGui and its Vulkan back end (render thread)
static bool g_ctx, g_vk_ok, g_fonts_ok;
static VkFormat g_fmt;
static float g_font_scale, g_style_scale;
static ImGuiStyle g_base_style;
static bool g_has_font;   // a TrueType font with Cyrillic: else English
static uint64_t g_last;

static float menu_scale(uint32_t h) { return (h >= 720 ? h : 720) / 1080.0f; }

// The TotK port's colours (after SavageWindRecomp): dark green panels, gold accents.
static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
}

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) printf("[menu] Vulkan error %d\n", (int)r);
}

static bool vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_fmt == t->format) return true;
    if (g_vk_ok) {  // (the swapchain changed format: pipelines for the new one)
        vkQueueWaitIdle(t->queue);
        ImGui_ImplVulkan_Shutdown();
        g_vk_ok = false;
        g_font_scale = 0;
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = t->images > 4 ? t->images : 4;   // (vertex buffers per recorded frame: more than the 3 in flight)
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.CheckVkResultFn = check_vk;
    g_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    g_vk_ok = true;
    return true;
}

// The system font (AC_FONT) at 26 px per 1080 lines, Latin and Cyrillic; ImGui's own font without it.
static bool fonts_up(float scale) {
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x04FF, 0x2010, 0x203A, 0x2190, 0x2193, 0x2212, 0x2212, 0};
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    const char *path = getenv("AC_FONT");
    g_has_font = false;
    if (path && *path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            g_has_font = fa->AddFontFromFileTTF(path, 26.0f * scale, NULL, ranges) != NULL;
        }
    }
    if (!g_has_font) {
        ImFontConfig fc;
        fc.SizePixels = 13.0f * 2 * scale;
        fa->AddFontDefault(&fc);
    }
    bool ok = ImGui_ImplVulkan_CreateFontsTexture();
    printf("[menu] font %s at %.0f px%s\n", g_has_font ? path : "(ImGui's own)", 26.0f * scale, ok ? "" : ": upload failed");
    g_font_scale = ok ? scale : 0;
    return ok;
}

static void layout(const MenuTarget *t, float scale) {
    float w = (float)t->width, h = (float)t->height;
    g_scale = scale;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 120));
    float ww = fminf(w * 0.88f, 1400 * scale), wh = fminf(h * 0.86f, 820 * scale);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings", "Настройки"));
    const char *hint = tr("Astral Chain PC  ·  F1 or − and + close", "Astral Chain PC  ·  F1 или − и + закрывают");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(hint).x);
    ImGui::TextDisabled("%s", hint);

    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
    if (step) g_tab_request = (g_tab + step + PAGES) % PAGES;
    if (g_tab_request >= 0) g_focus_page = g_tab_request;

    const ImGuiStyle &st = ImGui::GetStyle();
    float bottom = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < PAGES; k++) {
            char label[128];
            snprintf(label, sizeof label, "  %s  ###tab%d", tr(pages[k].en, pages[k].ru), k);
            if (!ImGui::BeginTabItem(label, NULL, g_tab_request == k ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            if (g_tab != k) g_shown[0] = 0;
            g_tab = k;
            if (g_focus_page == k) {
                g_focus_first = true;
                g_focus_page = -1;
                ImGui::NavMoveRequestCancel();   // (this frame's move goes to the first row instead)
            }
            ImGui::Spacing();
            page(pages[k], bottom);
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }

    // bottom row: the buttons on the right
    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    const char *cont = tr("Continue", "Продолжить");
    char quit[96];   // (one id for both texts: a new id lost the cursor, and the confirmation reset at once)
    snprintf(quit, sizeof quit, "%s###quit", g_quit_armed ? tr("Quit? Press again", "Выйти? Ещё раз") : tr("Quit game", "Выйти из игры"));
    float bw = fmaxf(fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(tr("Quit? Press again", "Выйти? Ещё раз")).x),
                     ImGui::CalcTextSize(tr("Quit game", "Выйти из игры")).x) + st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - st.WindowPadding.x - bw * 2 - st.ItemSpacing.x, y));
    if (ImGui::Button(cont, ImVec2(bw, 0))) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button(quit, ImVec2(bw, 0))) {
        if (g_quit_armed) { settings_save(); g_dirty = false; host_menu_quit(); set_open(0); }
        else g_quit_armed = 1;
    }
    if (!ImGui::IsItemFocused() && !ImGui::IsItemHovered()) g_quit_armed = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) set_open(0);
    ImGui::End();
}

extern "C" int menu_overlay_frame(const MenuTarget *t) {
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_dirty) { settings_save(); g_dirty = false; }
        return 0;
    }
    if (!g_ctx) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendPlatformName = "astral_chain_menu";
        style_init(g_base_style);
        ImGui::GetStyle() = g_base_style;
        const char *lang = getenv("AC_LANGUAGE");
        g_ru = lang && !strncmp(lang, "ru", 2);
        g_ctx = true;
    }
    float scale = menu_scale(t->height);
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        printf("[menu] Dear ImGui Vulkan back end set-up failed: no menu\n");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (!g_has_font) g_ru = false;   // (ImGui's own font has no Cyrillic)
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_quit_armed = 0;
        g_last = 0;
        g_focus_page = g_tab;
        g_shown[0] = 0;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        double dt = g_last ? (now - g_last) / 1e9 : 1 / 60.0;
        io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
        g_last = now;
    }
    drain_events(io, (float)t->width, (float)t->height);
    pad_events(io, reset);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    layout(t, scale);
    g_step = 0;
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb); }
