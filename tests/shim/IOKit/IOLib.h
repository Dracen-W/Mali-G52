/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <IOKit/IOLib.h>: just the allocation macros the shared
 * handle-index template uses. The kernel build gets the real header. */
#ifndef NATIVE_MALI_TEST_IOKIT_IOLIB_H
#define NATIVE_MALI_TEST_IOKIT_IOLIB_H
#include <stdlib.h>
#define IONewData(type, count) static_cast<type *>(malloc(sizeof(type) * (count)))
#define IODeleteData(pointer, type, count) free(pointer)
#endif
