/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IOSERVICE_H
#define NATIVE_MALI_STUB_IOSERVICE_H
#include <IOKit/IOTypes.h>

class OSObject;
class OSDictionary;
class OSData;
class IODeviceMemory;

#define OSDeclareDefaultStructors(name) \
    public:                             \
    name();                             \
    virtual ~name();
#define OSDefineMetaClassAndStructors(name, base) \
    name::name() {}                               \
    name::~name() {}

class OSObject {
public:
    OSObject() = default;
    virtual ~OSObject() = default;
    virtual bool init();
    virtual void free();
    virtual void retain() const;
    virtual void release() const;
};

class OSData : public OSObject {
public:
    const void *getBytesNoCopy() const;
    unsigned int getLength() const;
};

class OSDictionary : public OSObject {};

#define OSDynamicCast(type, object) (dynamic_cast<type *>(object))

class IODeviceMemory;

class IOService : public OSObject {
public:
    virtual bool init(OSDictionary *dictionary = nullptr);
    virtual void free() override;
    virtual IOService *probe(IOService *provider, SInt32 *score);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual bool attach(IOService *provider);
    virtual void detach(IOService *provider);
    virtual bool terminate(IOOptionBits options = 0);
    void registerService();
    OSObject *getProperty(const char *key) const;
    IODeviceMemory *getDeviceMemoryWithIndex(unsigned int index);
};

#endif
