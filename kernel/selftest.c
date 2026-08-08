#include "selftest.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define PMM_TEST_PAGES 256
#define HEAP_TEST_BLOCKS 128
#define HEAP_ALIGNMENT 16
#define HEAP_GUARD_SIZE 16

static void fill_bytes(uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i) p[i] = value;
}

static size_t align16(size_t n) {
    return (n + (HEAP_ALIGNMENT - 1)) & ~(size_t)(HEAP_ALIGNMENT - 1);
}

static int check_payload(void *ptr, size_t n, uint8_t value) {
    if (!ptr || n == 0) return 0;
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != value) return 0;
    }
    return 1;
}

static int test_pmm(void) {
    uint64_t before = pmm_free_pages();
    uint64_t pages[PMM_TEST_PAGES];

    for (size_t i = 0; i < PMM_TEST_PAGES; ++i) {
        pages[i] = pmm_alloc_page();
        if (!pages[i] || (pages[i] & (PAGE_SIZE - 1)) != 0) {
            serial_puts("[SELFTEST][PMM] FAIL: allocation\n");
            for (size_t j = 0; j < i; ++j) pmm_free_page(pages[j]);
            return 0;
        }
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pages[i];
        p[0] = 0xA55AA55A12345678ULL ^ (uint64_t)i;
        p[7] = 0x5AA55AA5DEADBEEFULL ^ (uint64_t)i;
    }

    if (pmm_free_pages() != before - PMM_TEST_PAGES) {
        serial_puts("[SELFTEST][PMM] FAIL: free counter after alloc\n");
        for (size_t i = 0; i < PMM_TEST_PAGES; ++i) pmm_free_page(pages[i]);
        return 0;
    }

    for (size_t i = 0; i < PMM_TEST_PAGES; ++i) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pages[i];
        if (p[0] != (0xA55AA55A12345678ULL ^ (uint64_t)i) ||
            p[7] != (0x5AA55AA5DEADBEEFULL ^ (uint64_t)i)) {
            serial_puts("[SELFTEST][PMM] FAIL: page data corruption\n");
            for (size_t j = 0; j < PMM_TEST_PAGES; ++j) pmm_free_page(pages[j]);
            return 0;
        }
    }

    for (size_t i = PMM_TEST_PAGES; i-- > 0;) pmm_free_page(pages[i]);
    if (pmm_free_pages() != before) {
        serial_puts("[SELFTEST][PMM] FAIL: free counter after free\n");
        return 0;
    }

    uint64_t reserved_before = pmm_free_pages();
    pmm_free_page(0);
    if (pmm_free_pages() != reserved_before) {
        serial_puts("[SELFTEST][PMM] FAIL: reserved page was freed\n");
        return 0;
    }

    serial_puts("[SELFTEST][PMM] 256 alloc/write/verify/free: OK\n");
    return 1;
}

