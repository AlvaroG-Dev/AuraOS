// kernel/paging.c

#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include <stddef.h>

#define IA32_PAT_MSR 0x277

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

static void pat_init(void) {
  uint64_t pat = rdmsr(IA32_PAT_MSR);
  pat &= ~(0xFFULL << 24);
  pat |= (0x01ULL << 24); // PAT3 = WC
  wrmsr(IA32_PAT_MSR, pat);
  serial_puts("[PAGING] IA32_PAT: PAT3 = Write-Combining (WC)\n");
}

static uint64_t *alloc_page_table(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    serial_puts("[PAGING] ERROR: PMM sin paginas libres\n");
    return NULL;
  }

  // Durante esta fase el bootloader mantiene un identity-map para las tablas.
  uint64_t *table = (uint64_t *)phys;
  __builtin_memset(table, 0, PAGE_SIZE);
  return table;
}

static uint64_t *kernel_pml4 = NULL;

void paging_init(uint64_t *boot_pml4) {
  kernel_pml4 = boot_pml4;
  pat_init();
  serial_puts("[PAGING] PML4 en ");
  serial_hex((uint64_t)boot_pml4);
  serial_puts(" | tablas nuevas gestionadas por PMM\n");
}

uint64_t *paging_get_pml4(void) { return kernel_pml4; }

static int split_2m_page(uint64_t *pd, uint64_t pd_idx) {
  uint64_t old = pd[pd_idx];
  if (!(old & PTE_PRESENT) || !(old & PTE_HUGE))
    return 0;

  uint64_t base = old & PDE_HUGE_FRAME;
  uint64_t inherited = old & (PTE_WRITABLE | PTE_USER | PTE_WRITETHRU |
                              PTE_NOCACHE | PTE_ACCESSED | PTE_GLOBAL);

  uint64_t *pt = alloc_page_table();
  if (!pt) return -1;

  for (uint64_t i = 0; i < PAGE_ENTRIES; i++) {
    pt[i] = (base + i * PAGE_SIZE) | inherited | PTE_PRESENT;
  }

  pd[pd_idx] = ((uint64_t)pt & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE;
  paging_invalidate_tlb(base);
  return 0;
}

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
  if (!kernel_pml4 || (virt & (PAGE_SIZE - 1)) != 0 ||
      (phys & (PAGE_SIZE - 1)) != 0)
    return -1;

  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT)) {
    uint64_t *new_pdpt = alloc_page_table();
    if (!new_pdpt) return -1;
    pml4[pml4_idx] = ((uint64_t)new_pdpt & PTE_FRAME) |
                      PTE_PRESENT | PTE_WRITABLE;
  }
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);

  if (pdpt[pdpt_idx] & PTE_HUGE)
    return -1; // A 1 GiB page must be explicitly managed, never silently overwritten.

  if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
    uint64_t *new_pd = alloc_page_table();
    if (!new_pd) return -1;
    pdpt[pdpt_idx] = ((uint64_t)new_pd & PTE_FRAME) |
                     PTE_PRESENT | PTE_WRITABLE;
  }
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);

  if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_HUGE)) {
    if (split_2m_page(pd, pd_idx) != 0)
      return -1;
  }

  if (!(pd[pd_idx] & PTE_PRESENT)) {
    uint64_t *new_pt = alloc_page_table();
    if (!new_pt) return -1;
    pd[pd_idx] = ((uint64_t)new_pt & PTE_FRAME) |
                 PTE_PRESENT | PTE_WRITABLE;
  }

  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);
  pt[pt_idx] = (phys & PTE_FRAME) | (flags & 0xFFFULL) | PTE_PRESENT;
  paging_invalidate_tlb(virt);
  return 0;
}

