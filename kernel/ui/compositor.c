/*
 * nextOS - compositor.c
 * Skeuomorphic window compositor
 *
 * Design philosophy: every surface features glossy gradients, bevels,
 * and drop shadows to emulate a rich, tactile "real-world" UI.
 * No flat colours — every panel has at least a two-stop gradient.
 */
#include "compositor.h"
#include "../gfx/framebuffer.h"
#include "../mem/heap.h"
#include "../drivers/timer.h"

/* ── Internal state ───────────────────────────────────────────────────── */
static window_t windows[MAX_WINDOWS];
static int      window_count = 0;
static theme_t  current_theme = THEME_BRUSHED_METAL;
static int      prev_mouse_buttons = 0;
static int      start_menu_open = 0;
static void   (*start_menu_callback)(int item) = (void *)0;
static int      current_scroll = 0;  /* Scroll delta for current frame */

/* Smooth scroll state */
static int      smooth_scroll_target = 0;
static int      smooth_scroll_current = 0;  /* Fixed-point x256 */

/* Wallpaper cache to avoid expensive per-frame redraws */
static uint32_t *wallpaper_cache = (void *)0;
static int       wallpaper_dirty = 1;
static theme_t   wallpaper_theme = THEME_BRUSHED_METAL;
static uint32_t  wallpaper_w = 0, wallpaper_h = 0;

/* ── Theme colour palettes ────────────────────────────────────────────── */
typedef struct {
    uint32_t titlebar_top;
    uint32_t titlebar_bot;
    uint32_t titlebar_text;
    uint32_t border;
    uint32_t shadow;
    uint32_t taskbar_top;
    uint32_t taskbar_bot;
    uint32_t taskbar_text;
    uint32_t desktop_top;
    uint32_t desktop_bot;
    uint32_t button_top;
    uint32_t button_bot;
    uint32_t close_btn;
} theme_colors_t;

static const theme_colors_t themes[THEME_COUNT] = {
    [THEME_BRUSHED_METAL] = {
        .titlebar_top  = 0xC8C8C8,
        .titlebar_bot  = 0x8A8A8A,
        .titlebar_text = 0x1A1A1A,
        .border        = 0x505050,
        .shadow        = 0x303030,
        .taskbar_top   = 0xB0B0B0,
        .taskbar_bot   = 0x707070,
        .taskbar_text  = 0x1A1A1A,
        .desktop_top   = 0x4A6FA5,
        .desktop_bot   = 0x1B2838,
        .button_top    = 0xD0D0D0,
        .button_bot    = 0x909090,
        .close_btn     = 0xCC4444,
    },
    [THEME_GLOSSY_GLASS] = {
        .titlebar_top  = 0xE8F0FF,
        .titlebar_bot  = 0x6090D0,
        .titlebar_text = 0x1A1A1A,
        .border        = 0x3060A0,
        .shadow        = 0x203050,
        .taskbar_top   = 0xD0E0F8,
        .taskbar_bot   = 0x5080C0,
        .taskbar_text  = 0x1A1A1A,
        .desktop_top   = 0x2060B0,
        .desktop_bot   = 0x0A1A30,
        .button_top    = 0xC0D8F0,
        .button_bot    = 0x5080B0,
        .close_btn     = 0xE04040,
    },
    [THEME_DARK_OBSIDIAN] = {
        .titlebar_top  = 0x484848,
        .titlebar_bot  = 0x1E1E1E,
        .titlebar_text = 0xE0E0E0,
        .border        = 0x2A2A2A,
        .shadow        = 0x101010,
        .taskbar_top   = 0x3A3A3A,
        .taskbar_bot   = 0x1A1A1A,
        .taskbar_text  = 0xFFFFFF,
        .desktop_top   = 0x2A1A30,
        .desktop_bot   = 0x0A0510,
        .button_top    = 0x505050,
        .button_bot    = 0x282828,
        .close_btn     = 0xD04040,
    },
    [THEME_WARM_MAHOGANY] = {
        .titlebar_top  = 0xB07848,
        .titlebar_bot  = 0x704020,
        .titlebar_text = 0xFFF0E0,
        .border        = 0x503018,
        .shadow        = 0x281808,
        .taskbar_top   = 0x986840,
        .taskbar_bot   = 0x583020,
        .taskbar_text  = 0xFFFFFF,
        .desktop_top   = 0x4A6030,
        .desktop_bot   = 0x1A2810,
        .button_top    = 0xC08858,
        .button_bot    = 0x805030,
        .close_btn     = 0xC84040,
    },
};

/* ── Gradient helper ──────────────────────────────────────────────────── */
static uint32_t lerp_color(uint32_t a, uint32_t b, int t, int max)
{
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * t / max;
    int rg = ag + (bg - ag) * t / max;
    int rb = ab + (bb - ab) * t / max;
    if (rr < 0) rr = 0;
    if (rr > 255) rr = 255;
    if (rg < 0) rg = 0;
    if (rg > 255) rg = 255;
    if (rb < 0) rb = 0;
    if (rb > 255) rb = 255;
    return rgb((uint8_t)rr, (uint8_t)rg, (uint8_t)rb);
}

static void draw_gradient_rect(int x, int y, int w, int h,
                               uint32_t top, uint32_t bot)
{
    for (int row = 0; row < h; row++) {
        uint32_t c = lerp_color(top, bot, row, h);
        for (int col = 0; col < w; col++) {
            fb_putpixel(x + col, y + row, c);
        }
    }
}

/* ── Animation helpers ─────────────────────────────────────────────────── */
#define ANIM_OPEN_MS     200
#define ANIM_CLOSE_MS    150
#define ANIM_MIN_MS      250
#define ANIM_MAX_MS      200
#define START_MENU_ANIM_MS 150

static int      start_menu_anim = 0;
static uint64_t start_menu_anim_start = 0;

/* Ease-out cubic: 1 - (1-t)^3, returns 0..1000 */
static int ease_out_cubic(uint64_t elapsed_ms, int duration_ms)
{
    if (duration_ms <= 0 || elapsed_ms >= (uint64_t)duration_ms) return 1000;
    if (elapsed_ms == 0) return 0;
    int t = (int)(elapsed_ms * 1000 / (uint64_t)duration_ms);
    int inv = 1000 - t;
    int64_t inv3 = (int64_t)inv * inv * inv;
    return (int)(1000 - inv3 / 1000000);
}

static int anim_lerp(int from, int to, int p1000)
{
    return from + (to - from) * p1000 / 1000;
}

static int get_anim_duration(int anim_type)
{
    switch (anim_type) {
    case ANIM_OPEN:        return ANIM_OPEN_MS;
    case ANIM_CLOSE:       return ANIM_CLOSE_MS;
    case ANIM_MINIMIZE:    return ANIM_MIN_MS;
    case ANIM_UNMINIMIZE:  return ANIM_MIN_MS;
    case ANIM_MAXIMIZE:    return ANIM_MAX_MS;
    case ANIM_RESTORE:     return ANIM_MAX_MS;
    default:               return 200;
    }
}

