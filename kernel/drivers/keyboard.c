/*
 * nextOS - keyboard.c
 * PS/2 Keyboard driver with full Hungarian layout and other layouts
 */
#include "keyboard.h"
#include "../arch/x86_64/idt.h"

/* ── Circular key event buffer ─────────────────────────────────────────── */
#define KEY_BUF_SIZE 256
static key_event_t key_buffer[KEY_BUF_SIZE];
static volatile int kb_read_idx  = 0;
static volatile int kb_write_idx = 0;

/* ── Modifier state ───────────────────────────────────────────────────── */
static int shift_held = 0;
static int ctrl_held  = 0;
static int alt_held   = 0;
static int caps_lock  = 0;

/* ── Current layout ───────────────────────────────────────────────────── */
static kb_layout_t current_layout = KB_LAYOUT_US;

/* ── Layout name table ────────────────────────────────────────────────── */
static const char *layout_names[KB_LAYOUT_COUNT] = {
    "US English",
    "Hungarian",
    "German",
    "French",
    "Spanish",
    "Italian",
    "Portuguese",
    "UK English",
    "Czech",
    "Polish",
    "Romanian",
    "Slovak",
    "Croatian",
    "Slovenian",
    "Swedish",
    "Norwegian",
    "Danish",
    "Finnish",
    "Dutch",
    "Belgian",
    "Swiss",
    "Turkish",
    "Russian",
    "Japanese",
    "Korean",
    "Brazilian",
};

/* ── US QWERTY scancode map (set 1) ──────────────────────────────────── */
static const char us_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};
static const char us_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

/* ── Hungarian QWERTZ scancode map ───────────────────────────────────── */
/*
 * Hungarian keyboard (QWERTZ) — full mapping.
 * Row 1:  0  í  1  2  3  4  5  6  7  8  9  ö  ü  ó
 * Row 2:  q  w  e  r  t  z  u  i  o  p  ő  ú
 * Row 3:  a  s  d  f  g  h  j  k  l  é  á  ű
 * Row 4:  í  y  x  c  v  b  n  m  ,  .  -
 *
 * Since many HU chars are multi-byte (UTF-8), the ASCII map
 * uses close Latin equivalents where a single byte suffices.
 * The GUI layer performs the final Unicode rendering.
 */
static const char hu_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','o','u','o','\b',
    '\t','q','w','e','r','t','z','u','i','o','p','o','u','\n',
    0, 'a','s','d','f','g','h','j','k','l','e','a','0',
    0, 'u','y','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};