int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
  if (size == 0) return 0;
  if ((virt & (PAGE_SIZE - 1)) != 0 || (phys & (PAGE_SIZE - 1)) != 0)
    return -1;
  if (size > UINT64_MAX - (PAGE_SIZE - 1)) return -1;

  uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages > UINT64_MAX / PAGE_SIZE) return -1;
  uint64_t bytes = pages * PAGE_SIZE;
  if (virt > UINT64_MAX - (bytes - 1) || phys > UINT64_MAX - (bytes - 1))
    return -1;

  for (uint64_t i = 0; i < pages; i++) {
    if (paging_map_page(virt + i * PAGE_SIZE,
                        phys + i * PAGE_SIZE, flags) != 0)
      return -1;
  }
  return 0;
}

int paging_unmap_page(uint64_t virt) {
  if (!kernel_pml4 || (virt & (PAGE_SIZE - 1)) != 0)
    return -1;

  uint64_t *pml4 = kernel_pml4;
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  if (!(pml4[pml4_idx] & PTE_PRESENT)) return -1;
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);
  if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -1;
  if (pdpt[pdpt_idx] & PTE_HUGE) return -1;

  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);
  if (!(pd[pd_idx] & PTE_PRESENT)) return -1;
  if (pd[pd_idx] & PTE_HUGE) return -1;

  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);
  if (!(pt[pt_idx] & PTE_PRESENT)) return -1;

  pt[pt_idx] = 0;
  paging_invalidate_tlb(virt);
  return 0;
}

uint64_t paging_get_phys(uint64_t virt) {
  if (!kernel_pml4) return 0;

  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);
  if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;

  if (pdpt[pdpt_idx] & PTE_HUGE) {
    return (pdpt[pdpt_idx] & 0x000FFFFFC0000000ULL) |
           (virt & 0x3FFFFFFFULL);
  }

  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);
  if (!(pd[pd_idx] & PTE_PRESENT)) return 0;

  if (pd[pd_idx] & PTE_HUGE) {
    return (pd[pd_idx] & PDE_HUGE_FRAME) | (virt & (HUGE_PAGE_SIZE - 1));
  }

  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);
  if (!(pt[pt_idx] & PTE_PRESENT)) return 0;
  return (pt[pt_idx] & PTE_FRAME) | (virt & (PAGE_SIZE - 1));
}

void paging_invalidate_tlb(uint64_t virt) {
  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

int vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags) {
  if (num_pages == 0 || (vaddr & (PAGE_SIZE - 1)) != 0)
    return -1;
  if (num_pages > UINT64_MAX / PAGE_SIZE) return -1;
  uint64_t bytes = num_pages * PAGE_SIZE;
  if (vaddr > UINT64_MAX - (bytes - 1)) return -1;

  uint64_t allocated = 0;
  for (; allocated < num_pages; allocated++) {
    uint64_t v = vaddr + allocated * PAGE_SIZE;

    // VMM no debe reemplazar silenciosamente una pagina existente.
    if (paging_get_phys(v) != 0) {
      serial_puts("[VMM] ERROR: region virtual ya mapeada\n");
      goto rollback;
    }

    uint64_t phys = pmm_alloc_page();
    if (!phys) goto rollback;

    if (paging_map_page(v, phys, flags) != 0) {
      pmm_free_page(phys);
      goto rollback;
    }
  }
  return 0;

rollback:
  while (allocated > 0) {
    allocated--;
    uint64_t v = vaddr + allocated * PAGE_SIZE;
    uint64_t p = paging_get_phys(v);
    if (p != 0) {
      if (paging_unmap_page(v) == 0)
        pmm_free_page(p & ~(PAGE_SIZE - 1));
    }
  }
  serial_puts("[VMM] ERROR: no se pudo reservar la region virtual\n");
  return -1;
}

void vmm_free_pages(uint64_t vaddr, uint64_t num_pages) {
  if ((vaddr & (PAGE_SIZE - 1)) != 0) return;

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t v = vaddr + i * PAGE_SIZE;
    uint64_t phys = paging_get_phys(v);
    if (phys == 0) continue;
    if (paging_unmap_page(v) == 0)
      pmm_free_page(phys & ~(PAGE_SIZE - 1));
  }
}