/* ── Rounded rectangle helper ─────────────────────────────────────────── */
/* Returns 1 if (col,row) is inside a rounded rect of size w x h with radius r */
static int inside_rounded_rect(int col, int row, int w, int h, int r)
{
    /* Check corners */
    if (col < r && row < r) {
        int dx = r - col - 1, dy = r - row - 1;
        return (dx * dx + dy * dy) <= (r * r);
    }
    if (col >= w - r && row < r) {
        int dx = col - (w - r), dy = r - row - 1;
        return (dx * dx + dy * dy) <= (r * r);
    }
    if (col < r && row >= h - r) {
        int dx = r - col - 1, dy = row - (h - r);
        return (dx * dx + dy * dy) <= (r * r);
    }
    if (col >= w - r && row >= h - r) {
        int dx = col - (w - r), dy = row - (h - r);
        return (dx * dx + dy * dy) <= (r * r);
    }
    return 1;
}

static void draw_rounded_gradient_rect(int x, int y, int w, int h, int r,
                                       uint32_t top, uint32_t bot)
{
    for (int row = 0; row < h; row++) {
        uint32_t c = lerp_color(top, bot, row, h);
        for (int col = 0; col < w; col++) {
            if (inside_rounded_rect(col, row, w, h, r))
                fb_putpixel(x + col, y + row, c);
        }
    }
}

static void draw_rounded_rect_outline(int x, int y, int w, int h, int r, uint32_t color)
{
    /* Top edge (between corners) */
    for (int col = r; col < w - r; col++) fb_putpixel(x + col, y, color);
    /* Bottom edge */
    for (int col = r; col < w - r; col++) fb_putpixel(x + col, y + h - 1, color);
    /* Left edge */
    for (int row = r; row < h - r; row++) fb_putpixel(x, y + row, color);
    /* Right edge */
    for (int row = r; row < h - r; row++) fb_putpixel(x + w - 1, y + row, color);
    /* Corner arcs */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int d2 = (r - dx - 1) * (r - dx - 1) + (r - dy - 1) * (r - dy - 1);
            if (d2 >= (r - 1) * (r - 1) && d2 <= r * r) {
                fb_putpixel(x + dx, y + dy, color);             /* TL */
                fb_putpixel(x + w - 1 - dx, y + dy, color);    /* TR */
                fb_putpixel(x + dx, y + h - 1 - dy, color);    /* BL */
                fb_putpixel(x + w - 1 - dx, y + h - 1 - dy, color); /* BR */
            }
        }
    }
}

/* ── Drop shadow ──────────────────────────────────────────────────────── */
static void draw_shadow(int x, int y, int w, int h, uint32_t color)
{
    int offset = 4;
    for (int i = 0; i < offset; i++) {
        uint8_t alpha = 80 - i * 20;
        uint32_t sc = rgba_blend(0x000000, color, alpha);
        /* Bottom edge */
        for (int col = x + i; col < x + w + offset; col++)
            fb_putpixel(col, y + h + i, sc);
        /* Right edge */
        for (int row = y + i; row < y + h + offset; row++)
            fb_putpixel(x + w + i, row, sc);
    }
}

/* ── Bevel effect ─────────────────────────────────────────────────────── */
static void draw_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t light = raised ? 0xFFFFFF : 0x404040;
    uint32_t dark  = raised ? 0x404040 : 0xFFFFFF;

    /* Top & left highlight */
    for (int i = x; i < x + w; i++) {
        fb_putpixel(i, y, rgba_blend(fb_getpixel(i, y), light, 100));
    }
    for (int i = y; i < y + h; i++) {
        fb_putpixel(x, i, rgba_blend(fb_getpixel(x, i), light, 100));
    }
    /* Bottom & right shadow */
    for (int i = x; i < x + w; i++) {
        fb_putpixel(i, y + h - 1, rgba_blend(fb_getpixel(i, y + h - 1), dark, 100));
    }
    for (int i = y; i < y + h; i++) {
        fb_putpixel(x + w - 1, i, rgba_blend(fb_getpixel(x + w - 1, i), dark, 100));
    }
}

/* ── Glossy highlight (top 40% bright, bottom 60% darker) ─────────── */
static void draw_gloss(int x, int y, int w, int h)
{
    int gloss_h = h * 2 / 5;
    for (int row = 0; row < gloss_h; row++) {
        uint8_t alpha = 60 - (uint8_t)(row * 60 / gloss_h);
        for (int col = 0; col < w; col++) {
            uint32_t px = fb_getpixel(x + col, y + row);
            fb_putpixel(x + col, y + row, rgba_blend(px, 0xFFFFFF, alpha));
        }
    }
}

/* ── String helpers ───────────────────────────────────────────────────── */
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_cpy(char *d, const char *s)
{
    int i = 0;
    while (s[i] && i < WIN_TITLE_LEN - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ── Draw a single window ─────────────────────────────────────────────── */
#define TITLEBAR_H 28
#define BORDER_W   2
#define WIN_CORNER_R 6  /* Rounded corner radius for windows */

static void draw_titlebar_circle(int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r)
                fb_putpixel(cx + dx, cy + dy, color);
        }
    }
}

