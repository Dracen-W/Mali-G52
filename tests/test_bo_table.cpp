/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliBoTable: the user client's per-object state machine.
 *
 * The page tables are the real NativeMaliVm and the flush goes through the real
 * NativeMaliMmu on a FakeMmu register bank, so every mapping checked here is the
 * leaf the GPU would read and every retirement is the LOCK/FLUSH_PT sequence it
 * would see. The allocation behind the buffer is the XNU-side part that cannot
 * be host-tested (IOBufferMemoryDescriptor), so it is faked, exactly as
 * tests/test_memory.cpp does.
 *
 * Expectations come from the wire ABI (NativeMaliABI.h), panfrost_mmu_map()
 * (panfrost_mmu.c:466-493), panfrost_mmu_unmap() (panfrost_mmu.c:500-524) and
 * the shape NativeAdrenoUserClient established (MIT, see NOTICE): monotonic
 * handles, free = unmap then release, a CPU token that keeps the object alive,
 * and a teardown that drops the tokens before it retires what is left. */
#include "FakeMaliMemory.h"
#include "FakeMaliPlatform.h"
#include "NativeMaliBoTable.h"
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
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliBoTable table;
    uint32_t as = 0;

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
        CHECK(mmu.allocate(&as) == kIOReturnSuccess);
    }

    IOReturn open(uint64_t maxBytes = 0)
    {
        return table.open(&source, &cache, false, &vm, &mmu, as, maxBytes);
    }

    uint64_t alloc(uint64_t bytes = 0x4000)
    {
        NativeMaliAbiBoAlloc request{bytes};
        NativeMaliAbiBoAllocReply reply{};
        const IOReturn result = table.alloc(request, &reply);
        CHECK(result == kIOReturnSuccess);
        CHECK(reply.handle != 0);
        return reply.handle;
    }

    IOReturn map(uint64_t handle, uint64_t iova,
        uint32_t permissions = NativeMaliBoRead | NativeMaliBoWrite)
    {
        NativeMaliAbiBoMap request{};
        request.handle = handle;
        request.iova = iova;
        request.permissions = permissions;
        NativeMaliAbiBoMapReply reply{};
        return table.map(request, &reply);
    }
};

/* The four writes NativeMaliMmu::flushRange() makes for one range. */
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

constexpr uint64_t kFirstPhys = UINT64_C(0x50000000);

void testOpen()
{
    Rig rig;

    /* Nothing works before open. */
    {
        NativeMaliAbiBoAlloc alloc{0x4000};
        NativeMaliAbiBoAllocReply reply{0xdead};
        CHECK(rig.table.alloc(alloc, &reply) == kIOReturnNotReady);
        CHECK(reply.handle == 0);
        NativeMaliAbiBoMap map{};
        map.handle = 1;
        map.iova = 0x1000;
        map.permissions = NativeMaliBoRead;
        NativeMaliAbiBoMapReply mapReply{0xdead};
        CHECK(rig.table.map(map, &mapReply) == kIOReturnNotReady);
        CHECK(mapReply.iova == 0);
        CHECK(!rig.table.mapped(1));
        CHECK(rig.table.memory(1) == nullptr);
        CHECK(rig.table.memoryForToken(1) == nullptr);
        CHECK(rig.table.releaseAll() == kIOReturnNotReady);
    }

    /* Argument refusals, each before any allocation. */
    CHECK(rig.table.open(nullptr, &rig.cache, false, &rig.vm, &rig.mmu, rig.as,
        UINT64_C(0x100000)) == kIOReturnBadArgument);
    CHECK(rig.table.open(&rig.source, &rig.cache, false, nullptr, &rig.mmu,
        rig.as, UINT64_C(0x100000)) == kIOReturnBadArgument);
    CHECK(rig.table.open(&rig.source, &rig.cache, false, &rig.vm, nullptr,
        rig.as, UINT64_C(0x100000)) == kIOReturnBadArgument);
    /* A non-coherent client without a cache maintainer would map memory
     * without the clean/invalidate the GPU needs. */
    CHECK(rig.table.open(&rig.source, nullptr, false, &rig.vm, &rig.mmu, rig.as,
        UINT64_C(0x100000)) == kIOReturnBadArgument);
    CHECK(rig.table.open(&rig.source, &rig.cache, true, &rig.vm, &rig.mmu,
        NativeMaliMmuMaxAs, UINT64_C(0x100000)) == kIOReturnBadArgument);
    CHECK(!rig.table.open());

    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);
    CHECK(rig.table.open());
    /* One address space per client: opening twice would silently move the
     * mapping target of the objects already handed out. */
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnBusy);

    /* A slot the MMU has not allocated cannot answer the FLUSH_PT every map
     * ends in, so it is refused at open. */
    {
        Rig other;
        const uint32_t stray = 5;
        CHECK(other.table.open(&other.source, &other.cache, false, &other.vm,
            &other.mmu, stray, UINT64_C(0x100000)) == kIOReturnBadArgument);
    }
}

