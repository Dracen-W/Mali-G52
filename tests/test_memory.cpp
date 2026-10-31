/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliMemory against a fake source and cache maintainer.
 *
 * The page tables are the real NativeMaliVm and the flush goes through the
 * real NativeMaliMmu on a FakeMmu register bank, so the leaves and the
 * LOCK/FLUSH_PT sequence are the ones the GPU would see. The allocation is the
 * XNU-side part that cannot be host-tested (IOBufferMemoryDescriptor), so it is
 * behind NativeMaliMemorySource and faked here.
 *
 * Expectations come from panfrost_mmu_map() (panfrost_mmu.c:466-493): every
 * span at the next IOVA with IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, then one
 * range flush; a failure unmaps the part already mapped (mmu_map_sg(),
 * panfrost_mmu.c:421-464). Coherency is panfrost_device.c:269 and the absent
 * "dma-coherent" property: this phone is non-coherent, so clean before the GPU
 * reads and invalidate after it writes. */
#include "FakeMaliMemory.h"
#include "FakeMaliPlatform.h"
#include "NativeMaliMemory.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeMmu registers;
    NativeMaliPower power;
    NativeMaliMmu mmu;
    TestPages pages;
    NativeMaliVm vm;

    Rig()
    {
        const uint32_t controls[] = {
            NativeMaliPower::SpmMfgAsyncPowerCon,
            NativeMaliPower::SpmMfgPowerCon,
            NativeMaliPower::SpmMfgCore0PowerCon,
            NativeMaliPower::SpmMfgCore1PowerCon,
        };
        for(uint32_t control : controls)
            spm.memory[control / 4] = kPowerOn;
        top.memory[NativeMaliPower::TopMfgClockConfig / 4] = 0;
        mfgcfg.memory[0] = 0;
        NativeMaliPowerBlocks blocks{&spm, &infra, &top, &mfgcfg, &pwrap};
        CHECK(power.prepare(blocks) == kIOReturnSuccess);
        CHECK(mmu.prepare(&registers, &power) == kIOReturnSuccess);
        CHECK(mmu.setPresent(0x7) == kIOReturnSuccess);
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
    }

    uint32_t allocateAs()
    {
        uint32_t as = 0;
        CHECK(mmu.allocate(&as) == kIOReturnSuccess);
        return as;
    }
};

/* The default prot of map(): IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE. The golden
 * leaf word comes from the already-test_vm-checked encoding. */
constexpr uint32_t kProt = NativeMaliVmRead | NativeMaliVmWrite |
    NativeMaliVmCached;

void checkFlush(const FakeMmu &mmu, size_t index, uint32_t as, uint64_t iova,
    uint64_t size)
{
    uint64_t lock = 0;
    CHECK(NativeMaliMmu::lockRegionValue(iova, size, &lock));
    CHECK(mmu.writes.size() == index + 4);
    CHECK(mmu.writes[index + 0].offset == AS_LOCKADDR_LO(as));
    CHECK(mmu.writes[index + 0].value == static_cast<uint32_t>(lock));
    CHECK(mmu.writes[index + 1].offset == AS_LOCKADDR_HI(as));
    CHECK(mmu.writes[index + 1].value == static_cast<uint32_t>(lock >> 32));
    CHECK(mmu.writes[index + 2].offset == AS_COMMAND(as));
    CHECK(mmu.writes[index + 2].value == AS_COMMAND_LOCK);
    CHECK(mmu.writes[index + 3].offset == AS_COMMAND(as));
    CHECK(mmu.writes[index + 3].value == AS_COMMAND_FLUSH_PT);
}

