/*
 * nextOS - timer.h
 * PIT (Programmable Interval Timer) driver
 */
#ifndef NEXTOS_TIMER_H
#define NEXTOS_TIMER_H

#include <stdint.h>

void     timer_init(uint32_t freq_hz);
uint64_t timer_get_ticks(void);
void     timer_sleep_ms(uint32_t ms);

#endif /* NEXTOS_TIMER_H */
