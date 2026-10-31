/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IOBUFFERMEMORYDESCRIPTOR_H
#define NATIVE_MALI_STUB_IOBUFFERMEMORYDESCRIPTOR_H
#include <IOKit/IOMemoryDescriptor.h>

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t task,
        IOOptionBits options, mach_vm_size_t capacity,
        mach_vm_address_t physicalMask);
};

#endif
