/*
 * nextOS - disk.h
 * ATA/AHCI and NVMe disk driver interface
 */
#ifndef NEXTOS_DISK_H
#define NEXTOS_DISK_H

#include <stdint.h>

/* Disk types */
typedef enum {
    DISK_TYPE_ATA = 0,
    DISK_TYPE_AHCI,
    DISK_TYPE_NVME
} disk_type_t;

typedef struct {
    disk_type_t type;
    uint16_t    io_base;     /* For ATA: I/O port base */
    uint64_t    mmio_base;   /* For AHCI/NVMe: MMIO BAR */
    int         port_index;  /* For AHCI: port number (0-31) */
    uint64_t    total_sectors;
    int         present;
} disk_device_t;

void disk_init(void);
int  disk_read(disk_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int  disk_write(disk_device_t *dev, uint64_t lba, uint32_t count, const void *buf);
disk_device_t *disk_get_primary(void);

#endif /* NEXTOS_DISK_H */
