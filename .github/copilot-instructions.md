# nextOS - Copilot Instructions

nextOS is a highly optimized 64-bit operating system featuring a skeuomorphic desktop environment. It's written in C and x86_64 assembly, boots via GRUB 2 (Multiboot2), and runs as a monolithic kernel with a custom compositor, window system, and filesystem drivers.

## Project Overview

- **Architecture**: x86_64 (64-bit) monolithic kernel
- **Language**: C (freestanding) + x86_64 assembly (GNU as and NASM syntax)
- **Build System**: GNU Make
- **Bootloader**: GRUB 2 with Multiboot2 protocol
- **Graphics**: Custom double-buffered VESA/GOP framebuffer compositor
- **Design**: Skeuomorphic UI (glossy gradients, drop shadows, bevels, textures)

## Build and Test

### Build Commands
```bash
make          # Build kernel ELF and Hybrid ISO
make clean    # Remove all build artifacts
```

### Build Outputs
- `nextos.elf` — 64-bit kernel ELF binary (final kernel with embedded disk image)
- `nextos-base.elf` — Base kernel without embedded disk image (intermediate build artifact)
- `diskimg.bin` — Bootable disk image containing GRUB + kernel (intermediate)
- `diskimg.o` — Disk image converted to linkable object (intermediate)
- `nextOS.iso` — Bootable Hybrid ISO image (final deliverable)

### Testing
Test in QEMU:
```bash
qemu-system-x86_64 -cdrom nextOS.iso -m 256M
```

For comprehensive testing:
```bash
# Test with AHCI disk
qemu-system-x86_64 -cdrom nextOS.iso -m 256M -hda test.img -boot d

# Test with q35 machine (ICH9 chipset)
qemu-system-x86_64 -M q35 -cdrom nextOS.iso -m 256M
```

## Code Standards

### C Code Requirements
1. **Freestanding environment**: No standard library. Do not use libc functions (printf, malloc, memcpy, etc.) except what's provided by the kernel
2. **No red zone**: All code must compile with `-mno-red-zone` flag
3. **Kernel model**: Use `-mcmodel=kernel` for proper addressing in high memory
4. **Position independent**: Never use `-fPIC` or `-fPIE` — the kernel is loaded at a fixed address
5. **Include paths**: Use relative includes from project root (e.g., `#include "kernel/mem/heap.h"`)

### Assembly Code
- **Boot code** (`boot/boot.S`): GNU as syntax, compiled with `gcc`
- **ISR/IRQ stubs** (`kernel/arch/x86_64/isr.S`): GNU as syntax, compiled with `gcc`
- **Other assembly** (if added): Use NASM syntax with `.asm` extension

### Memory Safety
- Always check for NULL pointers before dereferencing
- Use proper alignment for hardware structures (e.g., 1024-byte for AHCI command lists, 256-byte for FIS)
- Be careful with DMA buffers — they must be in the identity-mapped region (first 4 GiB)
- All AHCI structures use static BSS memory for DMA safety

### Coding Style
- Use 4-space indentation (no tabs)
- Opening braces on same line for functions and control structures
- Variable names: lowercase with underscores (e.g., `cmd_list`, `port_base`)
- Function names: lowercase with underscores (e.g., `ahci_init`, `window_create`)
- Constants: UPPERCASE with underscores (e.g., `INSTALL_MARKER_SECTOR`)
- Limited comments — only for complex logic or hardware interactions

## Repository Structure

### Core Kernel (`kernel/`)
- `kernel.c` — Main entry point, initialization, main loop
- `arch/x86_64/` — Architecture-specific code (GDT, IDT, ISR)
- `mem/` — Memory management (physical allocator, heap, paging)
- `drivers/` — Hardware drivers (keyboard, mouse, disk, timer)
- `fs/` — Filesystems (VFS, FAT32, EXT2)
- `gfx/` — Graphics (framebuffer driver)
- `ui/` — User interface (compositor, window system)

### Applications (`apps/`)
- `settings/` — Settings application (display, theme, keyboard)
- `explorer/` — File explorer with manila folder icons
- `notepad/` — Text editor with yellow legal pad design

### Build System
- `Makefile` — Two-pass build: base kernel → disk image → final kernel with embedded image
- `linker.ld` — Custom linker script (note: `.diskimg` section MUST come before `.bss`)
- `tools/mkdiskimg.sh` — Creates bootable disk image with GRUB