static void draw_window(window_t *win)
{
    if (win->minimized && win->anim_type != ANIM_UNMINIMIZE &&
        win->anim_type != ANIM_MINIMIZE) return;

    const theme_colors_t *tc = &themes[current_theme];
    int x = win->x, y = win->y, w = win->width, h = win->height;
    uint8_t anim_alpha = 255;

    if (win->anim_type != ANIM_NONE) {
        uint64_t elapsed = timer_get_ticks() - win->anim_start;
        int duration = get_anim_duration(win->anim_type);
        int p = ease_out_cubic(elapsed, duration);
        switch (win->anim_type) {
        case ANIM_OPEN:
            y = win->y + 15 - 15 * p / 1000;
            anim_alpha = (uint8_t)(p * 255 / 1000);
            break;
        case ANIM_CLOSE:
            y = win->y + 10 * p / 1000;
            anim_alpha = (uint8_t)(255 - p * 255 / 1000);
            if ((int)elapsed >= duration) return;
            break;
        case ANIM_MINIMIZE:
            x = anim_lerp(win->anim_from_x, win->anim_to_x, p);
            y = anim_lerp(win->anim_from_y, win->anim_to_y, p);
            w = anim_lerp(win->anim_from_w, win->anim_to_w, p);
            h = anim_lerp(win->anim_from_h, win->anim_to_h, p);
            if (w < 4) w = 4;
            if (h < 4) h = 4;
            anim_alpha = (uint8_t)(255 - p * 255 / 1000);
            if ((int)elapsed >= duration) return;
            break;
        case ANIM_UNMINIMIZE:
            x = anim_lerp(win->anim_from_x, win->anim_to_x, p);
            y = anim_lerp(win->anim_from_y, win->anim_to_y, p);
            w = anim_lerp(win->anim_from_w, win->anim_to_w, p);
            h = anim_lerp(win->anim_from_h, win->anim_to_h, p);
            if (w < 4) w = 4;
            if (h < 4) h = 4;
            anim_alpha = (uint8_t)(p * 255 / 1000);
            break;
        case ANIM_MAXIMIZE:
            x = anim_lerp(win->anim_from_x, win->anim_to_x, p);
            y = anim_lerp(win->anim_from_y, win->anim_to_y, p);
            w = anim_lerp(win->anim_from_w, win->anim_to_w, p);
            h = anim_lerp(win->anim_from_h, win->anim_to_h, p);
            if (w < 4) w = 4;
            if (h < 4) h = 4;
            break;
        case ANIM_RESTORE:
            x = anim_lerp(win->anim_from_x, win->anim_to_x, p);
            y = anim_lerp(win->anim_from_y, win->anim_to_y, p);
            w = anim_lerp(win->anim_from_w, win->anim_to_w, p);
            h = anim_lerp(win->anim_from_h, win->anim_to_h, p);
            if (w < 4) w = 4;
            if (h < 4) h = 4;
            break;
        }
    }

    int total_h = h + TITLEBAR_H;
    framebuffer_t *fb = fb_get();
    int fw = (int)fb->width, fh = (int)fb->height;

    /* Save background behind the window region for proper alpha blending */
    int save_x0 = x - 1, save_y0 = y - 1;
    int save_w = w + 10, save_h = total_h + 10;
    if (save_x0 < 0) save_x0 = 0;
    if (save_y0 < 0) save_y0 = 0;
    if (save_x0 + save_w > fw) save_w = fw - save_x0;
    if (save_y0 + save_h > fh) save_h = fh - save_y0;

    uint32_t *bg_save = (void *)0;
    if (anim_alpha < 255 && save_w > 0 && save_h > 0) {
        bg_save = (uint32_t *)kmalloc(save_w * save_h * 4);
        if (bg_save) {
            for (int r = 0; r < save_h; r++) {
                for (int c = 0; c < save_w; c++) {
                    bg_save[r * save_w + c] = fb_getpixel(save_x0 + c, save_y0 + r);
                }
            }
        }
    }

    /* Drop shadow */
    draw_shadow(x, y, w, total_h, tc->shadow);

    /* Title bar with rounded top corners */
    draw_rounded_gradient_rect(x, y, w, TITLEBAR_H, WIN_CORNER_R,
                               tc->titlebar_top, tc->titlebar_bot);
    draw_gloss(x, y, w, TITLEBAR_H);

    /* Title text (centred) */
    int text_w = str_len(win->title) * 8;
    int tx = x + (w - text_w) / 2;
    int ty = y + (TITLEBAR_H - 16) / 2;
    fb_draw_string(tx, ty, win->title, tc->titlebar_text, 0x00000000);

    /* Title bar buttons (right side): Close / Maximize / Minimize */
    int btn_y = y + TITLEBAR_H / 2;

    /* Close button (red circle with X) */
    int close_cx = x + w - 16;
    draw_titlebar_circle(close_cx, btn_y, 7, tc->close_btn);
    for (int i = -3; i <= 3; i++) {
        fb_putpixel(close_cx + i, btn_y + i, 0xFFFFFF);
        fb_putpixel(close_cx + i, btn_y - i, 0xFFFFFF);
    }

    /* Maximize button (green circle with square) */
    int max_cx = x + w - 36;
    draw_titlebar_circle(max_cx, btn_y, 7, 0x44AA44);
    fb_draw_rect(max_cx - 3, btn_y - 3, 7, 7, 0xFFFFFF);

    /* Minimize button (yellow circle with dash) */
    int min_cx = x + w - 56;
    draw_titlebar_circle(min_cx, btn_y, 7, 0xDDAA22);
    for (int i = -3; i <= 3; i++)
        fb_putpixel(min_cx + i, btn_y, 0xFFFFFF);

    /* Client area */
    int cy = y + TITLEBAR_H;
    fb_fill_rect(x, cy, w, h, 0xF0F0F0);

    /* Rounded border for whole window */
    draw_rounded_rect_outline(x - 1, y - 1, w + 2, total_h + 2,
                              WIN_CORNER_R, tc->border);

    /* Blit client canvas onto the window's client area */
    if (win->canvas) {
        int cw = win->width - BORDER_W * 2;
        int ch = win->height - BORDER_W * 2;
        int draw_cw = w - BORDER_W * 2;
        int draw_ch = h - BORDER_W * 2;
        if (draw_cw > 0 && draw_ch > 0) {
            for (int row = 0; row < draw_ch; row++) {
                int src_row = row * ch / draw_ch;
                if (src_row >= ch) src_row = ch - 1;
                for (int col = 0; col < draw_cw; col++) {
                    int src_col = col * cw / draw_cw;
                    if (src_col >= cw) src_col = cw - 1;
                    fb_putpixel(x + BORDER_W + col, cy + BORDER_W + row,
                                win->canvas[src_row * cw + src_col]);
                }
            }
        }
    }

    /* Unfocused window dimming overlay (entire window) */
    if (!win->focused && win->anim_type != ANIM_CLOSE) {
        for (int row = 0; row < total_h; row++) {
            for (int col = 0; col < w; col++) {
                int px = x + col, py = y + row;
                uint32_t orig = fb_getpixel(px, py);
                fb_putpixel(px, py, rgba_blend(orig, 0x000000, 60));
            }
        }
    }

    /* Animation alpha blend — blend drawn window with saved background */
    if (anim_alpha < 255 && bg_save) {
        uint8_t inv = 255 - anim_alpha;
        for (int r = 0; r < save_h; r++) {
            for (int c = 0; c < save_w; c++) {
                int px = save_x0 + c, py = save_y0 + r;
                uint32_t bg_px = bg_save[r * save_w + c];
                uint32_t win_px = fb_getpixel(px, py);
                fb_putpixel(px, py, rgba_blend(win_px, bg_px, inv));
            }
        }
        kfree(bg_save);
    } else if (anim_alpha < 255) {
        /* Fallback if allocation failed — blend toward black */
        for (int row = -1; row < total_h + 5; row++) {
            for (int col = -1; col < w + 5; col++) {
                int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= fw || py >= fh) continue;
                uint32_t cur = fb_getpixel(px, py);
                fb_putpixel(px, py, rgba_blend(cur, 0x000000, 255 - anim_alpha));
            }
        }
    }
}

/* ── Desktop wallpaper (gradient + subtle texture) ────────────────────── */
void desktop_draw_wallpaper(void)
{
    const theme_colors_t *tc = &themes[current_theme];
    framebuffer_t *f = fb_get();

    /* Rebuild cache if theme changed or first render */
    if (wallpaper_dirty || wallpaper_theme != current_theme ||
        wallpaper_w != f->width || wallpaper_h != f->height) {
        if (wallpaper_cache) {
            kfree(wallpaper_cache);
            wallpaper_cache = (void *)0;
            wallpaper_w = 0;
            wallpaper_h = 0;
        }
        wallpaper_cache = (uint32_t *)kmalloc(f->width * f->height * 4);
        if (wallpaper_cache) {
            wallpaper_w = f->width;
            wallpaper_h = f->height;
            for (uint32_t y = 0; y < wallpaper_h; y++) {
                uint32_t c = lerp_color(tc->desktop_top, tc->desktop_bot, (int)y, (int)wallpaper_h);
                for (uint32_t x = 0; x < wallpaper_w; x++) {
                    wallpaper_cache[y * wallpaper_w + x] = c;
                }
            }
            /* Subtle diagonal texture lines */
            for (uint32_t y = 0; y < wallpaper_h; y += 6) {
                for (uint32_t x = 0; x < wallpaper_w; x++) {
                    if ((x + y) % 12 == 0) {
                        uint32_t px = wallpaper_cache[y * wallpaper_w + x];
                        wallpaper_cache[y * wallpaper_w + x] = rgba_blend(px, 0xFFFFFF, 8);
                    }
                }
            }
        }
        wallpaper_theme = current_theme;
        wallpaper_dirty = 0;
    }

    /* Fast blit cached wallpaper */
    if (wallpaper_cache) {
        fb_blit(0, 0, (int)wallpaper_w, (int)wallpaper_h, wallpaper_cache);
    } else {
        /* Fallback: direct draw */
        draw_gradient_rect(0, 0, f->width, f->height, tc->desktop_top, tc->desktop_bot);
    }
}

