// kernel/paging.h
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE      0x1000ULL
#define PAGE_ENTRIES   512ULL
#define HUGE_PAGE_SIZE 0x200000ULL
#define KERNEL_VMA     0xFFFFFFFF80000000ULL
#define HEAP_VMA       0xFFFFFFFF82000000ULL

#define PTE_PRESENT    0x001ULL
#define PTE_WRITABLE   0x002ULL
#define PTE_USER       0x004ULL
#define PTE_WRITETHRU  0x008ULL
#define PTE_NOCACHE    0x010ULL
#define PTE_ACCESSED   0x020ULL
#define PTE_DIRTY      0x040ULL
#define PTE_HUGE       0x080ULL
#define PTE_GLOBAL     0x100ULL
#define PTE_FRAME      0x000FFFFFFFFFF000ULL
#define PDE_HUGE_FRAME 0x000FFFFFFFE00000ULL

// PWT + PCD selects PAT entry 3 in the normal 4 KiB PTE encoding.
#define PTE_WRITECOMB  (PTE_WRITETHRU | PTE_NOCACHE)

#define PML4_INDEX(v)  (((v) >> 39) & 0x1FFULL)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FFULL)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FFULL)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FFULL)

void paging_init(uint64_t *boot_pml4);
uint64_t *paging_get_pml4(void);
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
int paging_unmap_page(uint64_t virt);
uint64_t paging_get_phys(uint64_t virt);
void paging_invalidate_tlb(uint64_t virt);

int  vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags);
void vmm_free_pages(uint64_t vaddr, uint64_t num_pages);

#endif
