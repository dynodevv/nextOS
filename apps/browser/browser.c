/*
 * nextOS - browser.c
 * Web browser with URL bar, navigation, and HTML renderer
 *
 * Supports: <html>, <head>, <title>, <body>, <h1>-<h6>,
 *           <p>, <b>, <i>, <u>, <br>, <hr>, <a>, <ul>, <ol>, <li>,
 *           <pre>, <code>, <div>, <span>, <table>, <tr>, <td>, <th>,
 *           <blockquote>, <center>, <font>, <input>, <button>, <form>,
 *           <sup>, <sub>, <s>, <strike>, <small>, <big>, <dl>, <dt>, <dd>,
 *           <img> (placeholder), <style>/<script> (skip content),
 *           bgcolor/text/color attributes, HTTPS via TLS
 *
 * Features: Back/Forward/Refresh navigation, vertical scrollbar
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

/* ── Browser State ───────────────────────────────────────────────────── */
static window_t *browser_win = 0;

#define URL_MAX      256
#define PAGE_BUF_SIZE 32768
#define TITLE_MAX    128
#define TOOLBAR_H    32
#define STATUS_H     20
#define SCROLLBAR_W  14
#define NAV_BTN_W    28

static char url_bar[URL_MAX];
static int  url_cursor = 0;
static int  url_focused = 1;

static char page_buf[PAGE_BUF_SIZE];
static int  page_len = 0;

static char page_title[TITLE_MAX];
static int  scroll_y = 0;
static int  content_total_h = 0;  /* Total rendered content height */

/* Clickable link regions */
#define MAX_LINKS 256
typedef struct {
    int x, y, w, h;       /* Bounding box (in content coordinates, not scrolled) */
    char href[URL_MAX];   /* Link URL */
} link_region_t;
static link_region_t page_links[MAX_LINKS];
static int link_count = 0;

/* Form state */
#define MAX_FORM_INPUTS 16
#define FORM_INPUT_MAX  128
typedef struct {
    int x, y, w, h;                  /* Bounding box in content coords */
    char name[64];                   /* Input name attribute */
    char value[FORM_INPUT_MAX];      /* Current value */
    int is_submit;                   /* 1 if submit button */
    int user_modified;               /* 1 if user has typed into this input */
} form_input_t;
static form_input_t form_inputs[MAX_FORM_INPUTS];
static int form_input_count = 0;
static int focused_input = -1;       /* Index of focused text input, -1 if none */
static char form_action[URL_MAX];    /* Form action URL */
static char form_method[8];          /* "get" or "post" */

/* Saved form values to preserve user input across re-renders */
#define SAVED_INPUT_MAX MAX_FORM_INPUTS
typedef struct {
    char name[64];
    char value[FORM_INPUT_MAX];
} saved_input_t;
static saved_input_t saved_inputs[SAVED_INPUT_MAX];
static int saved_input_count = 0;
static char saved_focused_name[64]; /* Name of focused input to restore */

/* Hover state for status bar */
static char hover_url[URL_MAX];

/* CSS style rules parsed from <style> blocks */
#define MAX_CSS_RULES 32
#define CSS_SELECTOR_MAX 32
#define CSS_VALUE_MAX 64
typedef struct {
    char selector[CSS_SELECTOR_MAX]; /* e.g. "body", "a", "h1" */
    uint32_t color;
    int has_color;
    uint32_t bg_color;
    int has_bg_color;
    int bold;        /* 1=force bold, -1=no change */
    int italic;      /* 1=force italic, -1=no change */
    int underline;   /* 1=force underline, -1=no change */
    int text_align;  /* 0=default, 1=left, 2=center, 3=right */
} css_rule_t;
static css_rule_t css_rules[MAX_CSS_RULES];
static int css_rule_count = 0;

/* Navigation states */
#define NAV_IDLE     0
#define NAV_LOADING  1
#define NAV_DONE     2
#define NAV_ERROR    3
static int nav_state = NAV_IDLE;
static char status_msg[128];

/* History stack for back/forward */
#define HISTORY_MAX 32
static char history_urls[HISTORY_MAX][URL_MAX];
static int  history_count = 0;
static int  history_pos = -1;  /* current position in history */

static void history_push(const char *url)
{
    /* Truncate any forward history */
    history_pos++;
    if (history_pos >= HISTORY_MAX) {
        /* Shift history down */
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            str_cpy(history_urls[i], history_urls[i + 1]);
        history_pos = HISTORY_MAX - 1;
    }
    str_cpy(history_urls[history_pos], url);
    history_count = history_pos + 1;
}

/* ── URL Parsing ─────────────────────────────────────────────────────── */
typedef struct {
    char host[128];
    char path[128];
    uint16_t port;
    int is_https;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
    mem_zero(out, sizeof(*out));
    out->port = 80;
    out->is_https = 0;
    str_cpy(out->path, "/");

    const char *p = url;
    /* Skip http:// or https:// */
    if (str_ncasecmp(p, "https://", 8) == 0) {
        p += 8;
        out->port = 443;
        out->is_https = 1;
    } else if (str_ncasecmp(p, "http://", 7) == 0) {
        p += 7;
    }

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

/* Resolve a potentially relative URL against the current page URL */
static void resolve_url(const char *href, const char *base_url, char *out, int out_size)
{
    out[0] = 0;
    if (!href || !href[0]) { str_cpy(out, base_url); return; }

    /* Absolute URL */
    if (str_ncasecmp(href, "http://", 7) == 0 || str_ncasecmp(href, "https://", 8) == 0) {
        int i;
        for (i = 0; href[i] && i < out_size - 1; i++) out[i] = href[i];
        out[i] = 0;
        return;
    }

    /* Protocol-relative //host/path */
    if (href[0] == '/' && href[1] == '/') {
        /* Use same scheme as base */
        int oi = 0;
        if (str_ncasecmp(base_url, "https://", 8) == 0) {
            str_cpy(out, "https:");
            oi = 6;
        } else {
            str_cpy(out, "http:");
            oi = 5;
        }
        for (int i = 0; href[i] && oi < out_size - 1; i++) out[oi++] = href[i];
        out[oi] = 0;
        return;
    }

    /* Extract scheme + host from base_url */
    int oi = 0;
    const char *bp = base_url;
    /* Copy scheme */
    while (*bp && oi < out_size - 1) {
        out[oi++] = *bp++;
        if (bp[-1] == '/' && bp[0] == '/') { out[oi++] = *bp++; break; }
    }
    /* Copy host */
    while (*bp && *bp != '/' && oi < out_size - 1)
        out[oi++] = *bp++;

    if (href[0] == '/') {
        /* Absolute path */
        for (int i = 0; href[i] && oi < out_size - 1; i++) out[oi++] = href[i];
    } else {
        /* Relative path - append to base directory */
        if (*bp == '/') {
            /* Find last / in path */
            const char *last_slash = bp;
            for (const char *s = bp; *s; s++) {
                if (*s == '/') last_slash = s;
            }
            /* Copy base path up to last slash */
            while (bp <= last_slash && oi < out_size - 1)
                out[oi++] = *bp++;
        } else {
            out[oi++] = '/';
        }
        for (int i = 0; href[i] && oi < out_size - 1; i++) out[oi++] = href[i];
    }
    out[oi] = 0;
}

/* URL-encode a string for form submission (minimal: encode spaces as +) */
static int url_encode(const char *src, char *dst, int max)
{
    int oi = 0;
    for (int i = 0; src[i] && oi < max - 3; i++) {
        char c = src[i];
        if (c == ' ') { dst[oi++] = '+'; }
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[oi++] = c;
        } else {
            dst[oi++] = '%';
            uint8_t hi = ((uint8_t)c >> 4) & 0xF;
            uint8_t lo = (uint8_t)c & 0xF;
            dst[oi++] = hi < 10 ? '0' + hi : 'A' + (hi - 10);
            dst[oi++] = lo < 10 ? '0' + lo : 'A' + (lo - 10);
        }
    }
    dst[oi] = 0;
    return oi;
}

/* ── Form Value Persistence ──────────────────────────────────────────── */
/* Save user-typed form values before re-render so they survive render_html reset */
static void save_form_inputs(void)
{
    saved_input_count = 0;
    saved_focused_name[0] = 0;
    for (int i = 0; i < form_input_count && saved_input_count < SAVED_INPUT_MAX; i++) {
        form_input_t *fi = &form_inputs[i];
        if (!fi->is_submit && fi->name[0]) {
            saved_input_t *si = &saved_inputs[saved_input_count++];
            str_cpy(si->name, fi->name);
            str_cpy(si->value, fi->value);
        }
    }
    if (focused_input >= 0 && focused_input < form_input_count)
        str_cpy(saved_focused_name, form_inputs[focused_input].name);
}

/* Restore user-typed value for a newly registered input (if previously saved) */
static void restore_form_input(form_input_t *fi)
{
    if (!fi->name[0]) return;
    for (int i = 0; i < saved_input_count; i++) {
        if (str_cmp(saved_inputs[i].name, fi->name) == 0) {
            str_cpy(fi->value, saved_inputs[i].value);
            fi->user_modified = 1;
            break;
        }
    }
}

/* Restore focused_input index by name after re-render */
static void restore_focused_input(void)
{
    if (!saved_focused_name[0]) return;
    for (int i = 0; i < form_input_count; i++) {
        if (str_cmp(form_inputs[i].name, saved_focused_name) == 0 &&
            !form_inputs[i].is_submit) {
            focused_input = i;
            return;
        }
    }
}

/* ── CSS Parser ─────────────────────────────────────────────────────── */
/* Forward declarations */
static uint32_t named_color(const char *name);
static uint32_t parse_html_color(const char *s);

static void css_skip_whitespace(const char *s, int *pos, int len)
{
    while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' ||
           s[*pos] == '\n' || s[*pos] == '\r')) (*pos)++;
}

