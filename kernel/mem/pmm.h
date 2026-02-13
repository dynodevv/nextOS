/*
 * nextOS - pmm.h
 * Physical Memory Manager (bitmap-based)
 */
#ifndef NEXTOS_PMM_H
#define NEXTOS_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void  pmm_init(uint64_t mem_size);
void *pmm_alloc_page(void);
void  pmm_free_page(void *page);

#endif /* NEXTOS_PMM_H */
