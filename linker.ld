/*
 * nextOS Kernel Linker Script
 * Links the kernel for loading by GRUB (Multiboot2).
 * Kernel is loaded at the 1 MiB physical boundary.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Multiboot2 header must appear very early in the binary */
    .multiboot ALIGN(4K) :
    {
        *(.multiboot)
    }

    .text ALIGN(4K) :
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    }

    /* Embedded bootable disk image (written to disk during installation).
     * Placed BEFORE .bss so it is part of the main LOAD segment's file
     * data.  If placed after .bss, the linker emits a separate LOAD
     * segment that GRUB's Multiboot2 ELF loader does not load.          */
    .diskimg ALIGN(4K) :
    {
        *(.diskimg)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    _end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
