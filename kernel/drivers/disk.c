/*
 * nextOS - disk.c
 * ATA PIO mode disk driver with AHCI (SATA) support
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

/* Timeout for ATA polling loops */
#define ATA_TIMEOUT_LOOPS 100000

/* ── PCI Configuration Space ─────────────────────────────────────────── */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                      uint32_t val)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* ── AHCI (SATA) Definitions ─────────────────────────────────────────── */
/* AHCI Generic Host Control registers (offsets from ABAR) */
#define AHCI_HBA_CAP    0x00  /* Host Capabilities */
#define AHCI_HBA_GHC    0x04  /* Global Host Control */
#define AHCI_HBA_PI     0x0C  /* Ports Implemented */

/* GHC bits */
#define AHCI_GHC_AE     (1u << 31)  /* AHCI Enable */

/* Port register base: 0x100 + port * 0x80 */
#define AHCI_PORT_BASE(p) (0x100 + (p) * 0x80)
#define AHCI_PxCLB   0x00  /* Command List Base (lower 32) */
#define AHCI_PxCLBU  0x04  /* Command List Base (upper 32) */
#define AHCI_PxFB    0x08  /* FIS Base (lower 32) */
#define AHCI_PxFBU   0x0C  /* FIS Base (upper 32) */
#define AHCI_PxIS    0x10  /* Interrupt Status */
#define AHCI_PxIE    0x14  /* Interrupt Enable */
#define AHCI_PxCMD   0x18  /* Command and Status */
#define AHCI_PxTFD   0x20  /* Task File Data */
#define AHCI_PxSIG   0x24  /* Signature */
#define AHCI_PxSSTS  0x28  /* SATA Status */
#define AHCI_PxCI    0x38  /* Command Issue */

/* PxCMD bits */
#define AHCI_CMD_ST   (1u << 0)   /* Start */
#define AHCI_CMD_FRE  (1u << 4)   /* FIS Receive Enable */
#define AHCI_CMD_FR   (1u << 14)  /* FIS Receive Running */
#define AHCI_CMD_CR   (1u << 15)  /* Command List Running */

/* PxSSTS DET field */
#define AHCI_SSTS_DET_MASK  0x0F
#define AHCI_SSTS_DET_OK    0x03  /* Device present and Phy communication established */

/* PxSERR offset */
#define AHCI_PxSERR  0x30  /* SATA Error */

/* PxIS error bits */
#define AHCI_IS_TFES (1u << 30)  /* Task File Error Status */

/* PxTFD bits */
#define AHCI_TFD_BSY (1 << 7)
#define AHCI_TFD_DRQ (1 << 3)

/* SATA device signature */
#define SATA_SIG_ATA  0x00000101  /* ATA device */

/* FIS types */
#define FIS_TYPE_REG_H2D  0x27

/* ATA commands used via AHCI */
#define ATA_CMD_IDENTIFY_AHCI  0xEC
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

