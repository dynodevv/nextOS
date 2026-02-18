/*
 * nextOS - settings.c
 * Settings application — tabbed control panel with skeuomorphic design
 *
 * Tabs: Display | Theme | Keyboard
 * Features:
 *   - Resolution selector (changes framebuffer mode)
 *   - Theme toggle (Brushed Metal / Glossy Glass)
 *   - Keyboard layout selector (26 layouts, including Hungarian)
 *   - All state persists for the session
 */
#include "settings.h"
#include "kernel/ui/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/disk.h"
#include "kernel/mem/heap.h"

/* ── Settings persistence ────────────────────────────────────────────── */
#define SETTINGS_LBA   8000
#define SETTINGS_MAGIC 0x43464731  /* "CFG1" */

typedef struct {
    uint32_t magic;
    uint32_t theme;
    uint32_t kb_layout;
    uint32_t mouse_speed;
    uint8_t  reserved[512 - 16];
} __attribute__((packed)) settings_disk_t;

void settings_save_to_disk(void)
{
    disk_device_t *disk = disk_get_primary();
    if (!disk) return;
    settings_disk_t cfg = {0};
    cfg.magic = SETTINGS_MAGIC;
    cfg.theme = (uint32_t)compositor_get_theme();
    cfg.kb_layout = (uint32_t)keyboard_get_layout();
    cfg.mouse_speed = (uint32_t)mouse_get_speed();
    disk_write(disk, SETTINGS_LBA, 1, &cfg);
}

void settings_load_from_disk(void)
{
    disk_device_t *disk = disk_get_primary();
    if (!disk) return;
    settings_disk_t cfg;
    if (disk_read(disk, SETTINGS_LBA, 1, &cfg) < 0) return;
    if (cfg.magic != SETTINGS_MAGIC) return;
    if (cfg.theme < THEME_COUNT)
        compositor_set_theme((theme_t)cfg.theme);
    if (cfg.kb_layout < KB_LAYOUT_COUNT)
        keyboard_set_layout((kb_layout_t)cfg.kb_layout);
    if (cfg.mouse_speed >= 1 && cfg.mouse_speed <= 10)
        mouse_set_speed((int)cfg.mouse_speed);
}

/* ── Tab identifiers ──────────────────────────────────────────────────── */
typedef enum { TAB_DISPLAY = 0, TAB_THEME, TAB_KEYBOARD, TAB_MOUSE, TAB_COUNT } tab_t;
static tab_t current_tab = TAB_DISPLAY;

/* ── Display settings state ───────────────────────────────────────────── */
static int resolution_index = 0;
typedef struct { int w; int h; const char *label; } resolution_t;
static const resolution_t resolutions[] = {
    { 640,  480,  "640x480"  },
    { 800,  600,  "800x600"  },
    {1024,  768,  "1024x768" },
    {1280, 1024,  "1280x1024"},
    {1920, 1080,  "1920x1080"},
};
#define RES_COUNT (int)(sizeof(resolutions)/sizeof(resolutions[0]))

/* ── Theme settings state ─────────────────────────────────────────────── */
static int theme_index = 0;  /* 0 = Brushed Metal, 1 = Glossy Glass */

/* ── Keyboard layout state ────────────────────────────────────────────── */
static int kb_layout_index = 0;
static int kb_scroll_offset = 0;
static int kb_scrollbar_dragging = 0;
static int kb_scrollbar_drag_offset = 0;
#define KB_VISIBLE_ROWS 8

/* ── Skeuomorphic colours ─────────────────────────────────────────────── */
#define COL_PANEL_BG     0xE8E0D4
#define COL_PANEL_BORDER 0x8B7D6B
#define COL_TAB_ACTIVE   0xF5EDE0
#define COL_TAB_INACTIVE 0xC8BFB0
#define COL_TAB_TEXT     0x3A3025
#define COL_LABEL        0x2A2015
#define COL_BTN_TOP      0xE0D8CC
#define COL_BTN_BOT      0xB0A898
#define COL_BTN_TEXT     0x1A1A1A
#define COL_SELECTED     0x5080B0
#define COL_SEL_TEXT     0xFFFFFF
#define COL_LEATHER      0xC4A882
#define COL_LEATHER_DARK 0x8B7355
#define COL_DIVIDER      0xA09080

