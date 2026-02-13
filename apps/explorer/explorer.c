/*
 * nextOS - explorer.c
 * File Explorer application — cabinet/manila-folder style interface
 *
 * Reads the actual filesystem via the kernel VFS layer.
 * Supports directory navigation, file execution, and properties view.
 */
#include "explorer.h"
#include "kernel/ui/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/fs/vfs.h"
#include "kernel/mem/heap.h"

/* ── State ────────────────────────────────────────────────────────────── */
#define MAX_ENTRIES  128
#define ENTRY_HEIGHT 24
#define VISIBLE_ROWS 12
#define PATH_MAX_LEN 256

static window_t *explorer_win = (void *)0;

static vfs_node_t current_dir;
static vfs_node_t entries[MAX_ENTRIES];
static int         entry_count = 0;
static int         selected_index = -1;
static int         scroll_offset = 0;
static char        current_path[PATH_MAX_LEN] = "/";

/* ── Skeuomorphic colours ─────────────────────────────────────────────── */
#define COL_CABINET_TOP  0xD4C4A0
#define COL_CABINET_BOT  0x8B7B5B
#define COL_TOOLBAR_TOP  0xE8DCC8
#define COL_TOOLBAR_BOT  0xC8BCA8
#define COL_FOLDER       0xE8C850   /* Manila folder yellow */
#define COL_FOLDER_DARK  0xC8A830
#define COL_FILE_ICON    0xF0F0F0
#define COL_FILE_BORDER  0x808080
#define COL_SELECTED_BG  0x4878A8
#define COL_TEXT         0x1A1A1A
#define COL_TEXT_SEL     0xFFFFFF
#define COL_PATH_BG      0xFFF8E8
#define COL_SCROLLBAR    0xB0A890

/* ── Helpers ──────────────────────────────────────────────────────────── */
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_cat(char *d, const char *s) {
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

/* ── Canvas font renderer ─────────────────────────────────────────────── */
extern const uint8_t font_8x16[95][16];

static void canvas_draw_char(uint32_t *canvas, int cw, int ch,
                             int x, int y, char c, uint32_t fg)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font_8x16[c - 32];
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= ch) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col;
                if (px >= 0 && px < cw)
                    canvas[py * cw + px] = fg;
            }
        }
    }
}

static void canvas_draw_string(uint32_t *canvas, int cw, int ch,
                               int x, int y, const char *s, uint32_t fg)
{
    int cx = x;
    while (*s) {
        if (*s >= 32 && *s <= 126)
            canvas_draw_char(canvas, cw, ch, cx, y, *s, fg);
        cx += 8;
        s++;
    }
}

