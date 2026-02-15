/*
 * nextOS - browser.c
 * Web browser with URL bar, navigation, and basic HTML renderer
 *
 * Supports: <html>, <head>, <title>, <body>, <h1>-<h6>,
 *           <p>, <b>, <i>, <u>, <br>, <hr>, <a>, <ul>, <ol>, <li>,
 *           <pre>, <code>, bgcolor attribute on body
 */
#include "browser.h"
#include "kernel/ui/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/drivers/net.h"
#include "kernel/net/net_stack.h"
#include "kernel/drivers/timer.h"
#include "kernel/mem/heap.h"

/* ── String Helpers (freestanding) ───────────────────────────────────── */
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_cpy(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = 0;
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

static int str_ncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int str_ncasecmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (uint8_t)ca - (uint8_t)cb;
        if (!ca) return 0;
    }
    return 0;
}

static void str_cat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

static void mem_zero(void *p, int n)
{
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

/* ── Canvas drawing helpers ──────────────────────────────────────────── */
static void fill_rect(uint32_t *canvas, int cw, int ch,
                      int x, int y, int w, int h, uint32_t color)
{
    for (int r = y; r < y + h && r < ch; r++) {
        if (r < 0) continue;
        for (int c = x; c < x + w && c < cw; c++) {
            if (c < 0) continue;
            canvas[r * cw + c] = color;
        }
    }
}

static void draw_hline(uint32_t *canvas, int cw, int ch,
                       int x, int y, int w, uint32_t color)
{
    if (y < 0 || y >= ch) return;
    for (int c = x; c < x + w && c < cw; c++) {
        if (c >= 0) canvas[y * cw + c] = color;
    }
}

/* 8x16 font glyph accessor (from framebuffer.c) */
extern const uint8_t font_8x16[95][16];

static void canvas_draw_char(uint32_t *canvas, int cw, int ch,
                             int x, int y, char c, uint32_t fg)
{
    if (c < 32 || c > 126) return;
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
    for (int i = 0; s[i]; i++) {
        canvas_draw_char(canvas, cw, ch, x + i * 8, y, s[i], fg);
    }
}

/* Bold: draw character twice with 1px offset */
static void canvas_draw_char_bold(uint32_t *canvas, int cw, int ch,
                                  int x, int y, char c, uint32_t fg)
{
    canvas_draw_char(canvas, cw, ch, x, y, c, fg);
    canvas_draw_char(canvas, cw, ch, x + 1, y, c, fg);
}

/* Underline: draw character with underline */
static void canvas_draw_char_underline(uint32_t *canvas, int cw, int ch,
                                       int x, int y, char c, uint32_t fg)
{
    canvas_draw_char(canvas, cw, ch, x, y, c, fg);
    if (y + 15 >= 0 && y + 15 < ch) {
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px >= 0 && px < cw)
                canvas[(y + 15) * cw + px] = fg;
        }
    }
}

/* ── Browser State ───────────────────────────────────────────────────── */
static window_t *browser_win = 0;

#define URL_MAX 256
#define PAGE_BUF_SIZE 32768
#define TITLE_MAX 128

static char url_bar[URL_MAX];
static int  url_cursor = 0;
static int  url_focused = 1;

static char page_buf[PAGE_BUF_SIZE];
static int  page_len = 0;

static char page_title[TITLE_MAX];
static int  scroll_y = 0;

/* Navigation states */
#define NAV_IDLE     0
#define NAV_LOADING  1
#define NAV_DONE     2
#define NAV_ERROR    3
static int nav_state = NAV_IDLE;
static char status_msg[128];

/* ── URL Parsing ─────────────────────────────────────────────────────── */
typedef struct {
    char host[128];
    char path[128];
    uint16_t port;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
    mem_zero(out, sizeof(*out));
    out->port = 80;
    str_cpy(out->path, "/");

    const char *p = url;
    /* Skip http:// */
    if (str_ncasecmp(p, "http://", 7) == 0) p += 7;
    else if (str_ncasecmp(p, "https://", 8) == 0) p += 8;

    /* Extract host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 127)
        out->host[hi++] = *p++;
    out->host[hi] = 0;

    /* Port */
    if (*p == ':') {
        p++;
        out->port = 0;
        while (*p >= '0' && *p <= '9')
            out->port = out->port * 10 + (*p++ - '0');
    }

    /* Path */
    if (*p == '/') {
        str_cpy(out->path, p);
    }

    return hi > 0;
}

