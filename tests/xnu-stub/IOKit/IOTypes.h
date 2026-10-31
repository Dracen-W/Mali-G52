/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub for a syntax check of xnu/. Never committed, never used
 * by tests/run.sh, and not evidence of any XNU behaviour. */
#ifndef NATIVE_MALI_STUB_IOTYPES_H
#define NATIVE_MALI_STUB_IOTYPES_H
#include <stdint.h>
#include <stddef.h>

typedef int IOReturn;
typedef unsigned int UInt32;
typedef int SInt32;
typedef unsigned long long UInt64;
typedef unsigned long long IOOptionBits;
typedef unsigned long long IOByteCount;
typedef unsigned long long addr64_t;
typedef unsigned long long mach_vm_address_t;
typedef unsigned long long mach_vm_size_t;
typedef unsigned long long vm_size_t;
typedef unsigned long long vm_offset_t;
typedef unsigned int IOItemCount;
typedef void *task_t;
typedef int kern_return_t;

enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNoResources = 3,
    kIOReturnNoMemory = 4,
    kIOReturnUnsupported = 5,
    kIOReturnNotReady = 6,
    kIOReturnIOError = 7,
    kIOReturnTimeout = 8,
    kIOReturnNotFound = 9
};

enum {
    kIOMapAnywhere = 0x1,
    kIOMapInhibitCache = 0x2,
    kIODirectionInOut = 0x4,
    kIOMemoryMapperNone = 0x8,
    kIOMemoryPhysicallyContiguous = 0x10
};

#define APPLE_KEXT_OVERRIDE
#endif