/* ── AHCI Command structures ─────────────────────────────────────────── */
/* Command Header (32 bytes, in Command List) */
typedef struct {
    uint16_t flags;        /* CFL (bits 0-4), W (bit 6), P (bit 7), etc. */
    uint16_t prdtl;        /* PRDT Length (entries) */
    uint32_t prdbc;        /* PRD Byte Count (transferred) */
    uint32_t ctba;         /* Command Table Base Address (lower) */
    uint32_t ctbau;        /* Command Table Base Address (upper) */
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

/* PRDT Entry (16 bytes) */
typedef struct {
    uint32_t dba;          /* Data Base Address (lower) */
    uint32_t dbau;         /* Data Base Address (upper) */
    uint32_t reserved;
    uint32_t dbc;          /* Data Byte Count (bit 31 = interrupt) */
} __attribute__((packed)) ahci_prdt_entry_t;

/* Command Table (at least 128 bytes header + PRDTs) */
typedef struct {
    uint8_t  cfis[64];     /* Command FIS */
    uint8_t  acmd[16];     /* ATAPI Command */
    uint8_t  reserved[48]; /* Reserved */
    ahci_prdt_entry_t prdt[1]; /* At least 1 PRDT entry */
} __attribute__((packed)) ahci_cmd_table_t;

/* Received FIS area (256 bytes minimum) */
typedef struct {
    uint8_t data[256];
} __attribute__((aligned(256))) ahci_received_fis_t;

/* ── AHCI static memory areas ────────────────────────────────────────── */
/* We use one command slot (slot 0) with one PRDT entry.
 * All structures must be aligned and below 4 GiB for our identity-mapped setup. */
static ahci_cmd_header_t   ahci_cmd_list[32] __attribute__((aligned(1024)));
static ahci_received_fis_t ahci_fis          __attribute__((aligned(256)));
static ahci_cmd_table_t    ahci_cmd_table    __attribute__((aligned(128)));
static uint8_t             ahci_data_buf[512] __attribute__((aligned(512)));

/* ── Helper: MMIO read/write ─────────────────────────────────────────── */
static inline volatile uint32_t *ahci_reg(uint64_t base, uint32_t off)
{
    return (volatile uint32_t *)(base + off);
}

static inline uint32_t ahci_read(uint64_t base, uint32_t off)
{
    return *ahci_reg(base, off);
}

static inline void ahci_write(uint64_t base, uint32_t off, uint32_t val)
{
    *ahci_reg(base, off) = val;
}

/* ── Primary disk storage ────────────────────────────────────────────── */
static disk_device_t primary_disk;

/* ═══════════════════════════════════════════════════════════════════════
 *  ATA PIO Mode
 * ═══════════════════════════════════════════════════════════════════════ */
static void ata_wait_bsy(uint16_t io)
{
    for (int t = 0; t < ATA_TIMEOUT_LOOPS; t++) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) return;
    }
}

static void ata_wait_drq(uint16_t io)
{
    for (int t = 0; t < ATA_TIMEOUT_LOOPS; t++) {
        if (inb(io + ATA_REG_STATUS) & ATA_SR_DRQ) return;
    }
}

static int ata_identify(disk_device_t *dev)
{
    uint16_t io = dev->io_base;

    /* Floating bus detection: if status is 0xFF, there's no controller */
    uint8_t probe = inb(io + ATA_REG_STATUS);
    if (probe == 0xFF) return 0;

    outb(io + ATA_REG_DRIVE, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait();
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0) return 0;  /* No device */

    ata_wait_bsy(io);

    /* Check for ATAPI / non-ATA (ATAPI sets LBA_MID=0x14, LBA_HI=0xEB) */
    if (inb(io + ATA_REG_LBA_MID) != 0 || inb(io + ATA_REG_LBA_HI) != 0)
        return 0;

    /* Check for error (IDENTIFY aborted) */
    status = inb(io + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) return 0;

    /* Wait for DRQ with timeout */
    int drq_ok = 0;
    for (int t = 0; t < ATA_TIMEOUT_LOOPS; t++) {
        status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) { drq_ok = 1; break; }
    }
    if (!drq_ok) return 0;

    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(io + ATA_REG_DATA);
    }

    /* LBA48 total sectors at words 100-103 */
    uint64_t lba48 = 0;
    for (int i = 0; i < 4; i++)
        lba48 |= (uint64_t)ident[100 + i] << (16 * i);
    dev->total_sectors = lba48;
    if (dev->total_sectors == 0) {
        dev->total_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
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

/* ═══════════════════════════════════════════════════════════════════════
 *  AHCI (SATA) Support
 * ═══════════════════════════════════════════════════════════════════════ */

/* Find the AHCI controller on the PCI bus.
 * AHCI class = 0x01 (Mass Storage), subclass = 0x06 (SATA),
 * prog IF = 0x01 (AHCI 1.0).
 * Returns the ABAR (AHCI Base Address Register, BAR5) or 0. */
static uint64_t ahci_find_controller(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;  /* No device */

                uint32_t class_reg = pci_read(bus, slot, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;

                /* Mass Storage (0x01) + SATA (0x06) */
                if (base_class == 0x01 && sub_class == 0x06) {
                    /* Enable bus mastering and memory space */
                    uint32_t cmd = pci_read(bus, slot, func, 0x04);
                    cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
                    pci_write(bus, slot, func, 0x04, cmd);

                    /* BAR5 is at offset 0x24 */
                    uint32_t bar5 = pci_read(bus, slot, func, 0x24);
                    /* Mask lower bits (memory BAR) */
                    return (uint64_t)(bar5 & 0xFFFFF000);
                }

                /* Only scan func>0 if function 0 is a multi-function device.
                 * Per PCI spec, the multi-function bit is in function 0's header. */
                if (func == 0) {
                    uint32_t hdr = pci_read(bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;  /* Not multi-function */
                }
            }
        }
    }
    return 0;
}

