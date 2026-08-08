#include "selftest.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>

#define PMM_TEST_PAGES 256
#define HEAP_TEST_BLOCKS 128
#define HEAP_GUARD_SIZE 16
#define HEAP_GUARD_A 0xA5
#define HEAP_GUARD_B 0x5A
#define HEAP_ALIGNMENT 16

static void fill_bytes(uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i) p[i] = value;
}

static size_t align_heap_size(size_t size) {
    return (size + (HEAP_ALIGNMENT - 1)) & ~(size_t)(HEAP_ALIGNMENT - 1);
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
    void *blocks[HEAP_TEST_BLOCKS];
    size_t sizes[HEAP_TEST_BLOCKS];
    size_t alloc_sizes[HEAP_TEST_BLOCKS];

    if (kmalloc(0) != NULL || kmalloc(SIZE_MAX) != NULL) {
        serial_puts("[SELFTEST][HEAP] FAIL: invalid allocation accepted\n");
        return 0;
    }

    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        sizes[i] = 1 + ((i * 73) % 4096);

        /* kmalloc() aligns the complete requested payload to 16 bytes.  The
         * guard must therefore be placed at the end of the actual allocated
         * payload, not immediately after the logical test payload.  The bytes
         * between sizes[i] and alloc_sizes[i] are allocator-owned padding. */
        alloc_sizes[i] = align_heap_size(sizes[i] + HEAP_GUARD_SIZE);
        blocks[i] = kmalloc(sizes[i] + HEAP_GUARD_SIZE);
        if (!blocks[i] || ((uintptr_t)blocks[i] & (HEAP_ALIGNMENT - 1)) != 0) {
            serial_puts("[SELFTEST][HEAP] FAIL: allocation/alignment at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j < i; ++j) kfree(blocks[j]);
            return 0;
        }

        uint64_t first_phys = paging_get_phys((uint64_t)(uintptr_t)blocks[i]);
        uint64_t last_addr = (uint64_t)(uintptr_t)blocks[i] + alloc_sizes[i] - 1;
        uint64_t last_phys = paging_get_phys(last_addr);
        if (!first_phys || !last_phys) {
            serial_puts("[SELFTEST][HEAP] FAIL: allocation not mapped at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j <= i; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }

        uint8_t *p = (uint8_t *)blocks[i];
        p[0] = (uint8_t)i;
        p[sizes[i] - 1] = (uint8_t)(0xFFU - i);
        fill_bytes(p + alloc_sizes[i] - HEAP_GUARD_SIZE, HEAP_GUARD_SIZE, HEAP_GUARD_A);

        for (size_t j = 0; j < i; ++j) {
            uint8_t *q = (uint8_t *)blocks[j];
            size_t guard = alloc_sizes[j] - HEAP_GUARD_SIZE;
            if (q[0] != (uint8_t)j ||
                q[sizes[j] - 1] != (uint8_t)(0xFFU - j) ||
                q[guard] != HEAP_GUARD_A ||
                q[guard + HEAP_GUARD_SIZE - 1] != HEAP_GUARD_A) {
                serial_puts("[SELFTEST][HEAP] FAIL: overwrite at block ");
                serial_putn(j, 10, 0);
                serial_puts(" caused by allocation ");
                serial_putn(i, 10, 0);
                serial_puts("\n");
                for (size_t k = 0; k <= i; ++k) if (blocks[k]) kfree(blocks[k]);
                return 0;
            }
        }
    }

    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        uint8_t *p = (uint8_t *)blocks[i];
        size_t guard = alloc_sizes[i] - HEAP_GUARD_SIZE;
        if (p[0] != (uint8_t)i || p[sizes[i] - 1] != (uint8_t)(0xFFU - i) ||
            p[guard] != HEAP_GUARD_A ||
            p[guard + HEAP_GUARD_SIZE - 1] != HEAP_GUARD_A) {
            serial_puts("[SELFTEST][HEAP] FAIL: data/guard corruption at block ");
            serial_putn(i, 10, 0); serial_puts("\n");
            for (size_t j = 0; j < HEAP_TEST_BLOCKS; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }
    }

    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = pass; i < HEAP_TEST_BLOCKS; i += 2) {
            kfree(blocks[i]);
            blocks[i] = NULL;
        }
    }

    void *large = kmalloc(32768);
    if (!large) {
        serial_puts("[SELFTEST][HEAP] FAIL: reuse/coalescing allocation\n");
        return 0;
    }
    uint8_t *lp = (uint8_t *)large;
    lp[0] = 0x3C;
    lp[32767] = 0xC3;
    if (lp[0] != 0x3C || lp[32767] != 0xC3) {
        serial_puts("[SELFTEST][HEAP] FAIL: large block data\n");
        kfree(large);
        return 0;
    }
    kfree(large);

    for (size_t i = 0; i < HEAP_TEST_BLOCKS; ++i) {
        blocks[i] = kmalloc(64 + (i % 17));
        if (!blocks[i]) {
            serial_puts("[SELFTEST][HEAP] FAIL: second allocation cycle\n");
            for (size_t j = 0; j < i; ++j) if (blocks[j]) kfree(blocks[j]);
            return 0;
        }
    }
    for (size_t i = HEAP_TEST_BLOCKS; i-- > 0;) kfree(blocks[i]);

    (void)HEAP_GUARD_B;
    serial_puts("[SELFTEST][HEAP] 128 fragmented alloc/free cycles: OK\n");
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
