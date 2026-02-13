/*
 * nextOS - disk.c
 * ATA PIO mode disk driver with AHCI/NVMe stubs
 */
#include "disk.h"
#include "../arch/x86_64/idt.h"

/* Primary ATA I/O ports */
#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

/* ATA registers (offsets from io_base) */
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LO     0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HI     0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_STATUS     0x07
#define ATA_REG_COMMAND    0x07

/* ATA status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* ATA commands */
#define ATA_CMD_READ_SECTORS_EXT  0x24
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY          0xEC

static disk_device_t primary_disk;

static void ata_wait_bsy(uint16_t io)
{
    while (inb(io + ATA_REG_STATUS) & ATA_SR_BSY);
}

static void ata_wait_drq(uint16_t io)
{
    while (!(inb(io + ATA_REG_STATUS) & ATA_SR_DRQ));
}

static int ata_identify(disk_device_t *dev)
{
    uint16_t io = dev->io_base;

    outb(io + ATA_REG_DRIVE, 0xA0);
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0) return 0;  /* No device */

    ata_wait_bsy(io);

    /* Check for ATAPI / non-ATA */
    if (inb(io + ATA_REG_LBA_MID) != 0 || inb(io + ATA_REG_LBA_HI) != 0)
        return 0;

    ata_wait_drq(io);

    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(io + ATA_REG_DATA);
    }

    /* LBA48 total sectors at words 100-103 */
    dev->total_sectors = *(uint64_t *)&ident[100];
    if (dev->total_sectors == 0) {
        dev->total_sectors = *(uint32_t *)&ident[60];
    }

    dev->present = 1;
    return 1;
}

static int ata_read_sectors(disk_device_t *dev, uint64_t lba,
                            uint32_t count, void *buf)
{
    uint16_t io = dev->io_base;
    uint16_t *ptr = (uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint64_t sector = lba + s;

        ata_wait_bsy(io);

        outb(io + ATA_REG_DRIVE, 0x40);  /* LBA48, master */

        /* High bytes */
        outb(io + ATA_REG_SECCOUNT, 0);
        outb(io + ATA_REG_LBA_LO,  (sector >> 24) & 0xFF);
        outb(io + ATA_REG_LBA_MID, (sector >> 32) & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (sector >> 40) & 0xFF);

        /* Low bytes */
        outb(io + ATA_REG_SECCOUNT, 1);
        outb(io + ATA_REG_LBA_LO,  sector & 0xFF);
        outb(io + ATA_REG_LBA_MID, (sector >> 8) & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (sector >> 16) & 0xFF);

        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS_EXT);

        ata_wait_bsy(io);
        ata_wait_drq(io);

        for (int i = 0; i < 256; i++) {
            *ptr++ = inw(io + ATA_REG_DATA);
        }
    }
    return (int)count;
}

static int ata_write_sectors(disk_device_t *dev, uint64_t lba,
                             uint32_t count, const void *buf)
{
    uint16_t io = dev->io_base;
    const uint16_t *ptr = (const uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint64_t sector = lba + s;

        ata_wait_bsy(io);

        outb(io + ATA_REG_DRIVE, 0x40);

        outb(io + ATA_REG_SECCOUNT, 0);
        outb(io + ATA_REG_LBA_LO,  (sector >> 24) & 0xFF);
        outb(io + ATA_REG_LBA_MID, (sector >> 32) & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (sector >> 40) & 0xFF);

        outb(io + ATA_REG_SECCOUNT, 1);
        outb(io + ATA_REG_LBA_LO,  sector & 0xFF);
        outb(io + ATA_REG_LBA_MID, (sector >> 8) & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (sector >> 16) & 0xFF);

        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS_EXT);

        ata_wait_bsy(io);
        ata_wait_drq(io);

        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, *ptr++);
        }

        /* Flush cache */
        outb(io + ATA_REG_COMMAND, 0xE7);
        ata_wait_bsy(io);
    }
    return (int)count;
}

void disk_init(void)
{
    primary_disk.type    = DISK_TYPE_ATA;
    primary_disk.io_base = ATA_PRIMARY_IO;
    primary_disk.present = 0;
    ata_identify(&primary_disk);
}

int disk_read(disk_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->present) return -1;
    switch (dev->type) {
    case DISK_TYPE_ATA:
        return ata_read_sectors(dev, lba, count, buf);
    case DISK_TYPE_AHCI:
    case DISK_TYPE_NVME:
        /* Stubs — would use MMIO DMA in a full implementation */
        return -1;
    }
    return -1;
}

int disk_write(disk_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->present) return -1;
    switch (dev->type) {
    case DISK_TYPE_ATA:
        return ata_write_sectors(dev, lba, count, buf);
    case DISK_TYPE_AHCI:
    case DISK_TYPE_NVME:
        return -1;
    }
    return -1;
}

disk_device_t *disk_get_primary(void)
{
    return primary_disk.present ? &primary_disk : (void *)0;
}
