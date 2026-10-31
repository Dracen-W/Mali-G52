/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IOMEMORYDESCRIPTOR_H
#define NATIVE_MALI_STUB_IOMEMORYDESCRIPTOR_H
#include <IOKit/IOService.h>

class IOMemoryDescriptor : public OSObject {
public:
    virtual void retain() const override;
    virtual void release() const override;
    virtual IOReturn prepare();
    virtual IOReturn complete();
    virtual void *getBytesNoCopy() const;
    virtual addr64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length,
        IOOptionBits options = 0);
    virtual IOByteCount getLength() const;
};

class IOMemoryMap : public OSObject {
public:
    virtual vm_offset_t getVirtualAddress() const;
    virtual IOByteCount getLength() const;
};

#endif