/* ── HTML Renderer ───────────────────────────────────────────────────── */
/*
 * Simple streaming HTML renderer. Parses tags and renders text
 * directly into the window canvas.
 */

/* Render state */
typedef struct {
    uint32_t *canvas;
    int cw, ch;
    int x, y;              /* Current cursor position */
    int start_x;           /* Left margin */
    int max_x;             /* Right margin */
    int line_height;        /* Current line height in pixels */
    int scroll;            /* Scroll offset */

    /* Text style */
    int bold;
    int italic;
    int underline;
    int preformatted;
    uint32_t text_color;
    uint32_t bg_color;
    uint32_t link_color;
    int in_link;
    int heading_level;     /* 0 = none, 1-6 = h1-h6 */
    int in_list;
    int list_ordered;
    int list_item;
    int in_title;
    int in_body;
    int in_head;
} render_state_t;

static render_state_t rs;

static uint32_t parse_html_color(const char *s)
{
    if (!s || !s[0]) return 0xFFFFFF;
    if (s[0] == '#') s++;

    /* Parse hex color */
    uint32_t val = 0;
    for (int i = 0; i < 6 && s[i]; i++) {
        val <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    }
    return val;
}

/* Named colors */
static uint32_t named_color(const char *name)
{
    if (str_ncasecmp(name, "white", 5) == 0) return 0xFFFFFF;
    if (str_ncasecmp(name, "black", 5) == 0) return 0x000000;
    if (str_ncasecmp(name, "red", 3) == 0) return 0xFF0000;
    if (str_ncasecmp(name, "green", 5) == 0) return 0x008000;
    if (str_ncasecmp(name, "blue", 4) == 0) return 0x0000FF;
    if (str_ncasecmp(name, "yellow", 6) == 0) return 0xFFFF00;
    if (str_ncasecmp(name, "gray", 4) == 0) return 0x808080;
    if (str_ncasecmp(name, "grey", 4) == 0) return 0x808080;
    if (str_ncasecmp(name, "silver", 6) == 0) return 0xC0C0C0;
    if (str_ncasecmp(name, "navy", 4) == 0) return 0x000080;
    if (str_ncasecmp(name, "teal", 4) == 0) return 0x008080;
    if (name[0] == '#') return parse_html_color(name);
    return 0xFFFFFF;
}

static void render_newline(void)
{
    rs.x = rs.start_x;
    rs.y += rs.line_height;
}

