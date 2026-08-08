#pragma once

#include <stdint.h>
#include "paging.h"

#define BITMAP_SET(bitmap, bit)   ((bitmap)[(bit) / 8] |= (uint8_t)(1u << ((bit) % 8)))
#define BITMAP_CLEAR(bitmap, bit) ((bitmap)[(bit) / 8] &= (uint8_t)~(1u << ((bit) % 8)))
#define BITMAP_TEST(bitmap, bit)  ((bitmap)[(bit) / 8] & (uint8_t)(1u << ((bit) % 8)))

int pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);

uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);
int pmm_is_ready(void);
