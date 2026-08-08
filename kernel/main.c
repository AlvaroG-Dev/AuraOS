// kernel/main.c
// Punto de entrada del kernel Aurora OS

#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/font_manager.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "ps2.h"
#include "rtc.h"
#include "tarfs.h"
#include "initrd.h"
#include "selftest.h"
#include <stddef.h>
#include <stdint.h>

struct kernel_boot_info {
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;
  uint32_t fb_bpp;
  uint64_t memmap;
  uint64_t memmap_size;
  uint64_t memmap_desc_size;
  uint32_t memmap_desc_ver;
};

static struct kernel_boot_info boot;
uint32_t *fb_ptr = NULL;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *pdest = (uint8_t *)dest;
  const uint8_t *psrc = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++) pdest[i] = psrc[i];
  return dest;
}

static void fb_init(uint64_t base, uint32_t w, uint32_t h, uint32_t pitch) {
  if (base == 0 || w == 0 || h == 0 || pitch == 0) {
    fb_ptr = NULL; fb_width = 0; fb_height = 0; fb_pitch = 0; return;
  }
  fb_ptr = (uint32_t *)base;
  fb_width = w; fb_height = h; fb_pitch = pitch / 4;
  serial_puts("[FB] Inicializado en memoria: "); serial_hex(base); serial_puts(" ");
  serial_putn(w, 10, 0); serial_puts("x"); serial_putn(h, 10, 0);
  serial_puts(" pitch="); serial_putn(pitch, 10, 0); serial_puts("\n");
}

static void fb_putpixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height) return;
  fb_ptr[y * fb_pitch + x] = color;
}

static void fb_fillrect(int x, int y, int w, int h, uint32_t color) {
  for (int row = y; row < y + h && row < (int)fb_height; row++)
    for (int col = x; col < x + w && col < (int)fb_width; col++)
      fb_putpixel(col, row, color);
}

#include "font.h"

static void fb_puts(int x, int y, const char *str, uint32_t fg, uint32_t bg) {
  int cx = x;
  while (*str) {
    unsigned char c = (unsigned char)*str;
    if (c < 128) {
      char *glyph = font8x8_basic[(int)c];
      for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
          fb_fillrect(cx + col * 2, y + row * 2, 2, 2,
                      (glyph[row] & (1 << col)) ? fg : bg);
    } else fb_fillrect(cx, y, 16, 16, bg);
    cx += 16; str++;
  }
}

#define EFI_CONVENTIONAL_MEMORY 7

static void parse_memmap(void) {
  if (!boot.memmap || boot.memmap_size == 0) {
    serial_puts("[MEM] No hay mapa de memoria disponible\n"); return;
  }
  serial_puts("[MEM] Mapa de memoria EFI:\n");
  uint8_t *ptr = (uint8_t *)boot.memmap;
  uint64_t total_usable = 0;
  for (uint64_t i = 0; i + boot.memmap_desc_size <= boot.memmap_size; i += boot.memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i);
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    uint64_t len = pages * 4096;
    if (type == EFI_CONVENTIONAL_MEMORY) {
      total_usable += len;
      serial_puts("  [USABLE] 0x"); serial_hex(phys); serial_puts(" - 0x");
      serial_hex(phys + len); serial_puts(" ("); serial_putn(len / (1024 * 1024), 10, 0);
      serial_puts(" MB)\n");
    }
  }
  serial_puts("[MEM] Total usable: "); serial_putn(total_usable / (1024 * 1024), 10, 0);
  serial_puts(" MB\n");
}

#define PIT_FREQ 1193182
#define PIT_HZ 1000

static void pit_init(void) {
  uint16_t divisor = PIT_FREQ / PIT_HZ;
  __asm__ volatile("outb %0, $0x43" : : "a"((uint8_t)0x36));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)(divisor & 0xFF)));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)((divisor >> 8) & 0xFF)));
  serial_puts("[PIT] Configurado a "); serial_putn(PIT_HZ, 10, 0); serial_puts(" Hz\n");
}

