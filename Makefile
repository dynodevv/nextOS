# ─────────────────────────────────────────────────────────────────────────────
# nextOS Makefile
# Builds the kernel ELF and generates a Hybrid ISO (Legacy BIOS + UEFI)
# ─────────────────────────────────────────────────────────────────────────────

# ── Toolchain ────────────────────────────────────────────────────────────────
CC      := gcc
AS      := nasm
LD      := ld

GCC_INCDIR := $(shell $(CC) -print-file-name=include)
CFLAGS  := -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INCDIR) \
           -fno-builtin -fno-stack-protector \
           -mno-red-zone -m64 -Wall -Wextra -O2 -mcmodel=kernel \
           -fno-pie -fno-pic -I.
LDFLAGS := -n -T linker.ld -nostdlib -z noexecstack
ASFLAGS := -f elf64

# ── Sources ──────────────────────────────────────────────────────────────────
# Boot assembly (uses GNU as syntax, compiled by gcc)
BOOT_S  := boot/boot.S

# Kernel architecture (uses GNU as syntax)
ISR_S   := kernel/arch/x86_64/isr.S

# C sources
C_SRCS  := kernel/kernel.c \
           kernel/arch/x86_64/gdt.c \
           kernel/arch/x86_64/idt.c \
           kernel/mem/pmm.c \
           kernel/mem/heap.c \
           kernel/mem/paging.c \
           kernel/drivers/keyboard.c \
           kernel/drivers/mouse.c \
           kernel/drivers/disk.c \
           kernel/drivers/timer.c \
           kernel/gfx/framebuffer.c \
           kernel/fs/vfs.c \
           kernel/fs/fat32.c \
           kernel/fs/ext2.c \
           kernel/fs/ramfs.c \
           kernel/ui/compositor.c \
           apps/settings/settings.c \
           apps/explorer/explorer.c \
           apps/notepad/notepad.c

# ── Object files ─────────────────────────────────────────────────────────────
BOOT_OBJ := $(BOOT_S:.S=.o)
ISR_OBJ  := $(ISR_S:.S=.o)
C_OBJS   := $(C_SRCS:.c=.o)

ALL_OBJS := $(BOOT_OBJ) $(ISR_OBJ) $(C_OBJS)

# ── Outputs ──────────────────────────────────────────────────────────────────
KERNEL       := nextos.elf
KERNEL_BASE  := nextos-base.elf
DISK_IMG     := diskimg.bin
DISK_OBJ     := diskimg.o
ISO          := nextOS.iso
ISODIR       := isodir

# ── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all clean iso

all: $(ISO)

# Compile boot assembly (GNU as syntax via gcc)
boot/boot.o: boot/boot.S
	$(CC) $(CFLAGS) -c $< -o $@

# Compile ISR assembly (GNU as syntax via gcc)
kernel/arch/x86_64/isr.o: kernel/arch/x86_64/isr.S
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pass 1: Link base kernel (without embedded disk image)
$(KERNEL_BASE): $(ALL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)

# Generate bootable disk image containing GRUB + base kernel
$(DISK_IMG): $(KERNEL_BASE) iso/boot/grub/grub.cfg tools/mkdiskimg.sh
	bash tools/mkdiskimg.sh $(KERNEL_BASE) $(DISK_IMG) iso/boot/grub/grub.cfg

# Convert disk image to a linkable object file
$(DISK_OBJ): $(DISK_IMG)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.diskimg,contents,alloc,load,readonly,data \
		$(DISK_IMG) $(DISK_OBJ)

# Pass 2: Link final kernel with embedded disk image
$(KERNEL): $(ALL_OBJS) $(DISK_OBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(DISK_OBJ)

# Build ISO
$(ISO): $(KERNEL) iso/boot/grub/grub.cfg
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/nextos.elf
	cp iso/boot/grub/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISODIR) 2>/dev/null || \
		grub-mkrescue -o $@ $(ISODIR)

clean:
	rm -f $(ALL_OBJS) $(KERNEL) $(KERNEL_BASE) $(DISK_IMG) $(DISK_OBJ) $(ISO)
	rm -rf $(ISODIR)