## Important Implementation Details

### Two-Pass Build System
The build uses a two-stage process:
1. **Pass 1**: Build base kernel (`nextos-base.elf`) without embedded disk image
2. Create bootable disk image (`diskimg.bin`) with GRUB + base kernel
3. Convert disk to object file (`diskimg.o`) using objcopy with proper flags
4. **Pass 2**: Link final kernel (`nextos.elf`) with all objects + embedded disk image

**Critical**: When using `objcopy --rename-section`, MUST include `contents` flag or the section becomes zero-filled. Correct usage:
```bash
objcopy --rename-section .data=.diskimg,contents,alloc,load,readonly,data
```

### Linker Script Constraints
In `linker.ld`, the `.diskimg` section MUST be placed BEFORE `.bss`. If placed after, the linker creates a separate LOAD segment that GRUB's Multiboot2 loader doesn't load, resulting in zeros in memory.

### AHCI Driver Architecture
- Uses shared static buffers for all AHCI operations (single-threaded by design)
- Only one AHCI port is initialized and used at a time
- All structures (command list, FIS, command table, data buffer) are in BSS with proper alignment
- Uses polling, not interrupts
- Must scan all 8 PCI functions for multi-function devices (ICH9 AHCI at bus 0, device 31, function 2)

### ATA Driver Requirements
- Needs floating bus detection (status 0xFF = no controller)
- Requires timeouts in `ata_wait_bsy`/`ata_wait_drq` to avoid hangs
- Port 0x1F0 may return 0xFF on q35 or have CDROM causing hangs without timeouts

### Installation System
- First boot shows installer with "Welcome to nextOS" and Install button
- Installation writes magic marker (0x6E785F4F + 0x494E5354) to disk sector 1
- Subsequent boots check for marker and skip installer if found

### Window System
- Windows have `on_close` callback — apps MUST set their static window pointer to NULL in the callback
- Z-order: unfocused windows drawn first, focused window last
- Hit-testing uses `window_at()` which checks focused window first to match visual z-order
- Titlebar buttons: close (red), maximize (green), minimize (yellow) circles with radius 7
- Desktop icons use double-click detection (500ms window)

### Keyboard Driver
- Handles E0 prefix for extended scancodes
- Win key is `KEY_SCANCODE_LWIN` (0x5B)
- Supports 26 keyboard layouts including Hungarian

## Design Philosophy

All UI elements follow skeuomorphic design:
- **Gradients**: Every surface uses two-stop gradients (no flat colors)
- **Bevels**: Raised/sunken border effects on all interactive elements
- **Drop Shadows**: Cast shadows behind windows and panels
- **Gloss**: Top-highlight effect simulating reflected light
- **Textures**: Leather-like panels, brushed metal, and glass themes
- **Icons**: Manila folders, document icons with dog-ear corners

When adding UI elements, maintain this visual style consistently.

## Testing and Quality

### Before Committing
1. Build with `make` and verify no warnings or errors
2. Test boot in QEMU with `qemu-system-x86_64 -cdrom nextOS.iso -m 256M`
3. Test basic functionality (mouse, keyboard, windows, apps)
4. For driver changes, test with both `qemu-system-x86_64` (default i440FX) and `-M q35` (ICH9)

### What NOT to Change
- Do not modify the two-pass build structure without careful consideration
- Do not change linker script section ordering (`.diskimg` before `.bss`)
- Do not add PIE/PIC compilation flags
- Do not remove working code unless absolutely necessary
- Do not break the skeuomorphic design aesthetic

## Common Pitfalls to Avoid

1. **Using standard library**: This is freestanding C. No libc functions available.
2. **Forgetting `-mno-red-zone`**: Required for kernel code that handles interrupts.
3. **Wrong objcopy flags**: Must include `contents` flag when embedding binary data.
4. **Linker section order**: `.diskimg` must come before `.bss`.
5. **AHCI alignment**: Command list needs 1024-byte alignment, FIS needs 256-byte.
6. **Memory addressing**: DMA buffers must be in first 4 GiB (identity-mapped).
7. **PCI scanning**: Must check all 8 functions for multi-function devices.
8. **Window callbacks**: Apps must NULL their window pointer in `on_close` callback.

## Documentation

When making changes:
- Update `README.md` if adding new features or changing build process
- Update `TODO.md` if implementing items from the TODO list
- Maintain this instructions file if discovering new conventions or requirements
