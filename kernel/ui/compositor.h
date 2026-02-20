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
    THEME_DARK_OBSIDIAN,
    THEME_WARM_MAHOGANY,
    THEME_COUNT
} theme_t;

/* ── Animation types ──────────────────────────────────────────────────── */
#define ANIM_NONE        0
#define ANIM_OPEN        1
#define ANIM_CLOSE       2
#define ANIM_MINIMIZE    3
#define ANIM_UNMINIMIZE  4
#define ANIM_MAXIMIZE    5
#define ANIM_RESTORE     6

/* ── Window structure ─────────────────────────────────────────────────── */
#define MAX_WINDOWS 16
#define WIN_TITLE_LEN 64

typedef struct window_s window_t;
struct window_s {
    int      active;
    int      x, y;
    int      width, height;
    char     title[WIN_TITLE_LEN];
    uint32_t *canvas;            /* Client-area pixel buffer */
    int      focused;
    int      dragging;
    int      drag_ox, drag_oy;
    int      minimized;
    int      maximized;
    int      orig_x, orig_y, orig_w, orig_h;  /* Pre-maximize geometry */
    int      close_hover;

    /* Animation state */
    int      anim_type;
    uint64_t anim_start;
    int      anim_from_x, anim_from_y;
    int      anim_to_x, anim_to_y;
    int      anim_from_w, anim_from_h;
    int      anim_to_w, anim_to_h;

    /* Callback: called each frame so the app can draw into canvas */
    void (*on_paint)(window_t *self);
    void (*on_key)(window_t *self, char ascii, int scancode, int pressed);
    void (*on_mouse)(window_t *self, int mx, int my, int buttons);
    void (*on_close)(window_t *self);
};

/* ── Public API ───────────────────────────────────────────────────────── */
void      compositor_init(void);
void      compositor_set_theme(theme_t theme);
theme_t   compositor_get_theme(void);
int       compositor_set_resolution(int w, int h);
void      compositor_set_utc_offset(int hours);
int       compositor_get_utc_offset(void);
window_t *compositor_create_window(const char *title, int x, int y, int w, int h);
void      compositor_destroy_window(window_t *win);
void      compositor_render_frame(void);
void      compositor_handle_mouse(int mx, int my, int buttons, int scroll);
int       compositor_get_scroll(void);
int       compositor_get_smooth_scroll(void);
void      compositor_handle_key(char ascii, int scancode, int pressed);
void      compositor_set_app_launcher(void (*callback)(int item));
void      compositor_toggle_start_menu(void);
void      compositor_draw_cursor(int x, int y);

/* Desktop */
void      desktop_draw_wallpaper(void);
void      desktop_draw_taskbar(void);

#endif /* NEXTOS_COMPOSITOR_H */
