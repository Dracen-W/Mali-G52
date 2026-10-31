/* SPDX-License-Identifier: GPL-2.0-only */
/* Local-only IOKit stub; see IOTypes.h in this directory. */
#ifndef NATIVE_MALI_STUB_IOLIB_H
#define NATIVE_MALI_STUB_IOLIB_H
#include <IOKit/IOTypes.h>
#include <new>

template <class T>
T *nativeMaliStubNewData(size_t count)
{
    return static_cast<T *>(::operator new(sizeof(T) * count));
}
#define IONewData(type, count) nativeMaliStubNewData<type>(count)
#define IODeleteData(pointer, type, count) (::operator delete(pointer))

extern "C" {
struct _lck_mtx_t;
typedef struct _lck_mtx_t IOLock;
IOLock *IOLockAlloc(void);
void IOLockFree(IOLock *);
void IOLockLock(IOLock *);
void IOLockUnlock(IOLock *);
void IOLog(const char *format, ...);
void IODelay(unsigned int microseconds);
void IOSleep(unsigned int milliseconds);
}

#endif
