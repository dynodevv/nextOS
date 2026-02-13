/*
 * nextOS - ext2.h
 * EXT2 filesystem driver
 */
#ifndef NEXTOS_EXT2_H
#define NEXTOS_EXT2_H

#include "vfs.h"

int ext2_init(void);
int ext2_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf);
int ext2_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf);
int ext2_readdir(vfs_node_t *dir, int index, vfs_node_t *child);

#endif /* NEXTOS_EXT2_H */