static void parse_css_block(const char *css, int css_len)
{
    int pos = 0;
    while (pos < css_len && css_rule_count < MAX_CSS_RULES) {
        css_skip_whitespace(css, &pos, css_len);
        if (pos >= css_len) break;

        /* Skip comments */
        if (pos + 1 < css_len && css[pos] == '/' && css[pos+1] == '*') {
            pos += 2;
            while (pos + 1 < css_len && !(css[pos] == '*' && css[pos+1] == '/')) pos++;
            if (pos + 1 < css_len) pos += 2;
            continue;
        }

        /* Read selector */
        char selector[CSS_SELECTOR_MAX];
        int si = 0;
        while (pos < css_len && css[pos] != '{' && css[pos] != ',' && si < CSS_SELECTOR_MAX - 1) {
            char c = css[pos];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                selector[si++] = c;
            pos++;
        }
        selector[si] = 0;

        /* Skip complex selectors with . # : [ + > ~ */
        int skip_rule = 0;
        for (int i = 0; selector[i]; i++) {
            if (selector[i] == '.' || selector[i] == '#' || selector[i] == ':' ||
                selector[i] == '[' || selector[i] == '+' || selector[i] == '>' ||
                selector[i] == '~') { skip_rule = 1; break; }
        }

        if (css[pos] == ',') { pos++; continue; } /* Multi-selector: skip */
        if (pos >= css_len || css[pos] != '{') continue;
        pos++; /* Skip { */

        /* Find end of declarations */
        int decl_start = pos;
        int brace_depth = 1;
        while (pos < css_len && brace_depth > 0) {
            if (css[pos] == '{') brace_depth++;
            else if (css[pos] == '}') brace_depth--;
            pos++;
        }
        int decl_end = pos - 1;

        if (skip_rule || !selector[0]) continue;

        css_rule_t *rule = &css_rules[css_rule_count];
        str_cpy(rule->selector, selector);
        rule->has_color = 0;
        rule->has_bg_color = 0;
        rule->bold = -1;
        rule->italic = -1;
        rule->underline = -1;
        rule->text_align = 0;

        /* Parse declarations */
        int dp = decl_start;
        while (dp < decl_end) {
            css_skip_whitespace(css, &dp, decl_end);
            /* Read property name */
            char prop[32];
            int pi = 0;
            while (dp < decl_end && css[dp] != ':' && css[dp] != ';' && pi < 31) {
                char c = css[dp];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != ' ' && c != '\t') prop[pi++] = c;
                dp++;
            }
            prop[pi] = 0;
            if (dp >= decl_end || css[dp] != ':') { dp++; continue; }
            dp++; /* Skip : */
            css_skip_whitespace(css, &dp, decl_end);

            /* Read value */
            char val[CSS_VALUE_MAX];
            int vi = 0;
            while (dp < decl_end && css[dp] != ';' && css[dp] != '}' && vi < CSS_VALUE_MAX - 1) {
                val[vi++] = css[dp++];
            }
            /* Trim trailing whitespace */
            while (vi > 0 && (val[vi-1] == ' ' || val[vi-1] == '\t' ||
                   val[vi-1] == '\n' || val[vi-1] == '\r')) vi--;
            val[vi] = 0;
            if (dp < decl_end && css[dp] == ';') dp++;

            /* Apply property */
            if (str_cmp(prop, "color") == 0) {
                rule->color = named_color(val);
                rule->has_color = 1;
            } else if (str_cmp(prop, "background-color") == 0 || str_cmp(prop, "background") == 0) {
                rule->bg_color = named_color(val);
                rule->has_bg_color = 1;
            } else if (str_cmp(prop, "font-weight") == 0) {
                rule->bold = (str_ncasecmp(val, "bold", 4) == 0) ? 1 : 0;
            } else if (str_cmp(prop, "font-style") == 0) {
                rule->italic = (str_ncasecmp(val, "italic", 6) == 0) ? 1 : 0;
            } else if (str_cmp(prop, "text-decoration") == 0) {
                rule->underline = (str_ncasecmp(val, "underline", 9) == 0) ? 1 : 0;
            } else if (str_cmp(prop, "text-align") == 0) {
                if (str_ncasecmp(val, "center", 6) == 0) rule->text_align = 2;
                else if (str_ncasecmp(val, "right", 5) == 0) rule->text_align = 3;
                else rule->text_align = 1;
            }
        }
        css_rule_count++;
    }
}

/* Look up a CSS rule for a given tag name and apply it */
static css_rule_t *css_lookup(const char *tag_name)
{
    for (int i = 0; i < css_rule_count; i++) {
        if (str_cmp(css_rules[i].selector, tag_name) == 0)
            return &css_rules[i];
    }
    return 0;
}

/* Parse inline style attribute and apply to render state */
static void apply_inline_style(const char *tag, int tag_len);

/* ── HTML Renderer ───────────────────────────────────────────────────── */
/*
 * Streaming HTML renderer. Parses tags and renders text
 * directly into the window canvas.
 * Supports basic centering via two-pass line measurement.
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
    int strikethrough;
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
    int in_style;          /* Inside <style> - skip content */
    int in_script;         /* Inside <script> - skip content */
    int centered;          /* <center> or align=center */
    int in_table;
    int in_table_row;
    int table_col;         /* Current column in table row */
    int table_col_x;       /* X position for current table column */

    /* Font color/style stack (simplified: 4 levels) */
#define COLOR_STACK_MAX 4
    uint32_t color_stack[COLOR_STACK_MAX];
    int color_stack_depth;

    /* Current link tracking */
    char link_href[URL_MAX];  /* href of current <a> */
    int link_start_x;         /* x position where link text starts */
    int link_start_y;         /* y position where link text starts */

    /* Centering support: accumulate line content for deferred render */
    int center_defer;         /* 1 if we're deferring render for centering */
#define CENTER_BUF_MAX 256
    char center_buf[CENTER_BUF_MAX]; /* Buffer for centered line */
    int center_buf_len;
    int center_line_bold;     /* bold state for centered line */

    /* Whitespace suppression after block tags */
    int last_was_block;       /* 1 if last element was a block tag (suppress leading space) */
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
    if (!name || !name[0]) return 0xFFFFFF;
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
    if (str_ncasecmp(name, "maroon", 6) == 0) return 0x800000;
    if (str_ncasecmp(name, "olive", 5) == 0) return 0x808000;
    if (str_ncasecmp(name, "aqua", 4) == 0) return 0x00FFFF;
    if (str_ncasecmp(name, "cyan", 4) == 0) return 0x00FFFF;
    if (str_ncasecmp(name, "fuchsia", 7) == 0) return 0xFF00FF;
    if (str_ncasecmp(name, "magenta", 7) == 0) return 0xFF00FF;
    if (str_ncasecmp(name, "lime", 4) == 0) return 0x00FF00;
    if (str_ncasecmp(name, "purple", 6) == 0) return 0x800080;
    if (str_ncasecmp(name, "orange", 6) == 0) return 0xFFA500;
    if (str_ncasecmp(name, "pink", 4) == 0) return 0xFFC0CB;
    if (str_ncasecmp(name, "brown", 5) == 0) return 0xA52A2A;
    if (str_ncasecmp(name, "coral", 5) == 0) return 0xFF7F50;
    if (str_ncasecmp(name, "crimson", 7) == 0) return 0xDC143C;
    if (str_ncasecmp(name, "darkblue", 8) == 0) return 0x00008B;
    if (str_ncasecmp(name, "darkred", 7) == 0) return 0x8B0000;
    if (str_ncasecmp(name, "gold", 4) == 0) return 0xFFD700;
    if (str_ncasecmp(name, "indigo", 6) == 0) return 0x4B0082;
    if (str_ncasecmp(name, "ivory", 5) == 0) return 0xFFFFF0;
    if (str_ncasecmp(name, "khaki", 5) == 0) return 0xF0E68C;
    if (str_ncasecmp(name, "lavender", 8) == 0) return 0xE6E6FA;
    if (str_ncasecmp(name, "linen", 5) == 0) return 0xFAF0E6;
    if (str_ncasecmp(name, "salmon", 6) == 0) return 0xFA8072;
    if (str_ncasecmp(name, "tan", 3) == 0) return 0xD2B48C;
    if (str_ncasecmp(name, "tomato", 6) == 0) return 0xFF6347;
    if (str_ncasecmp(name, "violet", 6) == 0) return 0xEE82EE;
    if (str_ncasecmp(name, "wheat", 5) == 0) return 0xF5DEB3;
    if (str_ncasecmp(name, "lightgray", 9) == 0 || str_ncasecmp(name, "lightgrey", 9) == 0) return 0xD3D3D3;
    if (str_ncasecmp(name, "lightgreen", 10) == 0) return 0x90EE90;
    if (str_ncasecmp(name, "lightblue", 9) == 0) return 0xADD8E6;
    if (str_ncasecmp(name, "darkgray", 8) == 0 || str_ncasecmp(name, "darkgrey", 8) == 0) return 0xA9A9A9;
    if (str_ncasecmp(name, "darkgreen", 9) == 0) return 0x006400;
    if (name[0] == '#') return parse_html_color(name);
    return 0xFFFFFF;
}

/* Flush centered line buffer: render accumulated text centered */
static void flush_center_buf(void)
{
    if (rs.center_buf_len <= 0) return;
    int char_w = rs.center_line_bold ? 9 : 8;
    int text_w = rs.center_buf_len * char_w;
    int avail = rs.max_x - rs.start_x;
    int offset = (avail - text_w) / 2;
    if (offset < 0) offset = 0;
    int draw_x = rs.start_x + offset;
    int draw_y = rs.y - rs.scroll;

    if (draw_y >= -16 && draw_y < rs.ch) {
        uint32_t fg = rs.in_link ? rs.link_color : rs.text_color;
        for (int i = 0; i < rs.center_buf_len; i++) {
            if (rs.center_line_bold)
                canvas_draw_char_bold(rs.canvas, rs.cw, rs.ch,
                    draw_x + i * char_w, draw_y, rs.center_buf[i], fg);
            else
                canvas_draw_char(rs.canvas, rs.cw, rs.ch,
                    draw_x + i * char_w, draw_y, rs.center_buf[i], fg);
            if (rs.underline && draw_y + 15 >= 0 && draw_y + 15 < rs.ch)
                for (int col = 0; col < char_w; col++)
                    if (draw_x + i * char_w + col >= 0 && draw_x + i * char_w + col < rs.cw)
                        rs.canvas[(draw_y + 15) * rs.cw + draw_x + i * char_w + col] = fg;
        }
    }
    /* Update rs.x to end of centered text for link tracking */
    rs.x = rs.start_x + offset + rs.center_buf_len * char_w;
    rs.center_buf_len = 0;
}

static void render_newline(void)
{
    if (rs.centered && rs.center_buf_len > 0)
        flush_center_buf();
    rs.x = rs.start_x;
    rs.y += rs.line_height;
    rs.center_buf_len = 0;
}

