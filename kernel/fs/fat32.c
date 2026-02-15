/*
 * nextOS - fat32.c
 * FAT32 read/write driver
 */
#include "fat32.h"
#include "../drivers/disk.h"
#include "../mem/heap.h"

/* ── FAT32 BPB (BIOS Parameter Block) ───────────────────────────────── */
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_serial;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* ── FAT32 directory entry ───────────────────────────────────────────── */
typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) fat32_dirent_t;

#define FAT32_ATTR_READONLY  0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

static fat32_bpb_t bpb;
static disk_device_t *disk;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t sectors_per_cluster;
static uint8_t *cluster_buf;

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

static uint32_t fat32_next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / bpb.bytes_per_sector);
    uint32_t ent_offset = fat_offset % bpb.bytes_per_sector;

    uint8_t sector_buf[512];
    disk_read(disk, fat_sector, 1, sector_buf);

    uint32_t val = *(uint32_t *)&sector_buf[ent_offset];
    return val & 0x0FFFFFFF;
}

static int read_cluster(uint32_t cluster, void *buf)
{
    uint32_t lba = cluster_to_lba(cluster);
    return disk_read(disk, lba, sectors_per_cluster, buf);
}

/* Convert 8.3 name to normal form */
static void fat32_format_name(const fat32_dirent_t *de, char *out)
{
    int i = 0, o = 0;
    /* Name part (trim trailing spaces) */
    for (i = 0; i < 8 && de->name[i] != ' '; i++)
        out[o++] = de->name[i];
    /* Extension */
    if (de->ext[0] != ' ') {
        out[o++] = '.';
        for (i = 0; i < 3 && de->ext[i] != ' '; i++)
            out[o++] = de->ext[i];
    }
    out[o] = 0;

    /* Convert to lowercase for display */
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] += 32;
    }
}

int fat32_init(void)
{
    disk = disk_get_primary();
    if (!disk) return -1;

    /* Read the boot sector */
    uint8_t sector[512];
    if (disk_read(disk, 0, 1, sector) < 0) return -1;

    /* Copy BPB */
    uint8_t *src = sector;
    uint8_t *dst = (uint8_t *)&bpb;
    for (int i = 0; i < (int)sizeof(bpb); i++) dst[i] = src[i];

    /* Validate FAT32 signature */
    if (bpb.bytes_per_sector != 512) return -1;
    if (bpb.fat_size_32 == 0) return -1;
    if (bpb.sectors_per_cluster == 0) return -1;
    if (bpb.num_fats == 0) return -1;
    if (bpb.root_entry_count != 0) return -1;  /* Must be 0 for FAT32 */
    /* Check boot signature byte (0x28 or 0x29) */
    if (bpb.boot_sig != 0x28 && bpb.boot_sig != 0x29) return -1;

    sectors_per_cluster = bpb.sectors_per_cluster;
    fat_start_lba = bpb.reserved_sectors;
    data_start_lba = fat_start_lba + bpb.num_fats * bpb.fat_size_32;

    cluster_buf = kmalloc(sectors_per_cluster * 512);
    return cluster_buf ? 0 : -1;
}

int fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf)
{
    if (!node || !buf) return -1;

    uint32_t cluster = (uint32_t)node->fs_data;
    uint32_t cluster_size = sectors_per_cluster * 512;
    uint8_t *out = (uint8_t *)buf;
    uint64_t bytes_read = 0;
    uint64_t pos = 0;

    while (cluster < 0x0FFFFFF8 && bytes_read < size) {
        read_cluster(cluster, cluster_buf);

        if (pos + cluster_size > offset) {
            uint64_t start = (offset > pos) ? offset - pos : 0;
            uint64_t end = cluster_size;
            if (bytes_read + (end - start) > size)
                end = start + (size - bytes_read);

            for (uint64_t i = start; i < end; i++)
                out[bytes_read++] = cluster_buf[i];
        }

        pos += cluster_size;
        cluster = fat32_next_cluster(cluster);
    }

    return (int)bytes_read;
}

int fat32_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf)
{
    if (!node || !buf) return -1;

    uint32_t cluster = (uint32_t)node->fs_data;
    uint32_t cluster_size = sectors_per_cluster * 512;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t bytes_written = 0;
    uint64_t pos = 0;

    while (cluster < 0x0FFFFFF8 && bytes_written < size) {
        read_cluster(cluster, cluster_buf);

        if (pos + cluster_size > offset) {
            uint64_t start = (offset > pos) ? offset - pos : 0;
            uint64_t end = cluster_size;
            if (bytes_written + (end - start) > size)
                end = start + (size - bytes_written);

            for (uint64_t i = start; i < end; i++)
                cluster_buf[i] = in[bytes_written++];

            uint32_t lba = cluster_to_lba(cluster);
            disk_write(disk, lba, sectors_per_cluster, cluster_buf);
        }

        pos += cluster_size;
        cluster = fat32_next_cluster(cluster);
    }

    return (int)bytes_written;
}

int fat32_readdir(vfs_node_t *dir, int index, vfs_node_t *child)
{
    if (!dir || !child) return -1;

    uint32_t cluster = (dir->fs_data != 0) ?
        (uint32_t)dir->fs_data : bpb.root_cluster;
    int entry_idx = 0;

    while (cluster < 0x0FFFFFF8) {
        read_cluster(cluster, cluster_buf);

        uint32_t cluster_size = sectors_per_cluster * 512;
        int entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);

        fat32_dirent_t *entries = (fat32_dirent_t *)cluster_buf;
        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) return -1;  /* No more entries */
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;  /* Deleted */
            if (entries[i].attr == FAT32_ATTR_LFN) continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;

            if (entry_idx == index) {
                fat32_format_name(&entries[i], child->name);
                child->type = (entries[i].attr & FAT32_ATTR_DIRECTORY) ?
                    VFS_DIRECTORY : VFS_FILE;
                child->size = entries[i].file_size;
                child->fs_data = ((uint32_t)entries[i].cluster_hi << 16) |
                                  entries[i].cluster_lo;
                child->inode = 0;
                child->read    = fat32_read;
                child->write   = fat32_write;
                child->readdir = (entries[i].attr & FAT32_ATTR_DIRECTORY) ?
                    fat32_readdir : (void *)0;
                return 0;
            }
            entry_idx++;
        }

        cluster = fat32_next_cluster(cluster);
    }

    return -1;
}
