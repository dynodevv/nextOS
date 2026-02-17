/*
 * nextOS - notepad.c
 * Notepad application — yellow legal pad style text editor
 *
 * Features:
 *   - Full text editing (cursor, backspace, enter, arrow keys)
 *   - Open / Save file dialogs using kernel VFS + disk I/O
 *   - Visual design: ruled yellow paper with red margin line
 */
#include "notepad.h"
#include "kernel/ui/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/fs/vfs.h"
#include "kernel/mem/heap.h"
#include "kernel/drivers/keyboard.h"

/* ── Text buffer ──────────────────────────────────────────────────────── */
#define TEXT_MAX    8192
#define LINE_HEIGHT 18
#define CHAR_WIDTH  8
#define MAX_PATH    256

static window_t *notepad_win = (void *)0;
static char      text_buf[TEXT_MAX];
static int       text_len = 0;
static int       cursor_pos = 0;
static int       scroll_y = 0;
static char      file_path[MAX_PATH] = "";
static int       dialog_mode = 0;  /* 0=none, 1=open, 2=save, 3=unsaved prompt */
static char      dialog_input[MAX_PATH];
static int       dialog_input_len = 0;
static int       np_scrollbar_dragging = 0;
static int       np_scrollbar_drag_offset = 0;
static int       modified = 0;  /* Track unsaved changes */
static int       select_all_active = 0;       /* Text select-all flag */
static int       dialog_select_all = 0;       /* Dialog input select-all flag */

/* ── Skeuomorphic colours ─────────────────────────────────────────────── */
#define COL_PAPER      0xFFF8C8   /* Legal pad yellow */
#define COL_PAPER_DARK 0xF0E8A0
#define COL_RULED_LINE 0xC0D0E0   /* Blue rule lines */
#define COL_MARGIN     0xE05050   /* Red margin line */
#define COL_TEXT_COL   0x1A1A30
#define COL_CURSOR     0xE03030
#define COL_TOOLBAR_T  0xE8DCC8
#define COL_TOOLBAR_B  0xC8BCA8
#define COL_BTN_T      0xD8D0C0
#define COL_BTN_B      0xA8A090
#define COL_DIALOG_BG  0xE8E0D0
#define COL_DIALOG_BRD 0x8B7D6B
#define COL_INPUT_BG   0xFFFFF0

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

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void fill_rect(uint32_t *c, int cw, int ch,
                      int x, int y, int w, int h, uint32_t color)
{
    for (int r = y; r < y + h && r < ch; r++) {
        if (r < 0) continue;
        for (int col = x; col < x + w && col < cw; col++) {
            if (col < 0) continue;
            c[r * cw + col] = color;
        }
    }
}

static void draw_gradient(uint32_t *c, int cw, int ch,
                          int x, int y, int w, int h,
                          uint32_t top, uint32_t bot)
{
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= ch) continue;
        int tr = (top>>16)&0xFF, tg = (top>>8)&0xFF, tb = top&0xFF;
        int bot_r = (bot>>16)&0xFF, bot_g = (bot>>8)&0xFF, bot_b = bot&0xFF;
        int denom = (h > 1 ? h - 1 : 1);
        int rr = tr + (bot_r - tr) * r / denom;
        int rg = tg + (bot_g - tg) * r / denom;
        int rb = tb + (bot_b - tb) * r / denom;
        if (rr < 0) rr = 0;
        if (rr > 255) rr = 255;
        if (rg < 0) rg = 0;
        if (rg > 255) rg = 255;
        if (rb < 0) rb = 0;
        if (rb > 255) rb = 255;
        uint32_t color = ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
        for (int col = x; col < x + w && col < cw; col++) {
            if (col < 0) continue;
            c[py * cw + col] = color;
        }
    }
}

/* Get column of cursor on its current line */
static int cursor_column(void)
{
    int col = 0;
    for (int i = cursor_pos - 1; i >= 0 && text_buf[i] != '\n'; i--)
        col++;
    return col;
}