void testAlloc()
{
    Rig rig;
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);

    /* Malformed sizes are the ABI's answer, not a source call. */
    for(uint64_t bytes : {UINT64_C(0), UINT64_C(1), UINT64_C(4095),
        UINT64_C(4097)}) {
        NativeMaliAbiBoAlloc request{bytes};
        NativeMaliAbiBoAllocReply reply{0xdead};
        CHECK(rig.table.alloc(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.handle == 0);
    }
    CHECK(rig.source.allocs == 0);
    /* Well formed but past the advertised maximum. */
    {
        NativeMaliAbiBoAlloc request{UINT64_C(0x200000)};
        NativeMaliAbiBoAllocReply reply{0xdead};
        CHECK(rig.table.alloc(request, &reply) == kIOReturnNoResources);
        CHECK(reply.handle == 0);
        CHECK(rig.source.allocs == 0);
    }
    /* A null reply cannot be zeroed, so the request is refused. */
    {
        NativeMaliAbiBoAlloc request{0x4000};
        CHECK(rig.table.alloc(request, nullptr) == kIOReturnBadArgument);
        CHECK(rig.source.allocs == 0);
    }

    /* The source's failure is returned unchanged and no slot is consumed. */
    rig.source.allocResult = kIOReturnNoMemory;
    {
        NativeMaliAbiBoAlloc request{0x4000};
        NativeMaliAbiBoAllocReply reply{0xdead};
        CHECK(rig.table.alloc(request, &reply) == kIOReturnNoMemory);
        CHECK(reply.handle == 0);
        CHECK(rig.table.live() == 0);
    }
    rig.source.allocResult = kIOReturnSuccess;

    const uint64_t first = rig.alloc();
    CHECK(rig.table.live() == 1);
    CHECK(rig.source.outstanding() == 1);
    CHECK(rig.table.memory(first) != nullptr);
    CHECK(rig.table.memory(first)->bytes() == 0x4000);
    CHECK(!rig.table.mapped(first));

    /* Handles are monotonic, so a stale one cannot name a newer object. */
    NativeMaliAbiBoFree release{first};
    CHECK(rig.table.freeBo(release) == kIOReturnSuccess);
    CHECK(rig.table.live() == 0);
    const uint64_t second = rig.alloc();
    CHECK(second > first);
    CHECK(rig.table.memory(first) == nullptr);
    CHECK(rig.table.memory(second) != nullptr);

    /* Full table: NoResources, and nothing is allocated from the source for
     * the refused request. */
    while(rig.table.live() < NativeMaliBoTableCapacity)rig.alloc();
    const int allocs = rig.source.allocs;
    {
        NativeMaliAbiBoAlloc request{0x4000};
        NativeMaliAbiBoAllocReply reply{0xdead};
        CHECK(rig.table.alloc(request, &reply) == kIOReturnNoResources);
        CHECK(reply.handle == 0);
    }
    CHECK(rig.source.allocs == allocs);
    CHECK(rig.table.live() == NativeMaliBoTableCapacity);

    CHECK(rig.table.releaseAll() == kIOReturnSuccess);
    CHECK(rig.table.live() == 0);
    CHECK(rig.source.outstanding() == 0);
}