static void render_char(char c)
{
    if (rs.in_title || rs.in_head || rs.in_style || rs.in_script) return;

    /* Centering mode: accumulate chars in buffer */
    if (rs.centered) {
        int char_w = rs.bold ? 9 : 8;
        /* Check word wrap */
        int buf_w = rs.center_buf_len * char_w;
        if (buf_w + char_w > rs.max_x - rs.start_x && !rs.preformatted) {
            flush_center_buf();
            rs.y += rs.line_height;
        }
        if (rs.center_buf_len < CENTER_BUF_MAX - 1) {
            rs.center_buf[rs.center_buf_len++] = c;
            rs.center_line_bold = rs.bold;
        }
        /* Keep rs.x updated for link region tracking */
        rs.x += char_w;
        return;
    }

    int draw_y = rs.y - rs.scroll;

    /* Word wrap */
    if (rs.x + 8 > rs.max_x && !rs.preformatted) {
        render_newline();
        draw_y = rs.y - rs.scroll;
    }

    if (draw_y >= -16 && draw_y < rs.ch) {
        uint32_t fg = rs.in_link ? rs.link_color : rs.text_color;
        if (rs.bold) {
            canvas_draw_char_bold(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
        } else {
            canvas_draw_char(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, c, fg);
        }
        if (rs.underline) {
            if (draw_y + 15 >= 0 && draw_y + 15 < rs.ch)
                for (int col = 0; col < (rs.bold ? 9 : 8); col++)
                    if (rs.x + col >= 0 && rs.x + col < rs.cw)
                        rs.canvas[(draw_y + 15) * rs.cw + rs.x + col] = fg;
        }
        if (rs.strikethrough) {
            int sy = draw_y + 7;
            if (sy >= 0 && sy < rs.ch)
                for (int col = 0; col < (rs.bold ? 9 : 8); col++)
                    if (rs.x + col >= 0 && rs.x + col < rs.cw)
                        rs.canvas[sy * rs.cw + rs.x + col] = fg;
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

/* Parse inline style="" attribute and apply to render state */
static void apply_inline_style(const char *tag, int tag_len)
{
    char style[128];
    if (!get_attr(tag, tag_len, "style", style, sizeof(style))) return;

    int pos = 0;
    int slen = str_len(style);
    while (pos < slen) {
        /* Skip whitespace */
        while (pos < slen && (style[pos] == ' ' || style[pos] == '\t')) pos++;

        /* Read property */
        char prop[32];
        int pi = 0;
        while (pos < slen && style[pos] != ':' && style[pos] != ';' && pi < 31) {
            char c = style[pos];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != ' ' && c != '\t') prop[pi++] = c;
            pos++;
        }
        prop[pi] = 0;
        if (pos >= slen || style[pos] != ':') { pos++; continue; }
        pos++; /* skip : */
        while (pos < slen && style[pos] == ' ') pos++;

        /* Read value */
        char val[CSS_VALUE_MAX];
        int vi = 0;
        while (pos < slen && style[pos] != ';' && vi < CSS_VALUE_MAX - 1) {
            val[vi++] = style[pos++];
        }
        while (vi > 0 && val[vi-1] == ' ') vi--;
        val[vi] = 0;
        if (pos < slen && style[pos] == ';') pos++;

        /* Apply */
        if (str_cmp(prop, "color") == 0) {
            rs.text_color = named_color(val);
        } else if (str_cmp(prop, "background-color") == 0 || str_cmp(prop, "background") == 0) {
            /* For block elements, could fill area */
        } else if (str_cmp(prop, "font-weight") == 0) {
            rs.bold = (str_ncasecmp(val, "bold", 4) == 0) ? 1 : 0;
        } else if (str_cmp(prop, "font-style") == 0) {
            rs.italic = (str_ncasecmp(val, "italic", 6) == 0) ? 1 : 0;
        } else if (str_cmp(prop, "text-decoration") == 0) {
            if (str_ncasecmp(val, "underline", 9) == 0) rs.underline = 1;
            else if (str_ncasecmp(val, "line-through", 12) == 0) rs.strikethrough = 1;
            else if (str_ncasecmp(val, "none", 4) == 0) { rs.underline = 0; rs.strikethrough = 0; }
        } else if (str_cmp(prop, "text-align") == 0) {
            if (str_ncasecmp(val, "center", 6) == 0) rs.centered = 1;
        }
    }
}

static void handle_tag(const char *tag, int tag_len)
{
    /* Skip < and find tag name */
    int is_close = 0;
    int i = 0;
    if (tag[0] == '/') { is_close = 1; i = 1; }

    /* Skip <!DOCTYPE ...> and other declarations */
    if (tag[0] == '!') return;

    char name[16];
    int ni = 0;
    while (i < tag_len && ni < 15 && tag[i] != ' ' && tag[i] != '>' && tag[i] != '/')  {
        name[ni++] = tag[i++];
        /* Convert to lowercase */
        if (name[ni-1] >= 'A' && name[ni-1] <= 'Z')
            name[ni-1] += 32;
    }
    name[ni] = 0;

    /* Flush centered text buffer before tags that draw inline content */
    if (rs.centered && rs.center_buf_len > 0 &&
        (str_cmp(name, "input") == 0 || str_cmp(name, "img") == 0 ||
         str_cmp(name, "button") == 0 || str_cmp(name, "textarea") == 0 ||
         str_cmp(name, "select") == 0)) {
        flush_center_buf();
    }

    /* Self-closing tags */
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
    } else if (str_cmp(name, "img") == 0) {
        /* Image placeholder */
        if (!is_close) {
            char alt[64];
            if (!get_attr(tag, tag_len, "alt", alt, sizeof(alt)))
                str_cpy(alt, "[image]");
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -20 && draw_y < rs.ch) {
                /* Draw a placeholder box */
                int pw = str_len(alt) * 8 + 12;
                int ph = 22;
                int dx = rs.x;
                if (dx + pw > rs.max_x) { render_newline(); draw_y = rs.y - rs.scroll; dx = rs.x; }
                fill_rect(rs.canvas, rs.cw, rs.ch, dx, draw_y, pw, ph, 0xE8E8E8);
                draw_hline(rs.canvas, rs.cw, rs.ch, dx, draw_y, pw, 0xC0C0C0);
                draw_hline(rs.canvas, rs.cw, rs.ch, dx, draw_y + ph - 1, pw, 0xC0C0C0);
                canvas_draw_string(rs.canvas, rs.cw, rs.ch, dx + 6, draw_y + 3, alt, 0x808080);
                rs.x += pw + 4;
            }
        }
    } else if (str_cmp(name, "input") == 0) {
        /* Input field visual placeholder */
        if (!is_close) {
            char type[32], value[64], placeholder[64], size_str[16], inp_name[64];
            if (!get_attr(tag, tag_len, "type", type, sizeof(type)))
                str_cpy(type, "text");
            if (!get_attr(tag, tag_len, "value", value, sizeof(value)))
                value[0] = 0;
            if (!get_attr(tag, tag_len, "placeholder", placeholder, sizeof(placeholder)))
                placeholder[0] = 0;
            if (!get_attr(tag, tag_len, "size", size_str, sizeof(size_str)))
                size_str[0] = 0;
            if (!get_attr(tag, tag_len, "name", inp_name, sizeof(inp_name)))
                inp_name[0] = 0;

            /* Hidden inputs: don't render but register name/value */
            if (str_ncasecmp(type, "hidden", 6) == 0) {
                if (inp_name[0] && form_input_count < MAX_FORM_INPUTS) {
                    form_input_t *fi = &form_inputs[form_input_count];
                    fi->x = fi->y = fi->w = fi->h = 0;
                    str_cpy(fi->name, inp_name);
                    str_cpy(fi->value, value);
                    fi->is_submit = 0;
                    form_input_count++;
                }
                return;
            }

            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -20 && draw_y < rs.ch) {
                if (str_ncasecmp(type, "submit", 6) == 0 || str_ncasecmp(type, "button", 6) == 0) {
                    /* Button-style input */
                    const char *label = value[0] ? value : "Submit";
                    int bw = str_len(label) * 8 + 16;
                    /* Button with 3D look */
                    fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, bw, 22, 0xE0E0E0);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, bw, 0xF0F0F0);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 21, bw, 0x808080);
                    for (int by = draw_y; by < draw_y + 22 && by < rs.ch; by++) {
                        if (by >= 0) {
                            if (rs.x >= 0 && rs.x < rs.cw) rs.canvas[by * rs.cw + rs.x] = 0xF0F0F0;
                            if (rs.x+bw-1 >= 0 && rs.x+bw-1 < rs.cw) rs.canvas[by * rs.cw + rs.x+bw-1] = 0x808080;
                        }
                    }
                    canvas_draw_string(rs.canvas, rs.cw, rs.ch, rs.x + 8, draw_y + 3, label, 0x1A1A1A);
                    /* Register as submit button */
                    if (form_input_count < MAX_FORM_INPUTS) {
                        form_input_t *fi = &form_inputs[form_input_count];
                        fi->x = rs.x; fi->y = rs.y; fi->w = bw; fi->h = 22;
                        str_cpy(fi->name, inp_name);
                        str_cpy(fi->value, value);
                        fi->is_submit = 1;
                        form_input_count++;
                    }
                    rs.x += bw + 4;
                } else if (str_ncasecmp(type, "checkbox", 8) == 0) {
                    fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 2, 14, 14, 0xFFFFFF);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 2, 14, 0x808080);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 15, 14, 0x808080);
                    for (int by = draw_y + 2; by <= draw_y + 15 && by < rs.ch; by++) {
                        if (by >= 0) {
                            if (rs.x >= 0 && rs.x < rs.cw) rs.canvas[by * rs.cw + rs.x] = 0x808080;
                            if (rs.x+13 >= 0 && rs.x+13 < rs.cw) rs.canvas[by * rs.cw + rs.x+13] = 0x808080;
                        }
                    }
                    rs.x += 18;
                } else if (str_ncasecmp(type, "radio", 5) == 0) {
                    int rcx = rs.x + 7, rcy = draw_y + 9;
                    for (int dy2 = -6; dy2 <= 6; dy2++)
                        for (int dx2 = -6; dx2 <= 6; dx2++)
                            if (dx2*dx2 + dy2*dy2 <= 36 && dx2*dx2 + dy2*dy2 >= 25)
                                if (rcy+dy2 >= 0 && rcy+dy2 < rs.ch && rcx+dx2 >= 0 && rcx+dx2 < rs.cw)
                                    rs.canvas[(rcy+dy2)*rs.cw + rcx+dx2] = 0x808080;
                    for (int dy2 = -5; dy2 <= 5; dy2++)
                        for (int dx2 = -5; dx2 <= 5; dx2++)
                            if (dx2*dx2 + dy2*dy2 <= 25)
                                if (rcy+dy2 >= 0 && rcy+dy2 < rs.ch && rcx+dx2 >= 0 && rcx+dx2 < rs.cw)
                                    rs.canvas[(rcy+dy2)*rs.cw + rcx+dx2] = 0xFFFFFF;
                    rs.x += 18;
                } else if (str_ncasecmp(type, "image", 5) == 0) {
                    fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, 40, 22, 0xE0E0E0);
                    canvas_draw_string(rs.canvas, rs.cw, rs.ch, rs.x + 4, draw_y + 3, "[Go]", 0x1A1A1A);
                    rs.x += 44;
                } else {
                    /* Text-like input field */
                    int fw = 150;
                    if (size_str[0]) {
                        int sz = 0;
                        for (int si = 0; size_str[si] >= '0' && size_str[si] <= '9'; si++)
                            sz = sz * 10 + (size_str[si] - '0');
                        if (sz > 0 && sz < 80) fw = sz * 8 + 8;
                    }
                    if (rs.x + fw > rs.max_x) fw = rs.max_x - rs.x - 4;
                    if (fw < 24) fw = 24;

                    /* Register text input */
                    int fi_idx = -1;
                    if (form_input_count < MAX_FORM_INPUTS) {
                        fi_idx = form_input_count;
                        form_input_t *fi = &form_inputs[form_input_count];
                        fi->x = rs.x; fi->y = rs.y; fi->w = fw; fi->h = 22;
                        str_cpy(fi->name, inp_name);
                        str_cpy(fi->value, value);
                        fi->is_submit = 0;
                        fi->user_modified = 0;
                        /* Restore user-typed value if available */
                        restore_form_input(fi);
                        form_input_count++;
                    }

                    /* Draw text field - highlight if focused */
                    int is_focused = (fi_idx >= 0 && fi_idx == focused_input);
                    uint32_t border_color = is_focused ? 0x4488CC : 0x808080;
                    fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, fw, 22, 0xFFFFFF);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, fw, border_color);
                    draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 21, fw, border_color);
                    for (int by = draw_y; by < draw_y + 22 && by < rs.ch; by++) {
                        if (by >= 0) {
                            if (rs.x >= 0 && rs.x < rs.cw) rs.canvas[by * rs.cw + rs.x] = border_color;
                            if (rs.x+fw-1 >= 0 && rs.x+fw-1 < rs.cw) rs.canvas[by * rs.cw + rs.x+fw-1] = border_color;
                        }
                    }
                    /* Display value or placeholder */
                    const char *txt;
                    uint32_t txt_color;
                    if (is_focused && form_inputs[fi_idx].value[0]) {
                        txt = form_inputs[fi_idx].value;
                        txt_color = 0x1A1A1A;
                    } else if (value[0]) {
                        txt = value;
                        txt_color = 0x1A1A1A;
                    } else {
                        txt = placeholder;
                        txt_color = 0xA0A0A0;
                    }
                    if (txt[0]) {
                        int max_txt = (fw - 8) / 8;
                        if (max_txt < 1) max_txt = 1;
                        char clipped[80];
                        int ci2 = 0;
                        while (txt[ci2] && ci2 < max_txt && ci2 < 79)
                            { clipped[ci2] = txt[ci2]; ci2++; }
                        clipped[ci2] = 0;
                        canvas_draw_string(rs.canvas, rs.cw, rs.ch, rs.x + 4, draw_y + 3,
                                           clipped, txt_color);
                    }
                    /* Cursor in focused input */
                    if (is_focused) {
                        int clen = str_len(form_inputs[fi_idx].value);
                        int cx = rs.x + 4 + clen * 8;
                        if (cx < rs.x + fw - 4)
                            fill_rect(rs.canvas, rs.cw, rs.ch, cx, draw_y + 3, 2, 16, 0x1A1A1A);
                    }
                    rs.x += fw + 4;
                }
            }
        }
    } else if (str_cmp(name, "button") == 0) {
        if (!is_close) {
            /* Draw button background; content will render on top */
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -20 && draw_y < rs.ch) {
                /* 3D raised button look */
                int bw = 80;
                fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, bw, 22, 0xE0E0E0);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, bw, 0xF0F0F0);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 21, bw, 0x808080);
                for (int by = draw_y; by < draw_y + 22 && by < rs.ch; by++) {
                    if (by >= 0) {
                        if (rs.x >= 0 && rs.x < rs.cw) rs.canvas[by * rs.cw + rs.x] = 0xF0F0F0;
                        if (rs.x+bw-1 >= 0 && rs.x+bw-1 < rs.cw) rs.canvas[by * rs.cw + rs.x+bw-1] = 0x808080;
                    }
                }
            }
            rs.x += 4;
        } else {
            rs.x += 4;
        }
    /* Block-level tags */
    } else if (str_cmp(name, "p") == 0) {
        if (is_close) {
            if (rs.color_stack_depth > 0)
                rs.text_color = rs.color_stack[--rs.color_stack_depth];
            rs.y += 8; rs.x = rs.start_x;
        } else {
            if (rs.color_stack_depth < COLOR_STACK_MAX)
                rs.color_stack[rs.color_stack_depth++] = rs.text_color;
            render_newline(); rs.y += 4;
            apply_inline_style(tag, tag_len);
        }
    } else if (str_cmp(name, "div") == 0) {
        if (!is_close) {
            if (rs.color_stack_depth < COLOR_STACK_MAX)
                rs.color_stack[rs.color_stack_depth++] = rs.text_color;
            render_newline();
            apply_inline_style(tag, tag_len);
        } else {
            if (rs.color_stack_depth > 0)
                rs.text_color = rs.color_stack[--rs.color_stack_depth];
            render_newline();
        }
    } else if (str_cmp(name, "span") == 0) {
        /* Inline element - check for style color */
        if (!is_close) {
            /* Push current color */
            if (rs.color_stack_depth < COLOR_STACK_MAX)
                rs.color_stack[rs.color_stack_depth++] = rs.text_color;
            apply_inline_style(tag, tag_len);
        } else {
            if (rs.color_stack_depth > 0)
                rs.text_color = rs.color_stack[--rs.color_stack_depth];
        }
    } else if (str_cmp(name, "blockquote") == 0) {
        if (is_close) { rs.start_x -= 30; rs.max_x += 10; render_newline(); rs.y += 4; }
        else { rs.start_x += 30; rs.max_x -= 10; render_newline(); rs.y += 4; }
    } else if (str_cmp(name, "center") == 0) {
        if (is_close) {
            if (rs.center_buf_len > 0) flush_center_buf();
            rs.centered = 0;
            render_newline();
        } else {
            render_newline();
            rs.centered = 1;
            rs.center_buf_len = 0;
        }
    /* Inline formatting tags */
    } else if (str_cmp(name, "b") == 0 || str_cmp(name, "strong") == 0) {
        rs.bold = !is_close;
    } else if (str_cmp(name, "i") == 0 || str_cmp(name, "em") == 0 || str_cmp(name, "cite") == 0 || str_cmp(name, "address") == 0) {
        rs.italic = !is_close;
    } else if (str_cmp(name, "u") == 0 || str_cmp(name, "ins") == 0) {
        rs.underline = !is_close;
    } else if (str_cmp(name, "s") == 0 || str_cmp(name, "strike") == 0 || str_cmp(name, "del") == 0) {
        rs.strikethrough = !is_close;
    } else if (str_cmp(name, "small") == 0 || str_cmp(name, "big") == 0 ||
               str_cmp(name, "sup") == 0 || str_cmp(name, "sub") == 0 ||
               str_cmp(name, "abbr") == 0 || str_cmp(name, "q") == 0 ||
               str_cmp(name, "nobr") == 0 || str_cmp(name, "wbr") == 0 ||
               str_cmp(name, "mark") == 0 || str_cmp(name, "var") == 0 ||
               str_cmp(name, "kbd") == 0 || str_cmp(name, "samp") == 0 ||
               str_cmp(name, "dfn") == 0 || str_cmp(name, "bdi") == 0 ||
               str_cmp(name, "bdo") == 0 || str_cmp(name, "time") == 0 ||
               str_cmp(name, "data") == 0 || str_cmp(name, "ruby") == 0 ||
               str_cmp(name, "rt") == 0 || str_cmp(name, "rp") == 0) {
        /* Inline tags we recognize but handle as pass-through (content renders) */
        (void)is_close;
    } else if (str_cmp(name, "a") == 0) {
        if (is_close) {
            /* Register link region - handle multi-line links */
            if (rs.in_link && rs.link_href[0]) {
                if (rs.y == rs.link_start_y) {
                    /* Single-line link */
                    if (link_count < MAX_LINKS) {
                        link_region_t *lr = &page_links[link_count];
                        lr->x = rs.link_start_x;
                        lr->y = rs.link_start_y;
                        lr->w = rs.x - rs.link_start_x;
                        lr->h = rs.line_height;
                        if (lr->w > 0) {
                            str_cpy(lr->href, rs.link_href);
                            link_count++;
                        }
                    }
                } else {
                    /* Multi-line: first line from start_x to max_x */
                    if (link_count < MAX_LINKS) {
                        link_region_t *lr = &page_links[link_count];
                        lr->x = rs.link_start_x;
                        lr->y = rs.link_start_y;
                        lr->w = rs.max_x - rs.link_start_x;
                        lr->h = rs.line_height;
                        str_cpy(lr->href, rs.link_href);
                        link_count++;
                    }
                    /* Middle lines: full width */
                    int mid_y = rs.link_start_y + rs.line_height;
                    while (mid_y < rs.y && link_count < MAX_LINKS) {
                        link_region_t *lr = &page_links[link_count];
                        lr->x = rs.start_x;
                        lr->y = mid_y;
                        lr->w = rs.max_x - rs.start_x;
                        lr->h = rs.line_height;
                        str_cpy(lr->href, rs.link_href);
                        link_count++;
                        mid_y += rs.line_height;
                    }
                    /* Last line: start_x to current x */
                    if (link_count < MAX_LINKS && rs.x > rs.start_x) {
                        link_region_t *lr = &page_links[link_count];
                        lr->x = rs.start_x;
                        lr->y = rs.y;
                        lr->w = rs.x - rs.start_x;
                        lr->h = rs.line_height;
                        str_cpy(lr->href, rs.link_href);
                        link_count++;
                    }
                }
            }
            rs.in_link = 0;
            rs.underline = 0;
            rs.link_href[0] = 0;
        } else {
            rs.in_link = 1;
            rs.underline = 1;
            rs.link_start_x = rs.x;
            rs.link_start_y = rs.y;
            /* Extract href attribute */
            char href_val[URL_MAX];
            if (get_attr(tag, tag_len, "href", href_val, sizeof(href_val))) {
                resolve_url(href_val, url_bar, rs.link_href, URL_MAX);
            } else {
                rs.link_href[0] = 0;
            }
        }
    } else if (str_cmp(name, "font") == 0) {
        if (is_close) {
            if (rs.color_stack_depth > 0)
                rs.text_color = rs.color_stack[--rs.color_stack_depth];
        } else {
            /* Push current color */
            if (rs.color_stack_depth < COLOR_STACK_MAX)
                rs.color_stack[rs.color_stack_depth++] = rs.text_color;
            char color_val[32];
            if (get_attr(tag, tag_len, "color", color_val, sizeof(color_val))) {
                rs.text_color = named_color(color_val);
            }
            /* Font size: sizes 5-7 render as bold */
            char size_val[8];
            if (get_attr(tag, tag_len, "size", size_val, sizeof(size_val))) {
                int sz = 0;
                int si = 0;
                if (size_val[0] == '+' || size_val[0] == '-') si = 1;
                while (size_val[si] >= '0' && size_val[si] <= '9')
                    sz = sz * 10 + (size_val[si++] - '0');
                if (size_val[0] == '+') sz += 3;
                if (sz >= 5) rs.bold = 1;
            }
        }
    } else if (str_cmp(name, "pre") == 0 || str_cmp(name, "code") == 0) {
        rs.preformatted = !is_close;
        if (!is_close) { render_newline(); }
    } else if (str_cmp(name, "title") == 0) {
        rs.in_title = !is_close;
    } else if (str_cmp(name, "head") == 0) {
        rs.in_head = !is_close;
    } else if (str_cmp(name, "style") == 0) {
        rs.in_style = !is_close;
    } else if (str_cmp(name, "script") == 0) {
        rs.in_script = !is_close;
    } else if (str_cmp(name, "noscript") == 0) {
        /* Render noscript content (since we don't support JS) */
        (void)is_close;
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
            char link_val[32];
            if (get_attr(tag, tag_len, "link", link_val, sizeof(link_val))) {
                rs.link_color = named_color(link_val);
            }
            /* Apply CSS for body and inline style */
            css_rule_t *css = css_lookup("body");
            if (css) {
                if (css->has_color) rs.text_color = css->color;
                if (css->has_bg_color) {
                    rs.bg_color = css->bg_color;
                    fill_rect(rs.canvas, rs.cw, rs.ch, 0, 0, rs.cw, rs.ch, rs.bg_color);
                }
            }
            apply_inline_style(tag, tag_len);
        }
    /* Heading tags */
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
    /* List tags */
    } else if (str_cmp(name, "ul") == 0) {
        if (is_close) { rs.in_list = 0; rs.start_x -= 20; render_newline(); }
        else { rs.in_list = 1; rs.list_ordered = 0; rs.list_item = 0; rs.start_x += 20; render_newline(); }
    } else if (str_cmp(name, "ol") == 0) {
        if (is_close) { rs.in_list = 0; rs.start_x -= 20; render_newline(); }
        else { rs.in_list = 1; rs.list_ordered = 1; rs.list_item = 0; rs.start_x += 20; render_newline(); }
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
    /* Definition list */
    } else if (str_cmp(name, "dl") == 0) {
        if (!is_close) render_newline();
        else render_newline();
    } else if (str_cmp(name, "dt") == 0) {
        if (!is_close) { render_newline(); rs.bold = 1; }
        else { rs.bold = 0; }
    } else if (str_cmp(name, "dd") == 0) {
        if (!is_close) { render_newline(); rs.start_x += 20; rs.x = rs.start_x; }
        else { rs.start_x -= 20; }
    /* Table tags */
    } else if (str_cmp(name, "table") == 0) {
        if (!is_close) {
            rs.in_table = 1;
            render_newline();
            rs.y += 4;
        } else {
            rs.in_table = 0;
            render_newline();
            rs.y += 4;
        }
    } else if (str_cmp(name, "tr") == 0) {
        if (!is_close) {
            rs.in_table_row = 1;
            rs.table_col = 0;
            rs.table_col_x = rs.start_x;
            render_newline();
        } else {
            rs.in_table_row = 0;
        }
    } else if (str_cmp(name, "td") == 0 || str_cmp(name, "th") == 0) {
        if (!is_close) {
            /* Move to next column position */
            int col_w = (rs.max_x - rs.start_x) / 4;  /* Simple: 4 equal columns */
            if (col_w < 60) col_w = 60;
            rs.x = rs.table_col_x;
            if (name[1] == 'h') rs.bold = 1;  /* <th> = bold */
            rs.table_col++;
            /* Draw a subtle cell border */
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= 0 && draw_y < rs.ch)
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y - 1,
                           col_w, 0xD0D0D0);
        } else {
            if (name[1] == 'h') rs.bold = 0;
            int col_w = (rs.max_x - rs.start_x) / 4;
            if (col_w < 60) col_w = 60;
            rs.table_col_x += col_w;
        }
    } else if (str_cmp(name, "caption") == 0) {
        if (!is_close) { render_newline(); rs.bold = 1; rs.centered = 1; }
        else { rs.bold = 0; rs.centered = 0; render_newline(); }
    } else if (str_cmp(name, "thead") == 0 || str_cmp(name, "tbody") == 0 ||
               str_cmp(name, "tfoot") == 0 || str_cmp(name, "colgroup") == 0 ||
               str_cmp(name, "col") == 0) {
        /* Table structure tags: just continue */
        (void)is_close;
    /* Form tags (visual only) */
    } else if (str_cmp(name, "form") == 0) {
        if (!is_close) {
            render_newline();
            /* Extract form action and method */
            char action_val[URL_MAX];
            if (get_attr(tag, tag_len, "action", action_val, sizeof(action_val)))
                resolve_url(action_val, url_bar, form_action, URL_MAX);
            else
                str_cpy(form_action, url_bar);
            if (!get_attr(tag, tag_len, "method", form_method, sizeof(form_method)))
                str_cpy(form_method, "get");
        }
    } else if (str_cmp(name, "fieldset") == 0) {
        if (!is_close) { render_newline(); rs.start_x += 10; rs.x = rs.start_x; }
        else { rs.start_x -= 10; render_newline(); }
    } else if (str_cmp(name, "legend") == 0) {
        if (!is_close) { rs.bold = 1; }
        else { rs.bold = 0; render_newline(); }
    } else if (str_cmp(name, "textarea") == 0) {
        if (!is_close) {
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -60 && draw_y < rs.ch) {
                int tw = 200, th = 60;
                fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, tw, th, 0xFFFFFF);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, tw, 0x808080);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + th - 1, tw, 0x808080);
                for (int by = draw_y; by < draw_y + th && by < rs.ch; by++) {
                    if (by >= 0) {
                        if (rs.x >= 0 && rs.x < rs.cw) rs.canvas[by * rs.cw + rs.x] = 0x808080;
                        if (rs.x+tw-1 >= 0 && rs.x+tw-1 < rs.cw) rs.canvas[by * rs.cw + rs.x+tw-1] = 0x808080;
                    }
                }
            }
            rs.y += 64;
            rs.x = rs.start_x;
        }
    } else if (str_cmp(name, "select") == 0) {
        if (!is_close) {
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -20 && draw_y < rs.ch) {
                int sw = 120;
                fill_rect(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, sw, 22, 0xFFFFFF);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y, sw, 0x808080);
                draw_hline(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 21, sw, 0x808080);
                /* Dropdown arrow */
                canvas_draw_char(rs.canvas, rs.cw, rs.ch, rs.x + sw - 14, draw_y + 3, 'v', 0x606060);
                rs.x += sw + 4;
            }
        }
    } else if (str_cmp(name, "option") == 0 || str_cmp(name, "optgroup") == 0) {
        /* Skip option/optgroup content visually */
    } else if (str_cmp(name, "label") == 0) {
        /* Label: just render content normally */
    /* HTML5 semantic block tags - treat as block */
    } else if (str_cmp(name, "section") == 0 || str_cmp(name, "article") == 0 ||
               str_cmp(name, "header") == 0 || str_cmp(name, "footer") == 0 ||
               str_cmp(name, "nav") == 0 || str_cmp(name, "main") == 0 ||
               str_cmp(name, "aside") == 0 || str_cmp(name, "details") == 0 ||
               str_cmp(name, "summary") == 0 || str_cmp(name, "figure") == 0 ||
               str_cmp(name, "figcaption") == 0) {
        render_newline();
    /* Tags to skip content for */
    } else if (str_cmp(name, "iframe") == 0 || str_cmp(name, "object") == 0 ||
               str_cmp(name, "embed") == 0 || str_cmp(name, "applet") == 0 ||
               str_cmp(name, "video") == 0 || str_cmp(name, "audio") == 0 ||
               str_cmp(name, "canvas") == 0 || str_cmp(name, "svg") == 0) {
        if (!is_close) {
            /* Show placeholder */
            int draw_y = rs.y - rs.scroll;
            if (draw_y >= -20 && draw_y < rs.ch) {
                canvas_draw_string(rs.canvas, rs.cw, rs.ch, rs.x, draw_y + 3,
                                   "[embedded content]", 0xA0A0A0);
            }
            rs.x += 150;
        }
    /* Map, area ignored */
    } else if (str_cmp(name, "map") == 0 || str_cmp(name, "area") == 0) {
        (void)is_close;
    }
    /* Set last_was_block for known block-level tags to suppress whitespace */
    if (str_cmp(name, "br") == 0 || str_cmp(name, "hr") == 0 ||
        str_cmp(name, "p") == 0 || str_cmp(name, "div") == 0 ||
        str_cmp(name, "center") == 0 || str_cmp(name, "blockquote") == 0 ||
        str_cmp(name, "h1") == 0 || str_cmp(name, "h2") == 0 ||
        str_cmp(name, "h3") == 0 || str_cmp(name, "h4") == 0 ||
        str_cmp(name, "h5") == 0 || str_cmp(name, "h6") == 0 ||
        str_cmp(name, "ul") == 0 || str_cmp(name, "ol") == 0 ||
        str_cmp(name, "li") == 0 || str_cmp(name, "table") == 0 ||
        str_cmp(name, "tr") == 0 || str_cmp(name, "td") == 0 ||
        str_cmp(name, "th") == 0 || str_cmp(name, "form") == 0 ||
        str_cmp(name, "pre") == 0 || str_cmp(name, "dl") == 0 ||
        str_cmp(name, "dt") == 0 || str_cmp(name, "dd") == 0 ||
        (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0))
        rs.last_was_block = 1;
}

