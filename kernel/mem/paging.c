/*
 * nextOS - paging.c
 * 4-level paging for x86_64 (identity-mapped kernel space)
 */
#include "paging.h"
#include "pmm.h"

#define PAGE_PRESENT  0x01
#define PAGE_WRITE    0x02
#define PAGE_HUGE     0x80

/* The boot.S already set up identity mapping for the first 1 GiB.
 * Here we extend it as needed and provide an API for further mappings. */

static uint64_t *kernel_pml4;

static void memzero_page(void *page)
{
    uint64_t *p = (uint64_t *)page;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

void paging_init(uint64_t mem_size)
{
    (void)mem_size;
    /* Read CR3 to get the PML4 the bootloader set up */
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk / allocate page table levels */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        void *page = pmm_alloc_page();
        memzero_page(page);
        kernel_pml4[pml4_idx] = (uint64_t)page | PAGE_PRESENT | PAGE_WRITE;
    }
    uint64_t *pdpt = (uint64_t *)(kernel_pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        void *page = pmm_alloc_page();
        memzero_page(page);
        pdpt[pdpt_idx] = (uint64_t)page | PAGE_PRESENT | PAGE_WRITE;
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        void *page = pmm_alloc_page();
        memzero_page(page);
        pd[pd_idx] = (uint64_t)page | PAGE_PRESENT | PAGE_WRITE;
    }
    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] = phys | flags | PAGE_PRESENT;

    /* Invalidate TLB for this page */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