/* ── Draw text content ────────────────────────────────────────────────── */
static void draw_text_area(uint32_t *canvas, int cw, int ch)
{
    int text_x = 50;  /* After margin line */
    int text_y_start = 36;
    int line = 0;
    int col = 0;

    for (int i = 0; i <= text_len; i++) {
        int screen_y = text_y_start + line * LINE_HEIGHT - scroll_y;

        /* Draw cursor */
        if (i == cursor_pos && screen_y >= text_y_start && screen_y < ch - 4) {
            fill_rect(canvas, cw, ch, text_x + col * CHAR_WIDTH,
                      screen_y, 2, LINE_HEIGHT, COL_CURSOR);
        }

        if (i >= text_len) break;

        if (text_buf[i] == '\n') {
            line++;
            col = 0;
        } else {
            /* Render character glyph into canvas */
            if (screen_y >= text_y_start && screen_y < ch - 4 &&
                text_x + col * CHAR_WIDTH < cw - 4) {
                canvas_draw_char(canvas, cw, ch,
                                 text_x + col * CHAR_WIDTH, screen_y,
                                 text_buf[i], COL_TEXT_COL);
            }
            col++;
        }
    }
}

/* ── Dialog overlay ───────────────────────────────────────────────────── */
static void draw_dialog(uint32_t *canvas, int cw, int ch)
{
    if (dialog_mode == 3) {
        /* Unsaved changes prompt */
        int dw = 320, dh = 120;
        int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

        fill_rect(canvas, cw, ch, dx, dy, dw, dh, COL_DIALOG_BG);
        /* Border */
        for (int i = dx; i < dx + dw; i++) {
            if (dy >= 0 && dy < ch) canvas[dy * cw + i] = COL_DIALOG_BRD;
            if (dy + dh - 1 < ch) canvas[(dy + dh - 1) * cw + i] = COL_DIALOG_BRD;
        }
        for (int i = dy; i < dy + dh; i++) {
            if (dx >= 0) canvas[i * cw + dx] = COL_DIALOG_BRD;
            if (dx + dw - 1 < cw) canvas[i * cw + dx + dw - 1] = COL_DIALOG_BRD;
        }

        canvas_draw_string(canvas, cw, ch, dx + 20, dy + 12,
                           "Unsaved changes!", 0x1A1A1A);
        canvas_draw_string(canvas, cw, ch, dx + 20, dy + 36,
                           "Save before creating", 0x1A1A1A);
        canvas_draw_string(canvas, cw, ch, dx + 20, dy + 52,
                           "a new document?", 0x1A1A1A);

        /* Save button */
        draw_gradient(canvas, cw, ch, dx + 20, dy + dh - 36, 70, 24,
                      COL_BTN_T, COL_BTN_B);
        canvas_draw_string(canvas, cw, ch, dx + 32, dy + dh - 32, "Save", 0x1A1A1A);

        /* Discard button */
        draw_gradient(canvas, cw, ch, dx + 100, dy + dh - 36, 80, 24,
                      COL_BTN_T, COL_BTN_B);
        canvas_draw_string(canvas, cw, ch, dx + 106, dy + dh - 32, "Discard", 0x1A1A1A);

        /* Cancel button */
        draw_gradient(canvas, cw, ch, dx + 190, dy + dh - 36, 80, 24,
                      COL_BTN_T, COL_BTN_B);
        canvas_draw_string(canvas, cw, ch, dx + 198, dy + dh - 32, "Cancel", 0x1A1A1A);
        return;
    }

    int dw = 300, dh = 110;
    int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

    fill_rect(canvas, cw, ch, dx, dy, dw, dh, COL_DIALOG_BG);
    /* Border */
    for (int i = dx; i < dx + dw; i++) {
        if (dy >= 0 && dy < ch) canvas[dy * cw + i] = COL_DIALOG_BRD;
        if (dy + dh - 1 < ch) canvas[(dy + dh - 1) * cw + i] = COL_DIALOG_BRD;
    }
    for (int i = dy; i < dy + dh; i++) {
        if (dx >= 0) canvas[i * cw + dx] = COL_DIALOG_BRD;
        if (dx + dw - 1 < cw) canvas[i * cw + dx + dw - 1] = COL_DIALOG_BRD;
    }

    /* Title */
    const char *title = (dialog_mode == 1) ? "Open File:" : "Save File:";
    canvas_draw_string(canvas, cw, ch, dx + 20, dy + 12, title, 0x1A1A1A);

    /* Hint: files default to /Documents/ */
    canvas_draw_string(canvas, cw, ch, dx + 20, dy + 28, "(in /Documents/)", 0x808080);

    /* Input field */
    fill_rect(canvas, cw, ch, dx + 20, dy + 44, dw - 40, 24, COL_INPUT_BG);
    /* Draw current input text */
    canvas_draw_string(canvas, cw, ch, dx + 24, dy + 48, dialog_input, 0x1A1A1A);

    /* OK button */
    draw_gradient(canvas, cw, ch, dx + dw - 80, dy + dh - 36, 60, 24,
                  COL_BTN_T, COL_BTN_B);
    canvas_draw_string(canvas, cw, ch, dx + dw - 68, dy + dh - 32, "OK", 0x1A1A1A);
}

