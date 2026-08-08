// kernel/panic.c
#include "panic.h"
#include "serial.h"

static const char hex[] = "0123456789ABCDEF";

static void panic_hex64(uint64_t value) {
    serial_puts("0x");
    for (int i = 15; i >= 0; i--)
        serial_putc(hex[(value >> (i * 4)) & 0xF]);
}

void kernel_panic(const char *reason, uint64_t value) {
    __asm__ volatile("cli");

    serial_puts("\n========================================\n");
    serial_puts("              AURA KERNEL PANIC\n");
    serial_puts("========================================\n");
    serial_puts("Reason: ");
    serial_puts(reason ? reason : "unknown");
    serial_puts("\nValue: ");
    panic_hex64(value);
    serial_puts("\n");

    __asm__ volatile("hlt");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
