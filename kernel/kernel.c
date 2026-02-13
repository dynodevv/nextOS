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
#define FALLBACK_FB_ADDR   0xFD000000  /* Common QEMU framebuffer address */

/* ── Installer / first boot state ─────────────────────────────────────── */
static int installer_active = 1;
static int installer_step = 0;  /* 0=welcome, 1=detect, 2=progress, 3=done */
static int install_progress = 0;
static int install_disk_found = 0;
static uint64_t install_disk_sectors = 0;

static void write_mbr_to_disk(void); /* forward declaration */

/* ── Draw a cursor arrow (used on the installer screen) ───────────────── */
/* Now uses the same bitmap cursor as the desktop compositor */

/* ── Helper: draw a gradient bar into the framebuffer ─────────────────── */
static uint32_t inst_lerp(uint32_t a, uint32_t b, int t, int max)
{
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * t / max;
    int rg = ag + (bg - ag) * t / max;
    int rb = ab + (bb - ab) * t / max;
    if (rr < 0) rr = 0;
    if (rr > 255) rr = 255;
    if (rg < 0) rg = 0;
    if (rg > 255) rg = 255;
    if (rb < 0) rb = 0;
    if (rb > 255) rb = 255;
    return rgb((uint8_t)rr, (uint8_t)rg, (uint8_t)rb);
}

static void inst_draw_button(int bx, int by, int bw, int bh,
                             const char *label, uint32_t top, uint32_t bot)
{
    for (int row = 0; row < bh; row++) {
        uint32_t c = inst_lerp(top, bot, row, bh);
        for (int col = 0; col < bw; col++)
            fb_putpixel(bx + col, by + row, c);
    }
    /* Bevel */
    for (int col = 0; col < bw; col++) {
        fb_putpixel(bx + col, by, rgba_blend(fb_getpixel(bx + col, by), 0xFFFFFF, 100));
        fb_putpixel(bx + col, by + bh - 1, rgba_blend(fb_getpixel(bx + col, by + bh - 1), 0x000000, 60));
    }
    int llen = 0; const char *s = label; while (*s++) llen++;
    fb_draw_string(bx + (bw - llen * 8) / 2, by + (bh - 16) / 2,
                   label, 0xFFFFFF, 0x00000000);
}

/* ── Draw the installer background ────────────────────────────────────── */
static void inst_draw_bg(int w, int h)
{
    for (int y = 0; y < h; y++) {
        uint8_t r = 20 + y * 40 / h;
        uint8_t g = 40 + y * 60 / h;
        uint8_t b = 80 + y * 100 / h;
        for (int x = 0; x < w; x++)
            fb_putpixel(x, y, rgb(r, g, b));
    }
}

/* ── Draw a panel (centred card with shadow and gloss) ────────────────── */
static void inst_draw_panel(int px, int py, int pw, int ph)
{
    /* Shadow */
    fb_fill_rect(px + 6, py + 6, pw, ph, 0x101820);
    /* Body gradient */
    for (int row = 0; row < ph; row++) {
        uint32_t c = rgb(220 - row / 4, 215 - row / 4, 200 - row / 4);
        for (int col = 0; col < pw; col++)
            fb_putpixel(px + col, py + row, c);
    }
    /* Gloss (top 40%) */
    for (int row = 0; row < ph * 2 / 5; row++) {
        uint8_t alpha = 50 - (uint8_t)(row * 50 / (ph * 2 / 5));
        for (int col = 0; col < pw; col++) {
            uint32_t p = fb_getpixel(px + col, py + row);
            fb_putpixel(px + col, py + row, rgba_blend(p, 0xFFFFFF, alpha));
        }
    }
    /* Border */
    fb_draw_rect(px, py, pw, ph, 0x605040);
}