/* ── Paint callback ───────────────────────────────────────────────────── */
static void notepad_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;
    int ch = win->height - 4;

    /* Toolbar */
    draw_gradient(win->canvas, cw, ch, 0, 0, cw, 32,
                  COL_TOOLBAR_T, COL_TOOLBAR_B);

    /* File | Open | Save buttons */
    draw_gradient(win->canvas, cw, ch, 4, 4, 50, 24, COL_BTN_T, COL_BTN_B);
    canvas_draw_string(win->canvas, cw, ch, 12, 8, "New", 0x1A1A1A);
    draw_gradient(win->canvas, cw, ch, 60, 4, 50, 24, COL_BTN_T, COL_BTN_B);
    canvas_draw_string(win->canvas, cw, ch, 64, 8, "Open", 0x1A1A1A);
    draw_gradient(win->canvas, cw, ch, 116, 4, 50, 24, COL_BTN_T, COL_BTN_B);
    canvas_draw_string(win->canvas, cw, ch, 120, 8, "Save", 0x1A1A1A);

    /* Paper area */
    int paper_y = 32;
    int paper_h = ch - paper_y;
    fill_rect(win->canvas, cw, ch, 0, paper_y, cw - 14, paper_h, COL_PAPER);

    /* Ruled lines */
    for (int y = paper_y + LINE_HEIGHT; y < ch; y += LINE_HEIGHT) {
        for (int x = 0; x < cw - 14; x++) {
            win->canvas[y * cw + x] = COL_RULED_LINE;
        }
    }

    /* Red margin line */
    for (int y = paper_y; y < ch; y++) {
        if (44 < cw) win->canvas[y * cw + 44] = COL_MARGIN;
        if (45 < cw) win->canvas[y * cw + 45] = COL_MARGIN;
    }

    /* Text content */
    draw_text_area(win->canvas, cw, ch);

    /* Scrollbar on right side */
    {
        int sb_x = cw - 14;
        fill_rect(win->canvas, cw, ch, sb_x, paper_y, 14, paper_h, 0xD0C8B8);
        /* Count total lines */
        int total_lines = 1;
        for (int i = 0; i < text_len; i++) {
            if (text_buf[i] == '\n') total_lines++;
        }
        int visible_lines = paper_h / LINE_HEIGHT;
        if (visible_lines < 1) visible_lines = 1;
        if (total_lines > visible_lines) {
            int max_scroll = (total_lines - visible_lines) * LINE_HEIGHT;
            if (max_scroll < 1) max_scroll = 1;
            int thumb_h = paper_h * visible_lines / total_lines;
            if (thumb_h < 20) thumb_h = 20;
            int thumb_y = paper_y + (paper_h - thumb_h) * scroll_y / max_scroll;
            if (thumb_y + thumb_h > paper_y + paper_h)
                thumb_y = paper_y + paper_h - thumb_h;
            fill_rect(win->canvas, cw, ch, sb_x + 2, thumb_y, 10, thumb_h, 0x807060);
        }
    }

    /* Dialog overlay if active */
    if (dialog_mode != 0) {
        draw_dialog(win->canvas, cw, ch);
    }
}

/* Build a full path from user input — default to /Documents/ if no path prefix */
static void build_full_path(const char *input, char *out, int max_len)
{
    if (max_len <= 0) return;
    /* If input already starts with / and is a ramfs path, use as-is */
    if (input[0] == '/') {
        int i = 0;
        while (input[i] && i < max_len - 1) { out[i] = input[i]; i++; }
        out[i] = 0;
        return;
    }
    /* Prepend /Documents/ */
    const char *prefix = "/Documents/";
    int pi = 0;
    while (prefix[pi] && pi < max_len - 1) { out[pi] = prefix[pi]; pi++; }
    int ii = 0;
    while (input[ii] && pi < max_len - 1) { out[pi++] = input[ii++]; }
    out[pi] = 0;
}

