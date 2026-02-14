/*
 * nextOS - ext2.c
 * EXT2 read/write filesystem driver
 */
#include "ext2.h"
#include "../drivers/disk.h"
#include "../mem/heap.h"

/* ── EXT2 Superblock ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* ... extended fields omitted for brevity ... */
} __attribute__((packed)) ext2_superblock_t;

/* ── Block Group Descriptor ──────────────────────────────────────────── */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

/* ── Inode ───────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* ── Directory entry ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];   /* variable length */
} __attribute__((packed)) ext2_dirent_t;

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INODE  2
#define EXT2_FT_DIR      2

static ext2_superblock_t sb;
static disk_device_t    *disk;
static uint32_t          block_size;
static uint32_t          inode_size;
static uint8_t          *block_buf;
static uint32_t          part_start_lba;  /* Partition offset (sectors) */

static int read_block(uint32_t block, void *buf)
{
    uint32_t sectors = block_size / 512;
    uint64_t lba = part_start_lba + (uint64_t)block * sectors;
    return disk_read(disk, lba, sectors, buf);
}

static int write_block(uint32_t block, const void *buf)
{
    uint32_t sectors = block_size / 512;
    uint64_t lba = part_start_lba + (uint64_t)block * sectors;
    return disk_write(disk, lba, sectors, buf);
}

static int read_inode(uint32_t inode_num, ext2_inode_t *out)
{
    uint32_t group = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;

    /* Read block group descriptor */
    uint32_t bgd_block = (block_size == 1024) ? 2 : 1;
    uint8_t bgd_buf[4096];
    read_block(bgd_block, bgd_buf);

    ext2_bgd_t *bgd = (ext2_bgd_t *)bgd_buf + group;
    uint32_t inode_table_block = bgd->bg_inode_table;

    /* Calculate block and offset within that block */
    uint32_t offset = index * inode_size;
    uint32_t block_num = inode_table_block + offset / block_size;
    uint32_t block_off = offset % block_size;

    uint8_t tmp[4096];
    read_block(block_num, tmp);

    uint8_t *src = tmp + block_off;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t) && i < inode_size; i++)
        dst[i] = src[i];

    return 0;
}

/* MBR partition entry (16 bytes each, 4 entries at offset 446) */
typedef struct {
    uint8_t  status;
    uint8_t  chs_start[3];
    uint8_t  type;
    uint8_t  chs_end[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_part_entry_t;

#define MBR_PART_TYPE_LINUX 0x83

/* Scan MBR partition table for a Linux (0x83) partition */
static uint32_t find_ext2_partition(void)
{
    uint8_t mbr[512];
    if (disk_read(disk, 0, 1, mbr) < 0) return 0;

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    mbr_part_entry_t *parts = (mbr_part_entry_t *)(mbr + 446);
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == MBR_PART_TYPE_LINUX && parts[i].lba_start > 0) {
            return parts[i].lba_start;
        }
    }
    return 0;
}

static int try_ext2_at(uint32_t start_lba)
{
    part_start_lba = start_lba;

    /* EXT2 superblock is at byte offset 1024 within the partition
     * For 1024-byte blocks, that's block 1 = sectors 2-3 from partition start */
    uint8_t sb_buf[1024];
    if (disk_read(disk, start_lba + 2, 2, sb_buf) < 0) return -1;

    uint8_t *src = sb_buf;
    uint8_t *dst = (uint8_t *)&sb;
    for (int i = 0; i < (int)sizeof(sb); i++) dst[i] = src[i];

    if (sb.s_magic != EXT2_MAGIC) return -1;

    block_size = 1024U << sb.s_log_block_size;
    inode_size = 128;  /* Standard inode size for rev0 and rev1 */

    block_buf = kmalloc(block_size);
    return block_buf ? 0 : -1;
}

int ext2_init(void)
{
    disk = disk_get_primary();
    if (!disk) return -1;

    /* First, scan MBR for a Linux partition */
    uint32_t part_lba = find_ext2_partition();
    if (part_lba > 0) {
        if (try_ext2_at(part_lba) == 0) return 0;
    }

    /* Fallback: try raw disk (partition starts at sector 0) */
    return try_ext2_at(0);
}

