/*
 * nextOS - vfs.c
 * Virtual File System — dispatches to FAT32 / EXT2 / ramfs drivers
 */
#include "vfs.h"
#include "fat32.h"
#include "ext2.h"
#include "ramfs.h"

static vfs_node_t root_node;
static int fs_type = 0;  /* 0 = FAT32, 1 = EXT2 */
static int disk_fs_ready = 0;  /* Whether a disk-based FS was initialized */

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

/* VFS root readdir: enumerates both disk FS entries and ramfs top-level dirs */
static int vfs_root_readdir(vfs_node_t *dir, int index, vfs_node_t *child);

void vfs_init(void)
{
    disk_fs_ready = 0;

    /* Try FAT32 first, then EXT2 */
    if (fat32_init() == 0) {
        fs_type = 0;
        disk_fs_ready = 1;
    } else if (ext2_init() == 0) {
        fs_type = 1;
        disk_fs_ready = 1;
    }

    /* Always initialize ramfs for user directories */
    ramfs_init();

    /* Set up root node */
    vfs_strcpy(root_node.name, "/");
    root_node.type  = VFS_DIRECTORY;
    root_node.size  = 0;
    root_node.inode = (fs_type == 1) ? 2 : 0;
    root_node.read    = (void *)0;
    root_node.write   = (void *)0;
    root_node.readdir = vfs_root_readdir;
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

    /* Check if this is a ramfs path */
    if (ramfs_is_ramfs_path(path)) {
        return ramfs_lookup(path, out);
    }

    /* Walk the path components using disk filesystem */
    if (!disk_fs_ready) return -1;

    vfs_node_t disk_root;
    vfs_strcpy(disk_root.name, "/");
    disk_root.type = VFS_DIRECTORY;
    disk_root.size = 0;
    disk_root.inode = (fs_type == 1) ? 2 : 0;
    disk_root.fs_data = 0;
    disk_root.read = (void *)0;
    disk_root.write = (void *)0;
    disk_root.readdir = (fs_type == 1) ? ext2_readdir : fat32_readdir;

    vfs_node_t current = disk_root;
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

int vfs_create(const char *path, vfs_node_type_t type)
{
    if (ramfs_is_ramfs_path(path)) {
        return ramfs_create(path, type);
    }
    /* Disk FS creation not supported */
    return -1;
}

int vfs_delete(const char *path)
{
    if (ramfs_is_ramfs_path(path)) {
        return ramfs_delete(path);
    }
    /* Disk FS deletion not supported */
    return -1;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    if (ramfs_is_ramfs_path(old_path)) {
        return ramfs_rename(old_path, new_path);
    }
    /* Disk FS rename not supported */
    return -1;
}

/* ── Root readdir: merges disk FS entries with ramfs top-level dirs ─── */
/* Checks if a name matches any ramfs built-in directory name */
static int is_ramfs_builtin(const char *name)
{
    const char *builtins[] = { "Desktop", "Documents", "Images" };
    for (int i = 0; i < 3; i++) {
        if (vfs_strcmp(name, builtins[i]) == 0) return 1;
    }
    return 0;
}

static int vfs_root_readdir(vfs_node_t *dir, int index, vfs_node_t *child)
{
    (void)dir;

    /* First enumerate ramfs top-level directories (Desktop, Documents, Images) */
    vfs_node_t ramfs_root;
    vfs_strcpy(ramfs_root.name, "/");
    ramfs_root.type = VFS_DIRECTORY;
    ramfs_root.size = 0;
    ramfs_root.inode = 0;
    ramfs_root.fs_data = (uint64_t)-1;  /* Sentinel for ramfs root */
    ramfs_root.readdir = ramfs_readdir;

    /* Count ramfs entries first */
    int ramfs_count = 0;
    vfs_node_t tmp;
    while (ramfs_readdir(&ramfs_root, ramfs_count, &tmp) == 0) {
        ramfs_count++;
    }

    if (index < ramfs_count) {
        return ramfs_readdir(&ramfs_root, index, child);
    }

    /* Then enumerate disk FS entries, skipping any that overlap with ramfs names */
    if (!disk_fs_ready) return -1;

    vfs_node_t disk_root;
    vfs_strcpy(disk_root.name, "/");
    disk_root.type = VFS_DIRECTORY;
    disk_root.size = 0;
    disk_root.inode = (fs_type == 1) ? 2 : 0;
    disk_root.fs_data = 0;
    disk_root.read = (void *)0;
    disk_root.write = (void *)0;
    disk_root.readdir = (fs_type == 1) ? ext2_readdir : fat32_readdir;

    int disk_idx = index - ramfs_count;
    int disk_scan = 0;

    for (int i = 0; ; i++) {
        vfs_node_t disk_child;
        if (disk_root.readdir(&disk_root, i, &disk_child) != 0) break;

        /* Skip . and .. entries */
        if (disk_child.name[0] == '.' &&
            (disk_child.name[1] == 0 ||
             (disk_child.name[1] == '.' && disk_child.name[2] == 0))) {
            continue;
        }

        /* Skip entries that overlap with ramfs names */
        if (is_ramfs_builtin(disk_child.name)) continue;

        /* Skip nextos.cfg if it somehow exists on disk (we show our virtual one) */
        if (vfs_strcmp(disk_child.name, "nextos.cfg") == 0) continue;

        if (disk_scan == disk_idx) {
            *child = disk_child;
            return 0;
        }
        disk_scan++;
    }

    /* After all disk entries, add the virtual nextos.cfg file */
    if (disk_scan == disk_idx) {
        vfs_strcpy(child->name, "nextos.cfg");
        child->type = VFS_FILE;
        child->size = 0;
        child->inode = 0;
        child->fs_data = 0;
        child->read = (void *)0;
        child->write = (void *)0;
        child->readdir = (void *)0;
        return 0;
    }

    return -1;
}