/* Stop the command engine on an AHCI port */
static void ahci_stop_cmd(uint64_t abar, int port)
{
    uint32_t pb = AHCI_PORT_BASE(port);
    uint32_t cmd = ahci_read(abar, pb + AHCI_PxCMD);

    cmd &= ~AHCI_CMD_ST;
    ahci_write(abar, pb + AHCI_PxCMD, cmd);

    /* Wait for CR to clear */
    for (int i = 0; i < 500000; i++) {
        if (!(ahci_read(abar, pb + AHCI_PxCMD) & AHCI_CMD_CR))
            break;
    }

    cmd = ahci_read(abar, pb + AHCI_PxCMD);
    cmd &= ~AHCI_CMD_FRE;
    ahci_write(abar, pb + AHCI_PxCMD, cmd);

    for (int i = 0; i < 500000; i++) {
        if (!(ahci_read(abar, pb + AHCI_PxCMD) & AHCI_CMD_FR))
            break;
    }
}

/* Start the command engine on an AHCI port */
static void ahci_start_cmd(uint64_t abar, int port)
{
    uint32_t pb = AHCI_PORT_BASE(port);

    /* Wait until CR clears */
    for (int i = 0; i < 500000; i++) {
        if (!(ahci_read(abar, pb + AHCI_PxCMD) & AHCI_CMD_CR))
            break;
    }

    uint32_t cmd = ahci_read(abar, pb + AHCI_PxCMD);
    cmd |= AHCI_CMD_FRE;
    ahci_write(abar, pb + AHCI_PxCMD, cmd);
    cmd |= AHCI_CMD_ST;
    ahci_write(abar, pb + AHCI_PxCMD, cmd);
}

/* Initialize an AHCI port: set CLB, FB, clear pending interrupts */
static void ahci_port_init(uint64_t abar, int port)
{
    uint32_t pb = AHCI_PORT_BASE(port);

    ahci_stop_cmd(abar, port);

    /* Set Command List Base */
    uint64_t clb = (uint64_t)(uintptr_t)&ahci_cmd_list[0];
    ahci_write(abar, pb + AHCI_PxCLB,  (uint32_t)(clb & 0xFFFFFFFF));
    ahci_write(abar, pb + AHCI_PxCLBU, (uint32_t)(clb >> 32));

    /* Set FIS Base */
    uint64_t fb = (uint64_t)(uintptr_t)&ahci_fis;
    ahci_write(abar, pb + AHCI_PxFB,  (uint32_t)(fb & 0xFFFFFFFF));
    ahci_write(abar, pb + AHCI_PxFBU, (uint32_t)(fb >> 32));

    /* Clear interrupt status */
    ahci_write(abar, pb + AHCI_PxIS, 0xFFFFFFFF);

    /* Clear error bits in SError */
    ahci_write(abar, pb + AHCI_PxSERR, 0xFFFFFFFF);

    ahci_start_cmd(abar, port);
}

/* Issue an AHCI command using slot 0 and wait for completion.
 * fis:  pointer to the 20-byte H2D FIS
 * buf:  data buffer (physical address, must be < 4 GiB)
 * len:  data length in bytes
 * write: 1 = host-to-device (write), 0 = device-to-host (read)
 * Returns 0 on success, -1 on error. */
