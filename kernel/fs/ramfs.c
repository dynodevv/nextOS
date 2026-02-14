/*
 * nextOS - ramfs.c
 * In-memory filesystem for user directories (Desktop, Documents, Images)
 *
 * Provides a writable filesystem layer that overlays on top of the real
 * disk-based ext2 filesystem. User directories and their contents live
 * in kernel heap memory. Paths starting with /Desktop/, /Documents/,
 * /Images/ are handled by ramfs; all other paths fall through to ext2.
 */
#include "ramfs.h"
#include "../mem/heap.h"

#define RAMFS_MAX_FILES   128
#define RAMFS_MAX_DATA    8192
#define RAMFS_NAME_MAX    VFS_MAX_NAME

typedef struct {
    char              name[RAMFS_NAME_MAX];
    char              parent[VFS_MAX_PATH];  /* Parent directory path */
    vfs_node_type_t   type;
    uint8_t          *data;
    uint64_t          size;
    uint64_t          capacity;
    int               used;
} ramfs_entry_t;

static ramfs_entry_t entries[RAMFS_MAX_FILES];
static int           entry_count = 0;

/* Built-in directories */
static const char *builtin_dirs[] = {
    "Desktop", "Documents", "Images"
};
#define BUILTIN_DIR_COUNT 3

/* ── String helpers ───────────────────────────────────────────────────── */
static int ramfs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static void ramfs_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++));
}

static int ramfs_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int ramfs_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

/* Check if a path is handled by ramfs */
int ramfs_is_ramfs_path(const char *path)
{
    if (!path) return 0;
    if (path[0] != '/') return 0;
    return ramfs_starts_with(path + 1, "Desktop") ||
           ramfs_starts_with(path + 1, "Documents") ||
           ramfs_starts_with(path + 1, "Images");
}

/* Initialize ramfs with default directories */
void ramfs_init(void)
{
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        entries[i].used = 0;
        entries[i].data = (void *)0;
        entries[i].size = 0;
        entries[i].capacity = 0;
    }
    entry_count = 0;

    /* Create built-in directories */
    for (int i = 0; i < BUILTIN_DIR_COUNT; i++) {
        ramfs_strcpy(entries[entry_count].name, builtin_dirs[i]);
        ramfs_strcpy(entries[entry_count].parent, "/");
        entries[entry_count].type = VFS_DIRECTORY;
        entries[entry_count].data = (void *)0;
        entries[entry_count].size = 0;
        entries[entry_count].capacity = 0;
        entries[entry_count].used = 1;
        entry_count++;
    }
}

/* Find entry by parent path and name */
static int find_entry(const char *parent, const char *name)
{
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (entries[i].used &&
            ramfs_strcmp(entries[i].parent, parent) == 0 &&
            ramfs_strcmp(entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Parse a path into parent + name components */
static void split_path(const char *path, char *parent, char *name)
{
    int len = ramfs_strlen(path);

    /* Remove trailing slash for non-root */
    int end = len;
    if (end > 1 && path[end - 1] == '/') end--;

    /* Find last slash */
    int last_slash = 0;
    for (int i = 0; i < end; i++) {
        if (path[i] == '/') last_slash = i;
    }

    /* Copy parent (up to and including last slash) */
    int pi = 0;
    for (int i = 0; i <= last_slash; i++) {
        parent[pi++] = path[i];
    }
    parent[pi] = 0;
    if (pi == 0) { parent[0] = '/'; parent[1] = 0; }

    /* Copy name (after last slash) */
    int ni = 0;
    for (int i = last_slash + 1; i < end; i++) {
        name[ni++] = path[i];
    }
    name[ni] = 0;
}

int ramfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf)
{
    if (!node || !buf) return -1;

    int idx = (int)node->fs_data;
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !entries[idx].used) return -1;
    if (entries[idx].type != VFS_FILE) return -1;

    uint8_t *out = (uint8_t *)buf;
    uint64_t file_size = entries[idx].size;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint8_t *data = entries[idx].data;
    if (!data) return 0;

    for (uint64_t i = 0; i < size; i++)
        out[i] = data[offset + i];

    return (int)size;
}

int ramfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf)
{
    if (!node || !buf) return -1;

    int idx = (int)node->fs_data;
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !entries[idx].used) return -1;
    if (entries[idx].type != VFS_FILE) return -1;

    uint64_t needed = offset + size;
    if (needed > entries[idx].capacity) {
        /* Grow buffer */
        uint64_t new_cap = needed + 1024;
        if (new_cap > RAMFS_MAX_DATA) new_cap = RAMFS_MAX_DATA;
        if (needed > new_cap) return -1;  /* Too large */

        uint8_t *new_data = (uint8_t *)kmalloc((uint32_t)new_cap);
        if (!new_data) return -1;

        /* Copy old data */
        if (entries[idx].data) {
            for (uint64_t i = 0; i < entries[idx].size; i++)
                new_data[i] = entries[idx].data[i];
            kfree(entries[idx].data);
        }
        entries[idx].data = new_data;
        entries[idx].capacity = new_cap;
    }

    const uint8_t *in = (const uint8_t *)buf;
    for (uint64_t i = 0; i < size; i++)
        entries[idx].data[offset + i] = in[i];

    if (offset + size > entries[idx].size)
        entries[idx].size = offset + size;

    return (int)size;
}

