// kernel/heap.c
// Kernel heap: aligned implicit free list with validation and coalescing.

#include "heap.h"
#include "paging.h"
#include "serial.h"

#define HEAP_GROW_PAGES 4
#define HEAP_ALIGNMENT 16
#define HEAP_MAGIC 0x4155524148454150ULL /* "AURAHEAP" */

typedef struct block_header {
  uint64_t magic;
  size_t size;
  uint32_t is_free;
  uint32_t reserved;
  struct block_header *next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

_Static_assert((HEADER_SIZE % HEAP_ALIGNMENT) == 0, "heap header must be ABI aligned");

static block_header_t *heap_head = NULL;
static uint64_t heap_top = 0;
static uint64_t heap_start = 0;
static uint64_t heap_bytes = 0;

static int block_is_valid(const block_header_t *blk) {
  if (!blk || blk->magic != HEAP_MAGIC)
    return 0;
  if ((uint64_t)blk < heap_start || (uint64_t)blk >= heap_top)
    return 0;
  if (blk->size > heap_top - (uint64_t)blk - HEADER_SIZE)
    return 0;
  return 1;
}

static block_header_t *find_block_for_payload(void *ptr) {
  block_header_t *cur = heap_head;
  while (cur) {
    if (!block_is_valid(cur))
      return NULL;
    if ((uint8_t *)cur + HEADER_SIZE == (uint8_t *)ptr)
      return cur;
    cur = cur->next;
  }
  return NULL;
}

static block_header_t *heap_grow(size_t pages) {
  if (pages == 0 || pages > (UINT64_MAX / PAGE_SIZE))
    return NULL;

  uint64_t bytes = (uint64_t)pages * PAGE_SIZE;
  if (heap_top > UINT64_MAX - bytes)
    return NULL;

  uint64_t vaddr = heap_top;
  if (vmm_alloc_pages(vaddr, pages, PTE_WRITABLE) != 0) {
    serial_puts("[HEAP] ERROR: vmm_alloc_pages fallo\n");
    return NULL;
  }

  heap_top += bytes;
  heap_bytes += bytes;

  block_header_t *blk = (block_header_t *)vaddr;
  blk->magic = HEAP_MAGIC;
  blk->size = bytes - HEADER_SIZE;
  blk->is_free = 1;
  blk->reserved = 0;
  blk->next = NULL;

  if (!heap_head) {
    heap_head = blk;
  } else {
    block_header_t *cur = heap_head;
    while (cur->next) {
      if (!block_is_valid(cur)) {
        serial_puts("[HEAP] CORRUPCION: lista invalida\n");
        return NULL;
      }
      cur = cur->next;
    }
    cur->next = blk;
  }

  return blk;
}

static void coalesce(void) {
  block_header_t *cur = heap_head;
  while (cur && cur->next) {
    block_header_t *next = cur->next;
    if (!block_is_valid(cur) || !block_is_valid(next)) {
      serial_puts("[HEAP] CORRUPCION: bloque invalido durante coalesce\n");
      return;
    }

    uint8_t *cur_end = (uint8_t *)cur + HEADER_SIZE + cur->size;
    if (cur->is_free && next->is_free && cur_end == (uint8_t *)next) {
      cur->size += HEADER_SIZE + next->size;
      cur->next = next->next;
      continue;
    }
    cur = next;
  }
}

void heap_init(void) {
  heap_start = HEAP_VMA;
  heap_top = HEAP_VMA;
  heap_bytes = 0;
  heap_head = NULL;

  if (!heap_grow(HEAP_GROW_PAGES)) {
    serial_puts("[HEAP] ERROR: no se pudo crear el heap inicial\n");
    return;
  }

  serial_puts("[HEAP] Heap inicializado en 0x");
  serial_hex(HEAP_VMA);
  serial_puts("\n");
}

void *kmalloc(size_t size) {
  if (size == 0 || size > SIZE_MAX - (HEAP_ALIGNMENT - 1))
    return NULL;

  size = (size + (HEAP_ALIGNMENT - 1)) & ~(size_t)(HEAP_ALIGNMENT - 1);

  for (block_header_t *cur = heap_head; cur; cur = cur->next) {
    if (!block_is_valid(cur)) {
      serial_puts("[HEAP] CORRUPCION: bloque invalido en kmalloc\n");
      return NULL;
    }
    if (!cur->is_free || cur->size < size)
      continue;

    if (cur->size >= size + HEADER_SIZE + HEAP_ALIGNMENT) {
      block_header_t *split = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
      split->magic = HEAP_MAGIC;
      split->size = cur->size - size - HEADER_SIZE;
      split->is_free = 1;
      split->reserved = 0;
      split->next = cur->next;
      cur->size = size;
      cur->next = split;
    }

    cur->is_free = 0;
    return (uint8_t *)cur + HEADER_SIZE;
  }

  size_t pages_needed = (size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed < HEAP_GROW_PAGES)
    pages_needed = HEAP_GROW_PAGES;

  block_header_t *new_blk = heap_grow(pages_needed);
  if (!new_blk)
    return NULL;

  if (new_blk->size >= size + HEADER_SIZE + HEAP_ALIGNMENT) {
    block_header_t *split = (block_header_t *)((uint8_t *)new_blk + HEADER_SIZE + size);
    split->magic = HEAP_MAGIC;
    split->size = new_blk->size - size - HEADER_SIZE;
    split->is_free = 1;
    split->reserved = 0;
    split->next = new_blk->next;
    new_blk->size = size;
    new_blk->next = split;
  }

  new_blk->is_free = 0;
  return (uint8_t *)new_blk + HEADER_SIZE;
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  if (((uint64_t)ptr & (HEAP_ALIGNMENT - 1)) != 0) {
    serial_puts("[HEAP] ERROR: kfree recibe un puntero no alineado\n");
    return;
  }

  block_header_t *blk = find_block_for_payload(ptr);
  if (!blk) {
    serial_puts("[HEAP] ERROR: kfree recibe un puntero desconocido\n");
    return;
  }
  if (blk->is_free) {
    serial_puts("[HEAP] ERROR: double-free detectado\n");
    return;
  }

  blk->is_free = 1;
  coalesce();
}

void heap_dump(void) {
  serial_puts("[HEAP] Estado del heap:\n");
  block_header_t *cur = heap_head;
  int idx = 0;
  while (cur) {
    if (!block_is_valid(cur)) {
      serial_puts("  [HEAP] CORRUPCION\n");
      return;
    }
    serial_puts("  [");
    serial_putn(idx++, 10, 0);
    serial_puts("] addr=0x");
    serial_hex((uint64_t)cur);
    serial_puts(" size=");
    serial_putn(cur->size, 10, 0);
    serial_puts(cur->is_free ? " FREE\n" : " USED\n");
    cur = cur->next;
  }
}
