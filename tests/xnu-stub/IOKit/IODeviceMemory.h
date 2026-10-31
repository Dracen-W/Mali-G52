/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IODEVICEMEMORY_H
#define NATIVE_MALI_STUB_IODEVICEMEMORY_H
#include <IOKit/IOMemoryDescriptor.h>

class IODeviceMemory : public OSObject {
public:
    static IODeviceMemory *withRange(mach_vm_address_t base,
        mach_vm_size_t length);
    IOMemoryMap *map(IOOptionBits options);
};

#endif