/* ── Drawing helpers ──────────────────────────────────────────────────── */
static window_t *settings_win = (void *)0;

static void fill_canvas_rect(uint32_t *canvas, int cw, int ch,
                             int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h && row < ch; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < cw; col++) {
            if (col < 0) continue;
            canvas[row * cw + col] = color;
        }
    }
}

static void draw_canvas_gradient(uint32_t *canvas, int cw, int ch,
                                 int x, int y, int w, int h,
                                 uint32_t top, uint32_t bot)
{
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= ch) continue;
        int tr = (top>>16)&0xFF, tg = (top>>8)&0xFF, tb = top&0xFF;
        int bot_r = (bot>>16)&0xFF, bot_g = (bot>>8)&0xFF, bot_b = bot&0xFF;
        int denom = (h > 1 ? h - 1 : 1);
        int rr = tr + (bot_r - tr) * row / denom;
        int rg = tg + (bot_g - tg) * row / denom;
        int rb = tb + (bot_b - tb) * row / denom;
        if (rr < 0) rr = 0;
        if (rr > 255) rr = 255;
        if (rg < 0) rg = 0;
        if (rg > 255) rg = 255;
        if (rb < 0) rb = 0;
        if (rb > 255) rb = 255;
        uint32_t c = ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
        for (int col = x; col < x + w && col < cw; col++) {
            if (col < 0) continue;
            canvas[py * cw + col] = c;
        }
    }
}

/* Minimal built-in font render directly into canvas */
extern const uint8_t font_8x16[95][16]; /* defined in framebuffer.c */

static void canvas_draw_char(uint32_t *canvas, int cw, int ch,
                             int x, int y, char c, uint32_t fg)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font_8x16[c - 32];
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= ch) continue;
        uint8_t bits = glyph[row];
        uint8_t bits_above = (row > 0)  ? glyph[row - 1] : 0;
        uint8_t bits_below = (row < 15) ? glyph[row + 1] : 0;
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= cw) continue;
            uint8_t mask = 0x80 >> col;
            if (bits & mask) {
                canvas[py * cw + px] = fg;
            } else {
                int neighbors = 0;
                if (bits_above & mask) neighbors++;
                if (bits_below & mask) neighbors++;
                if (col > 0 && (bits & (mask << 1))) neighbors++;
                if (col < 7 && (bits & (mask >> 1))) neighbors++;
                if (neighbors > 0) {
                    uint32_t base = canvas[py * cw + px];
                    uint8_t sr = (fg >> 16) & 0xFF, sg = (fg >> 8) & 0xFF, sb = fg & 0xFF;
                    uint8_t dr = (base >> 16) & 0xFF, dg = (base >> 8) & 0xFF, db = base & 0xFF;
                    uint8_t alpha = (uint8_t)(neighbors * 40);
                    uint8_t rr = (sr * alpha + dr * (255 - alpha)) / 255;
                    uint8_t rg = (sg * alpha + dg * (255 - alpha)) / 255;
                    uint8_t rb = (sb * alpha + db * (255 - alpha)) / 255;
                    canvas[py * cw + px] = ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
                }
            }
        }
    }
}

static void draw_canvas_string(uint32_t *canvas, int cw, int ch,
                               int x, int y, const char *s,
                               uint32_t fg, uint32_t bg)
{
    (void)bg;
    int cx = x;
    while (*s) {
        if (*s >= 32 && *s <= 126)
            canvas_draw_char(canvas, cw, ch, cx, y, *s, fg);
        cx += 8;
        s++;
    }
}

/* ── Tab drawing ──────────────────────────────────────────────────────── */
static const char *tab_names[TAB_COUNT] = { "Display", "Theme", "Keyboard", "Mouse" };

