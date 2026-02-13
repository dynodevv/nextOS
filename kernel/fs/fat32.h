/*
 * nextOS - fat32.h
 * FAT32 filesystem driver
 */
#ifndef NEXTOS_FAT32_H
#define NEXTOS_FAT32_H

#include "vfs.h"

int fat32_init(void);
int fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf);
int fat32_write(vfs_node_t *node, uint64_t offset, uint64_t size, const void *buf);
int fat32_readdir(vfs_node_t *dir, int index, vfs_node_t *child);

#endif /* NEXTOS_FAT32_H */