static void task_demo_a(void) {
  uint64_t count = 0;
  while (1) {
    if (count % 5000000 == 0) serial_puts("[TASK-A] vivo\n");
    count++; sched_yield();
  }
}

static void task_demo_b(void) {
  uint64_t count = 0;
  while (1) {
    if (count % 5000000 == 0) serial_puts("[TASK-B] vivo\n");
    count++; sched_yield();
  }
}

static void sse_init(void) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 0x200; cr4 |= 0x400;
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));
  serial_puts("[SSE] OSFXSR + OSXMMEXCPT habilitados\n");
}

volatile uint64_t tick_count = 0;
static void timer_handler(void) {
  tick_count++;
  if (tick_count % PIT_HZ == 0) compositor_notify_clock_tick();
  sched_tick();
}

void kmain(struct kernel_boot_info *kinfo) {
  serial_init();
  serial_puts("\n========================================\n");
  serial_puts("   AURORA OS KERNEL x86_64\n");
  serial_puts("   Fase 1 - Kernel Base (REV 4)\n");
  serial_puts("========================================\n\n");

  serial_puts("[INIT] SSE... "); sse_init(); serial_puts("OK\n");
  __builtin_memcpy(&boot, kinfo, sizeof(boot));

  serial_puts("[BOOT] Framebuffer: 0x"); serial_hex(boot.fb_base); serial_puts(" ");
  serial_putn(boot.fb_width, 10, 0); serial_puts("x"); serial_putn(boot.fb_height, 10, 0);
  serial_puts(" pitch="); serial_putn(boot.fb_pitch, 10, 0); serial_puts("\n");

  serial_puts("[INIT] GDT... "); gdt_init(); serial_puts("OK\n");
  serial_puts("[INIT] IDT... "); idt_init(); serial_puts("OK\n");
  serial_puts("[INIT] Paging... ");
  uint64_t cr3; __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3); serial_puts("OK\n");

  parse_memmap();
  serial_puts("[INIT] Iniciando PMM...\n");
  if (pmm_init(boot.memmap, boot.memmap_size, boot.memmap_desc_size) != 0) {
    serial_puts("[SELFTEST] FALLO CRITICO: PMM no pudo inicializarse\n"); return;
  }

  serial_puts("[INIT] Iniciando Heap (kmalloc)...\n"); heap_init();
  if (!selftest_memory()) {
    serial_puts("[SELFTEST] FALLO CRITICO: abortando arranque\n"); return;
  }

  pit_init();
  serial_puts("[INIT] Inicializando Initramfs (TarFS)...\n");
  size_t initrd_size = (size_t)(initrd_end - initrd_start);
  tarfs_init(initrd_start, initrd_size);
  tarfs_list("");
  tar_node_t *cfg = tarfs_open("system/config.txt");
  if (cfg) {
    serial_puts("[TARFS] Contenido de system/config.txt:\n    '");
    for (size_t i = 0; i < cfg->size; i++) serial_putc(cfg->data[i]);
    serial_puts("'\n");
  }

  serial_puts("[INIT] RTC CMOS... "); rtc_init(); serial_puts("OK\n");
  serial_puts("[INIT] Iniciando Scheduler...\n"); sched_init();
  irq_install_handler(0, timer_handler);
  ps2_init();

  int fb_ok = 0;
  if (boot.fb_base != 0 && boot.fb_width > 0 && boot.fb_height > 0) {
    serial_puts("[FB] Mapeando framebuffer en modo Write-Combining (WC)... ");
    uint64_t fb_aligned = boot.fb_base & ~0xFFFULL;
    uint64_t fb_end = (boot.fb_base + boot.fb_size + 0xFFF) & ~0xFFFULL;
    uint64_t fb_size_aligned = fb_end - fb_aligned;
    if (paging_map_range(fb_aligned, fb_aligned, fb_size_aligned,
                         PTE_WRITABLE | PTE_WRITECOMB) == 0) {
      serial_puts("OK\n"); fb_ok = 1;
      fb_init(boot.fb_base, boot.fb_width, boot.fb_height, boot.fb_pitch);
    } else serial_puts("FALLIDO (pool agotado)\n");
  }
  if (!fb_ok) serial_puts("[FB] No hay framebuffer disponible o fallo al mapear\n");

  if (fb_ptr && fb_ok) {
    fb_fillrect(0, 0, fb_width, fb_height, 0x0F0F23);
    fb_fillrect(0, 0, fb_width, 40, 0x1A1A2E);
    fb_puts(10, 12, "Aurora OS", 0xFFFFFF, 0x1A1A2E);
    int win_x = 100, win_y = 80, win_w = 600, win_h = 400;
    fb_fillrect(win_x + 8, win_y + 8, win_w, win_h, 0x000000);
    fb_fillrect(win_x, win_y, win_w, 30, 0x2D2D44);
    fb_fillrect(win_x, win_y + 30, win_w, win_h - 30, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 8, "Terminal", 0xFFFFFF, 0x2D2D44);
    fb_puts(win_x + 10, win_y + 45, "Aurora OS v0.1.0", 0x00FF88, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 65, "x86_64 Bare Metal", 0xAAAAAA, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 85,
            "GDT:OK | IDT:OK | PMM:OK | VMM:OK | HEAP:OK | SCHED:OK", 0xAAAAAA, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 105, "Timer: Running | Keyboard: Active", 0xAAAAAA, 0x1E1E2E);

    uint64_t test_page = pmm_alloc_page();
    if (test_page) {
      serial_puts("[PMM-TEST] Pagina reservada en 0x"); serial_hex(test_page); serial_puts("\n");
      pmm_free_page(test_page);
    }
    fb_puts(win_x + 10, win_y + 145, "> _", 0x00FF88, 0x1E1E2E);
    serial_puts("[INIT] Entorno grafico inicializado\n");
  }

  serial_puts("[INIT] Aurora OS listo. Multitarea activa.\n");
  if (fb_ok) {
    compositor_init();
    window_t *win_term = compositor_create_window(100, 80, 520, 340,
        "Aurora Terminal - x86_64", WIN_FLAGS_INACTIVE);
    if (win_term) {
      win_set_icon_text(win_term, ">", WIN11_ACCENT);
      win_clear(win_term, 0xFF1E1E1E);
      win_draw_string(win_term, 16, 16, "aurora-os:~$ ", WIN11_ACCENT, FONT_ID_MONO);
      win_update(win_term);
    }
    window_t *win_info = compositor_create_window(640, 80, 360, 220,
        "System Information", WIN_FLAGS_INACTIVE);
    if (win_info) {
      win_set_icon_text(win_info, "i", WIN11_ACCENT);
      win_clear(win_info, 0xFF181818);
      win_draw_string(win_info, 16, 16, "AURORA OS", WIN11_ACCENT, FONT_ID_MAIN_REGULAR);
      win_draw_string(win_info, 16, 42, "Kernel: x86_64", 0xFFFFFFFF, FONT_ID_MAIN_REGULAR);
      win_draw_string(win_info, 16, 64, "Memory: PMM + VMM", 0xFFFFFFFF, FONT_ID_MAIN_REGULAR);
      win_draw_string(win_info, 16, 86, "Scheduler: active", 0xFFFFFFFF, FONT_ID_MAIN_REGULAR);
      win_update(win_info);
    }
    sched_create_task(compositor_thread);
  }

  sched_create_task(task_demo_a);
  sched_create_task(task_demo_b);
  __asm__ volatile("sti");

  uint64_t last_tick = 0;
  while (1) {
    if (tick_count != last_tick) {
      last_tick = tick_count;
      if (tick_count % (PIT_HZ / 2) == 0) {
        static int on = 0;
        on = !on;
        uint32_t color = on ? 0x00FF88 : 0x1E1E2E;
        if (fb_ptr && fb_ok) fb_fillrect(130, 225, 8, 16, color);
      }
    }
    ps2_process();
    __asm__ volatile("hlt");
  }
}