/* ── Desktop icons ─────────────────────────────────────────────────────── */
/* Icon grid: top-left of desktop, 80x80 per icon slot with 16px padding */
#define ICON_W       64
#define ICON_H       64
#define ICON_PAD     16
#define ICON_SLOT    (ICON_W + ICON_PAD)
#define ICON_START_X 24
#define ICON_START_Y 24
#define ICON_LABEL_GAP 32  /* vertical gap between icon slots (includes label) */

typedef struct {
    const char *label;
    uint32_t    color;    /* Primary icon colour */
} desktop_icon_t;

static const desktop_icon_t desktop_icons[] = {
    { "Settings",  0x7090B0 },
    { "Files",     0xD4A840 },
    { "Notepad",   0xE8D860 },
    { "Browser",   0x3080C0 },
};
#define DESKTOP_ICON_COUNT 4

static void draw_desktop_icon(int x, int y, const desktop_icon_t *icon, int selected)
{
    /* Selection highlight */
    if (selected) {
        fb_fill_rect(x - 4, y - 4, ICON_W + 8, ICON_H + 24,
                     rgba_blend(0x4080C0, 0x000000, 80));
    }

    /* Icon shadow (rounded) */
    for (int r = 0; r < ICON_H; r++) {
        for (int col = 0; col < ICON_W; col++) {
            if (inside_rounded_rect(col, r, ICON_W, ICON_H, 8))
                fb_putpixel(x + col + 3, y + r + 3, rgba_blend(0x000000, 0x000000, 60));
        }
    }

    /* Icon body (rounded gradient) */
    #define ICON_R 8
    for (int r = 0; r < ICON_H; r++) {
        uint32_t c = lerp_color(icon->color, rgba_blend(icon->color, 0x000000, 80), r, ICON_H);
        for (int col = 0; col < ICON_W; col++) {
            if (inside_rounded_rect(col, r, ICON_W, ICON_H, ICON_R))
                fb_putpixel(x + col, y + r, c);
        }
    }
    /* Gloss highlight on top half */
    for (int r = 0; r < ICON_H / 2; r++) {
        uint8_t alpha = 70 - (uint8_t)(r * 70 / (ICON_H / 2));
        for (int col = 0; col < ICON_W; col++) {
            if (inside_rounded_rect(col, r, ICON_W, ICON_H, ICON_R)) {
                uint32_t px = fb_getpixel(x + col, y + r);
                fb_putpixel(x + col, y + r, rgba_blend(px, 0xFFFFFF, alpha));
            }
        }
    }
    /* Rounded border */
    draw_rounded_rect_outline(x, y, ICON_W, ICON_H, ICON_R, 0x303030);
    #undef ICON_R

    /* Draw icon-specific pixel art glyph */
    int cx = x + ICON_W / 2;
    int cy = y + ICON_H / 2;
    int idx = 0;
    while (idx < DESKTOP_ICON_COUNT && &desktop_icons[idx] != icon) idx++;
    if (idx >= DESKTOP_ICON_COUNT) return;

    if (idx == 0) {
        /* Settings: gear/cog icon */
        for (int dy = -20; dy <= 20; dy++) {
            for (int dx = -20; dx <= 20; dx++) {
                int r2 = dx * dx + dy * dy;
                int is_body = (r2 >= 8 * 8 && r2 <= 16 * 16);
                int is_hole = (r2 <= 5 * 5);
                /* Teeth: 8 rectangular protrusions */
                int is_tooth = 0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (r2 >= 14 * 14 && r2 <= 22 * 22) {
                    /* Axis-aligned teeth */
                    if ((adx <= 4 && ady <= 22) || (ady <= 4 && adx <= 22))
                        is_tooth = 1;
                    /* Diagonal teeth */
                    int s = adx + ady;
                    int d = adx - ady; if (d < 0) d = -d;
                    if (s <= 30 && s >= 18 && d <= 6) is_tooth = 1;
                }
                if ((is_body || is_tooth) && !is_hole) {
                    uint32_t gc = (r2 < 12 * 12) ? 0xD0D0D8 : 0xA0A0A8;
                    fb_putpixel(cx + dx, cy + dy, gc);
                }
            }
        }
    } else if (idx == 1) {
        /* Files: manila folder icon */
        int fx = x + 10, fy = y + 14;
        int fw = 44, fh = 34;
        /* Folder tab */
        fb_fill_rect(fx, fy, 18, 6, 0xE8C850);
        fb_draw_rect(fx, fy, 18, 6, 0xC8A830);
        /* Folder body */
        fb_fill_rect(fx, fy + 6, fw, fh, 0xE8C850);
        /* Fold shadow */
        fb_fill_rect(fx, fy + 6, fw, 4, 0xC8A830);
        /* Border */
        fb_draw_rect(fx, fy + 6, fw, fh, 0x987020);
        /* Inner highlight */
        for (int col = fx + 1; col < fx + fw - 1; col++)
            fb_putpixel(col, fy + 11, 0xF0D870);
    } else if (idx == 2) {
        /* Notepad: paper with lines icon */
        int px = x + 14, py = y + 8;
        int pw = 36, ph = 48;
        /* Paper body */
        fb_fill_rect(px, py, pw, ph, 0xFFF8C8);
        /* Dog-ear */
        for (int r = 0; r < 10; r++) {
            for (int c = 0; c < 10 - r; c++) {
                fb_putpixel(px + pw - 10 + c, py + r, 0xE0D8A0);
            }
        }
        /* Ruled lines */
        for (int ly = py + 10; ly < py + ph - 4; ly += 7) {
            for (int lx = px + 2; lx < px + pw - 2; lx++)
                fb_putpixel(lx, ly, 0xB0C0D8);
        }
        /* Red margin line */
        for (int ly = py + 4; ly < py + ph - 2; ly++) {
            fb_putpixel(px + 8, ly, 0xE05050);
        }
        /* Border */
        fb_draw_rect(px, py, pw, ph, 0x808080);
    } else if (idx == 3) {
        /* Browser: Earth globe with cursor */
        int gr = 20;  /* globe radius */
        /* Draw globe sphere */
        for (int dy = -gr; dy <= gr; dy++) {
            for (int dx = -gr; dx <= gr; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= gr * gr) {
                    /* Base ocean color with shading */
                    int shade = 255 - (d2 * 80 / (gr * gr));
                    uint32_t ocean = rgb(
                        (uint8_t)(30 * shade / 255),
                        (uint8_t)(100 * shade / 255),
                        (uint8_t)(200 * shade / 255));
                    /* Continent-like patches using simple pattern */
                    int land = 0;
                    /* Americas-like landmass */
                    if (dx >= -8 && dx <= 0 && dy >= -14 && dy <= -4) land = 1;
                    if (dx >= -6 && dx <= 2 && dy >= -4 && dy <= 8) land = 1;
                    /* Europe/Africa-like */
                    if (dx >= 6 && dx <= 14 && dy >= -12 && dy <= -2) land = 1;
                    if (dx >= 8 && dx <= 16 && dy >= -2 && dy <= 10) land = 1;
                    /* Clip to circle */
                    if (d2 > (gr - 2) * (gr - 2)) land = 0;
                    if (land) {
                        ocean = rgb(
                            (uint8_t)(50 * shade / 255),
                            (uint8_t)(160 * shade / 255),
                            (uint8_t)(60 * shade / 255));
                    }
                    fb_putpixel(cx + dx, cy + dy - 2, ocean);
                }
            }
        }
        /* Globe highlight (gloss) */
        for (int dy = -gr; dy <= 0; dy++) {
            for (int dx = -gr / 2; dx <= gr / 2; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= (gr - 4) * (gr - 4) && dy < -gr / 3) {
                    uint32_t px2 = fb_getpixel(cx + dx, cy + dy - 2);
                    fb_putpixel(cx + dx, cy + dy - 2, rgba_blend(px2, 0xFFFFFF, 40));
                }
            }
        }
        /* Globe outline */
        for (int dy = -gr - 1; dy <= gr + 1; dy++) {
            for (int dx = -gr - 1; dx <= gr + 1; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= gr * gr && d2 <= (gr + 1) * (gr + 1))
                    fb_putpixel(cx + dx, cy + dy - 2, 0x1A4080);
            }
        }
        /* Small cursor arrow at bottom-right of globe */
        int ax2 = cx + 12, ay2 = cy + 10;
        /* Cursor shape (small white arrow with black outline) */
        for (int cr = 0; cr < 12; cr++) {
            fb_putpixel(ax2, ay2 + cr, 0x000000);      /* left edge */
        }
        for (int cr = 0; cr < 8; cr++) {
            fb_putpixel(ax2 + 1, ay2 + 1 + cr, 0xFFFFFF);
        }
        for (int cr = 0; cr < 6; cr++) {
            fb_putpixel(ax2 + 2, ay2 + 2 + cr, 0xFFFFFF);
        }
        for (int cr = 0; cr < 4; cr++) {
            fb_putpixel(ax2 + 3, ay2 + 3 + cr, 0xFFFFFF);
        }
        fb_putpixel(ax2 + 4, ay2 + 4, 0xFFFFFF);
        fb_putpixel(ax2 + 4, ay2 + 5, 0xFFFFFF);
        fb_putpixel(ax2 + 5, ay2 + 5, 0x000000);
    }

    /* Label below icon */
    int label_len = str_len(icon->label);
    int label_x = x + (ICON_W - label_len * 8) / 2;
    int label_y = y + ICON_H + 4;
    /* Label shadow */
    fb_draw_string(label_x + 1, label_y + 1, icon->label, 0x000000, 0x00000000);
    fb_draw_string(label_x, label_y, icon->label, 0xFFFFFF, 0x00000000);
}

