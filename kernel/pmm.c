// kernel/pmm.c
#include "pmm.h"
#include "serial.h"

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

static uint8_t *bitmap = NULL;
static uint64_t bitmap_size = 0;
static uint64_t max_blocks = 0;
static uint64_t used_blocks = 0;
static uint64_t last_alloc_bit = 0;
static int pmm_ready = 0;

#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_DESCRIPTOR_MIN_SIZE 40
#define BITMAP_MAX_PHYS 0x100000000ULL
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    if (alignment == 0) return value;
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return 0;
    return (value + mask) & ~mask;
}

static int range_end(uint64_t start, uint64_t length, uint64_t *end) {
    if (length > UINT64_MAX - start) return 0;
    *end = start + length;
    return 1;
}

static void reserve_page(uint64_t bit) {
    if (bit >= max_blocks) return;
    if (!BITMAP_TEST(bitmap, bit)) {
        BITMAP_SET(bitmap, bit);
        used_blocks++;
    }
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start || max_blocks == 0) return;

    uint64_t first = start / PAGE_SIZE;
    uint64_t last = align_up_u64(end, PAGE_SIZE);
    if (last == 0) return;
    last /= PAGE_SIZE;

    if (first >= max_blocks) return;
    if (last > max_blocks) last = max_blocks;

    for (uint64_t bit = first; bit < last; bit++)
        reserve_page(bit);
}

int pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size) {
    bitmap = NULL;
    bitmap_size = 0;
    max_blocks = 0;
    used_blocks = 0;
    last_alloc_bit = 0;
    pmm_ready = 0;

    if (!memmap || memmap_size == 0 || memmap_desc_size < EFI_DESCRIPTOR_MIN_SIZE) {
        serial_puts("[PMM] ERROR: mapa EFI invalido\n");
        return -1;
    }

    uint8_t *ptr = (uint8_t *)memmap;
    uint64_t max_phys_addr = 0;

    // Encontrar la direccion fisica maxima con comprobacion de overflow.
    for (uint64_t offset = 0; offset + memmap_desc_size <= memmap_size; offset += memmap_desc_size) {
        uint64_t phys = *(uint64_t *)(ptr + offset + 8);
        uint64_t pages = *(uint64_t *)(ptr + offset + 24);
        if (pages > UINT64_MAX / PAGE_SIZE) continue;

        uint64_t length = pages * PAGE_SIZE;
        uint64_t end_addr;
        if (range_end(phys, length, &end_addr) && end_addr > max_phys_addr)
            max_phys_addr = end_addr;
    }

    if (max_phys_addr < PAGE_SIZE) {
        serial_puts("[PMM] ERROR: no se encontro RAM fisica\n");
        return -1;
    }

    max_blocks = (max_phys_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    if (max_blocks > UINT64_MAX - 7) return -1;
    bitmap_size = (max_blocks + 7) / 8;

    // El PMM inicial todavia usa el identity-map proporcionado por UEFI para
    // acceder al bitmap. Por eso exigimos que el bitmap completo este <4 GiB.
    uint64_t bitmap_phys = 0;
    for (uint64_t offset = 0; offset + memmap_desc_size <= memmap_size; offset += memmap_desc_size) {
        uint32_t type = *(uint32_t *)(ptr + offset);
        uint64_t phys = *(uint64_t *)(ptr + offset + 8);
        uint64_t pages = *(uint64_t *)(ptr + offset + 24);
        if (type != EFI_CONVENTIONAL_MEMORY || pages == 0) continue;
        if (pages > UINT64_MAX / PAGE_SIZE) continue;

        uint64_t length = pages * PAGE_SIZE;
        uint64_t end_addr;
        if (!range_end(phys, length, &end_addr)) continue;
        if (phys >= BITMAP_MAX_PHYS || bitmap_size > BITMAP_MAX_PHYS - phys) continue;
        uint64_t candidate_end = phys + bitmap_size;
        if (candidate_end <= end_addr && candidate_end <= BITMAP_MAX_PHYS) {
            bitmap_phys = phys;
            break;
        }
    }

    if (bitmap_phys == 0) {
        serial_puts("[PMM] ERROR: no hay RAM convencional <4 GiB para el bitmap\n");
        return -1;
    }

    bitmap = (uint8_t *)bitmap_phys;
    for (uint64_t i = 0; i < bitmap_size; i++) bitmap[i] = 0xFF;
    used_blocks = max_blocks;

    // Solo las regiones EFI_CONVENTIONAL se liberan inicialmente.
    for (uint64_t offset = 0; offset + memmap_desc_size <= memmap_size; offset += memmap_desc_size) {
        uint32_t type = *(uint32_t *)(ptr + offset);
        uint64_t phys = *(uint64_t *)(ptr + offset + 8);
        uint64_t pages = *(uint64_t *)(ptr + offset + 24);
        if (type != EFI_CONVENTIONAL_MEMORY || pages == 0) continue;

        uint64_t start_bit = phys / PAGE_SIZE;
        if (start_bit >= max_blocks || pages > max_blocks - start_bit)
            pages = max_blocks - start_bit;

        for (uint64_t b = 0; b < pages; b++) {
            if (BITMAP_TEST(bitmap, start_bit + b)) {
                BITMAP_CLEAR(bitmap, start_bit + b);
                used_blocks--;
            }
        }
    }

    // Reservas obligatorias independientemente de como UEFI etiquete la region.
    reserve_page(0);

    // Reservar explicitamente el kernel fisico. El linker define la VMA y AT()
    // hace que la imagen se cargue en VMA-KERNEL_VMA.
    uint64_t kernel_start = (uint64_t)&_kernel_start;
    uint64_t kernel_end = (uint64_t)&_kernel_end;
    if (kernel_start >= KERNEL_VMA && kernel_end > kernel_start) {
        reserve_range(kernel_start - KERNEL_VMA, kernel_end - KERNEL_VMA);
    }

    // Reservar el propio bitmap.
    uint64_t bitmap_end;
    if (!range_end(bitmap_phys, bitmap_size, &bitmap_end)) return -1;
    reserve_range(bitmap_phys, bitmap_end);

    last_alloc_bit = 0;
    pmm_ready = 1;

    serial_puts("[PMM] Bitmap: 0x");
    serial_hex(bitmap_phys);
    serial_puts(" | Max RAM: ");
    serial_putn(max_phys_addr / (1024 * 1024), 10, 0);
    serial_puts(" MB | Libres: ");
    serial_putn((max_blocks - used_blocks) * 4 / 1024, 10, 0);
    serial_puts(" MB\n");
    return 0;
}

uint64_t pmm_alloc_page(void) {
    if (!pmm_ready || !bitmap || max_blocks == 0) return 0;

    uint64_t start = last_alloc_bit;
    if (start >= max_blocks) start = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t begin = (pass == 0) ? start : 0;
        uint64_t end = (pass == 0) ? max_blocks : start;
        for (uint64_t bit = begin; bit < end; bit++) {
            if (!BITMAP_TEST(bitmap, bit)) {
                BITMAP_SET(bitmap, bit);
                used_blocks++;
                last_alloc_bit = (bit + 1 < max_blocks) ? bit + 1 : 0;
                return bit * PAGE_SIZE;
            }
        }
    }

    serial_puts("[PMM] ERROR CRITICO: memoria fisica agotada\n");
    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    if (!pmm_ready || !bitmap || phys_addr == 0 || (phys_addr & (PAGE_SIZE - 1)) != 0)
        return;

    uint64_t bit = phys_addr / PAGE_SIZE;
    if (bit >= max_blocks || !BITMAP_TEST(bitmap, bit))
        return;

    BITMAP_CLEAR(bitmap, bit);
    if (used_blocks > 0) used_blocks--;
    if (bit < last_alloc_bit || last_alloc_bit == 0) last_alloc_bit = bit;
}

uint64_t pmm_total_pages(void) { return max_blocks; }
uint64_t pmm_used_pages(void) { return used_blocks; }
uint64_t pmm_free_pages(void) { return max_blocks - used_blocks; }
int pmm_is_ready(void) { return pmm_ready; }