void testMap()
{
    Rig rig;
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);

    /* Unknown handle: the reply stays zero and no register is touched. */
    {
        NativeMaliAbiBoMap request{};
        request.handle = 0x1234;
        request.iova = 0x1000;
        request.permissions = NativeMaliBoRead;
        NativeMaliAbiBoMapReply reply{0xdead};
        CHECK(rig.table.map(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.iova == 0);
        CHECK(rig.registers.writes.empty());
    }

    const uint64_t handle = rig.alloc();
    /* The record's own checks: zero / unaligned / out-of-range address,
     * missing read permission, unknown permission bits. */
    CHECK(rig.map(handle, 0) == kIOReturnBadArgument);
    CHECK(rig.map(handle, 0x10800) == kIOReturnBadArgument);
    CHECK(rig.map(handle, UINT64_C(1) << 48) == kIOReturnBadArgument);
    CHECK(rig.map(handle, 0x10000, 0) == kIOReturnBadArgument);
    CHECK(rig.map(handle, 0x10000, NativeMaliBoWrite) == kIOReturnBadArgument);
    CHECK(rig.map(handle, 0x10000, NativeMaliBoRead | (1u << 2)) ==
        kIOReturnUnsupported);
    CHECK(!rig.table.mapped(handle));
    CHECK(rig.registers.writes.empty());

    /* Success: two 8 KiB spans at their own physical addresses, one flush for
     * the whole range, and the read/write/cached leaf panfrost_mmu_map() asks
     * for (panfrost_mmu.c:466-475). */
    {
        NativeMaliAbiBoMap request{};
        request.handle = handle;
        request.iova = 0x10000;
        request.permissions = NativeMaliBoRead | NativeMaliBoWrite;
        NativeMaliAbiBoMapReply reply{};
        CHECK(rig.table.map(request, &reply) == kIOReturnSuccess);
        CHECK(reply.iova == 0x10000);
        CHECK(rig.table.mapped(handle));
        CHECK(rig.table.memory(handle)->iova() == 0x10000);
        uint64_t leaf = 0;
        CHECK(rig.vm.descriptor(0x10000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == NativeMaliVm::encodeLeaf(kFirstPhys, NativeMaliVmRead |
            NativeMaliVmWrite | NativeMaliVmCached));
        CHECK(rig.vm.descriptor(0x12000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == NativeMaliVm::encodeLeaf(UINT64_C(0x60000000),
            NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmCached));
        checkFlush(rig.registers, 0, rig.as, 0x10000, 0x4000);

        /* covers() is the check the submission path uses for the chain head:
         * a range inside the mapping, including its last byte, and nothing
         * past either end. */
        CHECK(rig.table.covers(0x10000, 64));
        CHECK(rig.table.covers(0x13FC0, 64));
        CHECK(rig.table.covers(0x10000, 0x4000));
        CHECK(!rig.table.covers(0x13FC0, 65));
        CHECK(!rig.table.covers(0x10000, 0x4001));
        CHECK(!rig.table.covers(0x0FFF0, 64));
        CHECK(!rig.table.covers(0x10000, 0));
        CHECK(!rig.table.covers(UINT64_C(0xFFFFFFFFFFFFFFFF), 64));
    }

    /* Mapping the same object twice is refused before a single write. */
    {
        const size_t writes = rig.registers.writes.size();
        CHECK(rig.map(handle, 0x20000) == kIOReturnBusy);
        CHECK(rig.registers.writes.size() == writes);
        CHECK(rig.table.memory(handle)->iova() == 0x10000);
    }

    /* A read-only mapping is the one place this driver differs from panfrost,
     * which always maps write; the encoder expresses it and the leaf says so. */
    {
        const uint64_t ro = rig.alloc();
        CHECK(rig.map(ro, 0x20000, NativeMaliBoRead) == kIOReturnSuccess);
        uint64_t leaf = 0;
        CHECK(rig.vm.descriptor(0x20000, &leaf) == kIOReturnSuccess);
        uint64_t rw = 0;
        CHECK(rig.vm.descriptor(0x10000, &rw) == kIOReturnSuccess);
        CHECK(leaf == NativeMaliVm::encodeLeaf(kFirstPhys, NativeMaliVmRead |
            NativeMaliVmCached));
        CHECK(leaf != rw);
    }

    /* Overlap: two objects cannot own the same page, and the second attempt
     * writes nothing and leaves the second object unmapped. */
    {
        const uint64_t other = rig.alloc();
        const size_t writes = rig.registers.writes.size();
        CHECK(rig.map(other, 0x12000) == kIOReturnBadArgument);
        CHECK(rig.registers.writes.size() == writes);
        CHECK(!rig.table.mapped(other));
        uint64_t leaf = 0;
        CHECK(rig.vm.descriptor(0x12000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == NativeMaliVm::encodeLeaf(UINT64_C(0x60000000),
            NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmCached));

        /* Adjacent after the first range ends is not an overlap. */
        CHECK(rig.map(other, 0x14000) == kIOReturnSuccess);
        CHECK(rig.table.mapped(other));
    }

    /* A range that starts in free space but runs into an existing mapping is
     * refused before its first page is written, not after a partial map and a
     * rollback. */
    {
        const uint64_t third = rig.alloc();
        const size_t writes = rig.registers.writes.size();
        CHECK(rig.map(third, 0x0F000) == kIOReturnBadArgument);
        CHECK(rig.registers.writes.size() == writes);
        CHECK(!rig.table.mapped(third));
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x0F000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);
        /* The retry at a free address succeeds. */
        CHECK(rig.map(third, 0x18000) == kIOReturnSuccess);
    }

    /* Free the mapped object: the leaves are cleared, the range is flushed once
     * (panfrost_mmu_unmap(), panfrost_mmu.c:500-524) and only then are the
     * pages given back. */
    {
        const size_t writes = rig.registers.writes.size();
        NativeMaliAbiBoFree request{handle};
        CHECK(rig.table.freeBo(request) == kIOReturnSuccess);
        CHECK(!rig.table.mapped(handle));
        CHECK(rig.table.memory(handle) == nullptr);
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x10000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);
        checkFlush(rig.registers, writes, rig.as, 0x10000, 0x4000);
    }
}

void testFreeAndRetry()
{
    Rig rig;
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);

    /* Unknown and zero handles are refused by the ABI check. */
    {
        NativeMaliAbiBoFree zero{0};
        CHECK(rig.table.freeBo(zero) == kIOReturnBadArgument);
        NativeMaliAbiBoFree unknown{0x777};
        CHECK(rig.table.freeBo(unknown) == kIOReturnBadArgument);
    }

    /* An unmapped object is released without a flush. */
    {
        const uint64_t handle = rig.alloc();
        const size_t writes = rig.registers.writes.size();
        NativeMaliAbiBoFree request{handle};
        CHECK(rig.table.freeBo(request) == kIOReturnSuccess);
        CHECK(rig.registers.writes.size() == writes);
        CHECK(rig.source.outstanding() == 0);
    }

    /* An unmap that fails on its flush leaves the object live and mapped, so
     * the retry repeats only the flush and the pages survive until it works. */
    {
        const uint64_t handle = rig.alloc();
        CHECK(rig.map(handle, 0x30000) == kIOReturnSuccess);
        const size_t writes = rig.registers.writes.size();
        rig.registers.failWriteAt = AS_COMMAND(rig.as);
        const int releases = rig.source.releases;
        NativeMaliAbiBoFree request{handle};
        CHECK(rig.table.freeBo(request) == kIOReturnIOError);
        CHECK(rig.table.live() == 1);
        CHECK(rig.table.mapped(handle));
        CHECK(rig.source.releases == releases);
        CHECK(rig.registers.writes.size() > writes);
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x30000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);   /* the leaves are already clear */

        rig.registers.failWriteAt = -1;
        CHECK(rig.table.freeBo(request) == kIOReturnSuccess);
        CHECK(rig.table.live() == 0);
        CHECK(rig.source.outstanding() == 0);
    }

    /* A BO the caller still has mapped cannot be freed: the wrapper would keep
     * a mapping of pages the source has taken back. */
    {
        const uint64_t handle = rig.alloc();
        NativeMaliAbiBoCpuMap map{handle};
        NativeMaliAbiBoCpuMapReply reply{};
        CHECK(rig.table.cpuMap(map, &reply) == kIOReturnSuccess);
        const int releases = rig.source.releases;
        NativeMaliAbiBoFree request{handle};
        CHECK(rig.table.freeBo(request) == kIOReturnBusy);
        CHECK(rig.table.live() == 1);
        CHECK(rig.source.releases == releases);
        CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, reply.token, 0})
            == kIOReturnSuccess);
        CHECK(rig.table.freeBo(request) == kIOReturnSuccess);
    }
}

