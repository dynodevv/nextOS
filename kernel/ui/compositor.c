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

/* ── Internal state ───────────────────────────────────────────────────── */
static window_t windows[MAX_WINDOWS];
static int      window_count = 0;
static theme_t  current_theme = THEME_BRUSHED_METAL;
static int      prev_mouse_buttons = 0;
static int      start_menu_open = 0;
static void   (*start_menu_callback)(int item) = (void *)0;

/* ── Theme colour palettes ────────────────────────────────────────────── */
typedef struct {
    uint32_t titlebar_top;
    uint32_t titlebar_bot;
    uint32_t titlebar_text;
    uint32_t border;
    uint32_t shadow;
    uint32_t taskbar_top;
    uint32_t taskbar_bot;
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
        .desktop_top   = 0x4A6FA5,
        .desktop_bot   = 0x1B2838,
        .button_top    = 0xD0D0D0,
        .button_bot    = 0x909090,
        .close_btn     = 0xCC4444,
    },
    [THEME_GLOSSY_GLASS] = {
        .titlebar_top  = 0xE8F0FF,
        .titlebar_bot  = 0x6090D0,
        .titlebar_text = 0xFFFFFF,
        .border        = 0x3060A0,
        .shadow        = 0x203050,
        .taskbar_top   = 0xD0E0F8,
        .taskbar_bot   = 0x5080C0,
        .desktop_top   = 0x2060B0,
        .desktop_bot   = 0x0A1A30,
        .button_top    = 0xC0D8F0,
        .button_bot    = 0x5080B0,
        .close_btn     = 0xE04040,
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

static void draw_window(window_t *win)
{
    if (win->minimized) return;

    const theme_colors_t *tc = &themes[current_theme];
    int x = win->x, y = win->y, w = win->width, h = win->height;

    /* Drop shadow */
    draw_shadow(x, y, w, h + TITLEBAR_H, tc->shadow);

    /* Title bar gradient + gloss */
    draw_gradient_rect(x, y, w, TITLEBAR_H, tc->titlebar_top, tc->titlebar_bot);
    draw_gloss(x, y, w, TITLEBAR_H);
    draw_bevel(x, y, w, TITLEBAR_H, 1);

    /* Title text (centred) */
    int text_w = str_len(win->title) * 8;
    int tx = x + (w - text_w) / 2;
    int ty = y + (TITLEBAR_H - 16) / 2;
    fb_draw_string(tx, ty, win->title, tc->titlebar_text, 0x00000000);

    /* Close button (red circle) */
    int cbx = x + w - 22;
    int cby = y + 6;
    for (int dy = 0; dy < 16; dy++) {
        for (int dx = 0; dx < 16; dx++) {
            int cx = dx - 8, cy = dy - 8;
            if (cx * cx + cy * cy <= 64) {
                fb_putpixel(cbx + dx, cby + dy, tc->close_btn);
            }
        }
    }
    /* X mark on close button */
    for (int i = 4; i < 12; i++) {
        fb_putpixel(cbx + i, cby + i, 0xFFFFFF);
        fb_putpixel(cbx + i, cby + 15 - i, 0xFFFFFF);
    }

    /* Client area border */
    int cy = y + TITLEBAR_H;
    fb_fill_rect(x, cy, w, h, 0xF0F0F0);
    draw_bevel(x, cy, w, h, 0);

    /* Border */
    fb_draw_rect(x - 1, y - 1, w + 2, h + TITLEBAR_H + 2, tc->border);

    /* Blit client canvas onto the window's client area */
    if (win->canvas) {
        int cw = w - BORDER_W * 2;
        int ch = h - BORDER_W * 2;
        for (int row = 0; row < ch; row++) {
            for (int col = 0; col < cw; col++) {
                fb_putpixel(x + BORDER_W + col, cy + BORDER_W + row,
                            win->canvas[row * cw + col]);
            }
        }
    }
}

/* ── Desktop wallpaper (gradient + subtle texture) ────────────────────── */
void desktop_draw_wallpaper(void)
{
    const theme_colors_t *tc = &themes[current_theme];
    framebuffer_t *f = fb_get();
    draw_gradient_rect(0, 0, f->width, f->height, tc->desktop_top, tc->desktop_bot);

    /* Subtle diagonal texture lines (skeuomorphic detail) */
    for (int y = 0; y < (int)f->height; y += 6) {
        for (int x = 0; x < (int)f->width; x++) {
            if ((x + y) % 12 == 0) {
                uint32_t px = fb_getpixel(x, y);
                fb_putpixel(x, y, rgba_blend(px, 0xFFFFFF, 8));
            }
        }
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
};
#define DESKTOP_ICON_COUNT 3

static void draw_desktop_icon(int x, int y, const desktop_icon_t *icon)
{

    /* Icon shadow */
    fb_fill_rect(x + 3, y + 3, ICON_W, ICON_H, rgba_blend(0x000000, 0x000000, 60));

    /* Icon body (gradient square) */
    for (int r = 0; r < ICON_H; r++) {
        uint32_t c = lerp_color(icon->color, rgba_blend(icon->color, 0x000000, 80), r, ICON_H);
        for (int col = 0; col < ICON_W; col++)
            fb_putpixel(x + col, y + r, c);
    }
    /* Gloss highlight on top half */
    for (int r = 0; r < ICON_H / 2; r++) {
        uint8_t alpha = 70 - (uint8_t)(r * 70 / (ICON_H / 2));
        for (int col = 0; col < ICON_W; col++) {
            uint32_t px = fb_getpixel(x + col, y + r);
            fb_putpixel(x + col, y + r, rgba_blend(px, 0xFFFFFF, alpha));
        }
    }
    /* Border */
    fb_draw_rect(x, y, ICON_W, ICON_H, 0x303030);

    /* Inner icon glyph (first letter of label, large) */
    int lx = x + (ICON_W - 8) / 2;
    int ly = y + (ICON_H - 16) / 2;
    fb_draw_char(lx, ly, icon->label[0], 0xFFFFFF, 0x00000000);

    /* Label below icon */
    int label_len = str_len(icon->label);
    int label_x = x + (ICON_W - label_len * 8) / 2;
    int label_y = y + ICON_H + 4;
    /* Label shadow */
    fb_draw_string(label_x + 1, label_y + 1, icon->label, 0x000000, 0x00000000);
    fb_draw_string(label_x, label_y, icon->label, 0xFFFFFF, 0x00000000);
}

static void draw_desktop_icons(void)
{
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int x = ICON_START_X;
        int y = ICON_START_Y + i * (ICON_H + ICON_LABEL_GAP);
        draw_desktop_icon(x, y, &desktop_icons[i]);
    }
}

/* ── Taskbar ──────────────────────────────────────────────────────────── */
#define TASKBAR_H 40

/* ── Start menu ──────────────────────────────────────────────────────── */
#define START_MENU_W 180
#define START_MENU_ITEM_H 32
#define START_MENU_ITEMS 3

static const char *start_menu_labels[START_MENU_ITEMS] = {
    "Settings", "File Explorer", "Notepad"
};

static void draw_start_menu(void)
{
    if (!start_menu_open) return;
    framebuffer_t *f = fb_get();
    const theme_colors_t *tc = &themes[current_theme];

    int mh = START_MENU_ITEMS * START_MENU_ITEM_H + 8;
    int mx = 4;
    int my = (int)f->height - TASKBAR_H - mh;

    /* Menu shadow */
    fb_fill_rect(mx + 4, my + 4, START_MENU_W, mh, 0x202020);
    /* Menu background */
    draw_gradient_rect(mx, my, START_MENU_W, mh, tc->button_top, tc->button_bot);
    draw_bevel(mx, my, START_MENU_W, mh, 1);

    /* Items */
    for (int i = 0; i < START_MENU_ITEMS; i++) {
        int iy = my + 4 + i * START_MENU_ITEM_H;
        fb_draw_string(mx + 12, iy + 8, start_menu_labels[i], 0x1A1A1A, 0x00000000);
        /* Separator line */
        if (i < START_MENU_ITEMS - 1) {
            for (int sx = mx + 4; sx < mx + START_MENU_W - 4; sx++)
                fb_putpixel(sx, iy + START_MENU_ITEM_H - 1,
                            rgba_blend(fb_getpixel(sx, iy + START_MENU_ITEM_H - 1), 0x000000, 30));
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

    /* "nextOS" start button */
    draw_gradient_rect(4, y + 4, 80, TASKBAR_H - 8,
                       tc->button_top, tc->button_bot);
    draw_bevel(4, y + 4, 80, TASKBAR_H - 8, 1);
    draw_gloss(4, y + 4, 80, TASKBAR_H - 8);
    fb_draw_string(14, y + 12, "nextOS", 0x1A1A1A, 0x00000000);

    /* Window buttons on taskbar */
    int bx = 100;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) continue;
        draw_gradient_rect(bx, y + 4, 120, TASKBAR_H - 8,
                           tc->button_top, tc->button_bot);
        draw_bevel(bx, y + 4, 120, TASKBAR_H - 8,
                   windows[i].focused ? 0 : 1);
        fb_draw_string(bx + 8, y + 12, windows[i].title, 0x1A1A1A, 0x00000000);
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

static void draw_cursor(int mx, int my)
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
    if (theme < THEME_COUNT)
        current_theme = theme;
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
            windows[i].close_hover = 0;
            windows[i].on_paint = (void *)0;
            windows[i].on_key   = (void *)0;
            windows[i].on_mouse = (void *)0;
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
    if (!win) return;
    if (win->canvas) kfree(win->canvas);
    win->active = 0;
    window_count--;
}

void compositor_render_frame(void)
{
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

void compositor_handle_mouse(int mx, int my, int buttons)
{
    int click = (buttons & 1) && !(prev_mouse_buttons & 1);
    int release = !(buttons & 1) && (prev_mouse_buttons & 1);
    prev_mouse_buttons = buttons;

    /* Handle window dragging */
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        window_t *w = &windows[i];
        if (!w->active || w->minimized) continue;

        if (w->dragging) {
            w->x = mx - w->drag_ox;
            w->y = my - w->drag_oy;
            if (release) w->dragging = 0;
            draw_cursor(mx, my);
            return;
        }
    }

    if (click) {
        framebuffer_t *f = fb_get();

        /* Start menu button hit test */
        int tb_y = (int)f->height - TASKBAR_H;
        if (mx >= 4 && mx < 84 && my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4) {
            start_menu_open = !start_menu_open;
            draw_cursor(mx, my);
            return;
        }

        /* Start menu item hit test */
        if (start_menu_open) {
            int mh = START_MENU_ITEMS * START_MENU_ITEM_H + 8;
            int menu_x = 4;
            int menu_y = (int)f->height - TASKBAR_H - mh;
            if (mx >= menu_x && mx < menu_x + START_MENU_W &&
                my >= menu_y && my < menu_y + mh) {
                int item = (my - menu_y - 4) / START_MENU_ITEM_H;
                if (item >= 0 && item < START_MENU_ITEMS) {
                    start_menu_open = 0;
                    /* Launch the selected app via callback */
                    if (start_menu_callback)
                        start_menu_callback(item);
                }
                draw_cursor(mx, my);
                return;
            }
            /* Click outside menu closes it */
            start_menu_open = 0;
        }

        /* Desktop icon hit test */
        for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
            int ix = ICON_START_X;
            int iy = ICON_START_Y + i * (ICON_H + ICON_LABEL_GAP);
            if (mx >= ix && mx < ix + ICON_W &&
                my >= iy && my < iy + ICON_H) {
                if (start_menu_callback)
                    start_menu_callback(i);
                draw_cursor(mx, my);
                return;
            }
        }

        /* Check window title bars for drag / close / focus */
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            window_t *w = &windows[i];
            if (!w->active || w->minimized) continue;

            /* Close button hit test */
            int cbx = w->x + w->width - 22;
            int cby = w->y + 6;
            if (mx >= cbx && mx < cbx + 16 && my >= cby && my < cby + 16) {
                compositor_destroy_window(w);
                draw_cursor(mx, my);
                return;
            }

            /* Title bar hit test */
            if (mx >= w->x && mx < w->x + w->width &&
                my >= w->y && my < w->y + TITLEBAR_H) {
                w->dragging = 1;
                w->drag_ox  = mx - w->x;
                w->drag_oy  = my - w->y;

                /* Bring to front (focus) */
                for (int j = 0; j < MAX_WINDOWS; j++)
                    windows[j].focused = 0;
                w->focused = 1;
                draw_cursor(mx, my);
                return;
            }

            /* Client area hit */
            if (mx >= w->x && mx < w->x + w->width &&
                my >= w->y + TITLEBAR_H && my < w->y + TITLEBAR_H + w->height) {
                for (int j = 0; j < MAX_WINDOWS; j++)
                    windows[j].focused = 0;
                w->focused = 1;

                if (w->on_mouse) {
                    int lx = mx - w->x - BORDER_W;
                    int ly = my - w->y - TITLEBAR_H - BORDER_W;
                    w->on_mouse(w, lx, ly, buttons);
                }
                draw_cursor(mx, my);
                return;
            }
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
    draw_cursor(mx, my);
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