/* ── File I/O ─────────────────────────────────────────────────────────── */
static void load_file(const char *path)
{
    char full_path[MAX_PATH];
    build_full_path(path, full_path, MAX_PATH);

    vfs_node_t node;
    if (vfs_open(full_path, &node) != 0) return;
    if (node.type != VFS_FILE) return;

    int bytes = vfs_read(&node, 0, TEXT_MAX - 1, text_buf);
    if (bytes > 0) {
        text_len = bytes;
        text_buf[text_len] = 0;
    } else {
        text_len = 0;
        text_buf[0] = 0;
    }
    cursor_pos = 0;
    scroll_y = 0;
    modified = 0;

    /* Store file path only on successful read */
    if (bytes > 0) {
        int i = 0;
        while (full_path[i] && i < MAX_PATH - 1) { file_path[i] = full_path[i]; i++; }
        file_path[i] = 0;
    } else {
        file_path[0] = 0;
    }
}

static void save_file(const char *path)
{
    char full_path[MAX_PATH];
    build_full_path(path, full_path, MAX_PATH);

    vfs_node_t node;
    /* Try to open existing file first */
    if (vfs_open(full_path, &node) != 0) {
        /* File doesn't exist, try to create it */
        if (vfs_create(full_path, VFS_FILE) != 0) return;
        if (vfs_open(full_path, &node) != 0) return;
    }
    vfs_write(&node, 0, text_len, text_buf);
    modified = 0;

    /* Update stored path */
    int i = 0;
    while (full_path[i] && i < MAX_PATH - 1) { file_path[i] = full_path[i]; i++; }
    file_path[i] = 0;
}

/* ── New document helper ──────────────────────────────────────────────── */
static void new_document(void)
{
    text_len = 0;
    text_buf[0] = 0;
    cursor_pos = 0;
    scroll_y = 0;
    file_path[0] = 0;
    modified = 0;
}