/* ── Draw the first-boot installer screen ─────────────────────────────── */
static void draw_installer(void)
{
    framebuffer_t *f = fb_get();
    int w = f->width, h = f->height;
    int pw = 520, ph = 300;
    int px = (w - pw) / 2, py = (h - ph) / 2;

    inst_draw_bg(w, h);
    inst_draw_panel(px, py, pw, ph);

    if (installer_step == 0) {
        /* ── Welcome screen ───────────────────────────────────────── */
        const char *title = "Welcome to nextOS";
        int tlen = 17;
        fb_draw_string(px + (pw - tlen * 8) / 2, py + 30, title, 0x1A1A2A, 0x00000000);

        const char *sub = "Click Install to begin installation.";
        int slen = 36;
        fb_draw_string(px + (pw - slen * 8) / 2, py + 60, sub, 0x404050, 0x00000000);

        inst_draw_button(px + (pw - 200) / 2, py + 140, 200, 44,
                         "Install nextOS", rgb(60, 120, 200), rgb(30, 60, 120));

    } else if (installer_step == 1) {
        /* ── Disk detection screen ────────────────────────────────── */
        fb_draw_string(px + 30, py + 30, "Detecting disk...", 0x1A1A2A, 0x00000000);

        if (install_disk_found) {
            fb_draw_string(px + 30, py + 60, "Disk found!", 0x206020, 0x00000000);

            /* Show disk info */
            char info[64] = "Size: ";
            uint64_t mb = install_disk_sectors / 2048;
            char tmp[16]; int ti = 0;
            if (mb == 0) { tmp[ti++] = '0'; }
            else { while (mb > 0) { tmp[ti++] = '0' + mb % 10; mb /= 10; } }
            int si = 6;
            while (ti > 0) info[si++] = tmp[--ti];
            info[si++] = ' '; info[si++] = 'M'; info[si++] = 'B'; info[si] = 0;
            fb_draw_string(px + 30, py + 80, info, 0x303030, 0x00000000);

            inst_draw_button(px + (pw - 200) / 2, py + 180, 200, 44,
                             "Begin Installation", rgb(60, 160, 60), rgb(20, 80, 20));
        } else {
            fb_draw_string(px + 30, py + 60, "No disk detected.", 0xA02020, 0x00000000);
            fb_draw_string(px + 30, py + 80, "Continuing to live desktop...", 0x606060, 0x00000000);

            inst_draw_button(px + (pw - 200) / 2, py + 180, 200, 44,
                             "Continue", rgb(100, 100, 160), rgb(50, 50, 80));
        }

    } else if (installer_step == 2) {
        /* ── Installation progress ────────────────────────────────── */
        fb_draw_string(px + 30, py + 30, "Installing nextOS...", 0x1A1A2A, 0x00000000);

        /* Progress bar */
        int bar_x = px + 40, bar_y = py + 80, bar_w = pw - 80, bar_h = 30;
        fb_fill_rect(bar_x, bar_y, bar_w, bar_h, 0xC0C0C0);
        fb_draw_rect(bar_x, bar_y, bar_w, bar_h, 0x505050);

        int fill_w = (bar_w - 4) * install_progress / 100;
        for (int row = 0; row < bar_h - 4; row++) {
            uint32_t c = inst_lerp(rgb(80, 160, 80), rgb(40, 100, 40), row, bar_h - 4);
            for (int col = 0; col < fill_w; col++)
                fb_putpixel(bar_x + 2 + col, bar_y + 2 + row, c);
        }
        /* Gloss on progress bar */
        for (int row = 0; row < (bar_h - 4) / 2; row++) {
            uint8_t alpha = 40 - (uint8_t)(row * 40 / ((bar_h - 4) / 2));
            for (int col = 0; col < fill_w; col++) {
                uint32_t p = fb_getpixel(bar_x + 2 + col, bar_y + 2 + row);
                fb_putpixel(bar_x + 2 + col, bar_y + 2 + row,
                            rgba_blend(p, 0xFFFFFF, alpha));
            }
        }

        /* Percentage text */
        char pct[8];
        int pi = 0;
        int pv = install_progress;
        if (pv >= 100) { pct[pi++] = '1'; pct[pi++] = '0'; pct[pi++] = '0'; }
        else if (pv >= 10) { pct[pi++] = '0' + pv / 10; pct[pi++] = '0' + pv % 10; }
        else { pct[pi++] = '0' + pv; }
        pct[pi++] = '%'; pct[pi] = 0;
        fb_draw_string(px + (pw - pi * 8) / 2, py + 120, pct, 0x1A1A2A, 0x00000000);

        /* Status text */
        const char *status = "Copying system files...";
        if (install_progress > 30) status = "Installing drivers...";
        if (install_progress > 60) status = "Configuring desktop...";
        if (install_progress > 85) status = "Finalizing...";
        fb_draw_string(px + 30, py + 150, status, 0x505050, 0x00000000);

    } else if (installer_step == 3) {
        /* ── Installation complete ────────────────────────────────── */
        fb_draw_string(px + 30, py + 30, "Installation Complete!", 0x206020, 0x00000000);
        fb_draw_string(px + 30, py + 60, "nextOS has been installed.", 0x404050, 0x00000000);
        fb_draw_string(px + 30, py + 80, "Click below to start.", 0x404050, 0x00000000);

        inst_draw_button(px + (pw - 200) / 2, py + 180, 200, 44,
                         "Start nextOS", rgb(60, 120, 200), rgb(30, 60, 120));
    }

    /* Draw cursor on top */
    mouse_state_t ms = mouse_get_state();
    compositor_draw_cursor(ms.x, ms.y);

    fb_swap();
}

