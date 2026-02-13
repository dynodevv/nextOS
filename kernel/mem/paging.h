/*
 * nextOS - paging.h
 * Virtual memory / paging support for x86_64
 */
#ifndef NEXTOS_PAGING_H
#define NEXTOS_PAGING_H

#include <stdint.h>

void paging_init(uint64_t mem_size);
void paging_map(uint64_t virt, uint64_t phys, uint64_t flags);

#endif /* NEXTOS_PAGING_H */