void testCpuMap()
{
    Rig rig;
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);
    const uint64_t handle = rig.alloc();

    /* The ABI refuses a zero handle and reserved bytes; the table refuses an
     * unknown one. */
    CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{0}, nullptr) ==
        kIOReturnBadArgument);
    {
        NativeMaliAbiBoCpuMapReply reply{0xdead, 0xdead, 0xdead};
        CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{0}, &reply) ==
            kIOReturnBadArgument);
        CHECK(reply.token == 0 && reply.bytes == 0);
        CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{0x999}, &reply) ==
            kIOReturnBadArgument);
        CHECK(reply.token == 0 && reply.bytes == 0);
    }

    NativeMaliAbiBoCpuMapReply reply{};
    CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{handle}, &reply) ==
        kIOReturnSuccess);
    CHECK(reply.token != 0);
    CHECK(reply.bytes == 0x4000);
    CHECK(rig.table.memoryForToken(reply.token) == rig.table.memory(handle));
    CHECK(rig.table.memoryForToken(reply.token)->bytes() == 0x4000);
    CHECK(rig.table.memoryForToken(0) == nullptr);
    CHECK(rig.table.memoryForToken(reply.token + 1) == nullptr);

    /* Two CPU mappings of one object would need two owners to unmap; the
     * second is refused rather than sharing the token. */
    {
        NativeMaliAbiBoCpuMapReply second{0xdead, 0xdead, 0xdead};
        CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{handle}, &second) ==
            kIOReturnBusy);
        CHECK(second.token == 0 && second.bytes == 0);
    }

    /* The token must be exactly the one handed out. */
    CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, reply.token + 1,
        0}) == kIOReturnBadArgument);
    CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, 0, 0}) ==
        kIOReturnBadArgument);
    CHECK(rig.table.memoryForToken(reply.token) != nullptr);
    CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, reply.token, 1}) ==
        kIOReturnBadArgument);
    CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, reply.token, 0}) ==
        kIOReturnSuccess);
    CHECK(rig.table.memoryForToken(reply.token) == nullptr);
    /* A second unmap has nothing to give back. */
    CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, reply.token, 0}) ==
        kIOReturnBadArgument);

    /* A stale token cannot name a later object: the handle must match too. */
    {
        NativeMaliAbiBoFree release{handle};
        CHECK(rig.table.freeBo(release) == kIOReturnSuccess);
        const uint64_t next = rig.alloc();
        NativeMaliAbiBoCpuMapReply fresh{};
        CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{next}, &fresh) ==
            kIOReturnSuccess);
        CHECK(rig.table.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, fresh.token, 0})
            == kIOReturnBadArgument);
    }
}