static void draw_tabs(uint32_t *canvas, int cw, int ch)
{
    int tab_w = cw / TAB_COUNT;
    for (int t = 0; t < TAB_COUNT; t++) {
        uint32_t bg = (t == (int)current_tab) ? COL_TAB_ACTIVE : COL_TAB_INACTIVE;
        fill_canvas_rect(canvas, cw, ch, t * tab_w, 0, tab_w, 30, bg);
        /* Tab text */
        draw_canvas_string(canvas, cw, ch,
                           t * tab_w + 10, 8, tab_names[t],
                           COL_TAB_TEXT, bg);
        /* Bevel on active tab */
        if (t == (int)current_tab) {
            for (int col = t * tab_w; col < t * tab_w + tab_w && col < cw; col++)
                canvas[col] = 0xFFFFFF; /* top highlight */
        }
    }
    /* Divider line under tabs */
    for (int col = 0; col < cw; col++) {
        if (30 < ch) canvas[30 * cw + col] = COL_DIVIDER;
    }
}

/* ── Display tab ──────────────────────────────────────────────────────── */
static void draw_display_tab(uint32_t *canvas, int cw, int ch)
{
    /* Leather-textured panel background */
    draw_canvas_gradient(canvas, cw, ch, 0, 32, cw, ch - 32,
                         COL_LEATHER, COL_LEATHER_DARK);

    /* "Screen Resolution" label */
    draw_canvas_string(canvas, cw, ch, 20, 50, "Screen Resolution:",
                       COL_LABEL, COL_LEATHER);

    /* Resolution buttons */
    for (int i = 0; i < RES_COUNT; i++) {
        int by = 80 + i * 36;
        uint32_t bg = (i == resolution_index) ? COL_SELECTED : COL_BTN_TOP;
        uint32_t fg = (i == resolution_index) ? COL_SEL_TEXT : COL_BTN_TEXT;
        draw_canvas_gradient(canvas, cw, ch, 20, by, 200, 28,
                             bg, (i == resolution_index) ? 0x305880 : COL_BTN_BOT);
        draw_canvas_string(canvas, cw, ch, 30, by + 6,
                           resolutions[i].label, fg, bg);
    }
}

/* ── Theme tab ────────────────────────────────────────────────────────── */
static void draw_theme_tab(uint32_t *canvas, int cw, int ch)
{
    draw_canvas_gradient(canvas, cw, ch, 0, 32, cw, ch - 32,
                         COL_LEATHER, COL_LEATHER_DARK);

    draw_canvas_string(canvas, cw, ch, 20, 50, "Desktop Theme:",
                       COL_LABEL, COL_LEATHER);

    static const char *theme_names[] = {
        "Brushed Metal", "Glossy Glass",
        "Dark Obsidian", "Warm Mahogany"
    };
    for (int i = 0; i < 4; i++) {
        int by = 80 + i * 40;
        uint32_t bg = (i == theme_index) ? COL_SELECTED : COL_BTN_TOP;
        uint32_t fg = (i == theme_index) ? COL_SEL_TEXT : COL_BTN_TEXT;
        draw_canvas_gradient(canvas, cw, ch, 20, by, 240, 32,
                             bg, (i == theme_index) ? 0x305880 : COL_BTN_BOT);
        draw_canvas_string(canvas, cw, ch, 30, by + 8,
                           theme_names[i], fg, bg);
    }
}