static int ahci_issue_cmd(uint64_t abar, int port,
                          const uint8_t *fis, int fis_len,
                          void *buf, uint32_t len, int write)
{
    uint32_t pb = AHCI_PORT_BASE(port);

    /* Set up command header (slot 0) */
    ahci_cmd_header_t *hdr = &ahci_cmd_list[0];
    for (int i = 0; i < 8; i++) ((uint32_t *)hdr)[i] = 0;

    uint16_t flags = (uint16_t)(fis_len / 4);  /* CFL = FIS length in DWORDs */
    if (write) flags |= (1 << 6);  /* W bit */
    hdr->flags = flags;
    hdr->prdtl = (len > 0) ? 1 : 0;
    hdr->prdbc = 0;

    uint64_t ctba = (uint64_t)(uintptr_t)&ahci_cmd_table;
    hdr->ctba  = (uint32_t)(ctba & 0xFFFFFFFF);
    hdr->ctbau = (uint32_t)(ctba >> 32);

    /* Clear command table */
    uint8_t *ct = (uint8_t *)&ahci_cmd_table;
    for (int i = 0; i < (int)sizeof(ahci_cmd_table); i++) ct[i] = 0;

    /* Copy FIS */
    for (int i = 0; i < fis_len && i < 64; i++)
        ahci_cmd_table.cfis[i] = fis[i];

    /* Set up PRDT entry */
    if (len > 0) {
        uint64_t dba = (uint64_t)(uintptr_t)buf;
        ahci_cmd_table.prdt[0].dba  = (uint32_t)(dba & 0xFFFFFFFF);
        ahci_cmd_table.prdt[0].dbau = (uint32_t)(dba >> 32);
        ahci_cmd_table.prdt[0].dbc  = (len - 1);  /* 0-based byte count */
    }

    /* Clear interrupt status */
    ahci_write(abar, pb + AHCI_PxIS, 0xFFFFFFFF);

    /* Issue command in slot 0 */
    ahci_write(abar, pb + AHCI_PxCI, 1);

    /* Poll for completion */
    for (int timeout = 0; timeout < 1000000; timeout++) {
        uint32_t ci = ahci_read(abar, pb + AHCI_PxCI);
        if (!(ci & 1)) break;  /* Slot 0 completed */
        uint32_t is = ahci_read(abar, pb + AHCI_PxIS);
        if (is & AHCI_IS_TFES) return -1;  /* Task file error */
    }

    /* Check for errors */
    uint32_t tfd = ahci_read(abar, pb + AHCI_PxTFD);
    if (tfd & (AHCI_TFD_BSY | ATA_SR_ERR)) return -1;

    return 0;
}

/* Build an H2D Register FIS for an ATA command */
static void ahci_build_fis_h2d(uint8_t *fis, uint8_t command,
                                uint64_t lba, uint16_t count)
{
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = FIS_TYPE_REG_H2D;  /* FIS type */
    fis[1] = 0x80;              /* C bit = 1 (command) */
    fis[2] = command;           /* Command */
    fis[3] = 0;                 /* Features */
    fis[4] = (uint8_t)(lba & 0xFF);
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);
    fis[7] = 0x40;              /* Device: LBA mode */
    fis[8] = (uint8_t)((lba >> 24) & 0xFF);
    fis[9] = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[11] = 0;                /* Features (exp) */
    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);
}

/* Identify an AHCI device and fill in total_sectors */
static int ahci_identify(disk_device_t *dev)
{
    uint64_t abar = dev->mmio_base;
    int port = dev->port_index;

    uint8_t fis[20];
    ahci_build_fis_h2d(fis, ATA_CMD_IDENTIFY_AHCI, 0, 0);

    if (ahci_issue_cmd(abar, port, fis, 20, ahci_data_buf, 512, 0) < 0)
        return 0;

    /* Parse identify data */
    uint16_t *ident = (uint16_t *)ahci_data_buf;
    uint64_t lba48 = 0;
    for (int i = 0; i < 4; i++)
        lba48 |= (uint64_t)ident[100 + i] << (16 * i);
    dev->total_sectors = lba48;
    if (dev->total_sectors == 0) {
        dev->total_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    }

    dev->present = 1;
    return 1;
}

