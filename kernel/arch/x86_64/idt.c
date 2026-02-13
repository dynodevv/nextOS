/*
 * nextOS - idt.c
 * Interrupt Descriptor Table and PIC initialisation
 */
#include "idt.h"

static struct idt_entry idt_entries[IDT_ENTRIES];
static struct idt_ptr   idt_pointer;
static isr_handler_t    irq_handlers[IDT_ENTRIES];

/* Defined in isr.S */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void idt_set_gate(int idx, uint64_t handler, uint16_t sel, uint8_t flags)
{
    idt_entries[idx].offset_low  = handler & 0xFFFF;
    idt_entries[idx].offset_mid  = (handler >> 16) & 0xFFFF;
    idt_entries[idx].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_entries[idx].selector    = sel;
    idt_entries[idx].ist         = 0;
    idt_entries[idx].type_attr   = flags;
    idt_entries[idx].zero        = 0;
}

static void pic_remap(void)
{
    /* ICW1: init + ICW4 needed */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    /* ICW2: vector offsets */
    outb(0x21, 0x20); io_wait();   /* Master PIC -> IRQ 32-39 */
    outb(0xA1, 0x28); io_wait();   /* Slave PIC  -> IRQ 40-47 */
    /* ICW3: cascade */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    /* Mask all interrupts initially */
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void irq_register_handler(int irq, isr_handler_t handler)
{
    irq_handlers[irq] = handler;
}

/* Called from assembly ISR common stub */
void isr_handler(uint64_t irq_num, uint64_t error_code)
{
    if (irq_handlers[irq_num]) {
        irq_handlers[irq_num](irq_num, error_code);
    }

    /* Send EOI if this was a hardware IRQ */
    if (irq_num >= 32 && irq_num < 48) {
        if (irq_num >= 40) {
            outb(0xA0, 0x20);   /* EOI to slave PIC */
        }
        outb(0x20, 0x20);       /* EOI to master PIC */
    }
}

void idt_init(void)
{
    idt_pointer.limit = sizeof(idt_entries) - 1;
    idt_pointer.base  = (uint64_t)&idt_entries;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        irq_handlers[i] = 0;
    }

    /* CPU exceptions 0-31 */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    /* Remap the PIC */
    pic_remap();

    /* Hardware IRQs 0-15 mapped to vectors 32-47 */
    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

    /* Load IDT */
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));
    /* Enable interrupts */
    __asm__ volatile("sti");
}