/* ── Mouse callback ───────────────────────────────────────────────────── */
static void notepad_mouse(window_t *win, int mx, int my, int buttons)
{
    (void)win;
    int cw = win->width - 4;
    int ch = win->height - 4;
    int paper_y = 32;
    int paper_h = ch - paper_y;

    /* Handle scrollbar drag */
    if (np_scrollbar_dragging) {
        if (!(buttons & 1)) {
            np_scrollbar_dragging = 0;
            return;
        }
        int total_lines = 1;
        for (int i = 0; i < text_len; i++) {
            if (text_buf[i] == '\n') total_lines++;
        }
        int visible_lines = paper_h / LINE_HEIGHT;
        if (visible_lines < 1) visible_lines = 1;
        int max_scroll = (total_lines - visible_lines) * LINE_HEIGHT;
        if (max_scroll < 1) max_scroll = 1;
        if (total_lines > visible_lines) {
            int thumb_h = paper_h * visible_lines / total_lines;
            if (thumb_h < 20) thumb_h = 20;
            int track_range = paper_h - thumb_h;
            if (track_range > 0) {
                int thumb_top = my - np_scrollbar_drag_offset;
                int new_scroll = (thumb_top - paper_y) * max_scroll / track_range;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;
                scroll_y = new_scroll;
            }
        }
        return;
    }

    if (!(buttons & 1)) return;

    /* Dialog mode clicks */
    if (dialog_mode) {
        if (dialog_mode == 3) {
            /* Unsaved changes prompt */
            int dw = 320, dh = 120;
            int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

            /* Save button */
            if (mx >= dx + 20 && mx < dx + 90 &&
                my >= dy + dh - 36 && my < dy + dh - 12) {
                if (file_path[0]) {
                    save_file(file_path);
                    new_document();
                } else {
                    /* Need to ask for filename first */
                    dialog_mode = 2;
                    dialog_input_len = 0;
                    dialog_input[0] = 0;
                }
                return;
            }
            /* Discard button */
            if (mx >= dx + 100 && mx < dx + 180 &&
                my >= dy + dh - 36 && my < dy + dh - 12) {
                new_document();
                dialog_mode = 0;
                return;
            }
            /* Cancel button */
            if (mx >= dx + 190 && mx < dx + 270 &&
                my >= dy + dh - 36 && my < dy + dh - 12) {
                dialog_mode = 0;
                return;
            }
            return;
        }

        int dw = 300, dh = 110;
        int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

        /* OK button */
        if (mx >= dx + dw - 80 && mx < dx + dw - 20 &&
            my >= dy + dh - 36 && my < dy + dh - 12) {
            dialog_input[dialog_input_len] = 0;
            if (dialog_mode == 1) {
                load_file(dialog_input);
            } else if (dialog_mode == 2) {
                save_file(dialog_input);
            }
            dialog_mode = 0;
        }
        return;
    }

    /* Toolbar: New button */
    if (mx >= 4 && mx < 54 && my >= 4 && my < 28) {
        if (modified && text_len > 0) {
            /* Ask about unsaved changes */
            dialog_mode = 3;
        } else {
            new_document();
        }
        return;
    }
    /* Toolbar: Open button */
    if (mx >= 60 && mx < 110 && my >= 4 && my < 28) {
        dialog_mode = 1;
        dialog_input_len = 0;
        dialog_input[0] = 0;
        return;
    }
    /* Toolbar: Save button */
    if (mx >= 116 && mx < 166 && my >= 4 && my < 28) {
        if (file_path[0]) {
            save_file(file_path);
        } else {
            dialog_mode = 2;
            dialog_input_len = 0;
            dialog_input[0] = 0;
        }
        return;
    }

    /* Scrollbar click/drag on right side */
    if (mx >= cw - 14 && my >= paper_y && my < paper_y + paper_h) {
        int total_lines = 1;
        for (int i = 0; i < text_len; i++) {
            if (text_buf[i] == '\n') total_lines++;
        }
        int visible_lines = paper_h / LINE_HEIGHT;
        if (visible_lines < 1) visible_lines = 1;
        if (total_lines > visible_lines) {
            int max_scroll = (total_lines - visible_lines) * LINE_HEIGHT;
            if (max_scroll < 1) max_scroll = 1;
            int thumb_h = paper_h * visible_lines / total_lines;
            if (thumb_h < 20) thumb_h = 20;
            int track_range = paper_h - thumb_h;
            int thumb_y = paper_y;
            if (track_range > 0)
                thumb_y = paper_y + track_range * scroll_y / max_scroll;
            if (my >= thumb_y && my < thumb_y + thumb_h) {
                np_scrollbar_dragging = 1;
                np_scrollbar_drag_offset = my - thumb_y;
            } else if (my < thumb_y) {
                scroll_y -= visible_lines * LINE_HEIGHT;
                if (scroll_y < 0) scroll_y = 0;
            } else {
                scroll_y += visible_lines * LINE_HEIGHT;
                if (scroll_y > max_scroll) scroll_y = max_scroll;
            }
        }
        return;
    }

    /* Click in text area to position cursor */
    if (my >= 36) {
        int line_click = (my - 36 + scroll_y) / LINE_HEIGHT;
        int col_click  = (mx - 50) / CHAR_WIDTH;
        if (col_click < 0) col_click = 0;

        /* Find position in text buffer */
        int line = 0, col = 0, pos = 0;
        for (int i = 0; i < text_len; i++) {
            if (line == line_click && col == col_click) { pos = i; break; }
            if (text_buf[i] == '\n') { line++; col = 0; }
            else col++;
            pos = i + 1;
        }
        if (pos > text_len) pos = text_len;
        cursor_pos = pos;
    }
}

