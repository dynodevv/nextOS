/*
 * nextOS - vfs.h
 * Virtual File System layer
 */
#ifndef NEXTOS_VFS_H
#define NEXTOS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_PATH  256
#define VFS_MAX_NAME  128
#define VFS_MAX_FILES 64

typedef enum {
    VFS_FILE = 0,
    VFS_DIRECTORY
} vfs_node_type_t;

typedef struct vfs_node {
    char              name[VFS_MAX_NAME];
    vfs_node_type_t   type;
    uint64_t          size;
    uint64_t          inode;       /* FS-specific identifier */
    uint64_t          fs_data;     /* FS-specific data (cluster/block) */

    /* Operations (filled in by concrete FS) */
    int (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, void *buf);
    int (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buf);
    int (*readdir)(struct vfs_node *node, int index, struct vfs_node *child);
} vfs_node_t;

void       vfs_init(void);
vfs_node_t *vfs_get_root(void);
int        vfs_open(const char *path, vfs_node_t *out);
int        vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf);
int        vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf);
int        vfs_readdir(vfs_node_t *dir, int index, vfs_node_t *child);

#endif /* NEXTOS_VFS_H */
