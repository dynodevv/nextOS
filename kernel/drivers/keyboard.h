/*
 * nextOS - keyboard.h
 * PS/2 Keyboard driver with multi-layout support (including Hungarian)
 */
#ifndef NEXTOS_KEYBOARD_H
#define NEXTOS_KEYBOARD_H

#include <stdint.h>

/* Key event structure */
typedef struct {
    uint8_t  scancode;
    char     ascii;       /* Translated character (0 if non-printable) */
    int      pressed;     /* 1 = key down, 0 = key up */
    int      shift;
    int      ctrl;
    int      alt;
} key_event_t;

/* Keyboard layouts */
typedef enum {
    KB_LAYOUT_US = 0,
    KB_LAYOUT_HU,
    KB_LAYOUT_DE,
    KB_LAYOUT_FR,
    KB_LAYOUT_ES,
    KB_LAYOUT_IT,
    KB_LAYOUT_PT,
    KB_LAYOUT_UK,
    KB_LAYOUT_CZ,
    KB_LAYOUT_PL,
    KB_LAYOUT_RO,
    KB_LAYOUT_SK,
    KB_LAYOUT_HR,
    KB_LAYOUT_SI,
    KB_LAYOUT_SE,
    KB_LAYOUT_NO,
    KB_LAYOUT_DK,
    KB_LAYOUT_FI,
    KB_LAYOUT_NL,
    KB_LAYOUT_BE,
    KB_LAYOUT_CH,
    KB_LAYOUT_TR,
    KB_LAYOUT_RU,
    KB_LAYOUT_JP,
    KB_LAYOUT_KR,
    KB_LAYOUT_BR,
    KB_LAYOUT_COUNT
} kb_layout_t;

/* Extended scancode constants (after E0 prefix) */
#define KEY_SCANCODE_LWIN  0x5B

void         keyboard_init(void);
void         keyboard_set_layout(kb_layout_t layout);
kb_layout_t  keyboard_get_layout(void);
const char  *keyboard_layout_name(kb_layout_t layout);
int          keyboard_poll(key_event_t *event);

#endif /* NEXTOS_KEYBOARD_H */