/* ── Keyboard tab ─────────────────────────────────────────────────────── */
static void draw_keyboard_tab(uint32_t *canvas, int cw, int ch)
{
    draw_canvas_gradient(canvas, cw, ch, 0, 32, cw, ch - 32,
                         COL_LEATHER, COL_LEATHER_DARK);

    draw_canvas_string(canvas, cw, ch, 20, 50, "Keyboard Layout:",
                       COL_LABEL, COL_LEATHER);

    /* Scrollable list of layouts */
    int list_y = 80;
    int list_h = KB_VISIBLE_ROWS * 30;

    for (int vi = 0; vi < KB_VISIBLE_ROWS; vi++) {
        int li = kb_scroll_offset + vi;
        if (li >= KB_LAYOUT_COUNT) break;

        int by = list_y + vi * 30;
        uint32_t bg = (li == kb_layout_index) ? COL_SELECTED : COL_BTN_TOP;
        uint32_t fg = (li == kb_layout_index) ? COL_SEL_TEXT : COL_BTN_TEXT;
        draw_canvas_gradient(canvas, cw, ch, 20, by, 280, 24,
                             bg, (li == kb_layout_index) ? 0x305880 : COL_BTN_BOT);
        draw_canvas_string(canvas, cw, ch, 30, by + 4,
                           keyboard_layout_name((kb_layout_t)li), fg, bg);
    }

    /* Scrollbar track */
    int sb_x = 310;
    int sb_w = 12;
    fill_canvas_rect(canvas, cw, ch, sb_x, list_y, sb_w, list_h, 0xA09888);
    if (KB_LAYOUT_COUNT > KB_VISIBLE_ROWS) {
        int max_scroll = KB_LAYOUT_COUNT - KB_VISIBLE_ROWS;
        int thumb_h = list_h * KB_VISIBLE_ROWS / KB_LAYOUT_COUNT;
        if (thumb_h < 20) thumb_h = 20;
        int track_range = list_h - thumb_h;
        int thumb_y = list_y;
        if (track_range > 0 && max_scroll > 0)
            thumb_y = list_y + track_range * kb_scroll_offset / max_scroll;
        fill_canvas_rect(canvas, cw, ch, sb_x + 1, thumb_y, sb_w - 2, thumb_h, 0x706050);
    }
}

/* ── Mouse tab ────────────────────────────────────────────────────────── */
static void draw_mouse_tab(uint32_t *canvas, int cw, int ch)
{
    draw_canvas_gradient(canvas, cw, ch, 0, 32, cw, ch - 32,
                         COL_LEATHER, COL_LEATHER_DARK);

    draw_canvas_string(canvas, cw, ch, 20, 50, "Mouse Speed:",
                       COL_LABEL, COL_LEATHER);

    int speed = mouse_get_speed();

    /* Speed slider track */
    int track_x = 20, track_y = 80, track_w = 280, track_h = 8;
    fill_canvas_rect(canvas, cw, ch, track_x, track_y, track_w, track_h, 0x908070);

    /* Slider thumb */
    int thumb_x = track_x + (speed - 1) * (track_w - 16) / 9;
    draw_canvas_gradient(canvas, cw, ch, thumb_x, track_y - 8, 16, 24,
                         COL_BTN_TOP, COL_BTN_BOT);

    /* Speed label */
    char speed_str[4];
    if (speed >= 10) {
        speed_str[0] = '1'; speed_str[1] = '0'; speed_str[2] = 0;
    } else {
        speed_str[0] = '0' + speed; speed_str[1] = 0;
    }
    draw_canvas_string(canvas, cw, ch, 310, 76, speed_str, COL_LABEL, COL_LEATHER);

    /* Slow / Fast labels */
    draw_canvas_string(canvas, cw, ch, 20, 110, "Slow", COL_LABEL, COL_LEATHER);
    draw_canvas_string(canvas, cw, ch, 276, 110, "Fast", COL_LABEL, COL_LEATHER);

    /* Speed preset buttons */
    static const char *speed_labels[] = { "Slow", "Medium", "Fast" };
    static const int speed_vals[] = { 2, 5, 8 };
    for (int i = 0; i < 3; i++) {
        int by = 140 + i * 36;
        int is_sel = (speed == speed_vals[i]);
        uint32_t bg = is_sel ? COL_SELECTED : COL_BTN_TOP;
        uint32_t fg = is_sel ? COL_SEL_TEXT : COL_BTN_TEXT;
        draw_canvas_gradient(canvas, cw, ch, 20, by, 140, 28,
                             bg, is_sel ? 0x305880 : COL_BTN_BOT);
        draw_canvas_string(canvas, cw, ch, 30, by + 6,
                           speed_labels[i], fg, bg);
    }
}

