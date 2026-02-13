/*
 * nextOS - heap.h
 * Kernel heap allocator (first-fit free list)
 */
#ifndef NEXTOS_HEAP_H
#define NEXTOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

void  heap_init(uint64_t start, uint64_t size);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);

#endif /* NEXTOS_HEAP_H */