int ramfs_readdir(vfs_node_t *dir, int index, vfs_node_t *child)
{
    if (!dir || !child) return -1;

    /* Build the path for this directory */
    char dir_path[VFS_MAX_PATH];
    /* The dir->fs_data stores the entry index; for root-level we need parent="/" */
    int dir_idx = (int)dir->fs_data;

    /* Build the full path of this directory */
    if (dir_idx >= 0 && dir_idx < RAMFS_MAX_FILES && entries[dir_idx].used) {
        /* Construct path: parent + name + "/" */
        int pi = 0;
        const char *p = entries[dir_idx].parent;
        while (*p && pi < VFS_MAX_PATH - 1) dir_path[pi++] = *p++;
        const char *n = entries[dir_idx].name;
        while (*n && pi < VFS_MAX_PATH - 1) dir_path[pi++] = *n++;
        if (pi > 0 && dir_path[pi - 1] != '/') dir_path[pi++] = '/';
        dir_path[pi] = 0;
    } else {
        /* Root-level ramfs entries have parent "/" */
        dir_path[0] = '/';
        dir_path[1] = 0;
    }

    /* Enumerate children */
    int count = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!entries[i].used) continue;
        if (ramfs_strcmp(entries[i].parent, dir_path) != 0) continue;
        if (count == index) {
            ramfs_strcpy(child->name, entries[i].name);
            child->type = entries[i].type;
            child->size = entries[i].size;
            child->inode = 0;
            child->fs_data = (uint64_t)i;
            child->read = ramfs_read;
            child->write = ramfs_write;
            child->readdir = (entries[i].type == VFS_DIRECTORY) ?
                ramfs_readdir : (void *)0;
            return 0;
        }
        count++;
    }

    return -1;
}

int ramfs_lookup(const char *path, vfs_node_t *out)
{
    if (!path || !out) return -1;

    char parent[VFS_MAX_PATH];
    char name[RAMFS_NAME_MAX];
    split_path(path, parent, name);

    /* Check for one of the built-in directory roots (e.g., "/Desktop/") */
    if (name[0] == 0) {
        /* Path ends with / — might be looking up a directory itself */
        /* Try stripping trailing slash */
        char stripped[VFS_MAX_PATH];
        int len = ramfs_strlen(path);
        int si = 0;
        for (int i = 0; i < len; i++) stripped[si++] = path[i];
        if (si > 1 && stripped[si - 1] == '/') si--;
        stripped[si] = 0;
        split_path(stripped, parent, name);
    }

    if (name[0] == 0) return -1;

    int idx = find_entry(parent, name);
    if (idx < 0) return -1;

    ramfs_strcpy(out->name, entries[idx].name);
    out->type = entries[idx].type;
    out->size = entries[idx].size;
    out->inode = 0;
    out->fs_data = (uint64_t)idx;
    out->read = ramfs_read;
    out->write = ramfs_write;
    out->readdir = (entries[idx].type == VFS_DIRECTORY) ?
        ramfs_readdir : (void *)0;

    return 0;
}

int ramfs_create(const char *path, vfs_node_type_t type)
{
    if (!path) return -1;
    if (entry_count >= RAMFS_MAX_FILES) return -1;

    char parent[VFS_MAX_PATH];
    char name[RAMFS_NAME_MAX];
    split_path(path, parent, name);

    if (name[0] == 0) return -1;

    /* Check if already exists */
    if (find_entry(parent, name) >= 0) return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!entries[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ramfs_strcpy(entries[slot].name, name);
    ramfs_strcpy(entries[slot].parent, parent);
    entries[slot].type = type;
    entries[slot].data = (void *)0;
    entries[slot].size = 0;
    entries[slot].capacity = 0;
    entries[slot].used = 1;
    entry_count++;

    return 0;
}

int ramfs_delete(const char *path)
{
    if (!path) return -1;

    char parent[VFS_MAX_PATH];
    char name[RAMFS_NAME_MAX];
    split_path(path, parent, name);

    int idx = find_entry(parent, name);
    if (idx < 0) return -1;

    /* Don't allow deleting built-in directories */
    if (ramfs_strcmp(parent, "/") == 0) {
        for (int i = 0; i < BUILTIN_DIR_COUNT; i++) {
            if (ramfs_strcmp(name, builtin_dirs[i]) == 0) return -1;
        }
    }

    if (entries[idx].data) kfree(entries[idx].data);
    entries[idx].data = (void *)0;
    entries[idx].size = 0;
    entries[idx].capacity = 0;
    entries[idx].used = 0;
    entry_count--;

    return 0;
}

int ramfs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;

    char old_parent[VFS_MAX_PATH], old_name[RAMFS_NAME_MAX];
    char new_parent[VFS_MAX_PATH], new_name[RAMFS_NAME_MAX];
    split_path(old_path, old_parent, old_name);
    split_path(new_path, new_parent, new_name);

    int idx = find_entry(old_parent, old_name);
    if (idx < 0) return -1;

    ramfs_strcpy(entries[idx].name, new_name);
    ramfs_strcpy(entries[idx].parent, new_parent);

    return 0;
}
