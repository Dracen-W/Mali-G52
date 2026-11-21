/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only kernel stub; see ../IOKit/IOTypes.h. */
#ifndef NATIVE_MALI_STUB_KERN_CLOCK_H
#define NATIVE_MALI_STUB_KERN_CLOCK_H
#include <stdint.h>
void clock_get_uptime(uint64_t *result);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
#endif