/* ── Double-click and icon selection state ─────────────────────────────── */
static int    selected_icon = -1;
static uint64_t last_icon_click_tick = 0;
static int    last_icon_click_idx = -1;
#define DBLCLICK_MS 500

static void draw_desktop_icons(void)
{
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int x = ICON_START_X;
        int y = ICON_START_Y + i * (ICON_H + ICON_LABEL_GAP);
        draw_desktop_icon(x, y, &desktop_icons[i], i == selected_icon);
    }
}

/* ── Taskbar ──────────────────────────────────────────────────────────── */
#define TASKBAR_H 40

/* ── Start menu ──────────────────────────────────────────────────────── */
#define START_MENU_W 180
#define START_MENU_ITEM_H 32
#define START_MENU_ITEMS 7

static const char *start_menu_labels[START_MENU_ITEMS] = {
    "Settings", "File Explorer", "Notepad", "Browser",
    "About nextOS", "Restart", "Shutdown"
};

static void draw_start_menu(void)
{
    if (!start_menu_open) return;
    framebuffer_t *f = fb_get();
    const theme_colors_t *tc = &themes[current_theme];

    /* Extra space for separator between item 2 and item 3 */
    int separator_h = 6;
    int mh = START_MENU_ITEMS * START_MENU_ITEM_H + 8 + separator_h;
    int mx = 4;
    int my_base = (int)f->height - TASKBAR_H - mh;

    /* Animation offset and alpha */
    int y_offset = 0;
    uint8_t sm_alpha = 255;

    if (start_menu_anim == 1) {
        uint64_t elapsed = timer_get_ticks() - start_menu_anim_start;
        int p = ease_out_cubic(elapsed, START_MENU_ANIM_MS);
        y_offset = 20 - 20 * p / 1000;
        sm_alpha = (uint8_t)(p * 255 / 1000);
        if ((int)elapsed >= START_MENU_ANIM_MS)
            start_menu_anim = 0;
    } else if (start_menu_anim == 2) {
        uint64_t elapsed = timer_get_ticks() - start_menu_anim_start;
        int p = ease_out_cubic(elapsed, START_MENU_ANIM_MS);
        y_offset = 20 * p / 1000;
        sm_alpha = (uint8_t)(255 - p * 255 / 1000);
        if ((int)elapsed >= START_MENU_ANIM_MS) {
            start_menu_open = 0;
            start_menu_anim = 0;
            return;
        }
    }

    int my = my_base + y_offset;

    /* Save background behind menu for proper alpha blending */
    framebuffer_t *fb_ptr = fb_get();
    int fw = (int)fb_ptr->width, fh = (int)fb_ptr->height;
    int sm_save_x0 = mx - 1, sm_save_y0 = my;
    int sm_save_w = START_MENU_W + 6, sm_save_h = mh + 5;
    if (sm_save_x0 < 0) sm_save_x0 = 0;
    if (sm_save_y0 < 0) sm_save_y0 = 0;
    if (sm_save_x0 + sm_save_w > fw) sm_save_w = fw - sm_save_x0;
    if (sm_save_y0 + sm_save_h > fh) sm_save_h = fh - sm_save_y0;

    uint32_t *sm_bg_save = (void *)0;
    if (sm_alpha < 255 && sm_save_w > 0 && sm_save_h > 0) {
        sm_bg_save = (uint32_t *)kmalloc(sm_save_w * sm_save_h * 4);
        if (sm_bg_save) {
            for (int r = 0; r < sm_save_h; r++) {
                for (int c = 0; c < sm_save_w; c++) {
                    sm_bg_save[r * sm_save_w + c] = fb_getpixel(sm_save_x0 + c, sm_save_y0 + r);
                }
            }
        }
    }

    /* Menu shadow */
    fb_fill_rect(mx + 4, my + 4, START_MENU_W, mh, 0x202020);
    /* Menu background */
    draw_gradient_rect(mx, my, START_MENU_W, mh, tc->button_top, tc->button_bot);
    draw_bevel(mx, my, START_MENU_W, mh, 1);

    /* Items */
    for (int i = 0; i < START_MENU_ITEMS; i++) {
        int iy = my + 4 + i * START_MENU_ITEM_H;
        if (i >= 4) iy += separator_h;  /* offset items after separator */
        fb_draw_string(mx + 12, iy + 8, start_menu_labels[i], tc->taskbar_text, 0x00000000);
        /* Separator line between items (but not after last) */
        if (i < START_MENU_ITEMS - 1 && i != 3) {
            for (int sx = mx + 4; sx < mx + START_MENU_W - 4; sx++)
                fb_putpixel(sx, iy + START_MENU_ITEM_H - 1,
                            rgba_blend(fb_getpixel(sx, iy + START_MENU_ITEM_H - 1), 0x000000, 30));
        }
    }

    /* Draw thicker separator line between Browser and About */
    {
        int sep_y = my + 4 + 4 * START_MENU_ITEM_H;
        for (int row = 0; row < separator_h; row++) {
            for (int sx = mx + 4; sx < mx + START_MENU_W - 4; sx++) {
                if (row == separator_h / 2)
                    fb_putpixel(sx, sep_y + row,
                                rgba_blend(fb_getpixel(sx, sep_y + row), 0x000000, 60));
            }
        }
    }

    /* Fade overlay for animation — blend with saved background */
    if (sm_alpha < 255 && sm_bg_save) {
        uint8_t inv = 255 - sm_alpha;
        for (int r = 0; r < sm_save_h; r++) {
            for (int c = 0; c < sm_save_w; c++) {
                int px = sm_save_x0 + c, py = sm_save_y0 + r;
                uint32_t bg_px = sm_bg_save[r * sm_save_w + c];
                uint32_t win_px = fb_getpixel(px, py);
                fb_putpixel(px, py, rgba_blend(win_px, bg_px, inv));
            }
        }
        kfree(sm_bg_save);
    } else if (sm_alpha < 255) {
        /* Fallback */
        for (int row = 0; row < mh + 5; row++) {
            for (int col = -1; col < START_MENU_W + 5; col++) {
                int px = mx + col, py = my + row;
                if (px < 0 || py < 0 || px >= fw || py >= fh) continue;
                uint32_t cur = fb_getpixel(px, py);
                fb_putpixel(px, py, rgba_blend(cur, 0x000000, 255 - sm_alpha));
            }
        }
    }
}

