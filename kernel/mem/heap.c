/*
 * nextOS - heap.c
 * Simple first-fit heap allocator for the kernel
 */
#include "heap.h"

typedef struct block_header {
    size_t               size;
    int                  free;
    struct block_header *next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

static block_header_t *heap_start = (void *)0;

void heap_init(uint64_t start, uint64_t size)
{
    heap_start = (block_header_t *)start;
    heap_start->size = size - HEADER_SIZE;
    heap_start->free = 1;
    heap_start->next = (void *)0;
}

void *kmalloc(size_t size)
{
    if (!size) return (void *)0;

    /* Align to 16 bytes */
    size = (size + 15) & ~15ULL;

    block_header_t *cur = heap_start;
    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Split if there is enough room for another block */
            if (cur->size >= size + HEADER_SIZE + 16) {
                block_header_t *new_blk =
                    (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
                new_blk->size = cur->size - size - HEADER_SIZE;
                new_blk->free = 1;
                new_blk->next = cur->next;
                cur->next = new_blk;
                cur->size = size;
            }
            cur->free = 0;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }
    return (void *)0; /* out of memory */
}

void *kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    blk->free = 1;

    /* Coalesce adjacent free blocks */
    block_header_t *cur = heap_start;
    while (cur) {
        if (cur->free && cur->next && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
            continue; /* re-check current after merge */
        }
        cur = cur->next;
    }
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return (void *)0; }

    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (blk->size >= new_size) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (new_ptr) {
        uint8_t *src = (uint8_t *)ptr;
        uint8_t *dst = (uint8_t *)new_ptr;
        for (size_t i = 0; i < blk->size; i++) dst[i] = src[i];
        kfree(ptr);
    }
    return new_ptr;
}