/* Decode HTML entities */
static char decode_entity(const char *s, int *advance)
{
    /* Named entities */
    if (str_ncmp(s, "&amp;", 5) == 0) { *advance = 5; return '&'; }
    if (str_ncmp(s, "&lt;", 4) == 0) { *advance = 4; return '<'; }
    if (str_ncmp(s, "&gt;", 4) == 0) { *advance = 4; return '>'; }
    if (str_ncmp(s, "&nbsp;", 6) == 0) { *advance = 6; return ' '; }
    if (str_ncmp(s, "&quot;", 6) == 0) { *advance = 6; return '"'; }
    if (str_ncmp(s, "&apos;", 6) == 0) { *advance = 6; return '\''; }
    if (str_ncasecmp(s, "&copy;", 6) == 0) { *advance = 6; return 'c'; }
    if (str_ncasecmp(s, "&reg;", 5) == 0) { *advance = 5; return 'R'; }
    if (str_ncasecmp(s, "&trade;", 7) == 0) { *advance = 7; return 'T'; }
    if (str_ncasecmp(s, "&mdash;", 7) == 0) { *advance = 7; return '-'; }
    if (str_ncasecmp(s, "&ndash;", 7) == 0) { *advance = 7; return '-'; }
    if (str_ncasecmp(s, "&laquo;", 7) == 0) { *advance = 7; return '<'; }
    if (str_ncasecmp(s, "&raquo;", 7) == 0) { *advance = 7; return '>'; }
    if (str_ncasecmp(s, "&ldquo;", 7) == 0) { *advance = 7; return '"'; }
    if (str_ncasecmp(s, "&rdquo;", 7) == 0) { *advance = 7; return '"'; }
    if (str_ncasecmp(s, "&lsquo;", 7) == 0) { *advance = 7; return '\''; }
    if (str_ncasecmp(s, "&rsquo;", 7) == 0) { *advance = 7; return '\''; }
    if (str_ncasecmp(s, "&bull;", 6) == 0) { *advance = 6; return '*'; }
    if (str_ncasecmp(s, "&middot;", 8) == 0) { *advance = 8; return '.'; }
    if (str_ncasecmp(s, "&hellip;", 8) == 0) { *advance = 8; return '.'; }
    if (str_ncasecmp(s, "&rarr;", 6) == 0) { *advance = 6; return '>'; }
    if (str_ncasecmp(s, "&larr;", 6) == 0) { *advance = 6; return '<'; }
    if (str_ncasecmp(s, "&times;", 7) == 0) { *advance = 7; return 'x'; }
    if (str_ncasecmp(s, "&divide;", 8) == 0) { *advance = 8; return '/'; }
    if (str_ncasecmp(s, "&deg;", 5) == 0) { *advance = 5; return 'o'; }
    if (str_ncasecmp(s, "&pound;", 7) == 0) { *advance = 7; return '#'; }
    if (str_ncasecmp(s, "&euro;", 6) == 0) { *advance = 6; return 'E'; }
    if (str_ncasecmp(s, "&cent;", 6) == 0) { *advance = 6; return 'c'; }
    if (str_ncasecmp(s, "&yen;", 5) == 0) { *advance = 5; return 'Y'; }
    if (str_ncasecmp(s, "&iquest;", 8) == 0) { *advance = 8; return '?'; }
    if (str_ncasecmp(s, "&iexcl;", 7) == 0) { *advance = 7; return '!'; }
    if (str_ncasecmp(s, "&frac12;", 8) == 0) { *advance = 8; return '/'; }
    if (str_ncasecmp(s, "&frac14;", 8) == 0) { *advance = 8; return '/'; }
    if (str_ncasecmp(s, "&frac34;", 8) == 0) { *advance = 8; return '/'; }
    if (str_ncasecmp(s, "&para;", 6) == 0) { *advance = 6; return 'P'; }
    if (str_ncasecmp(s, "&sect;", 6) == 0) { *advance = 6; return 'S'; }

    /* Numeric character references: &#NNN; or &#xHH; */
    if (s[1] == '#') {
        int val = 0;
        int pos = 2;
        if (s[pos] == 'x' || s[pos] == 'X') {
            pos++;
            while (s[pos] && s[pos] != ';' && pos < 10) {
                char c = s[pos];
                if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
                else break;
                pos++;
            }
        } else {
            while (s[pos] >= '0' && s[pos] <= '9' && pos < 10) {
                val = val * 10 + (s[pos] - '0');
                pos++;
            }
        }
        if (s[pos] == ';') pos++;
        *advance = pos;
        /* Map to printable ASCII or substitute */
        if (val >= 32 && val <= 126) return (char)val;
        if (val == 160) return ' ';  /* non-breaking space */
        if (val == 169) return 'c';  /* copyright */
        if (val == 174) return 'R';  /* registered */
        if (val == 8211 || val == 8212) return '-';  /* en/em dash */
        if (val == 8216 || val == 8217) return '\''; /* smart quotes */
        if (val == 8220 || val == 8221) return '"';  /* double smart quotes */
        if (val == 8226) return '*';  /* bullet */
        if (val == 8230) return '.';  /* ellipsis */
        if (val == 8364) return 'E';  /* euro */
        return '?';
    }

    /* Unknown entity: try to skip to semicolon */
    int pos = 1;
    while (s[pos] && s[pos] != ';' && s[pos] != ' ' && s[pos] != '<' && pos < 10)
        pos++;
    if (s[pos] == ';') {
        *advance = pos + 1;
        return '?';
    }
    *advance = 1;
    return '&';
}

