/*
 * nextOS - boot.S
 * Multiboot2 header and 64-bit entry point
 * Sets up long mode, initialises page tables, and jumps to the C kernel.
 */

.set MULTIBOOT2_MAGIC,    0xE85250D6
.set MULTIBOOT2_ARCH,     0            /* i386 protected mode */
.set HEADER_LENGTH,       (multiboot_header_end - multiboot_header)
.set CHECKSUM,            -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + HEADER_LENGTH)

/* ── Multiboot2 Header ─────────────────────────────────────────────────── */
.section .multiboot
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long HEADER_LENGTH
    .long CHECKSUM

    /* ── Framebuffer tag ── request 1024x768x32 ── */
    .align 8
framebuffer_tag:
    .short 5                    /* type  = framebuffer */
    .short 0                    /* flags = optional    */
    .long  20                   /* size                */
    .long  1024                 /* width               */
    .long  768                  /* height              */
    .long  32                   /* depth               */

    /* ── End tag ── */
    .align 8
    .short 0
    .short 0
    .long  8
multiboot_header_end:

/* ── Bootstrap Stack ───────────────────────────────────────────────────── */
.section .bss
.align 16
stack_bottom:
    .skip 65536                /* 64 KiB bootstrap stack */
stack_top:

/* ── Initial Page Tables (identity-map first 2 GiB) ───────────────────── */
.section .bss
.align 4096
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4096

/* ── 32-bit Entry Point ───────────────────────────────────────────────── */
.section .text
.code32
.global _start
_start:
    cli
    /* Save multiboot info pointer (ebx) */
    movl %ebx, %edi

    /* ── Build page tables ── */
    /* PML4[0] -> PDPT */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax           /* present + writable */
    movl %eax, boot_pml4

    /* PDPT[0] -> PD */
    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt

    /* PD: map 512 x 2 MiB pages (1 GiB identity map) */
    movl $0, %ecx
    movl $boot_pd, %ebx
.fill_pd:
    movl %ecx, %eax
    shll $21, %eax             /* ecx * 2 MiB */
    orl  $0x83, %eax           /* present + writable + huge page */
    movl %eax, (%ebx)
    movl $0,   4(%ebx)
    addl $8, %ebx
    incl %ecx
    cmpl $512, %ecx
    jne  .fill_pd

    /* ── Enable PAE ── */
    movl %cr4, %eax
    orl  $0x20, %eax           /* CR4.PAE */
    movl %eax, %cr4

    /* ── Load PML4 ── */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* ── Enable Long Mode ── */
    movl $0xC0000080, %ecx     /* IA32_EFER */
    rdmsr
    orl  $0x100, %eax          /* LME bit */
    wrmsr

    /* ── Enable Paging ── */
    movl %cr0, %eax
    orl  $0x80000001, %eax     /* PG + PE */
    movl %eax, %cr0

    /* ── Load 64-bit GDT and far jump ── */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

/* ── 64-bit Code ──────────────────────────────────────────────────────── */
.code64
long_mode_entry:
    /* Set up segment registers */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Set up stack */
    movq $stack_top, %rsp

    /* edi already has multiboot info pointer from 32-bit code */
    /* Sign-extend to rdi for System V ABI */
    movl %edi, %edi

    /* Call the C kernel main */
    call kernel_main

    /* Halt if kernel returns */
.halt:
    cli
    hlt
    jmp .halt

/* ── Bootstrap GDT (64-bit) ───────────────────────────────────────────── */
.section .rodata
.align 16
gdt64:
    .quad 0x0000000000000000   /* null descriptor */
    .quad 0x00AF9A000000FFFF   /* 64-bit code: DPL0 */
    .quad 0x00CF92000000FFFF   /* 64-bit data: DPL0 */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64
