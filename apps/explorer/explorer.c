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
#define SIDEBAR_W    120

static window_t *explorer_win = (void *)0;

static vfs_node_t current_dir;
static vfs_node_t entries[MAX_ENTRIES];
static int         entry_count = 0;
static int         selected_index = -1;
static int         scroll_offset = 0;
static char        current_path[PATH_MAX_LEN] = "/";
static int         scrollbar_dragging = 0;
static int         scrollbar_drag_offset = 0;

/* Context menu state */
static int         ctx_menu_open = 0;
static int         ctx_menu_x = 0, ctx_menu_y = 0;
static int         ctx_menu_target = -1;  /* index of file right-clicked */

/* Clipboard for copy/cut/paste */
static char        clipboard_path[PATH_MAX_LEN] = "";
static char        clipboard_name[VFS_MAX_NAME] = "";
static int         clipboard_cut = 0;  /* 0=copy, 1=cut */

/* Sidebar quick-access folders */
typedef struct { const char *label; const char *path; } sidebar_item_t;
static const sidebar_item_t sidebar_folders[] = {
    { "Desktop",    "/Desktop/"    },
    { "Documents",  "/Documents/"  },
    { "Images",     "/Images/"     },
};
#define SIDEBAR_FOLDER_COUNT 3

/* System-protected paths (cannot rename/delete/modify) */
static int is_system_path(const char *path)
{
    /* Root entries that are system-critical */
    const char *sys_dirs[] = {
        "/boot/", "/kernel/", "/grub/", "/boot", "/kernel", "/grub",
        "/lost+found/", "/lost+found",
    };
    for (int i = 0; i < 8; i++) {
        const char *s = sys_dirs[i];
        const char *p = path;
        int match = 1;
        while (*s) {
            if (*p != *s) { match = 0; break; }
            s++; p++;
        }
        if (match) return 1;
    }
    /* Root directory itself is protected */
    if (path[0] == '/' && path[1] == 0) return 1;
    return 0;
}

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

