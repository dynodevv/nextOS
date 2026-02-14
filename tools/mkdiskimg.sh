#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# nextOS - mkdiskimg.sh
# Creates a bootable disk image containing GRUB + the nextOS kernel.
# This image is embedded into the ISO kernel so the installer can
# write it to a target disk, making the disk independently bootable.
# ─────────────────────────────────────────────────────────────────────────────
set -e

KERNEL_ELF="$1"          # Path to the (first-pass) kernel ELF
OUTPUT_IMG="$2"           # Output disk image path
GRUB_CFG="$3"             # Path to grub.cfg

if [ -z "$KERNEL_ELF" ] || [ -z "$OUTPUT_IMG" ] || [ -z "$GRUB_CFG" ]; then
    echo "Usage: $0 <kernel.elf> <output.img> <grub.cfg>" >&2
    exit 1
fi

TMPDIR=$(mktemp -d)
cleanup() {
    sudo umount "${TMPDIR}/mnt" 2>/dev/null || true
    rm -rf "${TMPDIR}"
}
trap cleanup EXIT

DISK_IMG="${TMPDIR}/disk.img"
MOUNT_DIR="${TMPDIR}/mnt"
GRUB_DIR="${TMPDIR}/grub-files"

mkdir -p "${MOUNT_DIR}" "${GRUB_DIR}"

# ── Step 1: Create GRUB core.img with embedded early config ──────────────
cat > "${TMPDIR}/grub_early.cfg" << 'GRUBCFG'
set root='(hd0,msdos1)'
set prefix=(hd0,msdos1)/boot/grub
insmod normal
normal
# Fallback: if normal-mode config loading fails, boot directly
multiboot2 /boot/nextos.elf
boot
GRUBCFG

grub-mkimage -O i386-pc -o "${GRUB_DIR}/core.img" \
    -c "${TMPDIR}/grub_early.cfg" \
    -p /boot/grub \
    biosdisk part_msdos ext2 multiboot2 boot normal configfile

cp /usr/lib/grub/i386-pc/boot.img "${GRUB_DIR}/boot.img"

CORE_SIZE=$(wc -c < "${GRUB_DIR}/core.img")
CORE_SECTORS=$(( (CORE_SIZE + 511) / 512 ))
KERNEL_SIZE=$(wc -c < "${KERNEL_ELF}")

# ── Step 2: Create raw disk image ───────────────────────────────────────
# Layout: 1 MiB MBR gap (sectors 0-2047) + partition (sector 2048+)
# Partition needs: kernel + grub.cfg + ext2 overhead
# Use 2 MiB total (plenty of room)
dd if=/dev/zero of="${DISK_IMG}" bs=1M count=2 2>/dev/null

# ── Step 3: Create MBR partition table ──────────────────────────────────
echo "label: dos
start=2048, type=83, bootable" | sfdisk "${DISK_IMG}" >/dev/null 2>&1

# ── Step 4: Set up loop device, format, and populate ────────────────────
LOOP=$(sudo losetup --find --show --partscan "${DISK_IMG}")

sudo mkfs.ext2 -b 1024 -N 64 -L NEXTOS "${LOOP}p1" >/dev/null 2>&1

sudo mount "${LOOP}p1" "${MOUNT_DIR}"
sudo mkdir -p "${MOUNT_DIR}/boot/grub"
sudo cp "${KERNEL_ELF}" "${MOUNT_DIR}/boot/nextos.elf"
sudo cp "${GRUB_CFG}" "${MOUNT_DIR}/boot/grub/grub.cfg"
sudo umount "${MOUNT_DIR}"

# ── Step 5: Install GRUB boot sector + core ─────────────────────────────
sudo grub-bios-setup \
    -d "${GRUB_DIR}" \
    -f -s \
    "${LOOP}"

sudo losetup -d "${LOOP}"

# ── Step 6: Copy result ─────────────────────────────────────────────────
cp "${DISK_IMG}" "${OUTPUT_IMG}"

IMG_SIZE=$(wc -c < "${OUTPUT_IMG}")
IMG_SECTORS=$((IMG_SIZE / 512))
echo "Disk image created: ${OUTPUT_IMG} (${IMG_SIZE} bytes, ${IMG_SECTORS} sectors)"
