/*
 * nextOS - vfs.c
 * Virtual File System — dispatches to FAT32 / EXT2 drivers
 */
#include "vfs.h"
#include "fat32.h"
#include "ext2.h"

static vfs_node_t root_node;
static int fs_type = 0;  /* 0 = FAT32, 1 = EXT2 */

/* Simple string helpers */
static int vfs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static void vfs_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++));
}

void vfs_init(void)
{
    /* Try FAT32 first, then EXT2 */
    if (fat32_init() == 0) {
        fs_type = 0;
        vfs_strcpy(root_node.name, "/");
        root_node.type  = VFS_DIRECTORY;
        root_node.size  = 0;
        root_node.inode = 0;
        root_node.read    = (void *)0;
        root_node.write   = (void *)0;
        root_node.readdir = fat32_readdir;
    } else if (ext2_init() == 0) {
        fs_type = 1;
        vfs_strcpy(root_node.name, "/");
        root_node.type  = VFS_DIRECTORY;
        root_node.size  = 0;
        root_node.inode = 2; /* EXT2 root inode */
        root_node.read    = (void *)0;
        root_node.write   = (void *)0;
        root_node.readdir = ext2_readdir;
    }
}

vfs_node_t *vfs_get_root(void)
{
    return &root_node;
}

int vfs_open(const char *path, vfs_node_t *out)
{
    if (!path || !out) return -1;

    /* Root path */
    if (vfs_strcmp(path, "/") == 0) {
        *out = root_node;
        return 0;
    }

    /* Walk the path components */
    vfs_node_t current = root_node;
    const char *p = path;
    if (*p == '/') p++;

    while (*p) {
        /* Extract next component */
        char component[VFS_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/' && ci < VFS_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = 0;
        if (*p == '/') p++;

        /* Search directory */
        int found = 0;
        for (int i = 0; ; i++) {
            vfs_node_t child;
            if (vfs_readdir(&current, i, &child) != 0) break;
            if (vfs_strcmp(child.name, component) == 0) {
                current = child;
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }

    *out = current;
    return 0;
}

int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf)
{
    if (!node || !node->read) return -1;
    return node->read(node, offset, size, buf);
}

int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf)
{
    if (!node || !node->write) return -1;
    return node->write(node, offset, size, buf);
}

int vfs_readdir(vfs_node_t *dir, int index, vfs_node_t *child)
{
    if (!dir || dir->type != VFS_DIRECTORY || !dir->readdir) return -1;
    return dir->readdir(dir, index, child);
}