static void handle_installer_input(void)
{
    static int prev_btn = 0;
    mouse_state_t ms = mouse_get_state();
    framebuffer_t *f = fb_get();
    int w = f->width, h = f->height;
    int pw = 520, ph = 300;
    int px = (w - pw) / 2, py = (h - ph) / 2;

    int click = (ms.buttons & 1) && !(prev_btn & 1);
    prev_btn = ms.buttons;
    if (!click && installer_step != 2) return;

    if (installer_step == 0) {
        /* Welcome screen — Install button */
        int bw = 200, bh = 44;
        int bx = px + (pw - bw) / 2, by = py + 140;
        if (ms.x >= bx && ms.x < bx + bw && ms.y >= by && ms.y < by + bh) {
            /* Detect disk */
            disk_device_t *disk = disk_get_primary();
            if (disk) {
                install_disk_found = 1;
                install_disk_sectors = disk->total_sectors;
            } else {
                install_disk_found = 0;
            }
            installer_step = 1;
        }
    } else if (installer_step == 1) {
        /* Disk detection screen — Begin/Continue button */
        int bw = 200, bh = 44;
        int bx = px + (pw - bw) / 2, by = py + 180;
        if (ms.x >= bx && ms.x < bx + bw && ms.y >= by && ms.y < by + bh) {
            installer_step = 2;
            install_progress = 0;
        }
    } else if (installer_step == 2) {
        /* Progress auto-advances; write MBR at the right moment */
        install_progress++;
        if (install_progress == 50 && install_disk_found) {
            write_mbr_to_disk();
        }
        if (install_progress > 100) {
            installer_step = 3;
        }
    } else if (installer_step == 3) {
        /* Done screen — Start button */
        int bw = 200, bh = 44;
        int bx = px + (pw - bw) / 2, by = py + 180;
        if (ms.x >= bx && ms.x < bx + bw && ms.y >= by && ms.y < by + bh) {
            installer_active = 0;
        }
    }
}

/* ── MBR boot record for disk installation ────────────────────────────── */
/*
 * Minimal x86 real-mode MBR that prints a message and reboots on keypress.
 * Hand-assembled 16-bit code loaded at 0x7C00 by BIOS:
 *   cli; xor ax,ax; mov ds/es/ss,ax; mov sp,0x7C00; sti
 *   mov si, msg; lodsb; test al,al; jz done
 *   mov ah,0x0E; mov bx,7; int 0x10; jmp loop
 *   done: xor ah,ah; int 0x16; int 0x19; jmp $
 */
static void write_mbr_to_disk(void)
{
    disk_device_t *disk = disk_get_primary();
    if (!disk) return;

    static uint8_t mbr[512];
    /* Zero the sector */
    for (int i = 0; i < 512; i++) mbr[i] = 0;

    /* Boot code */
    static const uint8_t code[] = {
        0xFA,                       /*  0: cli                    */
        0x31, 0xC0,                 /*  1: xor ax, ax             */
        0x8E, 0xD8,                 /*  3: mov ds, ax             */
        0x8E, 0xC0,                 /*  5: mov es, ax             */
        0x8E, 0xD0,                 /*  7: mov ss, ax             */
        0xBC, 0x00, 0x7C,           /*  9: mov sp, 0x7C00         */
        0xFB,                       /* 12: sti                    */
        0xBE, 0x26, 0x7C,           /* 13: mov si, 0x7C26 (msg)  */
        /* print_loop: */
        0xAC,                       /* 16: lodsb                  */
        0x84, 0xC0,                 /* 17: test al, al            */
        0x74, 0x09,                 /* 19: jz done (-> 30)        */
        0xB4, 0x0E,                 /* 21: mov ah, 0x0E           */
        0xBB, 0x07, 0x00,           /* 23: mov bx, 0x0007         */
        0xCD, 0x10,                 /* 26: int 0x10               */
        0xEB, 0xF2,                 /* 28: jmp print_loop (-> 16) */
        /* done: */
        0xB4, 0x00,                 /* 30: mov ah, 0              */
        0xCD, 0x16,                 /* 32: int 0x16               */
        0xCD, 0x19,                 /* 34: int 0x19               */
        0xEB, 0xFE,                 /* 36: jmp $ (halt)           */
    };
    for (int i = 0; i < (int)sizeof(code); i++) mbr[i] = code[i];

    /* Message at offset 0x26 (38) */
    static const char msg[] =
        "nextOS installed successfully.\r\n"
        "Boot from CD/USB to start nextOS.\r\n"
        "Press any key to reboot...\r\n";
    for (int i = 0; msg[i] && (38 + i) < 510; i++)
        mbr[38 + i] = (uint8_t)msg[i];

    /* Boot signature at offset 510-511 */
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    disk_write(disk, 0, 1, mbr);
}

