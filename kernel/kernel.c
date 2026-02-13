/*
 * nextOS - kernel.c
 * Kernel entry point and main loop
 *
 * Parses the Multiboot2 info structure, initialises all subsystems,
 * and runs the desktop compositor loop at ~60 Hz.
 */
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mem/pmm.h"
#include "mem/heap.h"
#include "mem/paging.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/disk.h"
#include "drivers/timer.h"
#include "gfx/framebuffer.h"
#include "fs/vfs.h"
#include "ui/compositor.h"
#include "../apps/settings/settings.h"
#include "../apps/explorer/explorer.h"
#include "../apps/notepad/notepad.h"

#include <stdint.h>

/* ── Multiboot2 tag types ─────────────────────────────────────────────── */
#define MB2_TAG_END          0
#define MB2_TAG_CMDLINE      1
#define MB2_TAG_BOOTLOADER   2
#define MB2_TAG_BASIC_MEMINFO 4
#define MB2_TAG_MMAP         6
#define MB2_TAG_FRAMEBUFFER  8

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
} __attribute__((packed)) mb2_tag_framebuffer_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed)) mb2_tag_meminfo_t;

/* ── Kernel heap region ───────────────────────────────────────────────── */
#define KERNEL_HEAP_START  0x400000    /* 4 MiB */
#define KERNEL_HEAP_SIZE   0x1000000   /* 16 MiB */

/* ── Installer / first boot state ─────────────────────────────────────── */
static int installer_active = 1;
static int installer_choice = -1;  /* -1=pending, 0=install, 1=live */

/* ── Draw the first-boot installer screen ─────────────────────────────── */
static void draw_installer(void)
{
    framebuffer_t *f = fb_get();
    int w = f->width, h = f->height;

    /* Gradient background */
    for (int y = 0; y < h; y++) {
        uint8_t r = 20 + y * 40 / h;
        uint8_t g = 40 + y * 60 / h;
        uint8_t b = 80 + y * 100 / h;
        for (int x = 0; x < w; x++) {
            fb_putpixel(x, y, rgb(r, g, b));
        }
    }

    /* Welcome panel (centred, skeuomorphic card) */
    int pw = 500, ph = 260;
    int px = (w - pw) / 2, py = (h - ph) / 2;

    /* Panel shadow */
    fb_fill_rect(px + 6, py + 6, pw, ph, 0x101820);
    /* Panel body gradient */
    for (int row = 0; row < ph; row++) {
        uint32_t c = rgb(220 - row / 4, 215 - row / 4, 200 - row / 4);
        for (int col = 0; col < pw; col++) {
            fb_putpixel(px + col, py + row, c);
        }
    }
    /* Gloss (top 40%) */
    for (int row = 0; row < ph * 2 / 5; row++) {
        uint8_t alpha = 50 - (uint8_t)(row * 50 / (ph * 2 / 5));
        for (int col = 0; col < pw; col++) {
            uint32_t px2 = fb_getpixel(px + col, py + row);
            fb_putpixel(px + col, py + row, rgba_blend(px2, 0xFFFFFF, alpha));
        }
    }

    /* Title */
    const char *title = "Welcome to nextOS";
    int tx = px + (pw - 17 * 8) / 2;
    fb_draw_string(tx, py + 30, title, 0x1A1A2A, 0x00000000);

    /* Subtitle */
    const char *sub = "Would you like to install to disk or try Live Mode?";
    int sx = px + (pw - 51 * 8) / 2;
    fb_draw_string(sx, py + 60, sub, 0x404050, 0x00000000);

    /* Install button */
    int bw = 180, bh = 40;
    int b1x = px + pw / 2 - bw - 20, b1y = py + 140;
    for (int row = 0; row < bh; row++) {
        uint32_t c = rgb(80 + row, 130 + row / 2, 200 - row);
        for (int col = 0; col < bw; col++) {
            fb_putpixel(b1x + col, b1y + row, c);
        }
    }
    fb_draw_string(b1x + (bw - 15 * 8) / 2, b1y + 12,
                   "Install to Disk", 0xFFFFFF, 0x00000000);

    /* Live Mode button */
    int b2x = px + pw / 2 + 20, b2y = py + 140;
    for (int row = 0; row < bh; row++) {
        uint32_t c = rgb(60 + row, 160 - row / 3, 80 + row);
        for (int col = 0; col < bw; col++) {
            fb_putpixel(b2x + col, b2y + row, c);
        }
    }
    fb_draw_string(b2x + (bw - 9 * 8) / 2, b2y + 12,
                   "Live Mode", 0xFFFFFF, 0x00000000);

    fb_swap();
}

