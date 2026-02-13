/*
 * nextOS - timer.c
 * PIT driver — provides system tick at the configured frequency
 */
#include "timer.h"
#include "../arch/x86_64/idt.h"

static volatile uint64_t tick_count = 0;
static uint32_t timer_freq = 0;

static void timer_irq(uint64_t irq, uint64_t err)
{
    (void)irq; (void)err;
    tick_count++;
}

void timer_init(uint32_t freq_hz)
{
    timer_freq = freq_hz;

    uint32_t divisor = 1193180 / freq_hz;
    outb(0x43, 0x36);                      /* Channel 0, lo/hi, mode 3 */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register_handler(32, timer_irq);   /* IRQ0 -> vector 32 */
}

uint64_t timer_get_ticks(void)
{
    return tick_count;
}

void timer_sleep_ms(uint32_t ms)
{
    uint64_t target = tick_count + (uint64_t)ms * timer_freq / 1000;
    while (tick_count < target) {
        __asm__ volatile("hlt");
    }
}
