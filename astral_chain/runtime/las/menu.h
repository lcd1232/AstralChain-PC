/* The in-game menu (menu.cpp, Dear ImGui): the settings of game_settings.h while the game runs.
 *
 * F1 on the keyboard, or Minus and Plus held together on a controller (Back and Start / View and Menu), open and close
 * it; Esc or B close it too. While it is open the game gets no input and keeps running; the mouse is released from
 * the camera. Settings that apply at once change as their row changes; the others say "after a restart". Each change
 * is saved to the settings file (AC_SETTINGS_FILE) when the menu closes.
 *
 * Threads: menu_toggle, the key, mouse and pad functions are called by the window thread (app_window.h,
 * hid_service.h), the overlay functions by the render thread inside backend_present (device.c). ImGui runs on the
 * render thread only; input is queued for it. The host side declares these weak: a build without the Vulkan layer
 * has no menu. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- window thread */
int menu_is_open(void);
void menu_toggle(void);
void menu_key(unsigned vk, int down);                          /* Windows virtual-key codes, while open */
void menu_mouse_move(float x, float y, float client_w, float client_h);
void menu_mouse_button(int button, int down);                  /* 0 left, 1 right, 2 middle */
void menu_mouse_wheel(float steps);                            /* + away from the user */
/* The controller sample as the game would get it (Switch buttons, hid_service.h). Watches for Minus and Plus; returns
 * 1 while the game must get nothing (menu open, or buttons still held from it). */
int menu_pad_gate(uint64_t buttons, int32_t lx, int32_t ly);

/* ---- render thread (backend_present) */
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkFormat format;  /* of the swapchain image */
    uint32_t width, height, images;
} MenuTarget;
/* Lays out this frame's menu; 1 when there is something to draw. */
int menu_overlay_frame(const MenuTarget *t);
/* Records it into cb, inside dynamic rendering on the swapchain image. */
void menu_overlay_draw(VkCommandBuffer cb);

/* ---- what the menu asks of the host (app_window.h) */
int host_menu_fullscreen(int on);  /* -1: only ask; 0 / 1: windowed / fullscreen. Returns the state. */
void host_menu_quit(void);         /* as the window's close button */

#ifdef __cplusplus
}
#endif
