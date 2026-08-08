#pragma once

#include <stdint.h>

void kernel_panic(const char *reason, uint64_t value);
