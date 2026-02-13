/*
 * nextOS - isr.S
 * Interrupt Service Routine stubs and common handler for x86_64
 */

.code64

/* ── GDT flush (called from C) ────────────────────────────────────────── */
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    /* Reload CS via far return */
    pushq $0x08
    leaq .gdt_reload(%rip), %rax
    pushq %rax
    lretq
.gdt_reload:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ret

/* ── Macro: ISR without error code ──────────────────────────────────── */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0           /* dummy error code */
    pushq $\num        /* interrupt number  */
    jmp isr_common
.endm

/* ── Macro: ISR with error code ─────────────────────────────────────── */
.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm

/* ── Macro: IRQ stub ────────────────────────────────────────────────── */
.macro IRQ num, mapped
.global irq\num
irq\num:
    pushq $0
    pushq $\mapped
    jmp isr_common
.endm

/* CPU exception stubs */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

/* Hardware IRQ stubs (mapped to vectors 32-47) */
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* ── Common ISR handler ─────────────────────────────────────────────── */
.extern isr_handler

isr_common:
    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Arguments for isr_handler(irq_num, error_code) */
    movq 120(%rsp), %rdi   /* interrupt number */
    movq 128(%rsp), %rsi   /* error code       */

    call isr_handler

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp   /* remove error code + irq number */
    iretq