void testPrepareRefusals()
{
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;

    CHECK(memory.prepare(nullptr, 0x4000, false, &cache) == kIOReturnBadArgument);
    CHECK(memory.prepare(&source, 0, false, &cache) == kIOReturnBadArgument);
    /* 100 bytes cannot be described by whole 4 KiB leaves. */
    CHECK(memory.prepare(&source, 100, false, &cache) == kIOReturnBadArgument);
    CHECK(memory.prepare(&source, 0x4001, false, &cache) == kIOReturnBadArgument);
    /* A non-coherent buffer without a maintainer is refused, not mapped
     * without its clean/invalidate. */
    CHECK(memory.prepare(&source, 0x4000, false, nullptr) == kIOReturnBadArgument);
    CHECK(source.allocs == 0);
    CHECK(!memory.prepared());

    /* The source's own failure is returned unchanged and nothing is recorded. */
    source.allocResult = kIOReturnNoMemory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnNoMemory);
    CHECK(!memory.prepared());
    CHECK(source.outstanding() == 0);
}

void testPrepareSuccess()
{
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;

    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
    CHECK(memory.prepared());
    CHECK(!memory.mapped());
    CHECK(!memory.coherent());
    CHECK(memory.bytes() == 0x4000);
    CHECK(memory.spans() == 2);
    CHECK(memory.cpu() != nullptr);
    CHECK(memory.iova() == 0);
    CHECK(source.allocs == 1 && source.outstanding() == 1);

    /* A second prepare() would lose the first allocation. */
    FakeMemorySource other;
    CHECK(memory.prepare(&other, 0x4000, false, &cache) == kIOReturnBusy);
    CHECK(other.allocs == 0);

    CHECK(memory.release() == kIOReturnSuccess);
    CHECK(!memory.prepared());
    CHECK(source.releases == 1 && source.outstanding() == 0);
    CHECK(memory.release() == kIOReturnNotReady);

    /* A coherent buffer needs no maintainer: there is nothing to maintain. */
    FakeMemorySource coherentSource;
    NativeMaliMemory coherent;
    CHECK(coherent.prepare(&coherentSource, 0x4000, true, nullptr) ==
        kIOReturnSuccess);
    CHECK(coherent.coherent());
    CHECK(coherent.cleanForDevice(0, 0x4000) == kIOReturnSuccess);
    CHECK(coherent.invalidateForCpu(0, 0x4000) == kIOReturnSuccess);
    coherent.reset();
    CHECK(coherentSource.outstanding() == 0);

    /* reset() releases without a release() call, and so does the destructor. */
    FakeMemorySource scopedSource;
    {
        NativeMaliMemory scoped;
        CHECK(scoped.prepare(&scopedSource, 0x4000, false, &cache) ==
            kIOReturnSuccess);
    }
    CHECK(scopedSource.outstanding() == 0);
}

void testPrepareSpanValidation()
{
    const FakeMemorySource::Bad bads[] = {
        FakeMemorySource::MisalignedPhysical,
        FakeMemorySource::Gap,
        FakeMemorySource::Overlap,
        FakeMemorySource::Short,
        FakeMemorySource::NoCpu,
        FakeMemorySource::ZeroSpans,
        FakeMemorySource::TooManySpans,
    };
    for(FakeMemorySource::Bad bad : bads) {
        FakeMemorySource source;
        FakeCache cache;
        NativeMaliMemory memory;
        source.bad = bad;
        CHECK(memory.prepare(&source, 0x4000, false, &cache) != kIOReturnSuccess);
        CHECK(!memory.prepared());
        /* Whatever the allocator handed out goes straight back. */
        CHECK(source.releases == 1);
        CHECK(source.outstanding() == 0);
    }
}

