/* SPDX-License-Identifier: GPL-2.0-only */
/* XNU-only adapters: register windows, page-table pages, BO descriptors, and
 * ARM64 EL1 cache maintenance. */
#include "NativeMaliXnuSupport.h"
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <string.h>

extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" void clean_dcache64(addr64_t addr, unsigned count, int phys);

namespace {

uint64_t outputLimit(uint32_t bits)
{
    if(bits >= 32 && bits <= NativeMaliVmMaxOas)return UINT64_C(1) << bits;
    /* A probe that did not answer leaves oas at 0; the Mali maximum is the
     * conservative ceiling then. */
    return UINT64_C(1) << NativeMaliVmMaxOas;
}

IOOptionBits bufferOptions(bool contiguous)
{
    IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options |= static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    return options;
}

void flushSpansPhysical(const NativeMaliMemorySpan *spans, size_t count)
{
    constexpr size_t kMaxChunk = 16u * 1024u * 1024u;
    for(size_t i = 0; i < count; ++i) {
        size_t done = 0;
        const size_t total = static_cast<size_t>(spans[i].bytes);
        while(done < total) {
            const size_t chunk = (total - done) > kMaxChunk ? kMaxChunk : (total - done);
            flush_dcache64(spans[i].physical + done, static_cast<unsigned>(chunk), 1);
            done += chunk;
        }
    }
}

} // namespace

NativeMaliXnuPages::NativeMaliXnuPages()
{
    freeCount_ = NativeMaliVmMaxTables;
    for(size_t i = 0; i < NativeMaliVmMaxTables; ++i)
        freeSlots_[i] = static_cast<uint32_t>(NativeMaliVmMaxTables - 1 - i);
}

void NativeMaliXnuPages::setOutputLimit(uint32_t oas_bits)
{
    limit_ = outputLimit(oas_bits);
}