/* ── Paint callback ───────────────────────────────────────────────────── */
static void settings_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;  /* BORDER_W * 2 */
    int ch = win->height - 4;

    /* Panel background */
    fill_canvas_rect(win->canvas, cw, ch, 0, 0, cw, ch, COL_PANEL_BG);

    /* Tabs */
    draw_tabs(win->canvas, cw, ch);

    /* Current tab content */
    switch (current_tab) {
    case TAB_DISPLAY:  draw_display_tab(win->canvas, cw, ch);  break;
    case TAB_THEME:    draw_theme_tab(win->canvas, cw, ch);    break;
    case TAB_KEYBOARD: draw_keyboard_tab(win->canvas, cw, ch); break;
    case TAB_MOUSE:    draw_mouse_tab(win->canvas, cw, ch);    break;
    default: break;
    }
}

/* ── Mouse callback ───────────────────────────────────────────────────── */
static void settings_mouse(window_t *win, int mx, int my, int buttons)
{
    (void)win;

    int cw = win->width - 4;

    /* Handle keyboard scrollbar drag */
    if (kb_scrollbar_dragging) {
        if (!(buttons & 1)) {
            kb_scrollbar_dragging = 0;
            return;
        }
        int list_y = 80;
        int list_h = KB_VISIBLE_ROWS * 30;
        int max_scroll = KB_LAYOUT_COUNT - KB_VISIBLE_ROWS;
        if (max_scroll > 0 && list_h > 0) {
            int thumb_h = list_h * KB_VISIBLE_ROWS / KB_LAYOUT_COUNT;
            if (thumb_h < 20) thumb_h = 20;
            int track_range = list_h - thumb_h;
            if (track_range > 0) {
                int thumb_top = my - kb_scrollbar_drag_offset;
                int new_offset = (thumb_top - list_y) * max_scroll / track_range;
                if (new_offset < 0) new_offset = 0;
                if (new_offset > max_scroll) new_offset = max_scroll;
                kb_scroll_offset = new_offset;
            }
        }
        return;
    }

    /* Handle scroll wheel for keyboard layout list */
    int scroll = compositor_get_scroll();
    if (scroll != 0 && current_tab == TAB_KEYBOARD) {
        int max_scroll = KB_LAYOUT_COUNT - KB_VISIBLE_ROWS;
        if (max_scroll > 0) {
            kb_scroll_offset += scroll * 2;
            if (kb_scroll_offset < 0) kb_scroll_offset = 0;
            if (kb_scroll_offset > max_scroll) kb_scroll_offset = max_scroll;
        }
    }

    if (!(buttons & 1)) return;

    /* Tab click */
    if (my >= 0 && my < 30) {
        int tab_w = cw / TAB_COUNT;
        int t = mx / tab_w;
        if (t >= 0 && t < TAB_COUNT) current_tab = (tab_t)t;
        return;
    }

    /* Display tab clicks */
    if (current_tab == TAB_DISPLAY) {
        for (int i = 0; i < RES_COUNT; i++) {
            int by = 80 + i * 36;
            if (mx >= 20 && mx < 220 && my >= by && my < by + 28) {
                if (i != resolution_index) {
                    if (compositor_set_resolution(resolutions[i].w, resolutions[i].h) == 0)
                        resolution_index = i;
                }
                return;
            }
        }
    }

    /* Theme tab clicks */
    if (current_tab == TAB_THEME) {
        for (int i = 0; i < 4; i++) {
            int by = 80 + i * 40;
            if (mx >= 20 && mx < 260 && my >= by && my < by + 32) {
                theme_index = i;
                compositor_set_theme((theme_t)i);
                settings_save_to_disk();
                return;
            }
        }
    }

    /* Keyboard tab clicks */
    if (current_tab == TAB_KEYBOARD) {
        /* Scrollbar interaction */
        int list_y = 80;
        int list_h = KB_VISIBLE_ROWS * 30;
        int sb_x = 310;
        if (mx >= sb_x && mx < sb_x + 12 && my >= list_y && my < list_y + list_h) {
            if (KB_LAYOUT_COUNT > KB_VISIBLE_ROWS) {
                int max_scroll = KB_LAYOUT_COUNT - KB_VISIBLE_ROWS;
                int thumb_h = list_h * KB_VISIBLE_ROWS / KB_LAYOUT_COUNT;
                if (thumb_h < 20) thumb_h = 20;
                int track_range = list_h - thumb_h;
                int thumb_y = list_y;
                if (track_range > 0 && max_scroll > 0)
                    thumb_y = list_y + track_range * kb_scroll_offset / max_scroll;
                if (my >= thumb_y && my < thumb_y + thumb_h) {
                    kb_scrollbar_dragging = 1;
                    kb_scrollbar_drag_offset = my - thumb_y;
                    return;
                }
                /* Click on track: page scroll */
                if (my < thumb_y) {
                    kb_scroll_offset -= KB_VISIBLE_ROWS;
                    if (kb_scroll_offset < 0) kb_scroll_offset = 0;
                } else {
                    kb_scroll_offset += KB_VISIBLE_ROWS;
                    if (kb_scroll_offset > max_scroll) kb_scroll_offset = max_scroll;
                }
            }
            return;
        }

        /* Layout item clicks */
        for (int vi = 0; vi < KB_VISIBLE_ROWS; vi++) {
            int li = kb_scroll_offset + vi;
            if (li >= KB_LAYOUT_COUNT) break;
            int by = list_y + vi * 30;
            if (mx >= 20 && mx < 300 && my >= by && my < by + 24) {
                kb_layout_index = li;
                keyboard_set_layout((kb_layout_t)li);
                settings_save_to_disk();
                return;
            }
        }
    }

    /* Mouse tab clicks */
    if (current_tab == TAB_MOUSE) {
        /* Slider track click */
        if (mx >= 20 && mx < 300 && my >= 72 && my < 104) {
            int speed = 1 + (mx - 20) * 9 / 280;
            if (speed < 1) speed = 1;
            if (speed > 10) speed = 10;
            mouse_set_speed(speed);
            settings_save_to_disk();
            return;
        }
        /* Preset buttons */
        static const int speed_vals[] = { 2, 5, 8 };
        for (int i = 0; i < 3; i++) {
            int by = 140 + i * 36;
            if (mx >= 20 && mx < 160 && my >= by && my < by + 28) {
                mouse_set_speed(speed_vals[i]);
                settings_save_to_disk();
                return;
            }
        }
    }
}