static void render_html(const char *html, int html_len,
                        uint32_t *canvas, int cw, int ch, int scroll)
{
    /* Save user-typed form values before resetting */
    save_form_inputs();

    /* Reset link and form tracking */
    link_count = 0;
    form_input_count = 0;
    form_action[0] = 0;
    str_cpy(form_method, "get");
    css_rule_count = 0;

    /* Pre-scan for <style> blocks to extract CSS rules */
    {
        int si = 0;
        while (si < html_len - 7) {
            if (html[si] == '<' && str_ncasecmp(html + si + 1, "style", 5) == 0 &&
                (html[si + 6] == '>' || html[si + 6] == ' ')) {
                /* Find end of style opening tag */
                int css_start = si + 6;
                while (css_start < html_len && html[css_start] != '>') css_start++;
                css_start++;
                /* Find </style> */
                int css_end = css_start;
                while (css_end + 7 < html_len) {
                    if (html[css_end] == '<' && html[css_end+1] == '/' &&
                        str_ncasecmp(html + css_end + 2, "style", 5) == 0) break;
                    css_end++;
                }
                if (css_end > css_start)
                    parse_css_block(html + css_start, css_end - css_start);
                si = css_end;
            }
            si++;
        }
    }

    /* Initialize render state */
    rs.canvas = canvas;
    rs.cw = cw;
    rs.ch = ch;
    rs.x = 8;
    rs.y = 4;
    rs.start_x = 8;
    rs.max_x = cw - SCROLLBAR_W - 8;  /* Leave space for scrollbar */
    rs.line_height = 18;
    rs.scroll = scroll;
    rs.bold = 0;
    rs.italic = 0;
    rs.underline = 0;
    rs.strikethrough = 0;
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
    rs.in_style = 0;
    rs.in_script = 0;
    rs.centered = 0;
    rs.in_table = 0;
    rs.in_table_row = 0;
    rs.table_col = 0;
    rs.table_col_x = 8;
    rs.color_stack_depth = 0;
    rs.link_href[0] = 0;
    rs.link_start_x = 0;
    rs.link_start_y = 0;
    rs.center_defer = 0;
    rs.center_buf_len = 0;
    rs.last_was_block = 1;  /* Suppress leading whitespace */

    /* Apply CSS rules for body if available */
    css_rule_t *body_css = css_lookup("body");
    if (body_css) {
        if (body_css->has_color) rs.text_color = body_css->color;
        if (body_css->has_bg_color) rs.bg_color = body_css->bg_color;
    }
    css_rule_t *a_css = css_lookup("a");
    if (a_css && a_css->has_color) rs.link_color = a_css->color;

    /* Clear canvas */
    fill_rect(canvas, cw, ch, 0, 0, cw, ch, rs.bg_color);

    int i = 0;
    while (i < html_len) {
        if (html[i] == '<') {
            /* Check for comments <!-- --> */
            if (i + 3 < html_len && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-') {
                /* Skip to end of comment */
                int ci = i + 4;
                while (ci + 2 < html_len) {
                    if (html[ci] == '-' && html[ci+1] == '-' && html[ci+2] == '>') {
                        ci += 3;
                        break;
                    }
                    ci++;
                }
                i = ci;
                continue;
            }
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
            if (!rs.in_title && !rs.in_style && !rs.in_script) {
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
            } else if (!rs.in_style && !rs.in_script) {
                char c = html[i];
                if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                    if (rs.preformatted) {
                        if (c == '\n') render_newline();
                        else if (c == '\t') { for (int t = 0; t < 4; t++) render_char(' '); }
                        else render_char(' ');
                    } else {
                        /* Collapse whitespace; suppress after block tags */
                        if (!rs.last_was_block && rs.x > rs.start_x) render_char(' ');
                    }
                } else if (c >= 32) {
                    rs.last_was_block = 0;
                    /* Render printable chars; map non-ASCII to '?' */
                    if (c > 126) c = '?';
                    render_char(c);
                }
            }
            i++;
        }
    }

    /* Flush any pending centered text */
    if (rs.centered && rs.center_buf_len > 0)
        flush_center_buf();

    /* Record total content height for scrollbar */
    content_total_h = rs.y + rs.line_height;

    /* Restore focused input by name */
    restore_focused_input();
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
    "<li><b>HTML Rendering</b> - Rich HTML support with headings, lists, tables, forms, and formatting</li>"
    "<li><b>Clickable Links</b> - Click hyperlinks to navigate; hover to see URL in status bar</li>"
    "<li><b>Interactive Forms</b> - Click text fields to type, press Enter or click Submit to search</li>"
    "<li><b>HTTP/1.1 &amp; HTTPS</b> - Fetch web pages over HTTP and TLS 1.2 HTTPS</li>"
    "<li><b>DNS Resolution</b> - Resolve hostnames to IP addresses</li>"
    "<li><b>Navigation</b> - Back, Forward, and Refresh buttons</li>"
    "<li><b>Scrollbar</b> - Visual scrollbar for page navigation</li>"
    "</ul>"
    "<h2>Supported HTML Tags</h2>"
    "<p><b>Block:</b> div, p, h1-h6, pre, blockquote, center, table, tr, td, th, ul, ol, li, dl, dt, dd, hr, br</p>"
    "<p><b>Inline:</b> b, strong, i, em, u, s, strike, a, font, span, code, small, big, sup, sub</p>"
    "<p><b>Forms:</b> input (text, submit, checkbox, radio), button, textarea, select</p>"
    "<p><b>CSS:</b> Inline style attributes (color, font-weight, text-decoration, text-align) and &lt;style&gt; block rules</p>"
    "<h2>Tips</h2>"
    "<ol>"
    "<li>Enter a URL like <u>http://frogfind.com</u> in the address bar</li>"
    "<li>Click on blue underlined links to follow them</li>"
    "<li>Click on text fields, type your text, then press Enter to submit</li>"
    "<li>Use the scroll wheel, arrow keys, or scrollbar to scroll the page</li>"
    "<li>Use Back/Forward buttons to navigate history</li>"
    "<li>HTTPS URLs are supported for secure sites</li>"
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
    focused_input = -1;
    hover_url[0] = 0;
    saved_input_count = 0;
    saved_focused_name[0] = 0;
    nav_state = NAV_DONE;
    str_cpy(status_msg, "Ready");
}