void testMap()
{
    Rig rig;
    const uint32_t as = rig.allocateAs();
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
    rig.registers.writes.clear();

    const uint64_t iova = UINT64_C(0x20000);
    CHECK(memory.map(&rig.vm, &rig.mmu, as, iova) == kIOReturnSuccess);
    CHECK(memory.mapped());
    CHECK(memory.iova() == iova);
    CHECK(rig.vm.mappedPages() == 4);

    /* Two spans, each at its own physical run, then one flush of the whole
     * 16 KiB range: LOCKADDR_LO/HI = 0x2000e (32 KiB-aligned block at
     * 0x20000), LOCK, FLUSH_PT. */
    const FakeRegisterBlock::Write expected[] = {
        {AS_LOCKADDR_LO(as), 0x2000e},
        {AS_LOCKADDR_HI(as), 0},
        {AS_COMMAND(as), AS_COMMAND_LOCK},
        {AS_COMMAND(as), AS_COMMAND_FLUSH_PT},
    };
    CHECK(rig.registers.writes.size() == 4);
    for(size_t i = 0; i < 4; ++i) {
        CHECK(rig.registers.writes[i].offset == expected[i].offset);
        CHECK(rig.registers.writes[i].value == expected[i].value);
    }
    CHECK(rig.registers.statusReads == 3);
    CHECK(rig.registers.commandWrites == 2);

    /* Every leaf points at the span's physical page, with the map() defaults.
     * Span 0 is at 0x50000000 and span 1 at 0x60000000: the two spans are not
     * contiguous in physical memory, so a wrong offset would show here. */
    const uint64_t spanPhys[] = {UINT64_C(0x50000000), UINT64_C(0x60000000)};
    for(uint32_t page = 0; page < 4; ++page) {
        uint64_t pte = 0;
        CHECK(rig.vm.descriptor(iova + page * NativeMaliVmGranule, &pte) ==
            kIOReturnSuccess);
        CHECK(pte == NativeMaliVm::encodeLeaf(
            spanPhys[page / 2] + (page % 2) * NativeMaliVmGranule, kProt));
    }

    /* Mapping the same buffer twice is refused before anything is written. */
    const size_t writesBefore = rig.registers.writes.size();
    CHECK(memory.map(&rig.vm, &rig.mmu, as, iova) == kIOReturnBusy);
    CHECK(rig.registers.writes.size() == writesBefore);
    CHECK(rig.vm.mappedPages() == 4);

    /* unmap() clears the same leaves and flushes the same range once more. */
    CHECK(memory.unmap() == kIOReturnSuccess);
    CHECK(!memory.mapped());
    CHECK(memory.iova() == 0);
    CHECK(rig.vm.mappedPages() == 0);
    for(uint32_t page = 0; page < 4; ++page) {
        uint64_t pte = UINT64_C(0xdeadbeef);
        CHECK(rig.vm.descriptor(iova + page * NativeMaliVmGranule, &pte) ==
            kIOReturnSuccess);
        CHECK(pte == 0);
    }
    checkFlush(rig.registers, writesBefore, as, iova, 0x4000);
    CHECK(rig.registers.statusReads == 6);

    /* Not mapped any more. */
    CHECK(memory.unmap() == kIOReturnBadArgument);
    /* The allocation survives the mapping. */
    CHECK(source.outstanding() == 1);
    CHECK(memory.release() == kIOReturnSuccess);
    CHECK(source.outstanding() == 0);
}

void testMapRefusals()
{
    Rig rig;
    const uint32_t as = rig.allocateAs();
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);

    /* Not prepared: a second object never allocated. */
    NativeMaliMemory cold;
    CHECK(cold.map(&rig.vm, &rig.mmu, as, 0x20000) == kIOReturnNotReady);

    rig.registers.writes.clear();
    CHECK(memory.map(nullptr, &rig.mmu, as, 0x20000) == kIOReturnBadArgument);
    CHECK(memory.map(&rig.vm, nullptr, as, 0x20000) == kIOReturnBadArgument);
    CHECK(memory.map(&rig.vm, &rig.mmu, as, 0x20001) == kIOReturnBadArgument);
    /* Slot 3 exists in the present mask but was never allocated, and slot 7
     * is not present at all. */
    CHECK(memory.map(&rig.vm, &rig.mmu, 3, 0x20000) == kIOReturnBadArgument);
    CHECK(memory.map(&rig.vm, &rig.mmu, 7, 0x20000) == kIOReturnBadArgument);
    CHECK(memory.map(&rig.vm, &rig.mmu, 32, 0x20000) == kIOReturnBadArgument);
    /* prot with neither read nor write is refused by NativeMaliVm. */
    CHECK(memory.map(&rig.vm, &rig.mmu, as, 0x20000, NativeMaliVmCached) ==
        kIOReturnBadArgument);
    /* Outside the address space. */
    CHECK(memory.map(&rig.vm, &rig.mmu, as, UINT64_C(1) << 48) ==
        kIOReturnBadArgument);
    CHECK(rig.vm.mappedPages() == 0);
    CHECK(rig.registers.writes.empty());
    CHECK(!memory.mapped());
}