void desktop_draw_taskbar(void)
{
    const theme_colors_t *tc = &themes[current_theme];
    framebuffer_t *f = fb_get();
    int y = f->height - TASKBAR_H;

    draw_gradient_rect(0, y, f->width, TASKBAR_H, tc->taskbar_top, tc->taskbar_bot);
    draw_gloss(0, y, f->width, TASKBAR_H);
    draw_bevel(0, y, f->width, TASKBAR_H, 1);

    /* "nextOS" start button with logo */
    draw_gradient_rect(4, y + 4, 90, TASKBAR_H - 8,
                       tc->button_top, tc->button_bot);
    draw_bevel(4, y + 4, 90, TASKBAR_H - 8, 1);
    draw_gloss(4, y + 4, 90, TASKBAR_H - 8);
    /* Tiny "N" logo (12x12 pixel art) */
    {
        int lx = 10, ly = y + 10;
        uint32_t lc = 0x3060B0;
        /* Left vertical */
        for (int r = 0; r < 12; r++) fb_putpixel(lx, ly + r, lc);
        for (int r = 0; r < 12; r++) fb_putpixel(lx + 1, ly + r, lc);
        /* Right vertical */
        for (int r = 0; r < 12; r++) fb_putpixel(lx + 9, ly + r, lc);
        for (int r = 0; r < 12; r++) fb_putpixel(lx + 10, ly + r, lc);
        /* Diagonal */
        for (int r = 0; r < 12; r++) {
            int c = r * 8 / 11;
            fb_putpixel(lx + 2 + c, ly + r, lc);
            fb_putpixel(lx + 3 + c, ly + r, lc);
        }
    }
    fb_draw_string(26, y + 12, "nextOS", tc->taskbar_text, 0x00000000);

    /* Window buttons on taskbar */
    int bx = 110;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) continue;
        draw_gradient_rect(bx, y + 4, 120, TASKBAR_H - 8,
                           tc->button_top, tc->button_bot);
        draw_bevel(bx, y + 4, 120, TASKBAR_H - 8,
                   windows[i].focused ? 0 : 1);
        fb_draw_string(bx + 8, y + 12, windows[i].title, tc->taskbar_text, 0x00000000);
        bx += 128;
    }
}

/* ── Mouse cursor (skeuomorphic arrow with shadow) ────────────────────── */
/* 16x20 bitmap arrow cursor — 1=black outline, 2=white fill, 0=transparent */
static const uint8_t cursor_bitmap[20][16] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,1,1,1,0,0,0,0,0},
    {1,2,2,2,1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0,0,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

void compositor_draw_cursor(int mx, int my)
{
    /* Shadow (offset +2,+2) */
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 16; c++) {
            if (cursor_bitmap[r][c])
                fb_putpixel(mx + c + 2, my + r + 2,
                            rgba_blend(fb_getpixel(mx + c + 2, my + r + 2), 0x000000, 60));
        }
    }
    /* Arrow body */
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 16; c++) {
            uint8_t v = cursor_bitmap[r][c];
            if (v == 1)
                fb_putpixel(mx + c, my + r, 0x000000);
            else if (v == 2)
                fb_putpixel(mx + c, my + r, 0xFFFFFF);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void compositor_init(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
    }
    window_count = 0;
}

void compositor_set_theme(theme_t theme)
{
    if (theme < THEME_COUNT) {
        current_theme = theme;
        wallpaper_dirty = 1;
    }
}

theme_t compositor_get_theme(void)
{
    return current_theme;
}

window_t *compositor_create_window(const char *title, int x, int y, int w, int h)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) {
            windows[i].active    = 1;
            windows[i].x        = x;
            windows[i].y        = y;
            windows[i].width    = w;
            windows[i].height   = h;
            windows[i].focused  = 1;
            windows[i].dragging = 0;
            windows[i].minimized = 0;
            windows[i].maximized = 0;
            windows[i].orig_x = x;
            windows[i].orig_y = y;
            windows[i].orig_w = w;
            windows[i].orig_h = h;
            windows[i].close_hover = 0;
            windows[i].anim_type = ANIM_OPEN;
            windows[i].anim_start = timer_get_ticks();
            windows[i].on_paint = (void *)0;
            windows[i].on_key   = (void *)0;
            windows[i].on_mouse = (void *)0;
            windows[i].on_close = (void *)0;
            str_cpy(windows[i].title, title);

            int cw = w - BORDER_W * 2;
            int ch = h - BORDER_W * 2;
            windows[i].canvas = (uint32_t *)kmalloc(cw * ch * 4);
            if (windows[i].canvas) {
                for (int p = 0; p < cw * ch; p++)
                    windows[i].canvas[p] = 0xF0F0F0;
            }

            /* Unfocus all others */
            for (int j = 0; j < MAX_WINDOWS; j++) {
                if (j != i) windows[j].focused = 0;
            }

            window_count++;
            return &windows[i];
        }
    }
    return (void *)0;
}

void compositor_destroy_window(window_t *win)
{
    if (!win || !win->active) return;
    if (win->anim_type == ANIM_CLOSE) return;
    if (win->on_close) win->on_close(win);
    win->on_paint = (void *)0;
    win->on_key   = (void *)0;
    win->on_mouse = (void *)0;
    win->on_close = (void *)0;
    win->focused  = 0;
    win->anim_type  = ANIM_CLOSE;
    win->anim_start = timer_get_ticks();
}