static void render_char(char c)
{
    if (rs.in_title || rs.in_head) return;

    int draw_y = rs.y - rs.scroll;

    /* Word wrap */
    if (rs.x + 8 > rs.max_x && !rs.preformatted) {
        render_newline();
        draw_y = rs.y - rs.scroll;
    }

    if (draw_y >= -16 && draw_y < rs.ch) {
        uint32_t fg = rs.in_link ? rs.link_color : rs.text_color;
        if (rs.bold && rs.underline) {
            canvas_draw_char_bold(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
            /* underline */
            if (draw_y + 15 >= 0 && draw_y + 15 < rs.ch)
                for (int col = 0; col < 9; col++)
                    if (rs.x + col >= 0 && rs.x + col < rs.cw)
                        rs.canvas[(draw_y + 15) * rs.cw + rs.x + col] = fg;
        } else if (rs.bold) {
            canvas_draw_char_bold(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
        } else if (rs.underline) {
            canvas_draw_char_underline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
        } else {
            canvas_draw_char(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
        }
    }
    rs.x += rs.bold ? 9 : 8;
}

static void render_text(const char *text, int len)
{
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            if (rs.preformatted) render_newline();
            else if (i > 0 && text[i-1] != ' ') render_char(' ');
            continue;
        }
        if (c == '\r') continue;
        if (c == '\t') {
            for (int t = 0; t < 4; t++) render_char(' ');
            continue;
        }
        if (!rs.preformatted && c == ' ') {
            /* Collapse whitespace */
            if (i > 0 && text[i-1] == ' ') continue;
        }
        if (c >= 32 && c <= 126)
            render_char(c);
    }
}

/* Extract attribute value from tag: find attr="value" */
static int get_attr(const char *tag, int tag_len, const char *attr, char *out, int out_size)
{
    int alen = str_len(attr);
    for (int i = 0; i < tag_len - alen; i++) {
        if (str_ncasecmp(tag + i, attr, alen) == 0 && tag[i + alen] == '=') {
            int vi = i + alen + 1;
            char quote = 0;
            if (vi < tag_len && (tag[vi] == '"' || tag[vi] == '\''))
                quote = tag[vi++];
            int oi = 0;
            while (vi < tag_len && oi < out_size - 1) {
                if (quote && tag[vi] == quote) break;
                if (!quote && (tag[vi] == ' ' || tag[vi] == '>')) break;
                out[oi++] = tag[vi++];
            }
            out[oi] = 0;
            return 1;
        }
    }
    return 0;
}

static void handle_tag(const char *tag, int tag_len)
{
    /* Skip < and find tag name */
    int is_close = 0;
    int i = 0;
    if (tag[0] == '/') { is_close = 1; i = 1; }

    char name[16];
    int ni = 0;
    while (i < tag_len && ni < 15 && tag[i] != ' ' && tag[i] != '>')  {
        name[ni++] = tag[i++];
        /* Convert to lowercase */
        if (name[ni-1] >= 'A' && name[ni-1] <= 'Z')
            name[ni-1] += 32;
    }
    name[ni] = 0;

    /* Handle tags */
    if (str_cmp(name, "br") == 0) {
        render_newline();
    } else if (str_cmp(name, "hr") == 0) {
        render_newline();
        int draw_y = rs.y - rs.scroll + 8;
        if (draw_y >= 0 && draw_y < rs.ch)
            draw_hline(rs.canvas, rs.cw, rs.ch, rs.start_x, draw_y,
                       rs.max_x - rs.start_x, 0x808080);
        rs.y += 20;
        rs.x = rs.start_x;
    } else if (str_cmp(name, "p") == 0) {
        if (is_close) { rs.y += 8; rs.x = rs.start_x; }
        else { render_newline(); rs.y += 4; }
    } else if (str_cmp(name, "b") == 0 || str_cmp(name, "strong") == 0) {
        rs.bold = !is_close;
    } else if (str_cmp(name, "i") == 0 || str_cmp(name, "em") == 0) {
        rs.italic = !is_close;
    } else if (str_cmp(name, "u") == 0) {
        rs.underline = !is_close;
    } else if (str_cmp(name, "a") == 0) {
        if (is_close) {
            rs.in_link = 0;
            rs.underline = 0;
        } else {
            rs.in_link = 1;
            rs.underline = 1;
        }
    } else if (str_cmp(name, "pre") == 0 || str_cmp(name, "code") == 0) {
        rs.preformatted = !is_close;
        if (!is_close) { render_newline(); }
    } else if (str_cmp(name, "title") == 0) {
        rs.in_title = !is_close;
    } else if (str_cmp(name, "head") == 0) {
        rs.in_head = !is_close;
    } else if (str_cmp(name, "body") == 0) {
        if (!is_close) {
            rs.in_body = 1;
            /* Check for bgcolor attribute */
            char color_val[32];
            if (get_attr(tag, tag_len, "bgcolor", color_val, sizeof(color_val))) {
                rs.bg_color = named_color(color_val);
                /* Fill background */
                fill_rect(rs.canvas, rs.cw, rs.ch, 0, 0, rs.cw, rs.ch, rs.bg_color);
            }
            char text_val[32];
            if (get_attr(tag, tag_len, "text", text_val, sizeof(text_val))) {
                rs.text_color = named_color(text_val);
            }
        }
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
        int level = name[1] - '0';
        if (is_close) {
            rs.bold = 0;
            rs.heading_level = 0;
            rs.line_height = 18;
            render_newline();
            rs.y += 4;
        } else {
            rs.heading_level = level;
            rs.bold = 1;
            render_newline();
            rs.y += 4;
            /* Larger line height for headings */
            int sizes[] = {0, 32, 28, 24, 20, 18, 18};
            rs.line_height = sizes[level];
        }
    } else if (str_cmp(name, "ul") == 0) {
        if (is_close) { rs.in_list = 0; rs.start_x -= 20; }
        else { rs.in_list = 1; rs.list_ordered = 0; rs.list_item = 0; rs.start_x += 20; }
    } else if (str_cmp(name, "ol") == 0) {
        if (is_close) { rs.in_list = 0; rs.start_x -= 20; }
        else { rs.in_list = 1; rs.list_ordered = 1; rs.list_item = 0; rs.start_x += 20; }
    } else if (str_cmp(name, "li") == 0 && !is_close) {
        render_newline();
        rs.list_item++;
        if (rs.list_ordered) {
            char num[8];
            int n = rs.list_item;
            int ni2 = 0;
            if (n >= 10) num[ni2++] = '0' + (n / 10);
            num[ni2++] = '0' + (n % 10);
            num[ni2++] = '.';
            num[ni2++] = ' ';
            num[ni2] = 0;
            render_text(num, ni2);
        } else {
            render_text("* ", 2);
        }
    }
}

/* Decode HTML entities */
static char decode_entity(const char *s, int *advance)
{
    if (str_ncmp(s, "&amp;", 5) == 0) { *advance = 5; return '&'; }
    if (str_ncmp(s, "&lt;", 4) == 0) { *advance = 4; return '<'; }
    if (str_ncmp(s, "&gt;", 4) == 0) { *advance = 4; return '>'; }
    if (str_ncmp(s, "&nbsp;", 6) == 0) { *advance = 6; return ' '; }
    if (str_ncmp(s, "&quot;", 6) == 0) { *advance = 6; return '"'; }
    if (str_ncmp(s, "&apos;", 6) == 0) { *advance = 6; return '\''; }
    *advance = 1;
    return '&';
}

static void render_html(const char *html, int html_len,
                        uint32_t *canvas, int cw, int ch, int scroll)
{
    /* Initialize render state */
    rs.canvas = canvas;
    rs.cw = cw;
    rs.ch = ch;
    rs.x = 8;
    rs.y = 4;
    rs.start_x = 8;
    rs.max_x = cw - 8;
    rs.line_height = 18;
    rs.scroll = scroll;
    rs.bold = 0;
    rs.italic = 0;
    rs.underline = 0;
    rs.preformatted = 0;
    rs.text_color = 0x1A1A1A;
    rs.bg_color = 0xFFFFFF;
    rs.link_color = 0x0066CC;
    rs.in_link = 0;
    rs.heading_level = 0;
    rs.in_list = 0;
    rs.list_ordered = 0;
    rs.list_item = 0;
    rs.in_title = 0;
    rs.in_body = 0;
    rs.in_head = 0;

    /* Clear canvas */
    fill_rect(canvas, cw, ch, 0, 0, cw, ch, rs.bg_color);

    int i = 0;
    while (i < html_len) {
        if (html[i] == '<') {
            /* Find end of tag */
            int tag_start = i + 1;
            int tag_end = tag_start;
            while (tag_end < html_len && html[tag_end] != '>') tag_end++;
            if (tag_end < html_len) {
                /* Capture title text */
                if (rs.in_title) {
                    /* Check if this is </title> */
                    if (str_ncasecmp(html + tag_start, "/title", 6) == 0) {
                        rs.in_title = 0;
                    }
                }
                handle_tag(html + tag_start, tag_end - tag_start);
                i = tag_end + 1;
            } else {
                render_char('<');
                i++;
            }
        } else if (html[i] == '&') {
            int advance;
            char c = decode_entity(html + i, &advance);
            if (rs.in_title) {
                /* Accumulate title */
            } else {
                render_char(c);
            }
            i += advance;
        } else {
            if (rs.in_title) {
                /* Store title */
                int ti = str_len(page_title);
                if (ti < TITLE_MAX - 1) {
                    page_title[ti] = html[i];
                    page_title[ti + 1] = 0;
                }
            } else {
                char c = html[i];
                if (c == '\n' || c == '\r') {
                    if (rs.preformatted) render_newline();
                    else {
                        /* Collapse to single space */
                        if (rs.x > rs.start_x) render_char(' ');
                    }
                } else if (c >= 32 && c <= 126) {
                    render_char(c);
                }
            }
            i++;
        }
    }
}

/* ── Default Homepage ────────────────────────────────────────────────── */
static const char homepage[] =
    "<html><head><title>nextOS Browser</title></head>"
    "<body bgcolor=\"#F0F0F0\">"
    "<h1>Welcome to nextOS Browser</h1>"
    "<hr>"
    "<p>This is the built-in web browser for <b>nextOS 2.5.0</b>.</p>"
    "<p>Type a URL in the address bar above and press Enter to navigate.</p>"
    "<h2>Features</h2>"
    "<ul>"
    "<li><b>HTML Rendering</b> - Basic HTML support with headings, lists, and formatting</li>"
    "<li><b>HTTP/1.1</b> - Fetch web pages over the network</li>"
    "<li><b>DNS Resolution</b> - Resolve hostnames to IP addresses</li>"
    "</ul>"
    "<h2>Tips</h2>"
    "<ol>"
    "<li>Enter a URL like <u>http://example.com</u> in the address bar</li>"
    "<li>Use the scroll wheel or arrow keys to scroll the page</li>"
    "<li>The browser supports basic HTML tags for viewing simple web pages</li>"
    "</ol>"
    "<hr>"
    "<p><i>nextOS 2.5.0 - A next-generation operating system</i></p>"
    "</body></html>";

static void load_homepage(void)
{
    str_cpy(page_buf, homepage);
    page_len = str_len(homepage);
    str_cpy(page_title, "nextOS Browser");
    str_cpy(url_bar, "about:home");
    url_cursor = str_len(url_bar);
    scroll_y = 0;
    nav_state = NAV_DONE;
    str_cpy(status_msg, "Ready");
}

/* ── Navigation ──────────────────────────────────────────────────────── */
static void navigate(const char *url)
{
    if (!url || !url[0]) return;

    /* Handle about: URLs */
    if (str_ncmp(url, "about:", 6) == 0) {
        load_homepage();
        return;
    }

    if (!net_is_available()) {
        str_cpy(page_buf,
            "<html><body bgcolor=\"#FFF0F0\">"
            "<h1>Network Unavailable</h1>"
            "<p>No network adapter was detected.</p>"
            "<p>To use networking in QEMU, start with:</p>"
            "<pre>qemu-system-x86_64 -cdrom nextOS.iso -m 256M -nic model=e1000</pre>"
            "</body></html>");
        page_len = str_len(page_buf);
        str_cpy(page_title, "Network Error");
        scroll_y = 0;
        nav_state = NAV_ERROR;
        str_cpy(status_msg, "No network adapter");
        return;
    }

    nav_state = NAV_LOADING;
    str_cpy(status_msg, "Loading...");

    parsed_url_t purl;
    if (!parse_url(url, &purl)) {
        str_cpy(page_buf,
            "<html><body><h1>Invalid URL</h1>"
            "<p>The URL could not be parsed.</p></body></html>");
        page_len = str_len(page_buf);
        str_cpy(page_title, "Error");
        scroll_y = 0;
        nav_state = NAV_ERROR;
        str_cpy(status_msg, "Invalid URL");
        return;
    }

    str_cpy(status_msg, "Resolving ");
    str_cat(status_msg, purl.host);
    str_cat(status_msg, "...");

    /* Perform HTTP GET */
    int result = http_get(purl.host, purl.port, purl.path, page_buf, PAGE_BUF_SIZE);

    if (result < 0) {
        str_cpy(page_buf,
            "<html><body bgcolor=\"#FFF0F0\">"
            "<h1>Connection Failed</h1>"
            "<p>Could not connect to the server.</p>"
            "<p>Please check the URL and try again.</p>"
            "</body></html>");
        page_len = str_len(page_buf);
        str_cpy(page_title, "Connection Error");
        scroll_y = 0;
        nav_state = NAV_ERROR;
        str_cpy(status_msg, "Connection failed");
        return;
    }

    page_len = result;
    page_title[0] = 0;  /* Will be set by renderer */
    scroll_y = 0;
    nav_state = NAV_DONE;
    str_cpy(status_msg, "Done");
}

/* ── Paint ───────────────────────────────────────────────────────────── */
#define TOOLBAR_H  32
#define STATUS_H   20

static void browser_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;
    int ch = win->height - 4;

    /* Background */
    fill_rect(win->canvas, cw, ch, 0, 0, cw, ch, 0xE8E8E8);

    /* ── Toolbar (URL bar area) ─────────────────────────────────────── */
    /* Toolbar background with gradient */
    for (int y = 0; y < TOOLBAR_H; y++) {
        uint32_t c = 0xDCDCDC + ((y * 0x10 / TOOLBAR_H) << 16) +
                     ((y * 0x10 / TOOLBAR_H) << 8) + (y * 0x10 / TOOLBAR_H);
        draw_hline(win->canvas, cw, ch, 0, y, cw, c);
    }
    /* Bottom border */
    draw_hline(win->canvas, cw, ch, 0, TOOLBAR_H - 1, cw, 0xA0A0A0);

    /* Go button */
    int go_w = 40;
    int go_x = cw - go_w - 6;
    fill_rect(win->canvas, cw, ch, go_x, 4, go_w, 24, 0x4488CC);
    canvas_draw_string(win->canvas, cw, ch, go_x + 10, 8, "Go", 0xFFFFFF);

    /* URL input field */
    int url_x = 8;
    int url_w = go_x - 14;
    fill_rect(win->canvas, cw, ch, url_x, 4, url_w, 24, 0xFFFFFF);
    /* Border */
    draw_hline(win->canvas, cw, ch, url_x, 4, url_w, 0x808080);
    draw_hline(win->canvas, cw, ch, url_x, 27, url_w, 0x808080);
    for (int y = 4; y < 28; y++) {
        if (url_x >= 0 && url_x < cw)
            win->canvas[y * cw + url_x] = 0x808080;
        if (url_x + url_w - 1 >= 0 && url_x + url_w - 1 < cw)
            win->canvas[y * cw + url_x + url_w - 1] = 0x808080;
    }

    /* URL text */
    int max_chars = (url_w - 8) / 8;
    int start = 0;
    if (url_cursor > max_chars - 2)
        start = url_cursor - max_chars + 2;
    for (int i = start; url_bar[i] && (i - start) < max_chars; i++) {
        canvas_draw_char(win->canvas, cw, ch,
                         url_x + 4 + (i - start) * 8, 8, url_bar[i], 0x1A1A1A);
    }

    /* Cursor blink */
    if (url_focused) {
        uint64_t t = timer_get_ticks();
        if ((t / 500) & 1) {
            int cx = url_x + 4 + (url_cursor - start) * 8;
            if (cx >= url_x && cx < url_x + url_w)
                fill_rect(win->canvas, cw, ch, cx, 8, 2, 16, 0x1A1A1A);
        }
    }

    /* ── Page Content Area ──────────────────────────────────────────── */
    int content_y = TOOLBAR_H;
    int content_h = ch - TOOLBAR_H - STATUS_H;

    if (page_len > 0) {
        /* Render HTML into a sub-canvas area */
        uint32_t *content_canvas = win->canvas + content_y * cw;
        /* Clear content area */
        fill_rect(win->canvas, cw, ch, 0, content_y, cw, content_h, 0xFFFFFF);
        render_html(page_buf, page_len, content_canvas, cw, content_h, scroll_y);
    } else {
        fill_rect(win->canvas, cw, ch, 0, content_y, cw, content_h, 0xFFFFFF);
        if (nav_state == NAV_LOADING) {
            canvas_draw_string(win->canvas, cw, ch, cw / 2 - 40,
                               content_y + content_h / 2, "Loading...", 0x808080);
        }
    }

    /* ── Status Bar ─────────────────────────────────────────────────── */
    int sb_y = ch - STATUS_H;
    fill_rect(win->canvas, cw, ch, 0, sb_y, cw, STATUS_H, 0xE0E0E0);
    draw_hline(win->canvas, cw, ch, 0, sb_y, cw, 0xC0C0C0);
    canvas_draw_string(win->canvas, cw, ch, 6, sb_y + 3, status_msg, 0x606060);

    /* Network status indicator */
    uint32_t ind_color = net_is_available() ? 0x40A040 : 0xA04040;
    fill_rect(win->canvas, cw, ch, cw - 14, sb_y + 5, 8, 8, ind_color);
}