IOReturn NativeMaliXnuWindow::read32(uint32_t offset, uint32_t *value)
{
    if(!value || !base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
        return kIOReturnBadArgument;
    *value = *reinterpret_cast<volatile uint32_t *>(base_ + offset);
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuWindow::write32(uint32_t offset, uint32_t value)
{
    if(!base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(base_ + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuWindow::delay(uint64_t microseconds)
{
    if(microseconds > UINT32_MAX)return kIOReturnBadArgument;
    IODelay(static_cast<UInt32>(microseconds));
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuPages::alloc(NativeMaliPage *page)
{
    if(!page)return kIOReturnBadArgument;
    if(freeCount_ == 0)return kIOReturnNoResources;
    const uint32_t slotIndex = freeSlots_[--freeCount_];
    Entry *slot = &entry_[slotIndex];

    IOBufferMemoryDescriptor *descriptor =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            bufferOptions(true), NativeMaliVmGranule,
            (limit_ - 1) & ~UINT64_C(4095));
    if(!descriptor) {
        freeSlots_[freeCount_++] = slotIndex;
        return kIOReturnNoMemory;
    }
    IOReturn status = descriptor->prepare();
    if(status != kIOReturnSuccess) {
        descriptor->release();
        freeSlots_[freeCount_++] = slotIndex;
        return status;
    }

    IOByteCount length = 0;
    void *cpu = descriptor->getBytesNoCopy();
    const addr64_t physical =
        descriptor->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if(!cpu || (reinterpret_cast<uintptr_t>(cpu) & (NativeMaliVmGranule - 1)) ||
        !physical || (physical & (NativeMaliVmGranule - 1)) ||
        length < NativeMaliVmGranule || physical >= limit_ ||
        NativeMaliVmGranule > limit_ - physical ||
        descriptor->getLength() != NativeMaliVmGranule) {
        (void)descriptor->complete();
        descriptor->release();
        freeSlots_[freeCount_++] = slotIndex;
        return kIOReturnUnsupported;
    }

    memset(cpu, 0, NativeMaliVmGranule);
    slot->cpu = cpu;
    slot->descriptor = descriptor;
    ++live_;
    page->cpu = cpu;
    page->phys = physical;
    page->bytes = NativeMaliVmGranule;
    return kIOReturnSuccess;
}

void NativeMaliXnuPages::release(const NativeMaliPage &page)
{
    for(size_t i = 0; i < NativeMaliVmMaxTables; ++i) {
        Entry &entry = entry_[i];
        if(entry.cpu != page.cpu || !entry.descriptor)continue;
        IOBufferMemoryDescriptor *descriptor = entry.descriptor;
        entry = Entry{};
        if(live_)--live_;
        if(freeCount_ < NativeMaliVmMaxTables)
            freeSlots_[freeCount_++] = static_cast<uint32_t>(i);
        const IOReturn status = descriptor->complete();
        if(status != kIOReturnSuccess) {
            IOLog("NativeMali: page-table complete failed 0x%x, leaking one page\n",
                status);
            return;
        }
        descriptor->release();
        return;
    }
}

size_t NativeMaliXnuPages::live() const
{
    return live_;
}

NativeMaliXnuBuffers::NativeMaliXnuBuffers()
{
    freeCount_ = NativeMaliBoTableCapacity;
    for(size_t i = 0; i < NativeMaliBoTableCapacity; ++i)
        freeSlots_[i] = static_cast<uint32_t>(NativeMaliBoTableCapacity - 1 - i);
}

NativeMaliXnuBuffers::~NativeMaliXnuBuffers()
{
    cpuIndex_.release();
    cpuIndexReady_ = false;
}

void NativeMaliXnuBuffers::setOutputLimit(uint32_t oas_bits)
{
    limit_ = outputLimit(oas_bits);
}

int32_t NativeMaliXnuBuffers::acquireEntry()
{
    if(freeCount_ == 0)return -1;
    return static_cast<int32_t>(freeSlots_[--freeCount_]);
}

void NativeMaliXnuBuffers::releaseEntry(uint32_t idx)
{
    if(freeCount_ < NativeMaliBoTableCapacity)
        freeSlots_[freeCount_++] = idx;
}

void NativeMaliXnuBuffers::trackImported(uint32_t idx)
{
    if(importedCount_ < NativeMaliBoTableCapacity) {
        entry_[idx].importedPos = static_cast<uint32_t>(importedCount_);
        importedList_[importedCount_++] = idx;
    }
}

void NativeMaliXnuBuffers::untrackImported(uint32_t idx)
{
    const uint32_t pos = entry_[idx].importedPos;
    if(pos < importedCount_ && importedList_[pos] == idx) {
        --importedCount_;
        if(pos < importedCount_) {
            const uint32_t moved = importedList_[importedCount_];
            importedList_[pos] = moved;
            entry_[moved].importedPos = pos;
        }
    }
    entry_[idx].importedPos = UINT32_MAX;
}

IOReturn NativeMaliXnuBuffers::alloc(size_t bytes, void **cpu,
    NativeMaliMemorySpan *spans, size_t *count)
{
    return allocBounded(bytes, cpu, spans, NativeMaliMemoryMaxSpans, count);
}

IOReturn NativeMaliXnuBuffers::allocBounded(size_t bytes, void **cpu,
    NativeMaliMemorySpan *spans, size_t maxSpans, size_t *count)
{
    if(!cpu || !spans || !count || maxSpans == 0)return kIOReturnBadArgument;
    if(bytes == 0 || (bytes % NativeMaliVmGranule) != 0 || bytes > limit_)
        return kIOReturnBadArgument;

    if(!cpuIndexReady_)
        cpuIndexReady_ = cpuIndex_.allocate();

    const int32_t slotIdx = acquireEntry();
    if(slotIdx < 0)return kIOReturnNoResources;
    Entry *slot = &entry_[slotIdx];

    IOBufferMemoryDescriptor *descriptor = nullptr;
    if(bytes <= 65536) {
        descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            bufferOptions(true), static_cast<vm_size_t>(bytes),
            (limit_ - 1) & ~UINT64_C(4095));
    }
    if(!descriptor) {
        descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            bufferOptions(false), static_cast<vm_size_t>(bytes),
            (limit_ - 1) & ~UINT64_C(4095));
    }
    if(!descriptor) {
        releaseEntry(static_cast<uint32_t>(slotIdx));
        return kIOReturnNoMemory;
    }
    IOReturn status = descriptor->prepare();
    if(status != kIOReturnSuccess) {
        descriptor->release();
        releaseEntry(static_cast<uint32_t>(slotIdx));
        return status;
    }

    void *mapping = descriptor->getBytesNoCopy();
    size_t spans_used = 0;
    status = kIOReturnUnsupported;
    if(!mapping || (reinterpret_cast<uintptr_t>(mapping) & (NativeMaliVmGranule - 1)) ||
        descriptor->getLength() != bytes)
        goto failed;

    status = walkSpans(descriptor, bytes, spans, maxSpans, &spans_used);
    if(status != kIOReturnSuccess)goto failed;

    /* Zero through kernel WB mapping, then immediately clean+invalidate to PoC
     * so dirty zero-lines never linger in CPU D-cache and later evict over
     * userspace's Write-Combine (kIOMapWriteCombineCache) shader/job writes. */
    memset(mapping, 0, bytes);
    flushSpansPhysical(spans, spans_used);

    slot->cpu = mapping;
    slot->firstPhysical = spans[0].physical;
    slot->lastPhysical = spans[spans_used - 1].physical;
    slot->descriptor = descriptor;
    slot->bytes = bytes;
    slot->importedPos = UINT32_MAX;
    if(cpuIndexReady_)
        cpuIndex_.insert(reinterpret_cast<uint64_t>(mapping),
            static_cast<uint32_t>(slotIdx));
    *cpu = mapping;
    *count = spans_used;
    return kIOReturnSuccess;

failed:
    (void)descriptor->complete();
    descriptor->release();
    releaseEntry(static_cast<uint32_t>(slotIdx));
    return status;
}

IOReturn NativeMaliXnuBuffers::walkSpans(IOMemoryDescriptor *descriptor,
    size_t bytes, NativeMaliMemorySpan *spans, size_t maxSpans,
    size_t *count) const
{
    const size_t cap = maxSpans < NativeMaliMemoryMaxSpans
        ? maxSpans : NativeMaliMemoryMaxSpans;
    size_t spans_used = 0;
    for(size_t offset = 0; offset < bytes; ) {
        IOByteCount length = 0;
        const addr64_t physical =
            descriptor->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
        size_t chunk = static_cast<size_t>(length) & ~(NativeMaliVmGranule - 1);
        if(chunk > bytes - offset)chunk = bytes - offset;
        if(!physical || (physical & (NativeMaliVmGranule - 1)) ||
            chunk < NativeMaliVmGranule || physical >= limit_ ||
            chunk > limit_ - physical)
            return kIOReturnUnsupported;
        if(spans_used &&
            physical == spans[spans_used - 1].physical + spans[spans_used - 1].bytes) {
            spans[spans_used - 1].bytes += chunk;
        } else {
            if(spans_used == cap)return kIOReturnNoResources;
            spans[spans_used].offset = offset;
            spans[spans_used].physical = physical;
            spans[spans_used].bytes = chunk;
            ++spans_used;
        }
        offset += chunk;
    }
    *count = spans_used;
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuBuffers::importHost(uint64_t userAddress, size_t bytes,
    void **cpu, NativeMaliMemorySpan *spans, size_t *count)
{
    return importHostBounded(userAddress, bytes, cpu, spans,
        NativeMaliMemoryMaxSpans, count);
}

IOReturn NativeMaliXnuBuffers::importHostBounded(uint64_t userAddress,
    size_t bytes, void **cpu, NativeMaliMemorySpan *spans, size_t maxSpans,
    size_t *count)
{
    if(!cpu || !spans || !count || maxSpans == 0)return kIOReturnBadArgument;
    if(!owningTask_)return kIOReturnNotReady;
    if(userAddress == 0 || (userAddress & (NativeMaliVmGranule - 1)) ||
        bytes == 0 || (bytes % NativeMaliVmGranule) != 0 || bytes > limit_)
        return kIOReturnBadArgument;

    const int32_t slotIdx = acquireEntry();
    if(slotIdx < 0)return kIOReturnNoResources;
    Entry *slot = &entry_[slotIdx];

    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withAddressRange(
        static_cast<mach_vm_address_t>(userAddress),
        static_cast<mach_vm_size_t>(bytes),
        static_cast<IOOptionBits>(kIODirectionInOut), owningTask_);
    if(!descriptor) {
        releaseEntry(static_cast<uint32_t>(slotIdx));
        return kIOReturnBadArgument;
    }

    IOReturn status = descriptor->prepare();
    if(status != kIOReturnSuccess) {
        descriptor->release();
        /* Read-only user mappings (e.g. PROT_READ mmap'd assets passed to
         * newBufferWithBytesNoCopy:) fail kIODirectionInOut with
         * KERN_PROTECTION_FAILURE (0x2). Retry with kIODirectionOut
         * (host-to-device read-only) so XNU wires them with VM_PROT_READ. */
        descriptor = IOMemoryDescriptor::withAddressRange(
            static_cast<mach_vm_address_t>(userAddress),
            static_cast<mach_vm_size_t>(bytes),
            static_cast<IOOptionBits>(kIODirectionOut), owningTask_);
        if(!descriptor) {
            releaseEntry(static_cast<uint32_t>(slotIdx));
            return status;
        }
        const IOReturn roStatus = descriptor->prepare();
        if(roStatus != kIOReturnSuccess) {
            descriptor->release();
            releaseEntry(static_cast<uint32_t>(slotIdx));
            return roStatus;
        }
        status = kIOReturnSuccess;
    }

    size_t spans_used = 0;
    if(descriptor->getLength() != bytes) {
        status = kIOReturnUnsupported;
        goto failed;
    }

    status = walkSpans(descriptor, bytes, spans, maxSpans, &spans_used);
    if(status != kIOReturnSuccess)goto failed;

    slot->cpu = nullptr;
    slot->firstPhysical = spans[0].physical;
    slot->lastPhysical = spans[spans_used - 1].physical;
    slot->descriptor = descriptor;
    slot->bytes = bytes;
    trackImported(static_cast<uint32_t>(slotIdx));
    *cpu = nullptr;
    *count = spans_used;
    return kIOReturnSuccess;

failed:
    (void)descriptor->complete();
    descriptor->release();
    releaseEntry(static_cast<uint32_t>(slotIdx));
    return status;
}

void NativeMaliXnuBuffers::release(void *cpu, const NativeMaliMemorySpan *spans,
    size_t count)
{
    uint32_t foundIdx = UINT32_MAX;
    if(cpu == nullptr) {
        if(!spans || count == 0)return;
        size_t totalBytes = 0;
        for(size_t i = 0; i < count; ++i)
            totalBytes += static_cast<size_t>(spans[i].bytes);
        const uint64_t firstPhys = spans[0].physical;
        const uint64_t lastPhys = spans[count - 1].physical;
        for(size_t i = 0; i < importedCount_; ++i) {
            const uint32_t idx = importedList_[i];
            const Entry &entry = entry_[idx];
            if(entry.descriptor && entry.cpu == nullptr &&
                entry.firstPhysical == firstPhys &&
                entry.lastPhysical == lastPhys &&
                entry.bytes == totalBytes) {
                foundIdx = idx;
                break;
            }
        }
    } else {
        if(cpuIndexReady_) {
            const uint32_t idx = cpuIndex_.find(reinterpret_cast<uint64_t>(cpu));
            if(idx < NativeMaliBoTableCapacity &&
                entry_[idx].descriptor && entry_[idx].cpu == cpu)
                foundIdx = idx;
        }
        if(foundIdx == UINT32_MAX) {
            for(size_t i = 0; i < NativeMaliBoTableCapacity; ++i) {
                if(entry_[i].descriptor && entry_[i].cpu == cpu) {
                    foundIdx = static_cast<uint32_t>(i);
                    break;
                }
            }
        }
    }
    if(foundIdx == UINT32_MAX)return;

    Entry &entry = entry_[foundIdx];
    IOMemoryDescriptor *descriptor = entry.descriptor;
    if(entry.cpu == nullptr)
        untrackImported(foundIdx);
    else if(cpuIndexReady_)
        cpuIndex_.erase(reinterpret_cast<uint64_t>(entry.cpu));
    entry = Entry{};
    releaseEntry(foundIdx);

    const IOReturn status = descriptor->complete();
    if(status != kIOReturnSuccess) {
        IOLog("NativeMali: buffer complete failed 0x%x, leaking %lu bytes\n",
            status, static_cast<unsigned long>(descriptor->getLength()));
        return;
    }
    descriptor->release();
}

IOMemoryDescriptor *NativeMaliXnuBuffers::descriptor(void *cpu)
{
    if(!cpu)return nullptr;
    if(cpuIndexReady_) {
        const uint32_t idx = cpuIndex_.find(reinterpret_cast<uint64_t>(cpu));
        if(idx < NativeMaliBoTableCapacity &&
            entry_[idx].cpu == cpu && entry_[idx].descriptor) {
            entry_[idx].descriptor->retain();
            return entry_[idx].descriptor;
        }
        return nullptr;
    }
    for(auto &entry : entry_)
        if(entry.cpu == cpu && entry.descriptor) {
            entry.descriptor->retain();
            return entry.descriptor;
        }
    return nullptr;
}

IOReturn NativeMaliXnuCache::maintain(void *cpu, size_t bytes,
    NativeMaliCacheOperation operation)
{
    if(!cpu || bytes == 0)return kIOReturnBadArgument;
    if(operation != NativeMaliCacheClean &&
        operation != NativeMaliCacheInvalidate &&
        operation != NativeMaliCacheCleanInvalidate)
        return kIOReturnBadArgument;

    const uintptr_t address = reinterpret_cast<uintptr_t>(cpu);
    if(bytes > UINTPTR_MAX - address)return kIOReturnBadArgument;
    if(address + bytes > UINTPTR_MAX - (NativeMaliVmGranule - 1))
        return kIOReturnBadArgument;
    const uintptr_t begin = address & ~static_cast<uintptr_t>(NativeMaliVmGranule - 1);
    const uintptr_t end = (address + bytes + NativeMaliVmGranule - 1) &
        ~static_cast<uintptr_t>(NativeMaliVmGranule - 1);

    if(operation == NativeMaliCacheInvalidate &&
        (begin != address || end != address + bytes))
        return kIOReturnBadArgument;

    disable_preemption();
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = static_cast<size_t>(4) << ((ctr >> 16) & 15);
    if(line == 0 || line > NativeMaliVmGranule) {
        enable_preemption();
        return kIOReturnUnsupported;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    for(uintptr_t at = begin; at < end; at += line) {
        if(operation == NativeMaliCacheClean)
            __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        else if(operation == NativeMaliCacheInvalidate)
            __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        else
            __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    enable_preemption();
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuCache::maintainPhysical(uint64_t phys, size_t bytes,
    NativeMaliCacheOperation operation)
{
    if(!phys || bytes == 0)return kIOReturnBadArgument;
    if(operation != NativeMaliCacheClean &&
        operation != NativeMaliCacheInvalidate &&
        operation != NativeMaliCacheCleanInvalidate)
        return kIOReturnBadArgument;
    if(bytes > UINT64_MAX - phys)return kIOReturnBadArgument;

    constexpr size_t kMaxChunk = 16u * 1024u * 1024u;
    size_t done = 0;
    while(done < bytes) {
        const size_t chunk = (bytes - done) > kMaxChunk ? kMaxChunk : (bytes - done);
        if(operation == NativeMaliCacheClean)
            clean_dcache64(phys + done, static_cast<unsigned>(chunk), 1);
        else
            flush_dcache64(phys + done, static_cast<unsigned>(chunk), 1);
        done += chunk;
    }
    return kIOReturnSuccess;
}