/* ── Key callback ─────────────────────────────────────────────────────── */
static void notepad_key(window_t *win, char ascii, int scancode, int pressed)
{
    (void)win;
    if (!pressed) return;
    int ctrl = keyboard_ctrl_held();

    /* Dialog mode typing */
    if (dialog_mode) {
        if (dialog_mode == 3) return;  /* No typing in unsaved prompt */
        /* CTRL+A: select all in dialog input */
        if (ctrl && scancode == 0x1E) {
            dialog_select_all = 1;
            return;
        }
        if (ascii == '\b') {
            if (dialog_select_all) {
                dialog_input_len = 0;
                dialog_input[0] = 0;
                dialog_select_all = 0;
            } else if (dialog_input_len > 0) {
                dialog_input[--dialog_input_len] = 0;
            }
        } else if (ascii == '\n') {
            dialog_select_all = 0;
            dialog_input[dialog_input_len] = 0;
            if (dialog_mode == 1) load_file(dialog_input);
            else if (dialog_mode == 2) save_file(dialog_input);
            dialog_mode = 0;
        } else if (ascii >= 32 && dialog_input_len < MAX_PATH - 1) {
            if (dialog_select_all) {
                dialog_input_len = 0;
                dialog_select_all = 0;
            }
            dialog_input[dialog_input_len++] = ascii;
            dialog_input[dialog_input_len] = 0;
        }
        return;
    }

    /* CTRL+A: select all text */
    if (ctrl && scancode == 0x1E) {
        select_all_active = 1;
        return;
    }

    /* Backspace */
    if (ascii == '\b') {
        if (select_all_active) {
            text_len = 0;
            cursor_pos = 0;
            text_buf[0] = 0;
            select_all_active = 0;
            modified = 1;
        } else if (cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < text_len - 1; i++)
                text_buf[i] = text_buf[i + 1];
            text_len--;
            cursor_pos--;
            modified = 1;
        }
        return;
    }

    /* Arrow keys clear selection */
    if (scancode == 0x4B || scancode == 0x4D || scancode == 0x48 || scancode == 0x50)
        select_all_active = 0;

    /* Arrow keys (scan codes) */
    if (scancode == 0x4B && cursor_pos > 0) { cursor_pos--; return; }         /* Left  */
    if (scancode == 0x4D && cursor_pos < text_len) { cursor_pos++; return; }   /* Right */
    if (scancode == 0x48) { /* Up */
        int col = cursor_column();
        /* Move to previous line */
        int pos = cursor_pos - col - 1;  /* end of previous line */
        if (pos >= 0) {
            int prev_start = pos;
            while (prev_start > 0 && text_buf[prev_start - 1] != '\n')
                prev_start--;
            int prev_len = pos - prev_start;
            cursor_pos = prev_start + (col < prev_len ? col : prev_len);
        }
        return;
    }
    if (scancode == 0x50) { /* Down */
        int col = cursor_column();
        /* Find start of next line */
        int pos = cursor_pos;
        while (pos < text_len && text_buf[pos] != '\n') pos++;
        if (pos < text_len) {
            pos++; /* skip newline */
            int next_end = pos;
            while (next_end < text_len && text_buf[next_end] != '\n')
                next_end++;
            int next_len = next_end - pos;
            cursor_pos = pos + (col < next_len ? col : next_len);
        }
        return;
    }

    /* Normal character insertion */
    if (ascii >= 32 || ascii == '\n' || ascii == '\t') {
        if (select_all_active) {
            text_len = 0;
            cursor_pos = 0;
            select_all_active = 0;
        }
        if (text_len < TEXT_MAX - 1) {
            for (int i = text_len; i > cursor_pos; i--)
                text_buf[i] = text_buf[i - 1];
            text_buf[cursor_pos] = ascii;
            text_len++;
            cursor_pos++;
            modified = 1;
        }
    }
}

/* ── Close callback ────────────────────────────────────────────────────── */
static void notepad_close(window_t *win)
{
    (void)win;
    notepad_win = (void *)0;
}

/* ── Public: launch notepad ───────────────────────────────────────────── */
void notepad_launch(void)
{
    if (notepad_win && notepad_win->active) return;
    notepad_win = (void *)0;

    text_len = 0;
    cursor_pos = 0;
    text_buf[0] = 0;

    notepad_win = compositor_create_window("Notepad", 200, 100, 500, 400);
    if (!notepad_win) return;

    notepad_win->on_paint = notepad_paint;
    notepad_win->on_mouse = notepad_mouse;
    notepad_win->on_key   = notepad_key;
    notepad_win->on_close = notepad_close;
}

/* ── Public: open a file in notepad ──────────────────────────────────── */
void notepad_open_file(const char *path)
{
    /* Launch notepad window if not already open */
    if (!notepad_win || !notepad_win->active) {
        notepad_win = (void *)0;
        text_len = 0;
        cursor_pos = 0;
        text_buf[0] = 0;
        file_path[0] = 0;
        modified = 0;
        dialog_mode = 0;

        notepad_win = compositor_create_window("Notepad", 200, 100, 500, 400);
        if (!notepad_win) return;
        notepad_win->on_paint = notepad_paint;
        notepad_win->on_mouse = notepad_mouse;
        notepad_win->on_key   = notepad_key;
        notepad_win->on_close = notepad_close;
    }

    /* Load the file using the absolute path */
    load_file(path);
}
