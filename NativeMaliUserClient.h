/* SPDX-License-Identifier: GPL-2.0-only */
/* One client of the GPU: its buffer objects, its one address space, and the
 * jobs it started. This is what the future NativeMaliService IOUserClient will
 * dispatch into; it is pure (no IOKit object, no register of its own), so the
 * host test can run the real page tables and the real job-slot handshake
 * against fake registers. */
#ifndef NATIVE_MALI_USER_CLIENT_H
#define NATIVE_MALI_USER_CLIENT_H
#include "NativeMaliABI.h"
#include "NativeMaliBoTable.h"
#include "NativeMaliGpuCore.h"
#include "NativeMaliJobSlot.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

class NativeMaliResetTarget {
public:
    NativeMaliResetTarget() = default;
    virtual ~NativeMaliResetTarget() = default;
    NativeMaliResetTarget(const NativeMaliResetTarget &) = delete;
    NativeMaliResetTarget &operator=(const NativeMaliResetTarget &) = delete;

    virtual IOReturn resetDevice() = 0;
};

class NativeMaliUserClient final {
    NativeMaliBoTable bo_;
    NativeMaliJobSlot *jobs_ = nullptr;
    NativeMaliGpuCore *gpu_ = nullptr;
    NativeMaliVm *vm_ = nullptr;
    NativeMaliResetTarget *reset_ = nullptr;
    NativeMaliGpuObservation observation_{};
    uint32_t as_ = 0;
    uint64_t maxBoBytes_ = 0;
    uint32_t submitted_[NativeMaliJobSlotCount]{};
    uint32_t deadSeqno_[NativeMaliJobSlotCount]{};
    uint32_t nextSlot_ = 0;
    uint64_t faults_ = 0;
    uint64_t resets_ = 0;
    bool open_ = false;

    bool chooseSlot(uint32_t *slot);
    bool ownOutstanding(uint32_t js) const;
    IOReturn waitForFence(const NativeMaliFence &fence, uint64_t timeout_ns,
        NativeMaliJobCompletion *completion);
    static uint64_t timeoutMicroseconds(uint64_t timeout_ns);
public:
    NativeMaliUserClient() = default;
    ~NativeMaliUserClient() = default;
    NativeMaliUserClient(const NativeMaliUserClient &) = delete;
    NativeMaliUserClient &operator=(const NativeMaliUserClient &) = delete;

    IOReturn open(NativeMaliMemorySource *source,
        NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
        NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as,
        NativeMaliJobSlot *jobs, NativeMaliGpuCore *gpu,
        const NativeMaliGpuObservation &observation, uint64_t maxBoBytes,
        NativeMaliResetTarget *reset = nullptr);
    bool open() const { return open_; }

    IOReturn allocBo(const NativeMaliAbiBoAlloc &request,
        NativeMaliAbiBoAllocReply *reply);
    IOReturn mapBo(const NativeMaliAbiBoMap &request,
        NativeMaliAbiBoMapReply *reply);
    IOReturn freeBo(const NativeMaliAbiBoFree &request);
    IOReturn cpuMap(const NativeMaliAbiBoCpuMap &request,
        NativeMaliAbiBoCpuMapReply *reply);
    IOReturn cpuUnmap(const NativeMaliAbiBoCpuUnmap &request);
    IOReturn importHostBo(const NativeMaliAbiBoImportHost &request,
        NativeMaliAbiBoImportHostReply *reply);
    IOReturn syncBo(const NativeMaliAbiBoSync &request);
    NativeMaliMemory *memory(uint64_t handle)
    {
        return bo_.memory(handle);
    }
    uint32_t slotIndexForHandle(uint64_t handle) const
    {
        return bo_.slotIndexForHandle(handle);
    }
    NativeMaliMemory *memoryForToken(uint32_t token)
    {
        return bo_.memoryForToken(token);
    }

    IOReturn query(const NativeMaliAbiQuery &request,
        NativeMaliAbiQuery *reply);
    IOReturn deviceInfo(NativeMaliAbiDeviceInfo *reply);

    bool pendingOwnSlotFence(uint64_t jobChain, uint64_t *fence);

    IOReturn submit(const NativeMaliAbiSubmit &request,
        NativeMaliAbiSubmitReply *reply);
    IOReturn waitFence(const NativeMaliAbiWaitFence &request);
    IOReturn status(NativeMaliAbiStatus *reply);

    IOReturn dispatch(uint32_t method, const void *input, size_t input_size,
        void *output, size_t output_size);
    static size_t inputSize(uint32_t method);
    static size_t outputSize(uint32_t method);
    static constexpr size_t kUnsupportedSize = static_cast<size_t>(-1);

    void noteReset();
    IOReturn close();
    IOReturn releaseAll(uint64_t timeout_us = DefaultCompletionTimeoutUs);
    IOReturn abandon();

    size_t liveObjects() const { return bo_.live(); }
};

#endif
