#ifndef FREERTOS_HOOKS_H
#define FREERTOS_HOOKS_H

#include <stdint.h>

void freertos_assert_failed(const char *file, uint32_t line);

#endif
