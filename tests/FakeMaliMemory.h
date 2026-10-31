/* SPDX-License-Identifier: GPL-2.0-only */
/* Host doubles shared by the memory-related tests (NativeMaliMemory,
 * NativeMaliBoTable, NativeMaliUserClient).
 *
 * These stand in for the two XNU-side pieces that cannot exist on the host: the
 * page source behind the address-space tables and the buffer source behind a
 * BO. The classes are deliberately small authored doubles, not XNU headers or
 * ABI. Every value they fabricate is a contract check (page-aligned, contiguous
 * spans, exact coverage), not a hardware fact.
 *
 * tests/test_memory.cpp was the original home of these; they moved here when a
 * second and third test needed the same rig. */
#ifndef NATIVE_MALI_FAKE_MEMORY_H
#define NATIVE_MALI_FAKE_MEMORY_H
#include "FakeMaliPlatform.h"
#include "NativeMaliCache.h"
#include "NativeMaliMemory.h"
#include "NativeMaliVm.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* The four MFG domains powered, the state every rig starts from. */
inline constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;

class TestPages final : public NativeMaliPageSource {
public:
    enum : size_t { Capacity = NativeMaliVmMaxTables };
    void *cpu[Capacity]{};
    size_t live = 0;
    uint64_t firstPhys = UINT64_C(0x40000000);

    IOReturn alloc(NativeMaliPage *page) override
    {
        if(!page || live == Capacity)return kIOReturnNoMemory;
        void *memory = std::aligned_alloc(NativeMaliVmGranule,
            NativeMaliVmGranule);
        if(!memory)return kIOReturnNoMemory;
        std::memset(memory, 0xaa, NativeMaliVmGranule);
        cpu[live] = memory;
        page->cpu = memory;
        page->phys = firstPhys + live * NativeMaliVmGranule;
        page->bytes = NativeMaliVmGranule;
        ++live;
        return kIOReturnSuccess;
    }

    void release(const NativeMaliPage &page) override
    {
        for(size_t slot = 0; slot < live; ++slot)
            if(cpu[slot] == page.cpu) {
                std::free(cpu[slot]);
                --live;
                cpu[slot] = cpu[live];
                cpu[live] = nullptr;
                return;
            }
        std::fprintf(stderr, "released a page this source never handed out\n");
        std::exit(1);
    }
};

/* A page-aligned host allocation cut into spans of `stride` bytes. The bad
 * modes below are the contract violations NativeMaliMemory::prepare() must
 * refuse. */
class FakeMemorySource final : public NativeMaliMemorySource {
public:
    enum Bad {
        None, MisalignedPhysical, Gap, Overlap, Short, NoCpu, ZeroSpans,
        TooManySpans
    };
    size_t stride = 0x2000;            /* two spans in a 16 KiB buffer */
    uint64_t physBase = UINT64_C(0x50000000);
    Bad bad = None;
    IOReturn allocResult = kIOReturnSuccess;

    int allocs = 0;
    int releases = 0;
    size_t lastCount = 0;
    void *lastCpu = nullptr;

    IOReturn alloc(size_t bytes, void **cpu, NativeMaliMemorySpan *spans,
        size_t *count) override
    {
        ++allocs;
        lastCpu = nullptr;
        lastCount = 0;
        if(allocResult != kIOReturnSuccess) {
            --allocs;
            return allocResult;
        }
        if(!cpu || !spans || !count)return kIOReturnBadArgument;
        void *memory = nullptr;
        if(bad != NoCpu) {
            memory = std::aligned_alloc(NativeMaliVmGranule, bytes);
            if(!memory)return kIOReturnNoMemory;
            std::memset(memory, 0x5a, bytes);
        }
        size_t count_ = (bytes + stride - 1) / stride;
        if(bad == TooManySpans)count_ = NativeMaliMemoryMaxSpans + 1;
        if(bad == ZeroSpans)count_ = 0;
        if(bad == Short)count_ = count_ > 1 ? count_ - 1 : 0;
        for(size_t i = 0; i < count_ && i < NativeMaliMemoryMaxSpans; ++i) {
            spans[i].offset = i * stride;
            /* A gap between spans on purpose: the mapping must place every
             * span at its own physical address, not assume contiguity. */
            spans[i].physical = physBase + i * 0x10000000;
            spans[i].bytes = i + 1 == count_ && count_ > 1
                ? bytes - (count_ - 1) * stride
                : stride;
        }
        if(count_ > 1 && (bad == Gap || bad == Overlap)) {
            /* Gap: the second span starts past the first. Overlap: it starts
             * back at 0, so the first page would be mapped twice. */
            spans[1].offset = bad == Gap ? stride * 2 : 0;
        }
        if(count_ > 0 && bad == MisalignedPhysical)spans[0].physical += 4;
        *cpu = bad == NoCpu ? nullptr : memory;
        *count = count_;
        lastCpu = memory;
        lastCount = count_;
        return kIOReturnSuccess;
    }

    void release(void *cpu, const NativeMaliMemorySpan *spans,
        size_t count) override
    {
        (void)spans;
        ++releases;
        lastCount = count;
        lastCpu = cpu;
        std::free(cpu);
    }

    size_t outstanding() const { return static_cast<size_t>(allocs - releases); }
};

class FakeCache final : public NativeMaliCacheMaintainer {
public:
    struct Record {
        uint8_t *cpu;
        size_t bytes;
        NativeMaliCacheOperation operation;
    };
    std::vector<Record> calls;
    IOReturn result = kIOReturnSuccess;

    IOReturn maintain(void *cpu, size_t bytes,
        NativeMaliCacheOperation operation) override
    {
        calls.push_back({static_cast<uint8_t *>(cpu), bytes, operation});
        return result;
    }
};

#endif
