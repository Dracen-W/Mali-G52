/* SPDX-License-Identifier: GPL-2.0-only */
/* The client's BO state machine; the rules and their sources are in
 * NativeMaliBoTable.h. */
#include "NativeMaliBoTable.h"
#include <string.h>

NativeMaliBoTable::~NativeMaliBoTable()
{
    /* The slots' NativeMaliMemory destructors give the allocations back; the
     * handle index is the only heap this object owns. */
    index_.release();
}

NativeMaliBoTable::Slot *NativeMaliBoTable::find(uint64_t handle)
{
    if(!open_ || !handle)return nullptr;
    const uint32_t slot = index_.find(handle);
    if(slot >= NativeMaliBoTableCapacity)return nullptr;
    Slot &found = slot_[slot];
    /* The index is the fast path; the handle on the slot is the truth. A
     * disagreement means a bug, and the answer is "unknown", never a stale
     * object. */
    if(!found.live || found.handle != handle)return nullptr;
    return &found;
}

const NativeMaliBoTable::Slot *NativeMaliBoTable::find(uint64_t handle) const
{
    return const_cast<NativeMaliBoTable *>(this)->find(handle);
}

uint32_t NativeMaliBoTable::slotIndexForHandle(uint64_t handle) const
{
    const Slot *slot = find(handle);
    if(!slot)return UINT32_MAX;
    return static_cast<uint32_t>(slot - slot_);
}

NativeMaliBoTable::Slot *NativeMaliBoTable::acquireFreeSlot()
{
    for(size_t w = freeHintWord_; w < kBitmapWords; ++w) {
        const uint64_t mask = freeBitmap_[w];
        if(!mask)continue;
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask));
        freeBitmap_[w] &= ~(UINT64_C(1) << bit);
        freeHintWord_ = w;
        return &slot_[(w << 6) + bit];
    }
    return nullptr;
}

void NativeMaliBoTable::returnFreeSlot(uint32_t slotIndex)
{
    if(slotIndex >= NativeMaliBoTableCapacity)return;
    const size_t w = slotIndex >> 6;
    freeBitmap_[w] |= UINT64_C(1) << (slotIndex & 63u);
    if(w < freeHintWord_)freeHintWord_ = w;
}

