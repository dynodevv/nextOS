/*
 * nextOS - pmm.c
 * Physical Memory Manager — simple bitmap allocator
 */
#include "pmm.h"

static uint64_t *bitmap;
static uint64_t  total_pages;
static uint64_t  bitmap_size;  /* in uint64_t words */

/* Kernel end symbol from linker */
extern char _end[];

void pmm_init(uint64_t mem_size)
{
    total_pages = mem_size / PAGE_SIZE;
    bitmap_size = (total_pages + 63) / 64;

    /* Place bitmap right after the kernel image */
    bitmap = (uint64_t *)((((uint64_t)_end) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    /* Mark everything as used initially */
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFFFFFFFFFFFFFFFF;
    }

    /* Free usable pages above the kernel + bitmap area */
    uint64_t bitmap_end = (uint64_t)bitmap + bitmap_size * sizeof(uint64_t);
    bitmap_end = (bitmap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t first_free = bitmap_end / PAGE_SIZE;

    for (uint64_t i = first_free; i < total_pages; i++) {
        bitmap[i / 64] &= ~(1ULL << (i % 64));
    }
}

void *pmm_alloc_page(void)
{
    for (uint64_t i = 0; i < bitmap_size; i++) {
        if (bitmap[i] != 0xFFFFFFFFFFFFFFFF) {
            for (int bit = 0; bit < 64; bit++) {
                if (!(bitmap[i] & (1ULL << bit))) {
                    bitmap[i] |= (1ULL << bit);
                    return (void *)((i * 64 + bit) * PAGE_SIZE);
                }
            }
        }
    }
    return (void *)0; /* out of memory */
}

void pmm_free_page(void *page)
{
    uint64_t idx = (uint64_t)page / PAGE_SIZE;
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}