/* ── Mouse ───────────────────────────────────────────────────────────── */
static int prev_buttons = 0;

static void browser_mouse(window_t *win, int mx, int my, int buttons)
{
    if (!win) return;
    int cw = win->width - 4;
    int click = (buttons & 1) && !(prev_buttons & 1);
    prev_buttons = buttons;

    if (click) {
        /* URL bar click */
        if (my >= 4 && my < 28 && mx >= 8 && mx < cw - 52) {
            url_focused = 1;
            int rel_x = mx - 12;
            url_cursor = rel_x / 8;
            int len = str_len(url_bar);
            if (url_cursor > len) url_cursor = len;
            if (url_cursor < 0) url_cursor = 0;
            return;
        }

        /* Go button click */
        if (my >= 4 && my < 28 && mx >= cw - 46 && mx < cw - 6) {
            url_focused = 0;
            navigate(url_bar);
            return;
        }

        /* Content area click */
        if (my >= TOOLBAR_H) {
            url_focused = 0;
        }
    }

    /* Scroll with mouse wheel (buttons & 8 = scroll up, & 16 = scroll down) */
    /* Note: In nextOS, mouse scrolling is handled via button flags */
}

/* ── Keyboard ────────────────────────────────────────────────────────── */
static void browser_key(window_t *win, char ascii, int scancode, int pressed)
{
    if (!win || !pressed) return;
    (void)scancode;

    if (url_focused) {
        if (ascii == '\n' || ascii == '\r') {
            /* Navigate on Enter */
            url_focused = 0;
            navigate(url_bar);
            return;
        }
        if (ascii == '\b' || scancode == 0x0E) {
            /* Backspace */
            if (url_cursor > 0) {
                int len = str_len(url_bar);
                for (int i = url_cursor - 1; i < len; i++)
                    url_bar[i] = url_bar[i + 1];
                url_cursor--;
            }
            return;
        }
        if (ascii >= 32 && ascii <= 126) {
            int len = str_len(url_bar);
            if (len < URL_MAX - 1) {
                /* Insert character at cursor */
                for (int i = len + 1; i > url_cursor; i--)
                    url_bar[i] = url_bar[i - 1];
                url_bar[url_cursor] = ascii;
                url_cursor++;
            }
            return;
        }
    }

    /* Page scrolling */
    if (scancode == 0x48) {  /* Up arrow */
        if (scroll_y > 0) scroll_y -= 20;
    } else if (scancode == 0x50) {  /* Down arrow */
        scroll_y += 20;
    } else if (scancode == 0x49) {  /* Page Up */
        scroll_y -= 200;
        if (scroll_y < 0) scroll_y = 0;
    } else if (scancode == 0x51) {  /* Page Down */
        scroll_y += 200;
    } else if (scancode == 0x47) {  /* Home */
        scroll_y = 0;
    }
}

/* ── Close ───────────────────────────────────────────────────────────── */
static void browser_close(window_t *win)
{
    (void)win;
    browser_win = 0;
}

/* ── Launch ──────────────────────────────────────────────────────────── */
void browser_launch(void)
{
    if (browser_win && browser_win->active) return;

    browser_win = compositor_create_window("Browser", 80, 40, 640, 480);
    if (!browser_win) return;

    browser_win->on_paint = browser_paint;
    browser_win->on_mouse = browser_mouse;
    browser_win->on_key   = browser_key;
    browser_win->on_close = browser_close;

    /* Load default homepage */
    load_homepage();
    prev_buttons = 0;
}
