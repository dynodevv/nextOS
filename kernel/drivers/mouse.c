/*
 * nextOS - mouse.c
 * PS/2 Mouse driver with IRQ12 handling
 */
#include "mouse.h"
#include "../arch/x86_64/idt.h"

static mouse_state_t state = {0, 0, 0, 0, 0, 0};
static int max_x = 1024;
static int max_y = 768;
static int mouse_speed = 5;  /* 1-10, default 5 */

static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[4];
static int     has_scroll_wheel = 0;  /* 1 if Intellimouse mode active */

static void mouse_wait_write(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(0x64) & 0x02)) return;
    }
}

static void mouse_wait_read(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(0x64) & 0x01) return;
    }
}

static void mouse_write(uint8_t val)
{
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, val);
}

static void mouse_irq(uint64_t irq, uint64_t err)
{
    (void)irq; (void)err;
    uint8_t data = inb(0x60);

    switch (mouse_cycle) {
    case 0:
        mouse_bytes[0] = (int8_t)data;
        if (data & 0x08) mouse_cycle = 1;  /* valid packet start */
        break;
    case 1:
        mouse_bytes[1] = (int8_t)data;
        mouse_cycle = 2;
        break;
    case 2:
        mouse_bytes[2] = (int8_t)data;
        if (!has_scroll_wheel) {
            mouse_cycle = 0;
        } else {
            mouse_cycle = 3;
            break;
        }
        goto process_packet;
    case 3:
        mouse_bytes[3] = (int8_t)data;
        mouse_cycle = 0;
        goto process_packet;
    }
    return;

process_packet:
    state.buttons = mouse_bytes[0] & 0x07;
    state.dx = (int)mouse_bytes[1] * mouse_speed / 5;
    state.dy = (-(int)mouse_bytes[2]) * mouse_speed / 5;
    if (has_scroll_wheel)
        state.scroll += (int)mouse_bytes[3];

    state.x += state.dx;
    state.y += state.dy;

    /* Clamp to screen bounds */
    if (state.x < 0)     state.x = 0;
    if (state.y < 0)     state.y = 0;
    if (state.x >= max_x) state.x = max_x - 1;
    if (state.y >= max_y) state.y = max_y - 1;
}

void mouse_init(void)
{
    /* Enable auxiliary device (mouse) */
    mouse_wait_write();
    outb(0x64, 0xA8);

    /* Enable IRQ12 */
    mouse_wait_write();
    outb(0x64, 0x20);
    mouse_wait_read();
    uint8_t status = inb(0x60) | 0x02;
    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, status);

    /* Use default settings and enable streaming */
    mouse_write(0xF6); mouse_wait_read(); inb(0x60);

    /* Enable Intellimouse scroll wheel: magic sample rate sequence 200,100,80 */
    mouse_write(0xF3); mouse_wait_read(); inb(0x60);
    mouse_write(200);  mouse_wait_read(); inb(0x60);
    mouse_write(0xF3); mouse_wait_read(); inb(0x60);
    mouse_write(100);  mouse_wait_read(); inb(0x60);
    mouse_write(0xF3); mouse_wait_read(); inb(0x60);
    mouse_write(80);   mouse_wait_read(); inb(0x60);

    /* Get device ID to check if scroll wheel is available */
    mouse_write(0xF2); mouse_wait_read(); inb(0x60);  /* ACK */
    mouse_wait_read();
    uint8_t id = inb(0x60);
    has_scroll_wheel = (id == 3 || id == 4);  /* 3 = Intellimouse, 4 = 5-button */

    /* Set sample rate to 200 for smoother tracking */
    mouse_write(0xF3); mouse_wait_read(); inb(0x60);
    mouse_write(200);  mouse_wait_read(); inb(0x60);
    mouse_write(0xF4); mouse_wait_read(); inb(0x60);

    irq_register_handler(44, mouse_irq);  /* IRQ12 -> vector 44 */
}

mouse_state_t mouse_get_state(void)
{
    return state;
}

int mouse_consume_scroll(void)
{
    int s = state.scroll;
    state.scroll = 0;
    return s;
}

void mouse_set_bounds(int mx, int my)
{
    max_x = mx;
    max_y = my;
}

void mouse_set_speed(int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;
    mouse_speed = speed;
}

int mouse_get_speed(void)
{
    return mouse_speed;
}