/* Check if a specific entry at index is a system file/dir */
static int is_system_entry(int idx)
{
    if (idx < 0 || idx >= entry_count) return 0;
    /* System files at root level */
    if (current_path[0] == '/' && current_path[1] == 0) {
        const char *name = entries[idx].name;
        const char *sys_names[] = {
            "boot", "grub", "kernel", "lost+found",
        };
        for (int i = 0; i < 4; i++) {
            const char *s = sys_names[i];
            const char *n = name;
            int match = 1;
            while (*s) {
                if (*n != *s) { match = 0; break; }
                s++; n++;
            }
            if (match && *n == 0) return 1;
        }
    }
    return is_system_path(current_path);
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

    /* ── Sidebar ─────────────────────────────────────────────────── */
    int list_y = 36;
    int list_h = ch - list_y - 24;
    fill_rect(win->canvas, cw, ch, 0, list_y, SIDEBAR_W, list_h, 0xC8BDA5);
    /* Sidebar divider */
    for (int row = list_y; row < list_y + list_h; row++) {
        if (SIDEBAR_W < cw) win->canvas[row * cw + SIDEBAR_W] = 0x907050;
    }

    /* Quick-access folders section */
    canvas_draw_string(win->canvas, cw, ch, 8, list_y + 4, "Favorites", 0x605040);
    for (int i = 0; i < SIDEBAR_FOLDER_COUNT; i++) {
        int iy = list_y + 24 + i * 22;
        draw_folder_icon(win->canvas, cw, ch, 8, iy + 3);
        canvas_draw_string(win->canvas, cw, ch, 28, iy + 3,
                           sidebar_folders[i].label, COL_TEXT);
    }

    /* Separator */
    int sep_y = list_y + 24 + SIDEBAR_FOLDER_COUNT * 22 + 4;
    for (int x = 4; x < SIDEBAR_W - 4; x++) {
        if (sep_y < ch) win->canvas[sep_y * cw + x] = 0xA09080;
    }

    /* Root directory link */
    canvas_draw_string(win->canvas, cw, ch, 8, sep_y + 8, "Root (/)", COL_TEXT);

    /* ── File list area (to the right of sidebar) ────────────────── */
    int file_x = SIDEBAR_W + 2;
    int file_w = cw - file_x - 14;

    for (int vi = 0; vi < VISIBLE_ROWS; vi++) {
        int ei = scroll_offset + vi;
        if (ei >= entry_count) break;

        int ey = list_y + vi * ENTRY_HEIGHT;
        uint32_t bg = (ei == selected_index) ? COL_SELECTED_BG : 0x00000000;

        if (bg) fill_rect(win->canvas, cw, ch, file_x, ey, file_w, ENTRY_HEIGHT, bg);

        /* Icon */
        if (entries[ei].type == VFS_DIRECTORY) {
            draw_folder_icon(win->canvas, cw, ch, file_x + 4, ey + 4);
        } else {
            draw_file_icon(win->canvas, cw, ch, file_x + 4, ey + 5);
        }

        /* Name */
        uint32_t text_c = (ei == selected_index) ? COL_TEXT_SEL : COL_TEXT;
        canvas_draw_string(win->canvas, cw, ch, file_x + 24, ey + 4,
                           entries[ei].name, text_c);

        /* System file indicator (lock icon hint) */
        if (is_system_path(current_path) || is_system_entry(ei)) {
            canvas_draw_string(win->canvas, cw, ch, file_x + file_w - 16, ey + 4,
                               "*", 0xA04040);
        }
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

    /* Context menu overlay */
    if (ctx_menu_open) {
        #define CTX_MENU_W 100
        #define CTX_MENU_ITEM_H 22
        static const char *ctx_labels[] = { "Rename", "Delete", "Copy", "Cut", "Paste" };
        int ctx_items = 5;
        int cmh = ctx_items * CTX_MENU_ITEM_H + 4;
        int cmx = ctx_menu_x;
        int cmy = ctx_menu_y;
        /* Clamp to canvas */
        if (cmx + CTX_MENU_W > cw) cmx = cw - CTX_MENU_W;
        if (cmy + cmh > ch) cmy = ch - cmh;
        /* Shadow */
        fill_rect(win->canvas, cw, ch, cmx + 3, cmy + 3, CTX_MENU_W, cmh, 0x404040);
        /* Background */
        fill_rect(win->canvas, cw, ch, cmx, cmy, CTX_MENU_W, cmh, 0xF0EAD8);
        /* Border */
        for (int i = cmx; i < cmx + CTX_MENU_W; i++) {
            if (cmy >= 0 && cmy < ch) win->canvas[cmy * cw + i] = 0x807060;
            if (cmy + cmh - 1 < ch) win->canvas[(cmy + cmh - 1) * cw + i] = 0x807060;
        }
        for (int i = cmy; i < cmy + cmh; i++) {
            if (cmx >= 0) win->canvas[i * cw + cmx] = 0x807060;
            if (cmx + CTX_MENU_W - 1 < cw) win->canvas[i * cw + cmx + CTX_MENU_W - 1] = 0x807060;
        }
        /* Items */
        int protected = (ctx_menu_target >= 0 && is_system_entry(ctx_menu_target));
        for (int i = 0; i < ctx_items; i++) {
            int iy = cmy + 2 + i * CTX_MENU_ITEM_H;
            uint32_t fg = COL_TEXT;
            /* Grey out rename/delete/cut for system files */
            if (protected && (i == 0 || i == 1 || i == 3)) fg = 0xA0A0A0;
            canvas_draw_string(win->canvas, cw, ch, cmx + 8, iy + 3,
                               ctx_labels[i], fg);
        }
        #undef CTX_MENU_W
        #undef CTX_MENU_ITEM_H
    }
}

/* ── Navigate to a path ────────────────────────────────────────────────── */
static void navigate_to(const char *path)
{
    int i = 0;
    while (path[i] && i < PATH_MAX_LEN - 1) {
        current_path[i] = path[i]; i++;
    }
    current_path[i] = 0;
    vfs_open(current_path, &current_dir);
    refresh_listing();
}

/* ── Build full path for entry ────────────────────────────────────────── */
static void build_entry_path(int idx, char *out, int max_len)
{
    int i = 0;
    const char *p = current_path;
    while (*p && i < max_len - 1) out[i++] = *p++;
    const char *n = entries[idx].name;
    while (*n && i < max_len - 1) out[i++] = *n++;
    out[i] = 0;
}

/* ── Mouse callback ───────────────────────────────────────────────────── */
static void explorer_mouse(window_t *win, int mx, int my, int buttons)
{
    (void)win;
    int cw = win->width - 4;
    int ch = win->height - 4;
    int list_y = 36;
    int list_h = ch - list_y - 24;
    int max_scroll = entry_count > VISIBLE_ROWS ? entry_count - VISIBLE_ROWS : 0;
    int file_x = SIDEBAR_W + 2;

    /* Handle scrollbar drag */
    if (scrollbar_dragging) {
        if (!(buttons & 1)) {
            scrollbar_dragging = 0;
            return;
        }
        if (max_scroll > 0 && list_h > 0) {
            int thumb_h = list_h * VISIBLE_ROWS / (entry_count > VISIBLE_ROWS ? entry_count : VISIBLE_ROWS);
            if (thumb_h < 20) thumb_h = 20;
            int track_range = list_h - thumb_h;
            if (track_range > 0) {
                int thumb_top = my - scrollbar_drag_offset;
                int new_offset = (thumb_top - list_y) * max_scroll / track_range;
                if (new_offset < 0) new_offset = 0;
                if (new_offset > max_scroll) new_offset = max_scroll;
                scroll_offset = new_offset;
            }
        }
        return;
    }

    /* Right-click: open context menu */
    if (buttons & 2) {
        if (my >= list_y && mx >= file_x) {
            int vi = (my - list_y) / ENTRY_HEIGHT;
            int ei = scroll_offset + vi;
            if (ei < entry_count) {
                ctx_menu_open = 1;
                ctx_menu_x = mx;
                ctx_menu_y = my;
                ctx_menu_target = ei;
                selected_index = ei;
            }
        }
        return;
    }

    /* Left-click while context menu is open */
    if ((buttons & 1) && ctx_menu_open) {
        /* Check if click is inside context menu */
        int cmx = ctx_menu_x, cmy = ctx_menu_y;
        int cmw = 100, cmh = 5 * 22 + 4;
        if (cmx + cmw > cw) cmx = cw - cmw;
        if (cmy + cmh > ch) cmy = ch - cmh;
        if (mx >= cmx && mx < cmx + cmw && my >= cmy && my < cmy + cmh) {
            int item = (my - cmy - 2) / 22;
            int protected = (ctx_menu_target >= 0 && is_system_entry(ctx_menu_target));
            if (item >= 0 && item < 5) {
                char path_buf[PATH_MAX_LEN];
                if (ctx_menu_target >= 0)
                    build_entry_path(ctx_menu_target, path_buf, PATH_MAX_LEN);
                switch (item) {
                case 0: /* Rename */
                    if (!protected && ctx_menu_target >= 0) {
                        /* Rename not yet supported by FS driver */
                        vfs_rename(path_buf, path_buf);
                    }
                    break;
                case 1: /* Delete */
                    if (!protected && ctx_menu_target >= 0) {
                        vfs_delete(path_buf);
                        refresh_listing();
                    }
                    break;
                case 2: /* Copy */
                    if (ctx_menu_target >= 0) {
                        int ci = 0;
                        const char *p = path_buf;
                        while (*p && ci < PATH_MAX_LEN - 1) clipboard_path[ci++] = *p++;
                        clipboard_path[ci] = 0;
                        ci = 0;
                        const char *n = entries[ctx_menu_target].name;
                        while (*n && ci < VFS_MAX_NAME - 1) clipboard_name[ci++] = *n++;
                        clipboard_name[ci] = 0;
                        clipboard_cut = 0;
                    }
                    break;
                case 3: /* Cut */
                    if (!protected && ctx_menu_target >= 0) {
                        int ci = 0;
                        const char *p = path_buf;
                        while (*p && ci < PATH_MAX_LEN - 1) clipboard_path[ci++] = *p++;
                        clipboard_path[ci] = 0;
                        ci = 0;
                        const char *n = entries[ctx_menu_target].name;
                        while (*n && ci < VFS_MAX_NAME - 1) clipboard_name[ci++] = *n++;
                        clipboard_name[ci] = 0;
                        clipboard_cut = 1;
                    }
                    break;
                case 4: /* Paste */
                    if (clipboard_path[0]) {
                        /* Paste: copy file data to current directory */
                        char dest_path[PATH_MAX_LEN];
                        int di = 0;
                        const char *cp = current_path;
                        while (*cp && di < PATH_MAX_LEN - 1) dest_path[di++] = *cp++;
                        cp = clipboard_name;
                        while (*cp && di < PATH_MAX_LEN - 1) dest_path[di++] = *cp++;
                        dest_path[di] = 0;
                        /* Read source and write to dest */
                        vfs_node_t src_node;
                        if (vfs_open(clipboard_path, &src_node) == 0 &&
                            src_node.type == VFS_FILE) {
                            char copy_buf[512];
                            int bytes = vfs_read(&src_node, 0, 512, copy_buf);
                            if (bytes > 0) {
                                vfs_node_t dst_node;
                                if (vfs_open(dest_path, &dst_node) == 0) {
                                    vfs_write(&dst_node, 0, bytes, copy_buf);
                                }
                            }
                        }
                        if (clipboard_cut) {
                            vfs_delete(clipboard_path);
                            clipboard_path[0] = 0;
                            clipboard_name[0] = 0;
                        }
                        refresh_listing();
                    }
                    break;
                }
            }
        }
        ctx_menu_open = 0;
        return;
    }

    if (!(buttons & 1)) return;

    /* Close context menu on any left click */
    ctx_menu_open = 0;

    /* Back button */
    if (mx >= 4 && mx < 54 && my >= 4 && my < 28) {
        /* Navigate to parent */
        int len = str_len(current_path);
        if (len > 1) {
            current_path[len - 1] = 0;
            while (len > 1 && current_path[len - 2] != '/') {
                current_path[--len - 1] = 0;
            }
            if (len == 0) { current_path[0] = '/'; current_path[1] = 0; }
            vfs_open(current_path, &current_dir);
            refresh_listing();
        }
        return;
    }

    /* Sidebar clicks */
    if (mx < SIDEBAR_W && my >= list_y) {
        int rel_y = my - list_y;
        /* Quick-access folders */
        if (rel_y >= 24 && rel_y < 24 + SIDEBAR_FOLDER_COUNT * 22) {
            int idx = (rel_y - 24) / 22;
            if (idx >= 0 && idx < SIDEBAR_FOLDER_COUNT) {
                navigate_to(sidebar_folders[idx].path);
            }
            return;
        }
        /* Root directory link */
        int sep_y_rel = 24 + SIDEBAR_FOLDER_COUNT * 22 + 4;
        if (rel_y >= sep_y_rel + 4 && rel_y < sep_y_rel + 24) {
            navigate_to("/");
            return;
        }
        return;
    }

    /* Scrollbar thumb drag start */
    if (mx >= cw - 14 && mx < cw - 2 && my >= list_y && my < list_y + list_h) {
        if (entry_count > VISIBLE_ROWS) {
            int thumb_h = list_h * VISIBLE_ROWS / entry_count;
            if (thumb_h < 20) thumb_h = 20;
            int track_range = list_h - thumb_h;
            int thumb_y = list_y;
            if (track_range > 0 && max_scroll > 0)
                thumb_y = list_y + track_range * scroll_offset / max_scroll;
            if (my >= thumb_y && my < thumb_y + thumb_h) {
                scrollbar_dragging = 1;
                scrollbar_drag_offset = my - thumb_y;
                return;
            }
            if (my < thumb_y) {
                scroll_offset -= VISIBLE_ROWS;
                if (scroll_offset < 0) scroll_offset = 0;
            } else {
                scroll_offset += VISIBLE_ROWS;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            }
        }
        return;
    }

    /* File list click (right of sidebar) */
    if (my >= list_y && mx >= file_x) {
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

/* ── Close callback ────────────────────────────────────────────────────── */
static void explorer_close(window_t *win)
{
    (void)win;
    explorer_win = (void *)0;
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

    explorer_win = compositor_create_window("File Explorer", 120, 60, 550, 400);
    if (!explorer_win) return;

    explorer_win->on_paint = explorer_paint;
    explorer_win->on_mouse = explorer_mouse;
    explorer_win->on_key   = explorer_key;
    explorer_win->on_close = explorer_close;
}
