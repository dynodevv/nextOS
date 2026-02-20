# nextOS

```
                                               ,----..               
                                     ___      /   /   \   .--.--.    
                                   ,--.'|_   /   .     : /  /    '.  
      ,---,                        |  | :,' .   /   ;.  \  :  /`. /  
  ,-+-. /  |           ,--,  ,--,  :  : ' :.   ;   /  ` ;  |  |--`   
 ,--.'|'   |   ,---.   |'. \/ .`|.;__,'  / ;   |  ; \ ; |  :  ;_     
|   |  ,"' |  /     \  '  \/  / ;|  |   |  |   :  | ; | '\  \    `.  
|   | /  | | /    /  |  \  \.' / :__,'| :  .   |  ' ' ' : `----.   \ 
|   | |  | |.    ' / |   \  ;  ;   '  : |__'   ;  \; /  | __ \  \  | 
|   | |  |/ '   ;   /|  / \  \  \  |  | '.'|\   \  ',  / /  /`--'  / 
|   | |--'  '   |  / |./__;   ;  \ ;  :    ; ;   :    / '--'.     /  
|   |/      |   :    ||   :/\  \ ; |  ,   /   \   \ .'    `--'---'   
'---'        \   \  / `---'  `--`   ---`-'     `---`                 
              `----'                                                 
```

A highly optimized 64-bit operating system featuring a skeuomorphic desktop environment.

## Core Specifications

| Feature | Detail |
|---|---|
| **Architecture** | x86_64 (64-bit) |
| **Bootloader** | GRUB 2 (Multiboot2) |
| **Image Format** | Hybrid ISO (Legacy BIOS + UEFI) |
| **Kernel** | Monolithic (C + Assembly) |
| **Graphics** | Double-buffered VESA/GOP framebuffer at 60 Hz+ |
| **Design** | Skeuomorphic (glossy gradients, drop shadows, bevels, textures) |

## Directory Structure

```
nextOS/
├── boot/
│   └── boot.S              # Multiboot2 header, 64-bit entry point
├── kernel/
│   ├── kernel.c             # Main loop, Multiboot2 parsing, subsystem init
│   ├── arch/x86_64/
│   │   ├── gdt.c / gdt.h   # Global Descriptor Table
│   │   ├── idt.c / idt.h   # Interrupt Descriptor Table + PIC
│   │   └── isr.S            # ISR/IRQ stubs and common handler
│   ├── mem/
│   │   ├── pmm.c / pmm.h   # Physical memory manager (bitmap allocator)
│   │   ├── heap.c / heap.h  # Kernel heap (first-fit free list)
│   │   └── paging.c / paging.h  # 4-level page table management
│   ├── drivers/
│   │   ├── keyboard.c / keyboard.h  # PS/2 keyboard (26 layouts incl. Hungarian)
│   │   ├── mouse.c / mouse.h        # PS/2 mouse with IRQ12
│   │   ├── disk.c / disk.h          # ATA PIO + AHCI/NVMe stubs
│   │   └── timer.c / timer.h        # PIT driver (system tick)
│   ├── fs/
│   │   ├── vfs.c / vfs.h    # Virtual File System layer
│   │   ├── fat32.c / fat32.h  # FAT32 read/write driver
│   │   └── ext2.c / ext2.h    # EXT2 read/write driver
│   ├── gfx/
│   │   └── framebuffer.c / framebuffer.h  # Double-buffered framebuffer + 8×16 font
│   └── ui/
│       └── compositor.c / compositor.h    # Skeuomorphic window compositor
├── apps/
│   ├── settings/
│   │   └── settings.c / settings.h   # Settings app (Display, Theme, Keyboard)
│   ├── explorer/
│   │   └── explorer.c / explorer.h   # File Explorer (manila-folder style)
│   └── notepad/
│       └── notepad.c / notepad.h      # Notepad (yellow legal pad style)
├── iso/boot/grub/
│   └── grub.cfg             # GRUB bootloader configuration
├── linker.ld                # Kernel linker script
├── Makefile                 # Build system
└── README.md
```

## Building

### Prerequisites

- **GCC** (x86_64 target, with freestanding headers)
- **NASM** (assembler, for any NASM-syntax files)
- **GNU LD** (linker)
- **GRUB 2** (`grub-mkrescue`)
- **xorriso** (ISO creation backend)
- **mtools** (for GRUB UEFI image creation)

On Ubuntu/Debian:

```bash
sudo apt-get install build-essential nasm grub-pc-bin grub-efi-amd64-bin \
    xorriso mtools grub-common grub2-common
```

### Build Commands

```bash
make          # Build kernel ELF and Hybrid ISO
make clean    # Remove all build artifacts
```

The build produces:
- `nextos.elf` — 64-bit kernel ELF binary
- `nextos.iso` — Bootable Hybrid ISO image

## Running

### QEMU

```bash
qemu-system-x86_64 -cdrom nextos.iso -m 2G
```

### VirtualBox / VMware

Create a new x86_64 VM and attach `nextos.iso` as the CD-ROM.

### Real Hardware

Write the ISO to a USB drive:

```bash
sudo dd if=nextos.iso of=/dev/sdX bs=4M status=progress
```

## Boot Flow

1. **GRUB** loads `nextos.elf` via Multiboot2
2. **boot.S** sets up long mode (64-bit), identity-maps memory, loads GDT, enables SSE
3. **kernel_main()** initialises GDT, IDT, memory, drivers, filesystem, and graphics
4. **Installation check**: Kernel reads disk sector 1 for a magic marker. If found, the installer is skipped.
5. **First Boot Installer** displays: *"Welcome to nextOS."* with an Install button. Installation writes a marker to disk so subsequent boots skip the installer.
6. After installation (or on subsequent boots), the **desktop compositor** renders the skeuomorphic environment

## Applications

### Settings (`Ctrl+1`)

- **Display tab**: Change screen resolution
- **Theme tab**: Toggle between *Brushed Metal* and *Glossy Glass*
- **Keyboard tab**: Select from 26 keyboard layouts (including **Hungarian**)

### File Explorer (`Ctrl+2`)

- Cabinet-style interface with manila folder icons
- Navigates the actual disk filesystem via VFS/FAT32/EXT2
- Directory traversal with back button and keyboard navigation

### Notepad (`Ctrl+3`)

- Yellow legal pad with ruled lines and red margin
- Full text editing (cursor, backspace, enter, arrow keys)
- Open/Save dialogs for reading/writing files to disk

### Browser (`Ctrl+4`)

- Support for both HTTP and HTTPS websites
- Custom HTML renderer engine
- Partial HTML and CSS support

## Keyboard Layouts

The keyboard driver supports 26 layouts with full scancode-to-ASCII mapping:

US English, Hungarian, German, French, Spanish, Italian, Portuguese, UK English, Czech, Polish, Romanian, Slovak, Croatian, Slovenian, Swedish, Norwegian, Danish, Finnish, Dutch, Belgian, Swiss, Turkish, Russian, Japanese, Korean, Brazilian

## Design Philosophy

nextOS follows a **skeuomorphic** design philosophy throughout:

- **Gradients**: Every surface uses two-stop gradients (no flat colours)
- **Bevels**: Raised/sunken border effects on all interactive elements
- **Drop Shadows**: Cast shadows behind windows and panels
- **Gloss**: Top-highlight effect simulating reflected light
- **Textures**: Leather-like panels, brushed metal, and glass themes
- **Icons**: Manila folders, document icons with dog-ear corners

## Known Issues

- None ATM

## License

MIT License — see [LICENSE](LICENSE).