void testMapRollback()
{
    {
        /* The second span hits an existing leaf: the first span is taken back
         * out, leaving only the caller's own mapping. */
        Rig rig;
        const uint32_t as = rig.allocateAs();
        FakeMemorySource source;
        FakeCache cache;
        NativeMaliMemory memory;
        CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
        const uint64_t iova = UINT64_C(0x20000);
        CHECK(rig.vm.map(iova + 0x2000, UINT64_C(0x70000000),
            NativeMaliVmGranule, kProt) == kIOReturnSuccess);
        rig.registers.writes.clear();

        CHECK(memory.map(&rig.vm, &rig.mmu, as, iova) == kIOReturnBusy);
        CHECK(!memory.mapped());
        CHECK(rig.vm.mappedPages() == 1);
        uint64_t pte = 0;
        CHECK(rig.vm.descriptor(iova, &pte) == kIOReturnSuccess);
        CHECK(pte == 0);
        CHECK(rig.vm.descriptor(iova + 0x2000, &pte) == kIOReturnSuccess);
        CHECK(pte == NativeMaliVm::encodeLeaf(UINT64_C(0x70000000), kProt));
        /* Nothing was flushed: there was no complete range to publish. */
        CHECK(rig.registers.writes.empty());
    }
    {
        /* The flush itself fails: the whole range is taken back out and the
         * timeout is returned unchanged. */
        Rig rig;
        const uint32_t as = rig.allocateAs();
        FakeMemorySource source;
        FakeCache cache;
        NativeMaliMemory memory;
        CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
        const uint64_t iova = UINT64_C(0x20000);
        rig.registers.activeReads = 0xFFFFFFFF;
        CHECK(memory.map(&rig.vm, &rig.mmu, as, iova) == kIOReturnTimeout);
        CHECK(!memory.mapped());
        CHECK(rig.vm.mappedPages() == 0);
        uint64_t pte = UINT64_C(0xdeadbeef);
        CHECK(rig.vm.descriptor(iova, &pte) == kIOReturnSuccess);
        CHECK(pte == 0);
        CHECK(rig.registers.commandWrites == 0);
    }
}

void testUnmapFlushFailure()
{
    Rig rig;
    const uint32_t as = rig.allocateAs();
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
    const uint64_t iova = UINT64_C(0x20000);
    CHECK(memory.map(&rig.vm, &rig.mmu, as, iova) == kIOReturnSuccess);

    /* The flush times out: the leaves are already clear, but the buffer stays
     * marked mapped so the retry only repeats the flush. */
    rig.registers.activeReads = 0xFFFFFFFF;
    CHECK(memory.unmap() == kIOReturnTimeout);
    CHECK(memory.mapped());
    CHECK(rig.vm.mappedPages() == 0);

    rig.registers.activeReads = 0;
    CHECK(memory.unmap() == kIOReturnSuccess);
    CHECK(!memory.mapped());
}