/* ── Navigation ──────────────────────────────────────────────────────── */
static void navigate_internal(const char *url, int push_history);

/* Submit form by building a GET URL with query parameters */
static void submit_form(void)
{
    if (!form_action[0]) return;

    /* Build query string from form inputs */
    char submit_url[1024];
    int oi = 0;
    /* Copy action URL */
    for (int i = 0; form_action[i] && oi < 800; i++)
        submit_url[oi++] = form_action[i];

    /* Check if action already has query params */
    int has_query = 0;
    for (int i = 0; form_action[i]; i++) {
        if (form_action[i] == '?') { has_query = 1; break; }
    }

    /* Append form inputs as query parameters */
    int first_param = !has_query;
    for (int i = 0; i < form_input_count && oi < 900; i++) {
        form_input_t *fi = &form_inputs[i];
        if (fi->is_submit) continue;  /* Don't include submit button */
        if (!fi->name[0]) continue;    /* Skip unnamed inputs */
        submit_url[oi++] = first_param ? '?' : '&';
        first_param = 0;
        /* Name */
        char enc[128];
        url_encode(fi->name, enc, sizeof(enc));
        for (int j = 0; enc[j] && oi < 1000; j++) submit_url[oi++] = enc[j];
        submit_url[oi++] = '=';
        /* Value */
        url_encode(fi->value, enc, sizeof(enc));
        for (int j = 0; enc[j] && oi < 1020; j++) submit_url[oi++] = enc[j];
    }
    submit_url[oi] = 0;

    /* Update URL bar and navigate */
    str_cpy(url_bar, submit_url);
    url_cursor = str_len(url_bar);
    focused_input = -1;
    navigate_internal(submit_url, 1);
}