void compositor_render_frame(void)
{
    /* Finalize completed animations */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active || windows[i].anim_type == ANIM_NONE) continue;
        uint64_t elapsed = timer_get_ticks() - windows[i].anim_start;
        int duration = get_anim_duration(windows[i].anim_type);
        if ((int)elapsed < duration) continue;
        switch (windows[i].anim_type) {
        case ANIM_OPEN:
            windows[i].anim_type = ANIM_NONE;
            break;
        case ANIM_CLOSE:
            if (windows[i].canvas) kfree(windows[i].canvas);
            windows[i].canvas = (void *)0;
            windows[i].active = 0;
            window_count--;
            break;
        case ANIM_MINIMIZE:
            windows[i].minimized = 1;
            windows[i].anim_type = ANIM_NONE;
            break;
        case ANIM_UNMINIMIZE:
            windows[i].anim_type = ANIM_NONE;
            break;
        case ANIM_MAXIMIZE:
        case ANIM_RESTORE:
            windows[i].anim_type = ANIM_NONE;
            break;
        }
    }

    /* Draw desktop */
    desktop_draw_wallpaper();

    /* Draw desktop icons */
    draw_desktop_icons();

    /* Draw windows (back to front) */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && !windows[i].focused) {
            if (windows[i].on_paint) windows[i].on_paint(&windows[i]);
            draw_window(&windows[i]);
        }
    }
    /* Focused window on top */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].focused) {
            if (windows[i].on_paint) windows[i].on_paint(&windows[i]);
            draw_window(&windows[i]);
        }
    }

    /* Taskbar on top of everything */
    desktop_draw_taskbar();

    /* Start menu above taskbar */
    draw_start_menu();
}

/* ── Helper: find a window under (mx, my) — focused first ─────────────── */
static window_t *window_at(int mx, int my)
{
    /* Check focused window first (it's visually on top) */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->active || w->minimized || !w->focused) continue;
        if (w->anim_type == ANIM_CLOSE || w->anim_type == ANIM_MINIMIZE) continue;
        if (mx >= w->x && mx < w->x + w->width &&
            my >= w->y && my < w->y + TITLEBAR_H + w->height)
            return w;
    }
    /* Then unfocused, reverse order */
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        window_t *w = &windows[i];
        if (!w->active || w->minimized || w->focused) continue;
        if (w->anim_type == ANIM_CLOSE || w->anim_type == ANIM_MINIMIZE) continue;
        if (mx >= w->x && mx < w->x + w->width &&
            my >= w->y && my < w->y + TITLEBAR_H + w->height)
            return w;
    }
    return (void *)0;
}

/* ── Helper: compute taskbar button X position for a window ───────────── */
static int taskbar_button_x_for(window_t *target)
{
    int bx = 110;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (&windows[i] == target) return bx;
        if (windows[i].active) bx += 128;
    }
    return bx;
}

/* ── Helper: reallocate canvas after resize ───────────────────────────── */
static void resize_canvas(window_t *w, int new_w, int new_h)
{
    int cw = new_w - BORDER_W * 2;
    int ch = new_h - BORDER_W * 2;
    if (w->canvas) kfree(w->canvas);
    w->canvas = (uint32_t *)kmalloc(cw * ch * 4);
    if (w->canvas) {
        for (int p = 0; p < cw * ch; p++)
            w->canvas[p] = 0xF0F0F0;
    }
    w->width = new_w;
    w->height = new_h;
}

