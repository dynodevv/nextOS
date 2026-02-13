/*
 * nextOS - compositor.h
 * Skeuomorphic window compositor with double-buffered rendering
 */
#ifndef NEXTOS_COMPOSITOR_H
#define NEXTOS_COMPOSITOR_H

#include <stdint.h>

/* ── Theme definitions ────────────────────────────────────────────────── */
typedef enum {
    THEME_BRUSHED_METAL = 0,
    THEME_GLOSSY_GLASS,
    THEME_COUNT
} theme_t;

/* ── Window structure ─────────────────────────────────────────────────── */
#define MAX_WINDOWS 16
#define WIN_TITLE_LEN 64

typedef struct {
    int      active;
    int      x, y;
    int      width, height;
    char     title[WIN_TITLE_LEN];
    uint32_t *canvas;            /* Client-area pixel buffer */
    int      focused;
    int      dragging;
    int      drag_ox, drag_oy;
    int      minimized;
    int      close_hover;

    /* Callback: called each frame so the app can draw into canvas */
    void (*on_paint)(struct window *self);
    void (*on_key)(struct window *self, char ascii, int scancode, int pressed);
    void (*on_mouse)(struct window *self, int mx, int my, int buttons);
} window_t;

/* ── Public API ───────────────────────────────────────────────────────── */
void      compositor_init(void);
void      compositor_set_theme(theme_t theme);
theme_t   compositor_get_theme(void);
window_t *compositor_create_window(const char *title, int x, int y, int w, int h);
void      compositor_destroy_window(window_t *win);
void      compositor_render_frame(void);
void      compositor_handle_mouse(int mx, int my, int buttons);
void      compositor_handle_key(char ascii, int scancode, int pressed);

/* Desktop */
void      desktop_draw_wallpaper(void);
void      desktop_draw_taskbar(void);

#endif /* NEXTOS_COMPOSITOR_H */