static void handle_installer_input(void)
{
    mouse_state_t ms = mouse_get_state();
    framebuffer_t *f = fb_get();
    int w = f->width, h = f->height;
    int pw = 500, ph = 260;
    int px = (w - pw) / 2, py = (h - ph) / 2;
    int bw = 180, bh = 40;
    int b1x = px + pw / 2 - bw - 20, b1y = py + 140;
    int b2x = px + pw / 2 + 20, b2y = py + 140;

    if (ms.buttons & 1) {
        if (ms.x >= b1x && ms.x < b1x + bw &&
            ms.y >= b1y && ms.y < b1y + bh) {
            installer_choice = 0;
            installer_active = 0;
        }
        if (ms.x >= b2x && ms.x < b2x + bw &&
            ms.y >= b2y && ms.y < b2y + bh) {
            installer_choice = 1;
            installer_active = 0;
        }
    }
}

/* ── Parse Multiboot2 info ────────────────────────────────────────────── */
static uint64_t fb_addr = 0;
static uint32_t fb_width = 1024, fb_height = 768, fb_pitch = 0, fb_bpp = 32;
static uint64_t total_memory = 128 * 1024 * 1024; /* default 128 MiB */

static void parse_multiboot2(uint64_t mb_info_addr)
{
    if (!mb_info_addr) return;

    uint8_t *ptr = (uint8_t *)mb_info_addr;
    /* Skip total_size (4 bytes) + reserved (4 bytes) */
    ptr += 8;

    while (1) {
        mb2_tag_t *tag = (mb2_tag_t *)ptr;
        if (tag->type == MB2_TAG_END) break;

        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            mb2_tag_framebuffer_t *fb_tag = (mb2_tag_framebuffer_t *)tag;
            fb_addr   = fb_tag->addr;
            fb_width  = fb_tag->width;
            fb_height = fb_tag->height;
            fb_pitch  = fb_tag->pitch;
            fb_bpp    = fb_tag->bpp;
        }

        if (tag->type == MB2_TAG_BASIC_MEMINFO) {
            mb2_tag_meminfo_t *mem = (mb2_tag_meminfo_t *)tag;
            total_memory = ((uint64_t)mem->mem_upper + 1024) * 1024;
        }

        /* Advance to next tag (8-byte aligned) */
        uint32_t advance = (tag->size + 7) & ~7;
        ptr += advance;
    }
}

/* ── Kernel main ──────────────────────────────────────────────────────── */
void kernel_main(uint64_t mb_info_addr)
{
    /* 1. Initialise architecture */
    gdt_init();
    idt_init();

    /* 2. Parse Multiboot2 info */
    parse_multiboot2(mb_info_addr);

    /* 3. Memory management */
    pmm_init(total_memory);
    paging_init(total_memory);
    heap_init(KERNEL_HEAP_START, KERNEL_HEAP_SIZE);

    /* 4. Framebuffer */
    if (fb_addr == 0) {
        /* Fallback: use a known VGA text-mode area or halt */
        fb_addr = 0xFD000000; /* Common QEMU framebuffer address */
    }
    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    mouse_set_bounds(fb_width, fb_height);

    /* 5. Drivers */
    timer_init(1000);  /* 1 kHz tick */
    keyboard_init();
    mouse_init();
    disk_init();

    /* 6. Filesystem */
    vfs_init();

    /* 7. Compositor */
    compositor_init();

    /* ── Main loop ─────────────────────────────────────────────────── */
    while (1) {
        /* Process keyboard events */
        key_event_t kev;
        while (keyboard_poll(&kev)) {
            if (installer_active) {
                /* Installer handles keyboard if needed */
            } else {
                if (kev.pressed) {
                    compositor_handle_key(kev.ascii, kev.scancode, kev.pressed);

                    /* Ctrl+1/2/3 to launch apps */
                    if (kev.ctrl && kev.ascii == '1') settings_launch();
                    if (kev.ctrl && kev.ascii == '2') explorer_launch();
                    if (kev.ctrl && kev.ascii == '3') notepad_launch();
                }
            }
        }

        /* Process mouse */
        mouse_state_t ms = mouse_get_state();

        if (installer_active) {
            draw_installer();
            handle_installer_input();
        } else {
            /* Desktop compositor frame */
            compositor_render_frame();
            compositor_handle_mouse(ms.x, ms.y, ms.buttons);
            fb_swap();
        }

        /* Target ~60 FPS (sleep ~16 ms between frames) */
        timer_sleep_ms(16);
    }
}