/* ── Key callback ─────────────────────────────────────────────────────── */
static void settings_key(window_t *win, char ascii, int scancode, int pressed)
{
    (void)win; (void)ascii; (void)scancode; (void)pressed;
    /* Could handle keyboard shortcuts here */
}

/* ── Close callback ────────────────────────────────────────────────────── */
static void settings_close(window_t *win)
{
    (void)win;
    settings_win = (void *)0;
}

/* ── Public: launch settings ──────────────────────────────────────────── */
void settings_launch(void)
{
    if (settings_win && settings_win->active) return; /* already open */
    settings_win = (void *)0;

    settings_win = compositor_create_window("Settings", 100, 60, 380, 340);
    if (!settings_win) return;

    settings_win->on_paint = settings_paint;
    settings_win->on_mouse = settings_mouse;
    settings_win->on_key   = settings_key;
    settings_win->on_close = settings_close;

    /* Sync UI state with actual system state (loaded from disk at boot) */
    theme_index    = (int)compositor_get_theme();
    kb_layout_index = (int)keyboard_get_layout();
    /* Scroll keyboard list so the selected layout is visible */
    if (kb_layout_index >= KB_VISIBLE_ROWS)
        kb_scroll_offset = kb_layout_index - KB_VISIBLE_ROWS + 1;
    else
        kb_scroll_offset = 0;
    /* Sync resolution index with actual framebuffer */
    {
        framebuffer_t *f = fb_get();
        resolution_index = 0;
        for (int i = 0; i < RES_COUNT; i++) {
            if ((int)f->width == resolutions[i].w && (int)f->height == resolutions[i].h) {
                resolution_index = i;
                break;
            }
        }
    }
}
