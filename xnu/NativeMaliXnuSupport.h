#pragma once
// Unified CONFIG_XNU_DW_MALIG52_* macro bridge (keeps legacy -DNATIVE_MALI_M*_WRITES compatible).
#if defined(CONFIG_XNU_DW_MALIG52_M2_WRITES) && CONFIG_XNU_DW_MALIG52_M2_WRITES && !defined(NATIVE_MALI_M2_WRITES)
#define NATIVE_MALI_M2_WRITES 1
#endif
#if defined(CONFIG_XNU_DW_MALIG52_M3_WRITES) && CONFIG_XNU_DW_MALIG52_M3_WRITES && !defined(NATIVE_MALI_M3_WRITES)
#define NATIVE_MALI_M3_WRITES 1
#endif
#if defined(CONFIG_XNU_DW_MALIG52_M4_WRITES) && CONFIG_XNU_DW_MALIG52_M4_WRITES && !defined(NATIVE_MALI_M4_WRITES)
#define NATIVE_MALI_M4_WRITES 1
#endif
#if defined(CONFIG_XNU_DW_MALIG52_M6_WRITES) && CONFIG_XNU_DW_MALIG52_M6_WRITES && !defined(NATIVE_MALI_M6_WRITES)
#define NATIVE_MALI_M6_WRITES 1
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* XNU-only adapters: the four injected interfaces implemented on IOKit.
 *
 * The adapters are not thread-safe. The user client serialises every call with
 * one IOLock; without that, two BO allocations would race for the same entry.
 */
#ifndef NATIVE_MALI_XNU_SUPPORT_H
#define NATIVE_MALI_XNU_SUPPORT_H
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTypes.h>
#include <kern/task.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMaliBoTable.h"
#include "NativeMaliCache.h"
#include "NativeMaliHandleIndex.h"
#include "NativeMaliMemory.h"
#include "NativeMaliPlatform.h"
#include "NativeMaliVm.h"

/* One mapped register window. `base`/`bytes` are bound by the service while the
 * IOMemoryMap that owns the virtual range stays alive there; this object only
 * borrows the address. Offsets are relative to the window, as
 * NativeMaliPlatform.h requires. */
class NativeMaliXnuWindow final : public NativeMaliRegisterTarget {
public:
    NativeMaliXnuWindow() = default;
    void bind(volatile uint8_t *base, size_t bytes)
    {
        base_ = base;
        bytes_ = bytes;
    }
    bool bound() const { return base_ != nullptr && bytes_ != 0; }

    IOReturn read32(uint32_t offset, uint32_t *value) override;
    IOReturn write32(uint32_t offset, uint32_t value) override;
    IOReturn delay(uint64_t microseconds) override;

private:
    volatile uint8_t *base_ = nullptr;
    size_t bytes_ = 0;
};

/* Page-table storage. `limit` is the output-address ceiling (1 << oas) the same
 * buffer source uses; a page outside it is refused, not wrapped. */
class NativeMaliXnuPages final : public NativeMaliPageSource {
public:
    NativeMaliXnuPages();
    void setOutputLimit(uint32_t oas_bits);

    IOReturn alloc(NativeMaliPage *page) override;
    void release(const NativeMaliPage &page) override;
    size_t live() const;

private:
    struct Entry {
        void *cpu = nullptr;
        IOBufferMemoryDescriptor *descriptor = nullptr;
    };
    Entry entry_[NativeMaliVmMaxTables]{};
    uint32_t freeSlots_[NativeMaliVmMaxTables]{};
    size_t freeCount_ = 0;
    size_t live_ = 0;
    uint64_t limit_ = UINT64_C(1) << 40;
};

/* The buffer objects' storage. One entry per live NativeMaliMemory; release()
 * finds it in O(1) via cpuIndex_ for driver-allocated BOs, or by
 * (firstPhysical, lastPhysical, bytes) for zero-copy imported host BOs. */
class NativeMaliXnuBuffers final : public NativeMaliMemorySource {
public:
    NativeMaliXnuBuffers();
    ~NativeMaliXnuBuffers() override;
    void setOutputLimit(uint32_t oas_bits);
    void setOwningTask(task_t task) { owningTask_ = task; }

    bool supportsBoundedSpans() const override { return true; }
    IOReturn alloc(size_t bytes, void **cpu, NativeMaliMemorySpan *spans,
        size_t *count) override;
    IOReturn allocBounded(size_t bytes, void **cpu, NativeMaliMemorySpan *spans,
        size_t maxSpans, size_t *count) override;
    IOReturn importHost(uint64_t userAddress, size_t bytes, void **cpu,
        NativeMaliMemorySpan *spans, size_t *count) override;
    IOReturn importHostBounded(uint64_t userAddress, size_t bytes, void **cpu,
        NativeMaliMemorySpan *spans, size_t maxSpans, size_t *count) override;
    void release(void *cpu, const NativeMaliMemorySpan *spans,
        size_t count) override;

    /* The descriptor behind one live buffer, retained for the caller (the user
     * client's clientMemoryForType()). Null for an address this source did not
     * hand out. */
    IOMemoryDescriptor *descriptor(void *cpu);

private:
    IOReturn walkSpans(IOMemoryDescriptor *descriptor, size_t bytes,
        NativeMaliMemorySpan *spans, size_t maxSpans, size_t *count) const;
    int32_t acquireEntry();
    void releaseEntry(uint32_t idx);
    void trackImported(uint32_t idx);
    void untrackImported(uint32_t idx);

    struct Entry {
        void *cpu = nullptr;
        uint64_t firstPhysical = 0;
        uint64_t lastPhysical = 0;
        IOMemoryDescriptor *descriptor = nullptr;
        size_t bytes = 0;
        uint32_t importedPos = UINT32_MAX;
    };
    Entry entry_[NativeMaliBoTableCapacity]{};
    uint32_t freeSlots_[NativeMaliBoTableCapacity]{};
    size_t freeCount_ = 0;
    /* Dense list of active imported-BO entry indices so release(nullptr, ...)
     * only visits imported BOs instead of scanning all 8192 slots. */
    uint32_t importedList_[NativeMaliBoTableCapacity]{};
    size_t importedCount_ = 0;
    /* O(1) cpu pointer -> entry index lookup for allocated BOs. */
    NativeMaliHandleIndex<NativeMaliBoTableCapacity> cpuIndex_;
    bool cpuIndexReady_ = false;
    task_t owningTask_ = nullptr;
    uint64_t limit_ = UINT64_C(1) << 40;
};

/* dc cvac / dc civac / dc ivac over whole pages, preceded and followed by a
 * dsb sy, with preemption disabled while CTR_EL0 and the operations are read. */
class NativeMaliXnuCache final : public NativeMaliCacheMaintainer {
public:
    IOReturn maintain(void *cpu, size_t bytes,
        NativeMaliCacheOperation operation) override;
    IOReturn maintainPhysical(uint64_t phys, size_t bytes,
        NativeMaliCacheOperation operation) override;
};
#endif