void compositor_handle_mouse(int mx, int my, int buttons, int scroll)
{
    current_scroll = scroll;

    /* Smooth scroll: accumulate target, interpolate each frame */
    if (scroll != 0)
        smooth_scroll_target += scroll * 256;
    /* Interpolate toward target (ease ~30% per frame at 120fps) */
    if (smooth_scroll_current != smooth_scroll_target) {
        int diff = smooth_scroll_target - smooth_scroll_current;
        int step = diff * 3 / 10;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        smooth_scroll_current += step;
        /* Snap when close enough */
        int remaining = smooth_scroll_target - smooth_scroll_current;
        if (remaining < 0) remaining = -remaining;
        if (remaining < 4) {
            smooth_scroll_current = smooth_scroll_target;
        }
    }

    int click = (buttons & 1) && !(prev_mouse_buttons & 1);
    int right_click = (buttons & 2) && !(prev_mouse_buttons & 2);
    int release = !(buttons & 1) && (prev_mouse_buttons & 1);
    prev_mouse_buttons = buttons;

    /* Handle window dragging */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->active || w->minimized) continue;

        if (w->dragging) {
            w->x = mx - w->drag_ox;
            w->y = my - w->drag_oy;
            if (release) w->dragging = 0;
            compositor_draw_cursor(mx, my);
            return;
        }
    }

    if (click) {
        framebuffer_t *f = fb_get();

        /* Start menu button hit test (wider now: 90px) */
        int tb_y = (int)f->height - TASKBAR_H;
        if (mx >= 4 && mx < 94 && my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4) {
            if (start_menu_anim != 1 && start_menu_anim != 2) {
                if (start_menu_open) {
                    start_menu_anim = 2;
                    start_menu_anim_start = timer_get_ticks();
                } else {
                    start_menu_open = 1;
                    start_menu_anim = 1;
                    start_menu_anim_start = timer_get_ticks();
                }
            }
            compositor_draw_cursor(mx, my);
            return;
        }

        /* Taskbar window button hit test — click to unminimize/focus */
        if (my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4) {
            int bx = 110;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (!windows[i].active) continue;
                if (mx >= bx && mx < bx + 120) {
                    if (windows[i].minimized) {
                        windows[i].minimized = 0;
                        windows[i].anim_type = ANIM_UNMINIMIZE;
                        windows[i].anim_start = timer_get_ticks();
                        windows[i].anim_from_x = bx;
                        windows[i].anim_from_y = (int)f->height - TASKBAR_H;
                        windows[i].anim_from_w = 120;
                        windows[i].anim_from_h = 4;
                        windows[i].anim_to_x = windows[i].x;
                        windows[i].anim_to_y = windows[i].y;
                        windows[i].anim_to_w = windows[i].width;
                        windows[i].anim_to_h = windows[i].height;
                    }
                    for (int j = 0; j < MAX_WINDOWS; j++)
                        windows[j].focused = 0;
                    windows[i].focused = 1;
                    compositor_draw_cursor(mx, my);
                    return;
                }
                bx += 128;
            }
        }

        /* Start menu item hit test */
        if (start_menu_open && start_menu_anim != 2) {
            int separator_h = 6;
            int mh = START_MENU_ITEMS * START_MENU_ITEM_H + 8 + separator_h;
            int menu_x = 4;
            int menu_y = (int)f->height - TASKBAR_H - mh;
            if (mx >= menu_x && mx < menu_x + START_MENU_W &&
                my >= menu_y && my < menu_y + mh) {
                int rel_y = my - menu_y - 4;
                int item;
                if (rel_y < 4 * START_MENU_ITEM_H) {
                    item = rel_y / START_MENU_ITEM_H;
                } else if (rel_y < 4 * START_MENU_ITEM_H + separator_h) {
                    item = -1;
                } else {
                    item = 4 + (rel_y - 4 * START_MENU_ITEM_H - separator_h) / START_MENU_ITEM_H;
                }
                if (item >= 0 && item < START_MENU_ITEMS) {
                    start_menu_anim = 2;
                    start_menu_anim_start = timer_get_ticks();
                    if (start_menu_callback)
                        start_menu_callback(item);
                }
                compositor_draw_cursor(mx, my);
                return;
            }
            start_menu_anim = 2;
            start_menu_anim_start = timer_get_ticks();
        }

        /* Window hit-test: use z-order-aware helper (before desktop icons
         * so clicks cannot pass through windows onto the desktop) */
        window_t *w = window_at(mx, my);
        if (w) {
            int btn_y_center = w->y + TITLEBAR_H / 2;

            /* Close button (rightmost) */
            int close_cx = w->x + w->width - 16;
            if ((mx - close_cx) * (mx - close_cx) + (my - btn_y_center) * (my - btn_y_center) <= 49) {
                compositor_destroy_window(w);
                compositor_draw_cursor(mx, my);
                return;
            }

            /* Maximize button (middle) */
            int max_cx = w->x + w->width - 36;
            if ((mx - max_cx) * (mx - max_cx) + (my - btn_y_center) * (my - btn_y_center) <= 49) {
                if (w->maximized) {
                    /* Restore — animate from maximized to original */
                    int from_x = w->x, from_y = w->y;
                    int from_w = w->width, from_h = w->height;
                    w->x = w->orig_x;
                    w->y = w->orig_y;
                    resize_canvas(w, w->orig_w, w->orig_h);
                    w->maximized = 0;
                    w->anim_type = ANIM_RESTORE;
                    w->anim_start = timer_get_ticks();
                    w->anim_from_x = from_x;
                    w->anim_from_y = from_y;
                    w->anim_from_w = from_w;
                    w->anim_from_h = from_h;
                    w->anim_to_x = w->x;
                    w->anim_to_y = w->y;
                    w->anim_to_w = w->width;
                    w->anim_to_h = w->height;
                } else {
                    /* Maximize — animate from original to maximized */
                    w->orig_x = w->x;
                    w->orig_y = w->y;
                    w->orig_w = w->width;
                    w->orig_h = w->height;
                    int new_w = (int)f->width;
                    int new_h = (int)f->height - TASKBAR_H - TITLEBAR_H;
                    w->anim_type = ANIM_MAXIMIZE;
                    w->anim_start = timer_get_ticks();
                    w->anim_from_x = w->x;
                    w->anim_from_y = w->y;
                    w->anim_from_w = w->width;
                    w->anim_from_h = w->height;
                    w->anim_to_x = 0;
                    w->anim_to_y = 0;
                    w->anim_to_w = new_w;
                    w->anim_to_h = new_h;
                    w->x = 0;
                    w->y = 0;
                    resize_canvas(w, new_w, new_h);
                    w->maximized = 1;
                }
                for (int j = 0; j < MAX_WINDOWS; j++)
                    windows[j].focused = 0;
                w->focused = 1;
                compositor_draw_cursor(mx, my);
                return;
            }

            /* Minimize button (leftmost of the three) */
            int min_cx = w->x + w->width - 56;
            if ((mx - min_cx) * (mx - min_cx) + (my - btn_y_center) * (my - btn_y_center) <= 49) {
                w->focused = 0;
                w->anim_type = ANIM_MINIMIZE;
                w->anim_start = timer_get_ticks();
                w->anim_from_x = w->x;
                w->anim_from_y = w->y;
                w->anim_from_w = w->width;
                w->anim_from_h = w->height;
                w->anim_to_x = taskbar_button_x_for(w);
                w->anim_to_y = (int)f->height - TASKBAR_H;
                w->anim_to_w = 120;
                w->anim_to_h = 4;
                compositor_draw_cursor(mx, my);
                return;
            }

            /* Title bar drag (disabled when maximized) */
            if (my >= w->y && my < w->y + TITLEBAR_H) {
                if (!w->maximized) {
                    w->dragging = 1;
                    w->drag_ox = mx - w->x;
                    w->drag_oy = my - w->y;
                }
                for (int j = 0; j < MAX_WINDOWS; j++)
                    windows[j].focused = 0;
                w->focused = 1;
                compositor_draw_cursor(mx, my);
                return;
            }

            /* Client area */
            if (my >= w->y + TITLEBAR_H) {
                for (int j = 0; j < MAX_WINDOWS; j++)
                    windows[j].focused = 0;
                w->focused = 1;

                if (w->on_mouse) {
                    int lx = mx - w->x - BORDER_W;
                    int ly = my - w->y - TITLEBAR_H - BORDER_W;
                    w->on_mouse(w, lx, ly, buttons);
                }
                compositor_draw_cursor(mx, my);
                return;
            }
        }

        /* Desktop icon hit test (double-click to open, single click to select)
         * Only reached when no window is under the cursor */
        for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
            int ix = ICON_START_X;
            int iy = ICON_START_Y + i * (ICON_H + ICON_LABEL_GAP);
            if (mx >= ix - 4 && mx < ix + ICON_W + 4 &&
                my >= iy - 4 && my < iy + ICON_H + 24) {
                uint64_t now = timer_get_ticks();
                if (last_icon_click_idx == i &&
                    (now - last_icon_click_tick) < DBLCLICK_MS) {
                    selected_icon = -1;
                    last_icon_click_idx = -1;
                    if (start_menu_callback)
                        start_menu_callback(i);
                } else {
                    selected_icon = i;
                    last_icon_click_tick = now;
                    last_icon_click_idx = i;
                }
                compositor_draw_cursor(mx, my);
                return;
            }
        }

        /* Click on empty desktop deselects icons */
        selected_icon = -1;
    }

    /* Right-click: forward to window under cursor */
    if (right_click) {
        window_t *w = window_at(mx, my);
        if (w && my >= w->y + TITLEBAR_H && w->on_mouse) {
            /* Focus the window */
            for (int j = 0; j < MAX_WINDOWS; j++)
                windows[j].focused = 0;
            w->focused = 1;
            int lx = mx - w->x - BORDER_W;
            int ly = my - w->y - TITLEBAR_H - BORDER_W;
            w->on_mouse(w, lx, ly, buttons);
            compositor_draw_cursor(mx, my);
            return;
        }
    }

    /* Forward mouse move to focused window */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].focused && windows[i].on_mouse) {
            int lx = mx - windows[i].x - BORDER_W;
            int ly = my - windows[i].y - TITLEBAR_H - BORDER_W;
            windows[i].on_mouse(&windows[i], lx, ly, buttons);
        }
    }

    /* Draw cursor last */
    compositor_draw_cursor(mx, my);
}

void compositor_set_app_launcher(void (*callback)(int item))
{
    start_menu_callback = callback;
}

void compositor_handle_key(char ascii, int scancode, int pressed)
{
    /* Forward to focused window */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].focused && windows[i].on_key) {
            windows[i].on_key(&windows[i], ascii, scancode, pressed);
            return;
        }
    }
}

void compositor_toggle_start_menu(void)
{
    if (start_menu_anim == 1 || start_menu_anim == 2) return;
    if (start_menu_open) {
        start_menu_anim = 2;
        start_menu_anim_start = timer_get_ticks();
    } else {
        start_menu_open = 1;
        start_menu_anim = 1;
        start_menu_anim_start = timer_get_ticks();
    }
}

int compositor_get_scroll(void)
{
    return current_scroll;
}

int compositor_get_smooth_scroll(void)
{
    /* Returns accumulated smooth delta and resets it */
    int val = smooth_scroll_current / 256;
    if (val != 0) {
        smooth_scroll_current -= val * 256;
        smooth_scroll_target -= val * 256;
    }
    return val;
}
