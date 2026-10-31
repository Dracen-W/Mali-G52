/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IOUSERCLIENT_H
#define NATIVE_MALI_STUB_IOUSERCLIENT_H
#include <IOKit/IOService.h>

struct IOExternalMethodArguments {
    uint32_t selector;
    const void *structureInput;
    IOByteCount structureInputSize;
    void *structureOutput;
    IOByteCount structureOutputSize;
};

struct IOExternalMethodDispatch {
    IOReturn (*action)(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    uint32_t checkScalarInputCount;
    uint32_t checkStructureInputSize;
    uint32_t checkScalarOutputCount;
    uint32_t checkStructureOutputSize;
};

typedef IOReturn (*IOExternalMethodAction)(OSObject *target, void *reference,
    IOExternalMethodArguments *arguments);

class IOMemoryDescriptor;

class IOUserClient : public IOService {
public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties);
    virtual IOReturn clientClose();
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
        IOMemoryDescriptor **memory);
    virtual IOReturn externalMethod(uint32_t selector,
        IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference);
    virtual void free() override;
};

#endif