void testReleaseWhileMapped()
{
    Rig rig;
    const uint32_t as = rig.allocateAs();
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);
    CHECK(memory.map(&rig.vm, &rig.mmu, as, UINT64_C(0x20000)) ==
        kIOReturnSuccess);

    /* Releasing the pages under live leaves is refused. */
    CHECK(memory.release() == kIOReturnBusy);
    CHECK(source.releases == 0);
    CHECK(memory.prepared() && memory.mapped());

    CHECK(memory.unmap() == kIOReturnSuccess);
    CHECK(memory.release() == kIOReturnSuccess);
    CHECK(source.releases == 1);
}

void testCachePolicy()
{
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliMemory memory;
    CHECK(memory.prepare(&source, 0x4000, false, &cache) == kIOReturnSuccess);

    /* Non-coherent: clean before the GPU reads, invalidate after it writes. */
    const uint8_t *base = static_cast<const uint8_t *>(memory.cpu());
    CHECK(memory.cleanForDevice(0, 0x4000) == kIOReturnSuccess);
    CHECK(cache.calls.size() == 1);
    CHECK(cache.calls[0].cpu == base);
    CHECK(cache.calls[0].bytes == 0x4000);
    CHECK(cache.calls[0].operation == NativeMaliCacheClean);

    CHECK(memory.invalidateForCpu(0x1000, 0x800) == kIOReturnSuccess);
    CHECK(cache.calls.size() == 2);
    CHECK(cache.calls[1].cpu == base + 0x1000);
    CHECK(cache.calls[1].bytes == 0x800);
    CHECK(cache.calls[1].operation == NativeMaliCacheInvalidate);

    /* An empty range is a success and no primitive call. */
    CHECK(memory.cleanForDevice(0x4000, 0) == kIOReturnSuccess);
    CHECK(cache.calls.size() == 2);

    /* Ranges are checked against the allocation, never clipped. */
    CHECK(memory.cleanForDevice(0x4000, 1) == kIOReturnBadArgument);
    CHECK(memory.cleanForDevice(0x4001, 0) == kIOReturnBadArgument);
    CHECK(memory.invalidateForCpu(0, 0x4001) == kIOReturnBadArgument);
    CHECK(memory.invalidateForCpu(UINT64_C(1) << 40, 1) == kIOReturnBadArgument);
    CHECK(cache.calls.size() == 2);

    /* A maintainer failure is the caller's error, unchanged. */
    cache.result = kIOReturnNoResources;
    CHECK(memory.cleanForDevice(0, 0x1000) == kIOReturnNoResources);
    CHECK(memory.invalidateForCpu(0, 0x1000) == kIOReturnNoResources);
    cache.result = kIOReturnSuccess;

    /* A coherent buffer never calls the maintainer, even with one attached. */
    FakeMemorySource coherentSource;
    NativeMaliMemory coherent;
    CHECK(coherent.prepare(&coherentSource, 0x4000, true, &cache) ==
        kIOReturnSuccess);
    const size_t calls = cache.calls.size();
    CHECK(coherent.cleanForDevice(0, 0x4000) == kIOReturnSuccess);
    CHECK(coherent.invalidateForCpu(0, 0x4000) == kIOReturnSuccess);
    CHECK(cache.calls.size() == calls);

    /* And an unprepared object has no cache policy at all. */
    NativeMaliMemory cold;
    CHECK(cold.cleanForDevice(0, 1) == kIOReturnNotReady);
    CHECK(cold.invalidateForCpu(0, 1) == kIOReturnNotReady);
}

} // namespace

int main()
{
    testPrepareRefusals();
    testPrepareSuccess();
    testPrepareSpanValidation();
    testMap();
    testMapRefusals();
    testMapRollback();
    testUnmapFlushFailure();
    testReleaseWhileMapped();
    testCachePolicy();
    std::puts("PASS: buffer spans, allocation/release, span validation, per-span IOVA mapping with one range flush, rollback, double-map and release refusals, and the non-coherent clean/invalidate policy");
    return 0;
}