size_t NativeMaliBoTable::lowerBoundIova(uint64_t iova) const
{
    size_t lo = 0;
    size_t hi = mappedCount_;
    while(lo < hi) {
        const size_t mid = lo + ((hi - lo) >> 1);
        if(slot_[mappedSlots_[mid]].iova < iova)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void NativeMaliBoTable::insertMappedSlot(uint32_t slotIndex)
{
    const uint64_t iova = slot_[slotIndex].iova;
    const size_t pos = lowerBoundIova(iova);
    if(pos < mappedCount_) {
        memmove(&mappedSlots_[pos + 1], &mappedSlots_[pos],
            (mappedCount_ - pos) * sizeof(mappedSlots_[0]));
    }
    mappedSlots_[pos] = slotIndex;
    ++mappedCount_;
}

void NativeMaliBoTable::removeMappedSlot(uint32_t slotIndex)
{
    if(lastJobSlot_ == slotIndex)lastJobSlot_ = UINT32_MAX;
    const uint64_t iova = slot_[slotIndex].iova;
    const size_t pos = lowerBoundIova(iova);
    if(pos < mappedCount_ && mappedSlots_[pos] == slotIndex) {
        if(pos + 1 < mappedCount_) {
            memmove(&mappedSlots_[pos], &mappedSlots_[pos + 1],
                (mappedCount_ - pos - 1) * sizeof(mappedSlots_[0]));
        }
        --mappedCount_;
        return;
    }
    /* Defensive fallback if iova was already zeroed before removal. */
    for(size_t i = 0; i < mappedCount_; ++i) {
        if(mappedSlots_[i] == slotIndex) {
            if(i + 1 < mappedCount_) {
                memmove(&mappedSlots_[i], &mappedSlots_[i + 1],
                    (mappedCount_ - i - 1) * sizeof(mappedSlots_[0]));
            }
            --mappedCount_;
            return;
        }
    }
}

/* [iova, iova + bytes) against every live mapping via O(log M) binary search.
 * The records are validated first, so neither end can wrap here. */
bool NativeMaliBoTable::rangeTaken(uint64_t iova, uint64_t bytes) const
{
    if(mappedCount_ == 0)return false;
    const uint64_t end = iova + bytes;
    const size_t pos = lowerBoundIova(iova);
    if(pos < mappedCount_) {
        const Slot &next = slot_[mappedSlots_[pos]];
        if(next.iova < end)return true;
    }
    if(pos > 0) {
        const Slot &prev = slot_[mappedSlots_[pos - 1]];
        const uint64_t prevEnd = prev.iova + prev.memory.bytes();
        if(prevEnd > iova)return true;
    }
    return false;
}

const NativeMaliBoTable::Slot *NativeMaliBoTable::findCoveringSlot(
    uint64_t start, uint64_t bytes) const
{
    if(!bytes || mappedCount_ == 0)return nullptr;
    /* Find the first mapping with iova > start; the only candidate that can
     * cover `start` is the entry immediately before it. */
    size_t lo = 0;
    size_t hi = mappedCount_;
    while(lo < hi) {
        const size_t mid = lo + ((hi - lo) >> 1);
        if(slot_[mappedSlots_[mid]].iova <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo == 0)return nullptr;
    const Slot &slot = slot_[mappedSlots_[lo - 1]];
    if(!slot.live || !slot.mapped)return nullptr;
    const uint64_t begin = slot.iova;
    const uint64_t size = slot.memory.bytes();
    if(start < begin || start - begin > size)return nullptr;
    if(bytes > size - (start - begin))return nullptr;
    return &slot;
}

NativeMaliBoTable::Slot *NativeMaliBoTable::findCoveringSlot(
    uint64_t start, uint64_t bytes)
{
    return const_cast<Slot *>(
        static_cast<const NativeMaliBoTable *>(this)->findCoveringSlot(start, bytes));
}

void NativeMaliBoTable::clear(Slot &slot)
{
    const uint32_t slotIndex = static_cast<uint32_t>(&slot - slot_);
    if(slot.mapped)removeMappedSlot(slotIndex);
    if(lastJobSlot_ == slotIndex)lastJobSlot_ = UINT32_MAX;
    index_.erase(slot.handle);
    const bool wasLive = slot.live;
    if(wasLive && live_)--live_;
    slot.handle = 0;
    slot.iova = 0;
    slot.permissions = 0;
    slot.cpuToken = 0;
    slot.live = false;
    slot.mapped = false;
    slot.cpuMapped = false;
    slot.imported = false;
    if(wasLive)returnFreeSlot(slotIndex);
}

IOReturn NativeMaliBoTable::open(NativeMaliMemorySource *source,
    NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
    NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as, uint64_t maxBytes)
{
    if(open_)return kIOReturnBusy;
    if(!source || !vm || !mmu || as >= NativeMaliMmuMaxAs)return kIOReturnBadArgument;
    /* The clean/invalidate policy is not optional on this non-coherent GPU
     * (NativeMaliMemory.h); say so once instead of on every alloc(). */
    if(!cpuCoherent && !maintainer)return kIOReturnBadArgument;
    /* A map ends in FLUSH_PT on this AS, which a slot that is not present and
     * allocated cannot answer. */
    if((mmu->present() & (1u << as)) == 0 || !mmu->isAllocated(as))
        return kIOReturnBadArgument;

    if(!index_.allocate())return kIOReturnNoMemory;

    for(size_t w = 0; w < kBitmapWords; ++w)
        freeBitmap_[w] = ~UINT64_C(0);
    freeHintWord_ = 0;
    mappedCount_ = 0;
    lastJobSlot_ = UINT32_MAX;

    source_ = source;
    maintainer_ = maintainer;
    vm_ = vm;
    mmu_ = mmu;
    as_ = as;
    maxBytes_ = maxBytes ? maxBytes :
        static_cast<uint64_t>(NATIVE_MALI_ABI_MAX_BO_BYTES);
    coherent_ = cpuCoherent;
    open_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::alloc(const NativeMaliAbiBoAlloc &request,
    NativeMaliAbiBoAllocReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->handle = 0;
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::boAlloc(request, maxBytes_);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = acquireFreeSlot();
    if(!slot)return kIOReturnNoResources;

    result = slot->memory.prepare(source_, static_cast<size_t>(request.bytes),
        coherent_, maintainer_);
    if(result != kIOReturnSuccess) {
        returnFreeSlot(static_cast<uint32_t>(slot - slot_));
        return result;
    }

    /* Monotonic, never 0: a stale handle cannot name a later object. */
    uint64_t handle = nextHandle_++;
    if(nextHandle_ == 0)++nextHandle_;
    if(handle == 0)handle = nextHandle_++;

    slot->handle = handle;
    slot->iova = 0;
    slot->permissions = 0;
    slot->cpuToken = 0;
    slot->live = true;
    slot->mapped = false;
    slot->cpuMapped = false;
    slot->imported = false;
    index_.insert(handle, static_cast<uint32_t>(slot - slot_));
    ++live_;
    reply->handle = handle;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::map(const NativeMaliAbiBoMap &request,
    NativeMaliAbiBoMapReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->iova = 0;
    if(!open_)return kIOReturnNotReady;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    IOReturn result = NativeMaliAbiValidate::boMap(request, slot->memory.bytes());
    if(result != kIOReturnSuccess)return result;
    if(slot->mapped)return kIOReturnBusy;
    /* Refuse before the first leaf is written: a partial overlap would leave
     * the new object half-mapped and the old one partly shadowed. */
    if(rangeTaken(request.iova, slot->memory.bytes()))return kIOReturnBadArgument;

    uint32_t prot = NativeMaliVmRead | NativeMaliVmCached;
    if(request.permissions & NativeMaliBoWrite)prot |= NativeMaliVmWrite;

    result = slot->memory.map(vm_, mmu_, as_, request.iova, prot);
    if(result != kIOReturnSuccess)return result;

    slot->mapped = true;
    slot->iova = request.iova;
    slot->permissions = request.permissions;
    insertMappedSlot(static_cast<uint32_t>(slot - slot_));
    reply->iova = request.iova;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::freeBo(const NativeMaliAbiBoFree &request)
{
    if(!open_)return kIOReturnNotReady;
    IOReturn result = NativeMaliAbiValidate::boFree(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    /* The wrapper still has this memory mapped; freeing it now would leave the
     * mapping pointing at pages the source already took back. */
    if(slot->cpuMapped)return kIOReturnBusy;

    /* Leaves first, then the pages (panfrost_mmu.c:500-524). A failed unmap
     * keeps the object live and mapped, so the call can be repeated. */
    if(slot->mapped) {
        result = slot->memory.unmap();
        if(result != kIOReturnSuccess)return result;
        removeMappedSlot(static_cast<uint32_t>(slot - slot_));
        slot->mapped = false;
        slot->iova = 0;
        slot->permissions = 0;
    }

    result = slot->memory.release();
    if(result != kIOReturnSuccess)return result;

    clear(*slot);
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::cpuMap(const NativeMaliAbiBoCpuMap &request,
    NativeMaliAbiBoCpuMapReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    *reply = NativeMaliAbiBoCpuMapReply{};
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::boCpuMap(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    if(slot->cpuMapped)return kIOReturnBusy;

    /* Slot + 1: nonzero, and unique among the live tokens of this table. The
     * handle in cpuUnmap() is what keeps a stale token from naming a newer
     * object that landed in the same slot. */
    slot->cpuToken = static_cast<uint32_t>(slot - slot_) + 1;
    slot->cpuMapped = true;
    reply->token = slot->cpuToken;
    reply->bytes = slot->memory.bytes();
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::cpuUnmap(const NativeMaliAbiBoCpuUnmap &request)
{
    if(!open_)return kIOReturnNotReady;
    IOReturn result = NativeMaliAbiValidate::boCpuUnmap(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    /* A token of 0 is never handed out, so "not mapped" and "wrong token" are
     * both refusals rather than a silent no-op. */
    if(!slot->cpuMapped || slot->cpuToken != request.token)
        return kIOReturnBadArgument;

    slot->cpuMapped = false;
    slot->cpuToken = 0;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::importHost(const NativeMaliAbiBoImportHost &request,
    NativeMaliAbiBoImportHostReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->handle = 0;
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::boImportHost(request, maxBytes_);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = acquireFreeSlot();
    if(!slot)return kIOReturnNoResources;

    result = slot->memory.prepareImported(source_, request.address,
        static_cast<size_t>(request.bytes), coherent_, maintainer_);
    if(result != kIOReturnSuccess) {
        returnFreeSlot(static_cast<uint32_t>(slot - slot_));
        return result;
    }

    uint64_t handle = nextHandle_++;
    if(nextHandle_ == 0)++nextHandle_;
    if(handle == 0)handle = nextHandle_++;

    slot->handle = handle;
    slot->iova = 0;
    slot->permissions = 0;
    slot->cpuToken = 0;
    slot->live = true;
    slot->mapped = false;
    slot->cpuMapped = false;
    slot->imported = true;
    index_.insert(handle, static_cast<uint32_t>(slot - slot_));
    ++live_;
    reply->handle = handle;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::syncBo(const NativeMaliAbiBoSync &request)
{
    if(!open_)return kIOReturnNotReady;
    if(request.handle == 0)return kIOReturnBadArgument;
    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;

    const IOReturn valid = NativeMaliAbiValidate::boSync(request,
        slot->memory.bytes());
    if(valid != kIOReturnSuccess)return valid;

    if(request.op == NativeMaliBoSyncClean)
        return slot->memory.cleanForDevice(static_cast<size_t>(request.offset),
            static_cast<size_t>(request.bytes));
    if(request.op == NativeMaliBoSyncInvalidate)
        return slot->memory.invalidateForCpu(static_cast<size_t>(request.offset),
            static_cast<size_t>(request.bytes));
    return slot->memory.cleanInvalidate(static_cast<size_t>(request.offset),
        static_cast<size_t>(request.bytes));
}

IOReturn NativeMaliBoTable::releaseAll()
{
    if(!open_)return kIOReturnNotReady;
    if(live_ == 0)return kIOReturnSuccess;

    /* The connection that held these tokens is closing, so they cannot be
     * returned to their owner (NativeAdrenoUserClient::releaseAll() clears its
     * leases for the same reason). Drop every token first, then retire. */
    for(Slot &slot : slot_) {
        slot.cpuMapped = false;
        slot.cpuToken = 0;
    }

    for(size_t i = 0; i < NativeMaliBoTableCapacity && live_ > 0; ++i) {
        if(!slot_[i].live)continue;
        NativeMaliAbiBoFree request{};
        request.handle = slot_[i].handle;
        const IOReturn result = freeBo(request);
        if(result != kIOReturnSuccess)return result;
    }
    return kIOReturnSuccess;
}

void NativeMaliBoTable::reset()
{
    for(Slot &slot : slot_) {
        /* No registers: the caller promises the address space is already
         * gone, so a still-mapped object only has its CPU side dropped. */
        slot.memory.reset();
        slot.handle = 0;
        slot.iova = 0;
        slot.permissions = 0;
        slot.cpuToken = 0;
        slot.live = false;
        slot.mapped = false;
        slot.cpuMapped = false;
        slot.imported = false;
    }
    index_.release();
    memset(freeBitmap_, 0, sizeof(freeBitmap_));
    freeHintWord_ = 0;
    mappedCount_ = 0;
    lastJobSlot_ = UINT32_MAX;
    live_ = 0;
    open_ = false;
}

bool NativeMaliBoTable::covers(uint64_t start, uint64_t bytes) const
{
    return findCoveringSlot(start, bytes) != nullptr;
}

bool NativeMaliBoTable::readJobType(uint64_t start, uint64_t bytes,
    uint32_t *jobType)
{
    if(!bytes || !jobType)return false;
    *jobType = 0;

    Slot *slot = nullptr;
    if(lastJobSlot_ < NativeMaliBoTableCapacity) {
        Slot &cached = slot_[lastJobSlot_];
        if(cached.live && cached.mapped) {
            const uint64_t begin = cached.iova;
            const uint64_t size = cached.memory.bytes();
            if(start >= begin && start - begin <= size &&
                bytes <= size - (start - begin))
                slot = &cached;
        }
    }
    if(!slot) {
        slot = findCoveringSlot(start, bytes);
        if(!slot)return false;
        lastJobSlot_ = static_cast<uint32_t>(slot - slot_);
    }

    const uint64_t off = start - slot->iova;
    const uint8_t *cpu = static_cast<const uint8_t *>(slot->memory.cpu());
    if(cpu && bytes >= 20) {
        /* Invalidate the page-aligned range covering the job header so XNU's
         * whole-page DC IVAC check succeeds and the kernel WB mapping sees
         * userspace's Write-Combine writes instead of stale cache lines. */
        const size_t pageMask = static_cast<size_t>(NativeMaliVmGranule - 1);
        const size_t pageStart = static_cast<size_t>(off) & ~pageMask;
        const size_t pageEnd = (static_cast<size_t>(off + bytes) + pageMask) & ~pageMask;
        const size_t clampedEnd = pageEnd <= slot->memory.bytes()
            ? pageEnd : slot->memory.bytes();
        if(clampedEnd > pageStart) {
            (void)slot->memory.invalidateForCpu(pageStart, clampedEnd - pageStart);
        }
        uint32_t word4 = 0;
        memcpy(&word4, cpu + off + 16, sizeof(word4));
        *jobType = (word4 >> 1) & 0x7fu;
    }
    return true;
}

bool NativeMaliBoTable::mapped(uint64_t handle) const
{
    const Slot *slot = find(handle);
    return slot && slot->mapped;
}

NativeMaliMemory *NativeMaliBoTable::memory(uint64_t handle)
{
    Slot *slot = find(handle);
    return slot ? &slot->memory : nullptr;
}

const NativeMaliMemory *NativeMaliBoTable::memory(uint64_t handle) const
{
    const Slot *slot = find(handle);
    return slot ? &slot->memory : nullptr;
}

NativeMaliMemory *NativeMaliBoTable::memoryForToken(uint32_t token)
{
    if(!open_ || !token || token > NativeMaliBoTableCapacity)return nullptr;
    Slot &slot = slot_[token - 1];
    if(slot.live && slot.cpuMapped && slot.cpuToken == token)
        return &slot.memory;
    return nullptr;
}
