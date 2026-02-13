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
} mouse_state_t;

void         mouse_init(void);
mouse_state_t mouse_get_state(void);
void         mouse_set_bounds(int max_x, int max_y);

#endif /* NEXTOS_MOUSE_H */
