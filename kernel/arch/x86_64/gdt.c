/*
 * nextOS - gdt.c
 * Global Descriptor Table initialisation for 64-bit mode
 */
#include "gdt.h"

static struct gdt_entry gdt_entries[7];
static struct tss_entry tss;
static struct gdt_ptr   gdt_pointer;

extern void gdt_flush(uint64_t);

static void gdt_set_gate(int idx, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran)
{
    gdt_entries[idx].base_low    = base & 0xFFFF;
    gdt_entries[idx].base_mid    = (base >> 16) & 0xFF;
    gdt_entries[idx].base_high   = (base >> 24) & 0xFF;
    gdt_entries[idx].limit_low   = limit & 0xFFFF;
    gdt_entries[idx].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[idx].access      = access;
}

void gdt_init(void)
{
    gdt_pointer.limit = sizeof(gdt_entries) - 1;
    gdt_pointer.base  = (uint64_t)&gdt_entries;

    /* Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);
    /* Kernel Code 64-bit : selector 0x08 */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);
    /* Kernel Data 64-bit : selector 0x10 */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xC0);
    /* User Code 64-bit   : selector 0x18 */
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xA0);
    /* User Data 64-bit   : selector 0x20 */
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* TSS descriptor (occupies two GDT slots: 5 & 6) */
    uint64_t tss_base = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    gdt_set_gate(5, (uint32_t)tss_base, tss_limit, 0x89, 0x00);
    /* Upper 32 bits of TSS base */
    gdt_entries[6].limit_low   = (tss_base >> 32) & 0xFFFF;
    gdt_entries[6].base_low    = (tss_base >> 48) & 0xFFFF;
    gdt_entries[6].base_mid    = 0;
    gdt_entries[6].access      = 0;
    gdt_entries[6].granularity = 0;
    gdt_entries[6].base_high   = 0;

    gdt_flush((uint64_t)&gdt_pointer);
}
