/*
 * nextOS - ramfs.h
 * In-memory filesystem for user directories
 */
#ifndef NEXTOS_RAMFS_H
#define NEXTOS_RAMFS_H

#include "vfs.h"

void ramfs_init(void);
int  ramfs_create(const char *path, vfs_node_type_t type);
int  ramfs_delete(const char *path);
int  ramfs_rename(const char *old_path, const char *new_path);
int  ramfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf);
int  ramfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf);
int  ramfs_readdir(vfs_node_t *dir, int index, vfs_node_t *child);
int  ramfs_lookup(const char *path, vfs_node_t *out);
int  ramfs_is_ramfs_path(const char *path);

#endif /* NEXTOS_RAMFS_H */