static int test_heap(void) {
    void *blocks[HEAP_TEST_BLOCKS] = {0};
    size_t sizes[HEAP_TEST_BLOCKS];
    size_t capacity[HEAP_TEST_BLOCKS];

    if (kmalloc(0) != NULL || kmalloc(SIZE_MAX) != NULL) {
        serial_puts("[SELFTEST][HEAP] FAIL: invalid allocation accepted\n");
        return 0;
    }

    /* First test: allocations must not overlap.  We deliberately test exact
     * allocator boundaries instead of placing synthetic canaries inside the
     * payload. */
    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        sizes[i] = 1 + ((i * 73) % 4096);
        capacity[i] = align16(sizes[i]);
        blocks[i] = kmalloc(sizes[i]);

        if (!blocks[i] || ((uintptr_t)blocks[i] & (HEAP_ALIGNMENT - 1)) != 0) {
            serial_puts("[SELFTEST][HEAP] FAIL: allocation/alignment at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j < i; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }

        uint64_t first_phys = paging_get_phys((uint64_t)(uintptr_t)blocks[i]);
        uint64_t last_phys = paging_get_phys((uint64_t)(uintptr_t)blocks[i] + capacity[i] - 1);
        if (!first_phys || !last_phys) {
            serial_puts("[SELFTEST][HEAP] FAIL: unmapped allocation at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j <= i; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }

        uint8_t value = (uint8_t)(0x31 + (i & 0x3F));
        fill_bytes((uint8_t *)blocks[i], sizes[i], value);

        /* Verify every previous allocation immediately after this allocation.
         * If this fails, the allocator itself (not a canary calculation) has
         * allowed the live ranges to overlap or has overwritten old payload. */
        for (size_t j = 0; j < i; ++j) {
            uint8_t old_value = (uint8_t)(0x31 + (j & 0x3F));
            if (!check_payload(blocks[j], sizes[j], old_value)) {
                serial_puts("[SELFTEST][HEAP] FAIL: live payload corrupted; block ");
                serial_putn(j, 10, 0);
                serial_puts(" after allocation ");
                serial_putn(i, 10, 0);
                serial_puts("\n");
                for (size_t k = 0; k <= i; ++k) if (blocks[k]) kfree(blocks[k]);
                return 0;
            }
        }
    }

    /* Verify that the logical payloads survived the complete allocation run. */
    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        uint8_t value = (uint8_t)(0x31 + (i & 0x3F));
        if (!check_payload(blocks[i], sizes[i], value)) {
            serial_puts("[SELFTEST][HEAP] FAIL: final payload corruption at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j < HEAP_TEST_BLOCKS; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }
    }

    serial_puts("[SELFTEST][HEAP] allocation isolation: OK\n");

    /* Fragmentation/coalescing test. */
    for (size_t i = 0; i < HEAP_TEST_BLOCKS; i += 2) {
        kfree(blocks[i]);
        blocks[i] = NULL;
    }
    for (size_t i = 1; i < HEAP_TEST_BLOCKS; i += 2) {
        kfree(blocks[i]);
        blocks[i] = NULL;
    }

    void *large = kmalloc(32768);
    if (!large) {
        serial_puts("[SELFTEST][HEAP] FAIL: coalescing/reuse allocation\n");
        return 0;
    }
    fill_bytes((uint8_t *)large, 32768, 0xC3);
    if (!check_payload(large, 32768, 0xC3)) {
        serial_puts("[SELFTEST][HEAP] FAIL: large block data\n");
        kfree(large);
        return 0;
    }
    kfree(large);

    /* Reuse cycle across alignment boundaries. */
    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        size_t n = 1 + (i % 65);
        uint8_t value = (uint8_t)(0x70 + (i & 0x3F));
        blocks[i] = kmalloc(n);
        if (!blocks[i] || ((uintptr_t)blocks[i] & (HEAP_ALIGNMENT - 1)) != 0) {
            serial_puts("[SELFTEST][HEAP] FAIL: reuse allocation at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j <= i; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }
        fill_bytes((uint8_t *)blocks[i], n, value);
    }
    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        size_t n = 1 + (i % 65);
        uint8_t value = (uint8_t)(0x70 + (i & 0x3F));
        if (!check_payload(blocks[i], n, value)) {
            serial_puts("[SELFTEST][HEAP] FAIL: reuse payload corruption at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j < HEAP_TEST_BLOCKS; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }
    }
    for (size_t i = HEAP_TEST_BLOCKS; i-- > 0;) kfree(blocks[i]);

    serial_puts("[SELFTEST][HEAP] 128 isolation/fragmentation/reuse cycles: OK\n");
    return 1;
}

int selftest_memory(void) {
    serial_puts("[SELFTEST] Iniciando tests de memoria...\n");
    int pmm_ok = test_pmm();
    int heap_ok = test_heap();
    if (!pmm_ok || !heap_ok) {
        serial_puts("[SELFTEST] FALLO CRITICO: memoria no supera los tests\n");
        return 0;
    }
    serial_puts("[SELFTEST] Todos los tests de memoria: OK\n");
    return 1;
}