int ext2_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf)
{
    if (!node || !buf) return -1;

    ext2_inode_t inode;
    read_inode((uint32_t)node->inode, &inode);

    uint8_t *out = (uint8_t *)buf;
    uint64_t bytes_read = 0;
    uint64_t file_size = inode.i_size;

    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    /* Direct blocks (0-11) */
    for (int i = 0; i < 12 && bytes_read < size; i++) {
        if (inode.i_block[i] == 0) break;

        uint64_t block_start = (uint64_t)i * block_size;
        uint64_t block_end   = block_start + block_size;

        if (block_end <= offset) continue;

        read_block(inode.i_block[i], block_buf);

        uint64_t start = (offset > block_start) ? offset - block_start : 0;
        uint64_t end = block_size;
        if (bytes_read + (end - start) > size)
            end = start + (size - bytes_read);

        for (uint64_t j = start; j < end; j++)
            out[bytes_read++] = block_buf[j];
    }

    /* Indirect block (12) */
    if (bytes_read < size && inode.i_block[12] != 0) {
        uint32_t indirect[1024];
        read_block(inode.i_block[12], indirect);
        uint32_t ptrs = block_size / 4;

        for (uint32_t i = 0; i < ptrs && bytes_read < size; i++) {
            if (indirect[i] == 0) break;
            uint64_t block_start = (uint64_t)(12 + i) * block_size;
            if (block_start + block_size <= offset) continue;

            read_block(indirect[i], block_buf);

            uint64_t start = (offset > block_start) ? offset - block_start : 0;
            uint64_t end = block_size;
            if (bytes_read + (end - start) > size)
                end = start + (size - bytes_read);

            for (uint64_t j = start; j < end; j++)
                out[bytes_read++] = block_buf[j];
        }
    }

    return (int)bytes_read;
}

int ext2_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf)
{
    if (!node || !buf) return -1;

    ext2_inode_t inode;
    read_inode((uint32_t)node->inode, &inode);

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t bytes_written = 0;

    /* Write to direct blocks */
    for (int i = 0; i < 12 && bytes_written < size; i++) {
        if (inode.i_block[i] == 0) break;

        uint64_t block_start = (uint64_t)i * block_size;
        uint64_t block_end   = block_start + block_size;

        if (block_end <= offset) continue;

        read_block(inode.i_block[i], block_buf);

        uint64_t start = (offset > block_start) ? offset - block_start : 0;
        uint64_t end = block_size;
        if (bytes_written + (end - start) > size)
            end = start + (size - bytes_written);

        for (uint64_t j = start; j < end; j++)
            block_buf[j] = in[bytes_written++];

        write_block(inode.i_block[i], block_buf);
    }

    return (int)bytes_written;
}

int ext2_readdir(vfs_node_t *dir, int index, vfs_node_t *child)
{
    if (!dir || !child) return -1;

    ext2_inode_t inode;
    read_inode((uint32_t)dir->inode, &inode);

    int entry_idx = 0;

    for (int blk = 0; blk < 12; blk++) {
        if (inode.i_block[blk] == 0) break;
        read_block(inode.i_block[blk], block_buf);

        uint32_t off = 0;
        while (off < block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->inode == 0 || de->rec_len == 0) break;

            if (entry_idx == index) {
                /* Copy name */
                for (int i = 0; i < de->name_len && i < VFS_MAX_NAME - 1; i++)
                    child->name[i] = de->name[i];
                child->name[de->name_len] = 0;

                /* Read child inode for metadata */
                ext2_inode_t child_inode;
                read_inode(de->inode, &child_inode);

                child->type = (de->file_type == EXT2_FT_DIR) ?
                    VFS_DIRECTORY : VFS_FILE;
                child->size  = child_inode.i_size;
                child->inode = de->inode;
                child->fs_data = 0;
                child->read    = ext2_read;
                child->write   = ext2_write;
                child->readdir = (de->file_type == EXT2_FT_DIR) ?
                    ext2_readdir : (void *)0;
                return 0;
            }

            entry_idx++;
            off += de->rec_len;
        }
    }

    return -1;
}