static void fill_rect(uint32_t *canvas, int cw, int ch,
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

static void draw_gradient(uint32_t *canvas, int cw, int ch,
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

/* ── Refresh directory listing ────────────────────────────────────────── */
static void refresh_listing(void)
{
    entry_count = 0;
    selected_index = -1;
    scroll_offset = 0;

    for (int i = 0; i < MAX_ENTRIES; i++) {
        vfs_node_t child;
        if (vfs_readdir(&current_dir, i, &child) != 0) break;
        entries[entry_count++] = child;
    }
}

/* ── Draw a manila folder icon (16x16) ────────────────────────────────── */
static void draw_folder_icon(uint32_t *canvas, int cw, int ch, int x, int y)
{
    /* Folder tab (top) */
    fill_rect(canvas, cw, ch, x, y, 8, 3, COL_FOLDER);
    /* Folder body */
    fill_rect(canvas, cw, ch, x, y + 3, 16, 11, COL_FOLDER);
    /* Fold shadow */
    fill_rect(canvas, cw, ch, x, y + 3, 16, 2, COL_FOLDER_DARK);
    /* Border */
    for (int i = 0; i < 16; i++) {
        if (y + 14 < ch) canvas[(y + 14) * cw + x + i] = COL_FOLDER_DARK;
    }
}

/* ── Draw a file icon (16x14) ─────────────────────────────────────────── */
static void draw_file_icon(uint32_t *canvas, int cw, int ch, int x, int y)
{
    fill_rect(canvas, cw, ch, x, y, 12, 14, COL_FILE_ICON);
    /* Dog-ear corner */
    fill_rect(canvas, cw, ch, x + 8, y, 4, 4, COL_FILE_BORDER);
    /* Border */
    for (int i = 0; i < 12; i++) {
        if (y < ch && x + i < cw)          canvas[y * cw + x + i] = COL_FILE_BORDER;
        if (y + 13 < ch && x + i < cw)     canvas[(y+13) * cw + x + i] = COL_FILE_BORDER;
    }
    for (int i = 0; i < 14; i++) {
        if (y + i < ch && x < cw)          canvas[(y+i) * cw + x] = COL_FILE_BORDER;
        if (y + i < ch && x + 11 < cw)     canvas[(y+i) * cw + x + 11] = COL_FILE_BORDER;
    }
}

/* ── Paint callback ───────────────────────────────────────────────────── */
static void explorer_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;
    int ch = win->height - 4;

    /* Cabinet-style background gradient */
    draw_gradient(win->canvas, cw, ch, 0, 0, cw, ch,
                  COL_CABINET_TOP, COL_CABINET_BOT);

    /* Toolbar */
    draw_gradient(win->canvas, cw, ch, 0, 0, cw, 32,
                  COL_TOOLBAR_TOP, COL_TOOLBAR_BOT);

    /* Back button */
    draw_gradient(win->canvas, cw, ch, 4, 4, 50, 24,
                  0xD8D0C0, 0xA8A090);
    canvas_draw_string(win->canvas, cw, ch, 12, 8, "Back", COL_TEXT);

    /* Path bar */
    fill_rect(win->canvas, cw, ch, 60, 4, cw - 70, 24, COL_PATH_BG);
    canvas_draw_string(win->canvas, cw, ch, 64, 8, current_path, COL_TEXT);

    /* File list area */
    int list_y = 36;
    int list_h = ch - list_y - 24;  /* Reserve bottom for status */

    for (int vi = 0; vi < VISIBLE_ROWS; vi++) {
        int ei = scroll_offset + vi;
        if (ei >= entry_count) break;

        int ey = list_y + vi * ENTRY_HEIGHT;
        uint32_t bg = (ei == selected_index) ? COL_SELECTED_BG : 0x00000000;
        (void)(ei == selected_index ? COL_TEXT_SEL : COL_TEXT); /* fg used by text overlay */

        if (bg) fill_rect(win->canvas, cw, ch, 0, ey, cw - 16, ENTRY_HEIGHT, bg);

        /* Icon */
        if (entries[ei].type == VFS_DIRECTORY) {
            draw_folder_icon(win->canvas, cw, ch, 8, ey + 4);
        } else {
            draw_file_icon(win->canvas, cw, ch, 8, ey + 5);
        }

        /* Name */
        uint32_t text_c = (ei == selected_index) ? COL_TEXT_SEL : COL_TEXT;
        canvas_draw_string(win->canvas, cw, ch, 28, ey + 4,
                           entries[ei].name, text_c);
    }

    /* Scrollbar track */
    fill_rect(win->canvas, cw, ch, cw - 14, list_y, 12, list_h, COL_SCROLLBAR);
    if (entry_count > 0) {
        int thumb_h = list_h * VISIBLE_ROWS / (entry_count > VISIBLE_ROWS ? entry_count : VISIBLE_ROWS);
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = list_y + (list_h - thumb_h) * scroll_offset /
                      (entry_count > VISIBLE_ROWS ? entry_count - VISIBLE_ROWS : 1);
        fill_rect(win->canvas, cw, ch, cw - 14, thumb_y, 12, thumb_h, 0x807060);
    }

    /* Status bar */
    int sb_y = ch - 20;
    fill_rect(win->canvas, cw, ch, 0, sb_y, cw, 20, 0xD0C8B8);
    /* Show entry count */
    char status[64];
    int si = 0;
    /* itoa inline */
    int ec = entry_count;
    if (ec == 0) { status[si++] = '0'; }
    else {
        char tmp[16]; int ti = 0;
        while (ec > 0) { tmp[ti++] = '0' + ec % 10; ec /= 10; }
        while (ti > 0) status[si++] = tmp[--ti];
    }
    status[si++] = ' '; status[si++] = 'i'; status[si++] = 't';
    status[si++] = 'e'; status[si++] = 'm'; status[si++] = 's'; status[si] = 0;
    canvas_draw_string(win->canvas, cw, ch, 8, sb_y + 2, status, COL_TEXT);
}

/* ── Mouse callback ───────────────────────────────────────────────────── */
static void explorer_mouse(window_t *win, int mx, int my, int buttons)
{
    (void)win;
    if (!(buttons & 1)) return;

    int cw = win->width - 4;
    (void)cw;

    /* Back button */
    if (mx >= 4 && mx < 54 && my >= 4 && my < 28) {
        /* Navigate to parent */
        int len = str_len(current_path);
        if (len > 1) {
            /* Remove trailing component */
            current_path[len - 1] = 0; /* strip trailing '/' */
            while (len > 1 && current_path[len - 2] != '/') {
                current_path[--len - 1] = 0;
            }
            if (len == 0) { current_path[0] = '/'; current_path[1] = 0; }
            vfs_open(current_path, &current_dir);
            refresh_listing();
        }
        return;
    }

    /* File list click */
    int list_y = 36;
    if (my >= list_y) {
        int vi = (my - list_y) / ENTRY_HEIGHT;
        int ei = scroll_offset + vi;
        if (ei < entry_count) {
            if (ei == selected_index && entries[ei].type == VFS_DIRECTORY) {
                /* Double-click simulation: navigate into directory */
                str_cat(current_path, entries[ei].name);
                str_cat(current_path, "/");
                current_dir = entries[ei];
                refresh_listing();
            } else {
                selected_index = ei;
            }
        }
    }
}

/* ── Key callback ─────────────────────────────────────────────────────── */
static void explorer_key(window_t *win, char ascii, int scancode, int pressed)
{
    (void)win; (void)ascii;
    if (!pressed) return;

    /* Up/Down arrow navigation */
    if (scancode == 0x48) { /* Up */
        if (selected_index > 0) selected_index--;
        if (selected_index < scroll_offset) scroll_offset = selected_index;
    }
    if (scancode == 0x50) { /* Down */
        if (selected_index < entry_count - 1) selected_index++;
        if (selected_index >= scroll_offset + VISIBLE_ROWS)
            scroll_offset = selected_index - VISIBLE_ROWS + 1;
    }
    /* Enter to open directory */
    if (ascii == '\n' && selected_index >= 0 && selected_index < entry_count) {
        if (entries[selected_index].type == VFS_DIRECTORY) {
            str_cat(current_path, entries[selected_index].name);
            str_cat(current_path, "/");
            current_dir = entries[selected_index];
            refresh_listing();
        }
    }
}

/* ── Public: launch explorer ──────────────────────────────────────────── */
void explorer_launch(void)
{
    if (explorer_win && explorer_win->active) return;
    explorer_win = (void *)0;

    /* Initialize at root */
    vfs_node_t *root = vfs_get_root();
    if (root) {
        current_dir = *root;
        refresh_listing();
    }

    explorer_win = compositor_create_window("File Explorer", 150, 80, 450, 380);
    if (!explorer_win) return;

    explorer_win->on_paint = explorer_paint;
    explorer_win->on_mouse = explorer_mouse;
    explorer_win->on_key   = explorer_key;
}