void testTeardown()
{
    Rig rig;
    CHECK(rig.open(UINT64_C(0x100000)) == kIOReturnSuccess);

    /* releaseAll drops the tokens and retires mapped objects. */
    {
        const uint64_t unmapped = rig.alloc();
        const uint64_t mapped = rig.alloc();
        CHECK(rig.map(mapped, 0x50000) == kIOReturnSuccess);
        NativeMaliAbiBoCpuMapReply cpu{};
        CHECK(rig.table.cpuMap(NativeMaliAbiBoCpuMap{unmapped}, &cpu) ==
            kIOReturnSuccess);
        CHECK(rig.table.memoryForToken(cpu.token) != nullptr);

        CHECK(rig.table.releaseAll() == kIOReturnSuccess);
        CHECK(rig.table.live() == 0);
        CHECK(rig.source.outstanding() == 0);
        CHECK(rig.table.memoryForToken(cpu.token) == nullptr);
        CHECK(!rig.table.mapped(mapped));
        CHECK(rig.table.memory(unmapped) == nullptr);
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x50000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);
    }

    /* A failure stops the walk and leaves the rest live: nothing is forced. */
    {
        const uint64_t first = rig.alloc();
        const uint64_t second = rig.alloc();
        CHECK(rig.map(second, 0x60000) == kIOReturnSuccess);
        rig.registers.failWriteAt = AS_COMMAND(rig.as);
        CHECK(rig.table.releaseAll() == kIOReturnIOError);
        CHECK(rig.table.live() == 1);
        CHECK(rig.table.memory(first) == nullptr);
        CHECK(rig.table.memory(second) != nullptr);
        CHECK(rig.source.outstanding() == 1);
        rig.registers.failWriteAt = -1;
        CHECK(rig.table.releaseAll() == kIOReturnSuccess);
        CHECK(rig.table.live() == 0);
        CHECK(rig.source.outstanding() == 0);
    }

    /* reset() is for after the address space is gone: it gives the allocations
     * back without touching a register, even for an object still mapped. */
    {
        const uint64_t handle = rig.alloc();
        CHECK(rig.map(handle, 0x70000) == kIOReturnSuccess);
        const size_t writes = rig.registers.writes.size();
        rig.table.reset();
        CHECK(!rig.table.open());
        CHECK(rig.table.live() == 0);
        CHECK(rig.source.outstanding() == 0);
        CHECK(rig.registers.writes.size() == writes);
        uint64_t leaf = 0;
        CHECK(rig.vm.descriptor(0x70000, &leaf) == kIOReturnSuccess);
        CHECK(leaf != 0);   /* the address space was not this object's to tear down */
        NativeMaliAbiBoAlloc request{0x4000};
        NativeMaliAbiBoAllocReply reply{};
        CHECK(rig.table.alloc(request, &reply) == kIOReturnNotReady);
    }
}

} // namespace

int main()
{
    testOpen();
    testAlloc();
    testMap();
    testFreeAndRetry();
    testCpuMap();
    testTeardown();
    std::puts("PASS: BO table open/alloc/map/free/cpuMap/cpuUnmap against the real "
        "page tables and MMU flush, with monotonic handles, one address space, "
        "overlap refusal, unmap-then-release with retry, CPU tokens that keep an "
        "object alive, table-full and teardown rollback");
    return 0;
}