static void navigate(const char *url)
{
    navigate_internal(url, 1);
}

static void navigate_internal(const char *url, int push_history)
{
    if (!url || !url[0]) return;

    /* Clear saved form values for new page */
    saved_input_count = 0;
    saved_focused_name[0] = 0;

    /* Handle about: URLs */
    if (str_ncmp(url, "about:", 6) == 0) {
        load_homepage();
        if (push_history) history_push(url);
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

    if (purl.is_https) {
        str_cpy(status_msg, "Connecting (HTTPS)...");
        /* Use HTTPS GET via TLS */
        int result = https_get(purl.host, purl.port, purl.path, page_buf, PAGE_BUF_SIZE);
        if (result < 0) {
            str_cpy(page_buf,
                "<html><body bgcolor=\"#FFF0F0\">"
                "<h1>HTTPS Connection Failed</h1>"
                "<p>Could not establish a secure connection to the server.</p>"
                "<p>The server may not support the TLS version used by nextOS.</p>"
                "<p>Try using <b>http://</b> instead if available.</p>"
                "</body></html>");
            page_len = str_len(page_buf);
            str_cpy(page_title, "HTTPS Error");
            scroll_y = 0;
            nav_state = NAV_ERROR;
            str_cpy(status_msg, "HTTPS connection failed");
            return;
        }
        page_len = result;
    } else {
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
    }

    page_title[0] = 0;  /* Will be set by renderer */
    scroll_y = 0;
    focused_input = -1;
    nav_state = NAV_DONE;
    str_cpy(status_msg, "Done");

    if (push_history) history_push(url);
}

static void go_back(void)
{
    if (history_pos > 0) {
        history_pos--;
        str_cpy(url_bar, history_urls[history_pos]);
        url_cursor = str_len(url_bar);
        navigate_internal(url_bar, 0);
    }
}

static void go_forward(void)
{
    if (history_pos < history_count - 1) {
        history_pos++;
        str_cpy(url_bar, history_urls[history_pos]);
        url_cursor = str_len(url_bar);
        navigate_internal(url_bar, 0);
    }
}

static void refresh_page(void)
{
    navigate_internal(url_bar, 0);
}

/* ── Paint ───────────────────────────────────────────────────────────── */

static void browser_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;
    int ch = win->height - 4;

    /* Background */
    fill_rect(win->canvas, cw, ch, 0, 0, cw, ch, 0xE8E8E8);

    /* ── Toolbar (navigation + URL bar area) ───────────────────────── */
    /* Toolbar background with gradient */
    for (int y = 0; y < TOOLBAR_H; y++) {
        uint32_t c = 0xDCDCDC + ((y * 0x10 / TOOLBAR_H) << 16) +
                     ((y * 0x10 / TOOLBAR_H) << 8) + (y * 0x10 / TOOLBAR_H);
        draw_hline(win->canvas, cw, ch, 0, y, cw, c);
    }
    /* Bottom border */
    draw_hline(win->canvas, cw, ch, 0, TOOLBAR_H - 1, cw, 0xA0A0A0);

    /* Navigation buttons: Back, Forward, Refresh */
    int btn_y = 4, btn_h = 24;
    int bx = 4;

    /* Back button */
    {
        uint32_t bc = (history_pos > 0) ? 0x4488CC : 0xA0A0A0;
        fill_rect(win->canvas, cw, ch, bx, btn_y, NAV_BTN_W, btn_h, bc);
        /* Left arrow < */
        canvas_draw_char(win->canvas, cw, ch, bx + 10, btn_y + 4, '<', 0xFFFFFF);
    }
    bx += NAV_BTN_W + 2;

    /* Forward button */
    {
        uint32_t bc = (history_pos < history_count - 1) ? 0x4488CC : 0xA0A0A0;
        fill_rect(win->canvas, cw, ch, bx, btn_y, NAV_BTN_W, btn_h, bc);
        /* Right arrow > */
        canvas_draw_char(win->canvas, cw, ch, bx + 10, btn_y + 4, '>', 0xFFFFFF);
    }
    bx += NAV_BTN_W + 2;

    /* Refresh button */
    {
        fill_rect(win->canvas, cw, ch, bx, btn_y, NAV_BTN_W, btn_h, 0x4488CC);
        /* Circular arrow (simplified as 'R') */
        canvas_draw_char(win->canvas, cw, ch, bx + 10, btn_y + 4, 'R', 0xFFFFFF);
    }
    bx += NAV_BTN_W + 4;

    /* Go button */
    int go_w = 32;
    int go_x = cw - go_w - 4;
    fill_rect(win->canvas, cw, ch, go_x, btn_y, go_w, btn_h, 0x4488CC);
    canvas_draw_string(win->canvas, cw, ch, go_x + 8, btn_y + 4, "Go", 0xFFFFFF);

    /* URL input field */
    int url_x = bx;
    int url_w = go_x - bx - 4;
    fill_rect(win->canvas, cw, ch, url_x, btn_y, url_w, btn_h, 0xFFFFFF);
    /* Border */
    draw_hline(win->canvas, cw, ch, url_x, btn_y, url_w, 0x808080);
    draw_hline(win->canvas, cw, ch, url_x, btn_y + btn_h - 1, url_w, 0x808080);
    for (int y = btn_y; y < btn_y + btn_h; y++) {
        if (url_x >= 0 && url_x < cw)
            win->canvas[y * cw + url_x] = 0x808080;
        if (url_x + url_w - 1 >= 0 && url_x + url_w - 1 < cw)
            win->canvas[y * cw + url_x + url_w - 1] = 0x808080;
    }

    /* HTTPS lock indicator */
    if (str_ncasecmp(url_bar, "https://", 8) == 0) {
        canvas_draw_char(win->canvas, cw, ch, url_x + 4, btn_y + 4, '*', 0x40A040);
        url_x += 10;
        url_w -= 10;
    }

    /* URL text */
    int max_chars = (url_w - 8) / 8;
    int start = 0;
    if (url_cursor > max_chars - 2)
        start = url_cursor - max_chars + 2;
    for (int ci = start; url_bar[ci] && (ci - start) < max_chars; ci++) {
        canvas_draw_char(win->canvas, cw, ch,
                         url_x + 4 + (ci - start) * 8, btn_y + 4, url_bar[ci], 0x1A1A1A);
    }

    /* Cursor blink */
    if (url_focused) {
        uint64_t t = timer_get_ticks();
        if ((t / 500) & 1) {
            int cx = url_x + 4 + (url_cursor - start) * 8;
            if (cx >= url_x && cx < url_x + url_w)
                fill_rect(win->canvas, cw, ch, cx, btn_y + 4, 2, 16, 0x1A1A1A);
        }
    }

    /* ── Page Content Area ──────────────────────────────────────────── */
    int content_y = TOOLBAR_H;
    int content_h = ch - TOOLBAR_H - STATUS_H;
    int content_w = cw - SCROLLBAR_W;

    if (page_len > 0) {
        /* Render HTML into a sub-canvas area */
        uint32_t *content_canvas = win->canvas + content_y * cw;
        /* Clear content area */
        fill_rect(win->canvas, cw, ch, 0, content_y, content_w, content_h, 0xFFFFFF);
        render_html(page_buf, page_len, content_canvas, cw, content_h, scroll_y);
    } else {
        fill_rect(win->canvas, cw, ch, 0, content_y, content_w, content_h, 0xFFFFFF);
        if (nav_state == NAV_LOADING) {
            canvas_draw_string(win->canvas, cw, ch, content_w / 2 - 40,
                               content_y + content_h / 2, "Loading...", 0x808080);
        }
    }

    /* ── Scrollbar ──────────────────────────────────────────────────── */
    {
        int sb_x = cw - SCROLLBAR_W;
        /* Scrollbar track */
        fill_rect(win->canvas, cw, ch, sb_x, content_y, SCROLLBAR_W, content_h, 0xE0E0E0);
        /* Track border */
        for (int sy = content_y; sy < content_y + content_h && sy < ch; sy++) {
            if (sb_x >= 0 && sb_x < cw) win->canvas[sy * cw + sb_x] = 0xC0C0C0;
        }

        /* Scrollbar thumb */
        int total_h = content_total_h > content_h ? content_total_h : content_h;
        int thumb_h = content_h * content_h / total_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > content_h) thumb_h = content_h;
        int max_scroll = total_h - content_h;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = content_y + (scroll_y * (content_h - thumb_h)) / max_scroll;
        if (thumb_y < content_y) thumb_y = content_y;
        if (thumb_y + thumb_h > content_y + content_h)
            thumb_y = content_y + content_h - thumb_h;

        /* Thumb body with gradient */
        for (int ty = thumb_y; ty < thumb_y + thumb_h && ty < ch; ty++) {
            int rel = ty - thumb_y;
            uint32_t tc2 = 0xA0A0A0 + ((rel * 0x20 / thumb_h) << 16) +
                          ((rel * 0x20 / thumb_h) << 8) + (rel * 0x20 / thumb_h);
            for (int tx = sb_x + 2; tx < sb_x + SCROLLBAR_W - 1 && tx < cw; tx++) {
                if (ty >= 0) win->canvas[ty * cw + tx] = tc2;
            }
        }
        /* Thumb border */
        draw_hline(win->canvas, cw, ch, sb_x + 2, thumb_y, SCROLLBAR_W - 3, 0x808080);
        draw_hline(win->canvas, cw, ch, sb_x + 2, thumb_y + thumb_h - 1, SCROLLBAR_W - 3, 0x808080);
        /* Grip lines on thumb */
        int grip_y = thumb_y + thumb_h / 2;
        for (int gi = -2; gi <= 2; gi += 2) {
            int gy = grip_y + gi;
            if (gy >= content_y && gy < content_y + content_h && gy < ch && gy >= 0)
                for (int gx = sb_x + 4; gx < sb_x + SCROLLBAR_W - 3 && gx < cw; gx++)
                    win->canvas[gy * cw + gx] = 0x808080;
        }
    }

    /* ── Status Bar ─────────────────────────────────────────────────── */
    int sb_y = ch - STATUS_H;
    fill_rect(win->canvas, cw, ch, 0, sb_y, cw, STATUS_H, 0xE0E0E0);
    draw_hline(win->canvas, cw, ch, 0, sb_y, cw, 0xC0C0C0);
    /* Show hover link URL if hovering, otherwise status message */
    const char *sb_text = (hover_url[0]) ? hover_url : status_msg;
    canvas_draw_string(win->canvas, cw, ch, 6, sb_y + 3, sb_text, 0x606060);

    /* Network status indicator */
    uint32_t ind_color = net_is_available() ? 0x40A040 : 0xA04040;
    fill_rect(win->canvas, cw, ch, cw - 14, sb_y + 5, 8, 8, ind_color);

    /* HTTPS indicator */
    if (str_ncasecmp(url_bar, "https://", 8) == 0) {
        canvas_draw_string(win->canvas, cw, ch, cw - 60, sb_y + 3, "HTTPS", 0x40A040);
    }
}

