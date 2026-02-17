/*
 * nextOS - mouse.h
 * PS/2 Mouse driver
 */
#ifndef NEXTOS_MOUSE_H
#define NEXTOS_MOUSE_H

#include <stdint.h>

typedef struct {
    int      x, y;
    int      dx, dy;
    uint8_t  buttons;  /* bit 0 = left, bit 1 = right, bit 2 = middle */
    int      scroll;   /* scroll wheel delta: -1 = up, +1 = down, 0 = none */
} mouse_state_t;

void         mouse_init(void);
mouse_state_t mouse_get_state(void);
int          mouse_consume_scroll(void);  /* Returns and clears scroll delta */
void         mouse_set_bounds(int max_x, int max_y);
void         mouse_set_speed(int speed);  /* 1-10, default 5 */
int          mouse_get_speed(void);

#endif /* NEXTOS_MOUSE_H */