/* ── System actions (shutdown, restart, about window) ─────────────────── */
#define ACPI_PM1A_CTRL_PORT 0x604   /* QEMU ACPI power management port */
#define ACPI_SLP_TYPa_SLP_EN 0x2000 /* S5 sleep type | sleep enable */
#define ACPI_BOCHS_PORT     0xB004  /* Bochs-specific ACPI port */
#define KB_CTRL_PORT        0x64    /* Keyboard controller port */
#define KB_CMD_RESET        0xFE    /* Pulse reset line */

static void system_shutdown(void)
{
    /* QEMU / Bochs ACPI shutdown */
    outw(ACPI_PM1A_CTRL_PORT, ACPI_SLP_TYPa_SLP_EN);
    /* Fallback: Bochs-specific */
    outw(ACPI_BOCHS_PORT, ACPI_SLP_TYPa_SLP_EN);
    /* If still running, halt */
    while (1) __asm__ volatile("cli; hlt");
}

static void system_restart(void)
{
    /* Pulse the keyboard controller reset line */
    outb(KB_CTRL_PORT, KB_CMD_RESET);
    while (1) __asm__ volatile("cli; hlt");
}

static window_t *about_win = (void *)0;

static void about_close(window_t *win)
{
    (void)win;
    about_win = (void *)0;
}

static void about_paint(window_t *win)
{
    if (!win || !win->canvas) return;
    int cw = win->width - 4;
    int ch = win->height - 4;

    /* Background gradient */
    for (int row = 0; row < ch; row++) {
        int tr = 220, tg = 215, tb = 200;
        int br = 180, bg = 170, bb = 150;
        int denom = (ch > 1) ? ch - 1 : 1;
        int rr = tr + (br - tr) * row / denom;
        int rg = tg + (bg - tg) * row / denom;
        int rb = tb + (bb - tb) * row / denom;
        uint32_t c = ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
        for (int col = 0; col < cw; col++)
            win->canvas[row * cw + col] = c;
    }

    /* Title */
    extern const uint8_t font_8x16[95][16];
    const char *title = "nextOS";
    int tx = (cw - 6 * 8) / 2;
    for (int i = 0; title[i]; i++) {
        if (title[i] < 32 || title[i] > 126) continue;
        const uint8_t *glyph = font_8x16[title[i] - 32];
        for (int row = 0; row < 16; row++) {
            int py = 20 + row;
            if (py >= ch) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    int px = tx + i * 8 + col;
                    if (px >= 0 && px < cw)
                        win->canvas[py * cw + px] = 0x1A1A2A;
                }
            }
        }
    }

    /* Version & info lines */
    const char *lines[] = {
        "Version 1.0",
        "",
        "A next-generation",
        "operating system.",
        "",
        "64-bit x86_64 kernel",
    };
    for (int l = 0; l < 6; l++) {
        const char *s = lines[l];
        int sy = 50 + l * 18;
        for (int i = 0; s[i] && s[i] >= 32 && s[i] <= 126; i++) {
            const uint8_t *glyph = font_8x16[s[i] - 32];
            for (int row = 0; row < 16; row++) {
                int py = sy + row;
                if (py >= ch) continue;
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        int px = 20 + i * 8 + col;
                        if (px >= 0 && px < cw)
                            win->canvas[py * cw + px] = 0x303040;
                    }
                }
            }
        }
    }
}

static void launch_about(void)
{
    if (about_win && about_win->active) return;
    about_win = compositor_create_window("About nextOS", 280, 200, 250, 200);
    if (!about_win) return;
    about_win->on_paint = about_paint;
    about_win->on_close = about_close;
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

/* ── App launcher (used by start menu and desktop icon clicks) ────────── */
static void launch_app_by_index(int index)
{
    switch (index) {
    case 0: settings_launch(); break;
    case 1: explorer_launch(); break;
    case 2: notepad_launch();  break;
    case 3: launch_about();    break;
    case 4: system_restart();  break;
    case 5: system_shutdown(); break;
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
        fb_addr = FALLBACK_FB_ADDR;
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

    /* Register app launcher for start menu and desktop icon clicks */
    compositor_set_app_launcher(launch_app_by_index);

    /* ── Main loop ─────────────────────────────────────────────────── */
    while (1) {
        /* Process keyboard events */
        key_event_t kev;
        while (keyboard_poll(&kev)) {
            if (!installer_active && kev.pressed) {
                /* Windows/Super key toggles start menu */
                if (kev.scancode == KEY_SCANCODE_LWIN) {
                    compositor_toggle_start_menu();
                    continue;
                }

                compositor_handle_key(kev.ascii, kev.scancode, kev.pressed);

                /* Ctrl+1/2/3 to launch apps */
                if (kev.ctrl && kev.ascii == '1') settings_launch();
                if (kev.ctrl && kev.ascii == '2') explorer_launch();
                if (kev.ctrl && kev.ascii == '3') notepad_launch();
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