/* ── Mouse ───────────────────────────────────────────────────────────── */
static int prev_buttons = 0;
static int scrollbar_dragging = 0;
static int scrollbar_drag_offset = 0;

static void browser_mouse(window_t *win, int mx, int my, int buttons)
{
    if (!win) return;
    int cw = win->width - 4;
    int ch = win->height - 4;
    int click = (buttons & 1) && !(prev_buttons & 1);
    int release = !(buttons & 1) && (prev_buttons & 1);
    prev_buttons = buttons;

    int content_y = TOOLBAR_H;
    int content_h = ch - TOOLBAR_H - STATUS_H;

    /* Scrollbar dragging */
    if (scrollbar_dragging) {
        if (buttons & 1) {
            int total_h = content_total_h > content_h ? content_total_h : content_h;
            int thumb_h = content_h * content_h / total_h;
            if (thumb_h < 20) thumb_h = 20;
            if (thumb_h > content_h) thumb_h = content_h;
            int max_scroll = total_h - content_h;
            if (max_scroll < 1) max_scroll = 1;
            int track_range = content_h - thumb_h;
            if (track_range < 1) track_range = 1;
            int thumb_y_new = my - scrollbar_drag_offset - content_y;
            scroll_y = thumb_y_new * max_scroll / track_range;
            if (scroll_y < 0) scroll_y = 0;
            if (scroll_y > max_scroll) scroll_y = max_scroll;
        } else {
            scrollbar_dragging = 0;
        }
        return;
    }

    if (click) {
        int btn_y = 4, btn_h = 24;

        /* Back button */
        int bx = 4;
        if (my >= btn_y && my < btn_y + btn_h && mx >= bx && mx < bx + NAV_BTN_W) {
            go_back();
            return;
        }
        bx += NAV_BTN_W + 2;

        /* Forward button */
        if (my >= btn_y && my < btn_y + btn_h && mx >= bx && mx < bx + NAV_BTN_W) {
            go_forward();
            return;
        }
        bx += NAV_BTN_W + 2;

        /* Refresh button */
        if (my >= btn_y && my < btn_y + btn_h && mx >= bx && mx < bx + NAV_BTN_W) {
            refresh_page();
            return;
        }
        bx += NAV_BTN_W + 4;

        /* URL bar click */
        int go_x = cw - 36;
        int url_x = bx;
        int url_w = go_x - bx - 4;
        if (my >= btn_y && my < btn_y + btn_h && mx >= url_x && mx < url_x + url_w) {
            url_focused = 1;
            int rel_x = mx - url_x - 4;
            url_cursor = rel_x / 8;
            int len = str_len(url_bar);
            if (url_cursor > len) url_cursor = len;
            if (url_cursor < 0) url_cursor = 0;
            return;
        }

        /* Go button click */
        if (my >= btn_y && my < btn_y + btn_h && mx >= go_x && mx < cw - 4) {
            url_focused = 0;
            navigate(url_bar);
            return;
        }

        /* Scrollbar click */
        int sb_x = cw - SCROLLBAR_W;
        if (mx >= sb_x && my >= content_y && my < content_y + content_h) {
            /* Calculate thumb position */
            int total_h = content_total_h > content_h ? content_total_h : content_h;
            int thumb_h = content_h * content_h / total_h;
            if (thumb_h < 20) thumb_h = 20;
            if (thumb_h > content_h) thumb_h = content_h;
            int max_scroll = total_h - content_h;
            if (max_scroll < 1) max_scroll = 1;
            int thumb_y = content_y + (scroll_y * (content_h - thumb_h)) / max_scroll;

            if (my >= thumb_y && my < thumb_y + thumb_h) {
                /* Start dragging thumb */
                scrollbar_dragging = 1;
                scrollbar_drag_offset = my - thumb_y;
            } else if (my < thumb_y) {
                /* Click above thumb: page up */
                scroll_y -= content_h;
                if (scroll_y < 0) scroll_y = 0;
            } else {
                /* Click below thumb: page down */
                scroll_y += content_h;
                if (scroll_y > max_scroll) scroll_y = max_scroll;
            }
            return;
        }

        /* Content area click */
        if (my >= TOOLBAR_H && my < ch - STATUS_H && mx < cw - SCROLLBAR_W) {
            url_focused = 0;
            /* Translate to content coordinates (account for scroll) */
            int content_click_y = my - TOOLBAR_H + scroll_y;
            int content_click_x = mx;

            /* Check link regions */
            for (int li = 0; li < link_count; li++) {
                link_region_t *lr = &page_links[li];
                if (content_click_x >= lr->x && content_click_x < lr->x + lr->w &&
                    content_click_y >= lr->y && content_click_y < lr->y + lr->h) {
                    if (lr->href[0]) {
                        str_cpy(url_bar, lr->href);
                        url_cursor = str_len(url_bar);
                        focused_input = -1;
                        navigate(lr->href);
                        return;
                    }
                }
            }

            /* Check form inputs */
            int clicked_input = -1;
            for (int fi = 0; fi < form_input_count; fi++) {
                form_input_t *inp = &form_inputs[fi];
                if (inp->w == 0 && inp->h == 0) continue;  /* hidden input */
                if (content_click_x >= inp->x && content_click_x < inp->x + inp->w &&
                    content_click_y >= inp->y && content_click_y < inp->y + inp->h) {
                    if (inp->is_submit) {
                        /* Submit form */
                        submit_form();
                        return;
                    } else {
                        clicked_input = fi;
                    }
                    break;
                }
            }
            focused_input = clicked_input;
        } else if (my >= TOOLBAR_H) {
            url_focused = 0;
        }
    }

    /* Hover: show link URL in status bar */
    if (my >= TOOLBAR_H && my < ch - STATUS_H && mx < cw - SCROLLBAR_W) {
        int content_hover_y = my - TOOLBAR_H + scroll_y;
        hover_url[0] = 0;
        for (int li = 0; li < link_count; li++) {
            link_region_t *lr = &page_links[li];
            if (mx >= lr->x && mx < lr->x + lr->w &&
                content_hover_y >= lr->y && content_hover_y < lr->y + lr->h) {
                str_cpy(hover_url, lr->href);
                break;
            }
        }
    }

    if (release) {
        scrollbar_dragging = 0;
    }
}

/* ── Keyboard ────────────────────────────────────────────────────────── */
static void browser_key(window_t *win, char ascii, int scancode, int pressed)
{
    if (!win || !pressed) return;

    /* F5 = Refresh */
    if (scancode == 0x3F) {
        refresh_page();
        return;
    }

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
        /* Delete key */
        if (scancode == 0x53) {
            int len = str_len(url_bar);
            if (url_cursor < len) {
                for (int i = url_cursor; i < len; i++)
                    url_bar[i] = url_bar[i + 1];
            }
            return;
        }
        /* Left/Right arrow in URL bar */
        if (scancode == 0x4B) { /* Left */
            if (url_cursor > 0) url_cursor--;
            return;
        }
        if (scancode == 0x4D) { /* Right */
            if (url_cursor < str_len(url_bar)) url_cursor++;
            return;
        }
        /* Home/End in URL bar */
        if (scancode == 0x47) { url_cursor = 0; return; }
        if (scancode == 0x4F) { url_cursor = str_len(url_bar); return; }

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

    /* Form input focus: type into focused text input */
    if (focused_input >= 0 && focused_input < form_input_count) {
        form_input_t *fi = &form_inputs[focused_input];
        if (!fi->is_submit) {
            if (ascii == '\n' || ascii == '\r') {
                /* Enter in text input: submit form */
                submit_form();
                return;
            }
            if (ascii == '\b' || scancode == 0x0E) {
                int len = str_len(fi->value);
                if (len > 0) fi->value[len - 1] = 0;
                return;
            }
            if (ascii == '\t') {
                /* Tab to next input */
                focused_input++;
                while (focused_input < form_input_count && form_inputs[focused_input].is_submit)
                    focused_input++;
                if (focused_input >= form_input_count) focused_input = -1;
                return;
            }
            if (ascii >= 32 && ascii <= 126) {
                int len = str_len(fi->value);
                if (len < FORM_INPUT_MAX - 1) {
                    fi->value[len] = ascii;
                    fi->value[len + 1] = 0;
                }
                return;
            }
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

    browser_win = compositor_create_window("Browser", 80, 40, 700, 500);
    if (!browser_win) return;

    browser_win->on_paint = browser_paint;
    browser_win->on_mouse = browser_mouse;
    browser_win->on_key   = browser_key;
    browser_win->on_close = browser_close;

    /* Reset state */
    history_count = 0;
    history_pos = -1;
    scrollbar_dragging = 0;
    content_total_h = 0;
    focused_input = -1;
    hover_url[0] = 0;
    link_count = 0;
    form_input_count = 0;

    /* Load default homepage */
    load_homepage();
    history_push("about:home");
    prev_buttons = 0;
}