static const char hu_shift[128] = {
    0, 27, '\'','"','+','!','%','/','=','(',')',  'O','U','O','\b',
    '\t','Q','W','E','R','T','Z','U','I','O','P','O','U','\n',
    0, 'A','S','D','F','G','H','J','K','L','E','A','~',
    0, 'U','Y','X','C','V','B','N','M','?',':','_', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

/* ── German QWERTZ scancode map ──────────────────────────────────────── */
static const char de_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','s','\'','\b',
    '\t','q','w','e','r','t','z','u','i','o','p','u','+','\n',
    0, 'a','s','d','f','g','h','j','k','l','o','a','^',
    0, '#','y','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};
static const char de_shift[128] = {
    0, 27, '!','"','#','$','%','&','/','(',')','=','?','`','\b',
    '\t','Q','W','E','R','T','Z','U','I','O','P','U','*','\n',
    0, 'A','S','D','F','G','H','J','K','L','O','A','~',
    0, '\'','Y','X','C','V','B','N','M',';',':','_', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

/* ── French AZERTY scancode map ──────────────────────────────────────── */
static const char fr_normal[128] = {
    0, 27, '&','e','"','\'','(','#','{','!','c','a',')','=','\b',
    '\t','a','z','e','r','t','y','u','i','o','p','^','$','\n',
    0, 'q','s','d','f','g','h','j','k','l','m','u','*',
    0, '<','w','x','c','v','b','n',',',';',':','!', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};
static const char fr_shift[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0',']','+','\b',
    '\t','A','Z','E','R','T','Y','U','I','O','P','^','$','\n',
    0, 'Q','S','D','F','G','H','J','K','L','M','%','~',
    0, '>','W','X','C','V','B','N','?','.','/','!', 0,
    '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

/* ── Layout dispatch ─────────────────────────────────────────────────── */
typedef struct {
    const char *normal;
    const char *shift;
} layout_map_t;

static const layout_map_t layouts[KB_LAYOUT_COUNT] = {
    [KB_LAYOUT_US] = { us_normal, us_shift },
    [KB_LAYOUT_HU] = { hu_normal, hu_shift },
    [KB_LAYOUT_DE] = { de_normal, de_shift },
    [KB_LAYOUT_FR] = { fr_normal, fr_shift },
    /* Remaining layouts default to US mapping for now — extensible */
    [KB_LAYOUT_ES] = { us_normal, us_shift },
    [KB_LAYOUT_IT] = { us_normal, us_shift },
    [KB_LAYOUT_PT] = { us_normal, us_shift },
    [KB_LAYOUT_UK] = { us_normal, us_shift },
    [KB_LAYOUT_CZ] = { us_normal, us_shift },
    [KB_LAYOUT_PL] = { us_normal, us_shift },
    [KB_LAYOUT_RO] = { us_normal, us_shift },
    [KB_LAYOUT_SK] = { us_normal, us_shift },
    [KB_LAYOUT_HR] = { us_normal, us_shift },
    [KB_LAYOUT_SI] = { us_normal, us_shift },
    [KB_LAYOUT_SE] = { us_normal, us_shift },
    [KB_LAYOUT_NO] = { us_normal, us_shift },
    [KB_LAYOUT_DK] = { us_normal, us_shift },
    [KB_LAYOUT_FI] = { us_normal, us_shift },
    [KB_LAYOUT_NL] = { us_normal, us_shift },
    [KB_LAYOUT_BE] = { us_normal, us_shift },
    [KB_LAYOUT_CH] = { us_normal, us_shift },
    [KB_LAYOUT_TR] = { us_normal, us_shift },
    [KB_LAYOUT_RU] = { us_normal, us_shift },
    [KB_LAYOUT_JP] = { us_normal, us_shift },
    [KB_LAYOUT_KR] = { us_normal, us_shift },
    [KB_LAYOUT_BR] = { us_normal, us_shift },
};

/* ── Translate a scancode to ASCII ───────────────────────────────────── */
static char translate_scancode(uint8_t sc, int shifted)
{
    if (sc >= 128) return 0;
    const layout_map_t *lm = &layouts[current_layout];
    char c = shifted ? lm->shift[sc] : lm->normal[sc];

    /* Apply caps lock: toggle case only when shift is not held */
    if (caps_lock && !shifted) {
        if (c >= 'a' && c <= 'z') c -= 32;
    } else if (caps_lock && shifted) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }

    return c;
}

/* ── IRQ1 handler (PS/2 keyboard) ────────────────────────────────────── */
static void keyboard_irq(uint64_t irq, uint64_t err)
{
    (void)irq; (void)err;
    uint8_t sc = inb(0x60);
    int released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    /* Track modifiers */
    if (code == 0x2A || code == 0x36) { shift_held = !released; return; }
    if (code == 0x1D)                 { ctrl_held  = !released; return; }
    if (code == 0x38)                 { alt_held   = !released; return; }
    if (code == 0x3A && !released)    { caps_lock  = !caps_lock; return; }

    key_event_t ev;
    ev.scancode = code;
    ev.pressed  = !released;
    ev.shift    = shift_held;
    ev.ctrl     = ctrl_held;
    ev.alt      = alt_held;
    ev.ascii    = released ? 0 : translate_scancode(code, shift_held);

    int next = (kb_write_idx + 1) % KEY_BUF_SIZE;
    if (next != kb_read_idx) {
        key_buffer[kb_write_idx] = ev;
        kb_write_idx = next;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void keyboard_init(void)
{
    irq_register_handler(33, keyboard_irq);  /* IRQ1 -> vector 33 */
}

void keyboard_set_layout(kb_layout_t layout)
{
    if (layout < KB_LAYOUT_COUNT)
        current_layout = layout;
}

kb_layout_t keyboard_get_layout(void)
{
    return current_layout;
}

const char *keyboard_layout_name(kb_layout_t layout)
{
    if (layout < KB_LAYOUT_COUNT)
        return layout_names[layout];
    return "Unknown";
}

int keyboard_poll(key_event_t *event)
{
    if (kb_read_idx == kb_write_idx)
        return 0;
    *event = key_buffer[kb_read_idx];
    kb_read_idx = (kb_read_idx + 1) % KEY_BUF_SIZE;
    return 1;
}