/* Read sectors via AHCI DMA */
static int ahci_read_sectors(disk_device_t *dev, uint64_t lba,
                             uint32_t count, void *buf)
{
    uint64_t abar = dev->mmio_base;
    int port = dev->port_index;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint8_t fis[20];
        ahci_build_fis_h2d(fis, ATA_CMD_READ_DMA_EXT, lba + s, 1);

        if (ahci_issue_cmd(abar, port, fis, 20, ahci_data_buf, 512, 0) < 0)
            return -1;

        for (int i = 0; i < 512; i++)
            dst[s * 512 + i] = ahci_data_buf[i];
    }
    return (int)count;
}

/* Write sectors via AHCI DMA */
static int ahci_write_sectors(disk_device_t *dev, uint64_t lba,
                              uint32_t count, const void *buf)
{
    uint64_t abar = dev->mmio_base;
    int port = dev->port_index;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        /* Copy data to aligned DMA buffer */
        for (int i = 0; i < 512; i++)
            ahci_data_buf[i] = src[s * 512 + i];

        uint8_t fis[20];
        ahci_build_fis_h2d(fis, ATA_CMD_WRITE_DMA_EXT, lba + s, 1);

        if (ahci_issue_cmd(abar, port, fis, 20, ahci_data_buf, 512, 1) < 0)
            return -1;
    }
    return (int)count;
}

/* Probe AHCI ports for a SATA disk device */
static int ahci_probe(disk_device_t *dev)
{
    uint64_t abar = ahci_find_controller();
    if (!abar) return 0;

    /* Enable AHCI mode */
    uint32_t ghc = ahci_read(abar, AHCI_HBA_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_write(abar, AHCI_HBA_GHC, ghc);

    /* Scan implemented ports */
    uint32_t pi = ahci_read(abar, AHCI_HBA_PI);

    for (int port = 0; port < 32; port++) {
        if (!(pi & (1u << port))) continue;

        uint32_t pb = AHCI_PORT_BASE(port);
        uint32_t ssts = ahci_read(abar, pb + AHCI_PxSSTS);

        /* Check device detection (DET) */
        if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_OK) continue;

        uint32_t sig = ahci_read(abar, pb + AHCI_PxSIG);
        /* Only handle SATA ATA drives (skip ATAPI etc.) */
        if (sig != SATA_SIG_ATA) continue;

        /* Found a SATA ATA device on this port */
        dev->type = DISK_TYPE_AHCI;
        dev->mmio_base = abar;
        dev->port_index = port;
        dev->io_base = 0;

        ahci_port_init(abar, port);

        if (ahci_identify(dev))
            return 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void disk_init(void)
{
    primary_disk.type    = DISK_TYPE_ATA;
    primary_disk.io_base = ATA_PRIMARY_IO;
    primary_disk.present = 0;
    primary_disk.port_index = 0;

    /* Try legacy ATA PIO first */
    if (ata_identify(&primary_disk))
        return;

    /* Fall back to AHCI (SATA) */
    ahci_probe(&primary_disk);
}

int disk_read(disk_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->present) return -1;
    switch (dev->type) {
    case DISK_TYPE_ATA:
        return ata_read_sectors(dev, lba, count, buf);
    case DISK_TYPE_AHCI:
        return ahci_read_sectors(dev, lba, count, buf);
    case DISK_TYPE_NVME:
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
        return ahci_write_sectors(dev, lba, count, buf);
    case DISK_TYPE_NVME:
        return -1;
    }
    return -1;
}

disk_device_t *disk_get_primary(void)
{
    return primary_disk.present ? &primary_disk : (void *)0;
}
