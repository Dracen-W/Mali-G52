/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliMmu against fake registers.
 *
 * The page tables are the real NativeMaliVm (already tested against
 * io-pgtable-arm.c), so the AS_TRANSTAB/MEMATTR values written here are the
 * same ones the GPU would walk. What is fake is only the MMU register bank:
 * AS_STATUS reports AS_ACTIVE for a configurable number of reads, everything
 * else is a word bank that records writes. No real hardware is touched.
 *
 * The expected write order comes from panfrost_mmu_enable() /
 * panfrost_mmu_disable() (panfrost_mmu.c:167-205): FLUSH_MEM over the whole
 * space writes AS_LOCKADDR then LOCK then FLUSH_MEM, then the three LO/HI
 * pairs, then UPDATE. AS_COMMAND is written only after AS_STATUS says the
 * previous command finished. */
#include "FakeMaliPlatform.h"
#include "NativeMaliMmu.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;

/* Real 4 KiB pages, so NativeMaliVm can build the root table. Physical
 * addresses start at 0x40000000, which gives the golden TRANSTAB below. */
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

void checkWrites(const FakeMmu &mmu, const FakeRegisterBlock::Write *expected,
    size_t count)
{
    CHECK(mmu.writes.size() == count);
    for(size_t i = 0; i < count; ++i) {
        CHECK(mmu.writes[i].offset == expected[i].offset);
        CHECK(mmu.writes[i].value == expected[i].value);
    }
}

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

    explicit Rig(uint32_t asPresent = 0x7)
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
        CHECK(mmu.setPresent(asPresent) == kIOReturnSuccess);
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
    }
};

void testPrepare()
{
    FakeMmu registers;
    NativeMaliPower lonely;
    NativeMaliMmu mmu;

    CHECK(mmu.prepare(nullptr, &lonely) == kIOReturnBadArgument);
    CHECK(mmu.prepare(&registers, nullptr) == kIOReturnBadArgument);
    CHECK(mmu.prepare(&registers, &lonely) == kIOReturnNotReady);
    CHECK(!mmu.prepared());
    uint32_t as = 0;
    CHECK(mmu.allocate(&as) == kIOReturnNotReady);
    CHECK(mmu.setPresent(0x7) == kIOReturnNotReady);

    Rig rig;
    CHECK(rig.mmu.prepared());
    CHECK(rig.mmu.prepare(&rig.registers, &rig.power) == kIOReturnSuccess);
    FakeMmu other;
    CHECK(rig.mmu.prepare(&other, &rig.power) == kIOReturnBusy);
    NativeMaliPower otherPower;
    CHECK(rig.mmu.prepare(&rig.registers, &otherPower) == kIOReturnBusy);

    CHECK(rig.mmu.present() == 0x7);
    CHECK(rig.mmu.setPresent(0x7) == kIOReturnSuccess);
    CHECK(rig.mmu.setPresent(0x5) == kIOReturnBusy);
}

void testPresent()
{
    NativeMaliMmu mmu;
    FakeMmu registers;
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    NativeMaliPower power;
    NativeMaliPowerBlocks blocks{&spm, &infra, &top, &mfgcfg, &pwrap};
    CHECK(power.prepare(blocks) == kIOReturnSuccess);
    CHECK(mmu.prepare(&registers, &power) == kIOReturnSuccess);
    /* Zero means "the probe has not told us yet", not "no address spaces". */
    CHECK(mmu.setPresent(0) == kIOReturnBadArgument);
    CHECK(mmu.allocate(nullptr) == kIOReturnBadArgument);
    CHECK(mmu.present() == 0);
}

void testAllocate()
{
    Rig rig(0x5);   /* slots 0 and 2 exist, slot 1 does not */
    uint32_t as = 99;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(as == 0);
    CHECK(rig.mmu.isAllocated(0));
    CHECK(rig.mmu.allocatedMask() == 0x1);
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(as == 2);
    CHECK(rig.mmu.allocatedMask() == 0x5);
    CHECK(rig.mmu.allocate(&as) == kIOReturnNoResources);

    CHECK(rig.mmu.release(1) == kIOReturnBadArgument);   /* not allocated */
    CHECK(rig.mmu.release(0) == kIOReturnSuccess);
    CHECK(!rig.mmu.isAllocated(0));
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(as == 0);
    CHECK(rig.mmu.release(32) == kIOReturnBadArgument);
    CHECK(rig.mmu.release(0) == kIOReturnSuccess);
    CHECK(rig.mmu.release(0) == kIOReturnBadArgument);   /* double release */
}

/* panfrost_mmu_reset() (panfrost_mmu.c:345-365) after a GPU reset: the AS bits
 * the hardware had are gone, so the bookkeeping is cleared but GPU_AS_PRESENT
 * survives (it is a hardware fact, not an allocation). */
void testResetAllocation()
{
    Rig rig(0x5);
    uint32_t as = 99;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(rig.mmu.allocatedMask() == 0x5);

    rig.mmu.resetAllocation();
    CHECK(rig.mmu.allocatedMask() == 0);
    CHECK(!rig.mmu.isAllocated(0));
    CHECK(rig.mmu.present() == 0x5);
    CHECK(rig.mmu.release(0) == kIOReturnBadArgument);   /* already forgotten */

    /* The lowest present slot comes back first, exactly as before the reset. */
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(as == 0);

    /* A reset touches no register, so it works with no slot allocated, with no
     * present mask and even with writes compiled out. */
    FakeMmu registers;
    NativeMaliMmu mmu;
    mmu.resetAllocation();
    CHECK(registers.writes.empty());
    CHECK(registers.readCount == 0);
}

/* NativeMaliMmu::reclaim(): a reset forgot who owned each number, but the
 * owner did not go away. Putting the same bit back is pure bookkeeping and must
 * never be a way to take a slot somebody else still holds. */
void testReclaim()
{
    Rig rig(0x5);
    uint32_t as = 99;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(rig.mmu.allocatedMask() == 0x5);
    const size_t writes = rig.registers.writes.size();

    /* Still owned: two owners for one number is the bug this refusal prevents. */
    CHECK(rig.mmu.reclaim(0) == kIOReturnBusy);
    CHECK(rig.mmu.reclaim(2) == kIOReturnBusy);
    CHECK(rig.mmu.allocatedMask() == 0x5);

    /* A slot that does not exist, or one past the bitmap, is not a number this
     * MMU could ever have handed out. */
    CHECK(rig.mmu.reclaim(1) == kIOReturnBadArgument);
    CHECK(rig.mmu.reclaim(3) == kIOReturnBadArgument);
    CHECK(rig.mmu.reclaim(32) == kIOReturnBadArgument);

    rig.mmu.resetAllocation();
    CHECK(rig.mmu.allocatedMask() == 0);
    CHECK(rig.mmu.reclaim(0) == kIOReturnSuccess);
    CHECK(rig.mmu.reclaim(2) == kIOReturnSuccess);
    CHECK(rig.mmu.allocatedMask() == 0x5);
    CHECK(rig.mmu.allocate(&as) == kIOReturnNoResources);

    /* Pure bookkeeping: no register was touched by any of it. */
    CHECK(rig.registers.writes.size() == writes);
    CHECK(rig.registers.readCount == 0);
}

void testLockRegionValue()
{    uint64_t region = 0;
    /* The whole space: fls64(0 ^ (~0 - 1)) = 64, width 63, base 0. */
    CHECK(NativeMaliMmu::lockRegionValue(0, ~UINT64_C(0), &region));
    CHECK(region == 63);
    /* Anything below the 32 KiB minimum still locks a 32 KiB block at 0. */
    CHECK(NativeMaliMmu::lockRegionValue(0, 1, &region));
    CHECK(region == 14);
    CHECK(NativeMaliMmu::lockRegionValue(0, 0x1000, &region));
    CHECK(region == 14);
    CHECK(NativeMaliMmu::lockRegionValue(0x2000, 0x2000, &region));
    CHECK(region == 14);   /* rounds down to 0, 32 KiB */
    /* Naturally aligned power-of-two blocks: width is log2(size) - 1 and the
     * base keeps the high bits. */
    CHECK(NativeMaliMmu::lockRegionValue(0x10000, 0x10000, &region));
    CHECK(region == 0x1000f);
    CHECK(NativeMaliMmu::lockRegionValue(0x20000, 0x20000, &region));
    CHECK(region == 0x20010);
    /* The source writes nothing for size 0; a wrapped range is refused rather
     * than encoded on top of the wrap. */
    CHECK(!NativeMaliMmu::lockRegionValue(0, 0, &region));
    CHECK(!NativeMaliMmu::lockRegionValue(~UINT64_C(0) - 1, 0x10, &region));
    CHECK(!NativeMaliMmu::lockRegionValue(0, 1, nullptr));
}

void testEnable()
{
    Rig rig;
    uint32_t as = 0;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(NativeMaliMmu::kWriteAuthorized);
    CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnSuccess);

    const uint64_t transtab = UINT64_C(0x40000000) | 3 | 4;
    CHECK(rig.vm.transtab() == transtab);
    CHECK(rig.vm.memattr() == UINT64_C(0x00888d88));
    const FakeRegisterBlock::Write expected[] = {
        {AS_LOCKADDR_LO(0), 63},
        {AS_LOCKADDR_HI(0), 0},
        {AS_COMMAND(0), AS_COMMAND_LOCK},
        {AS_COMMAND(0), AS_COMMAND_FLUSH_MEM},
        {AS_TRANSTAB_LO(0), 0x40000007},
        {AS_TRANSTAB_HI(0), 0},
        {AS_MEMATTR_LO(0), 0x00888d88},
        {AS_MEMATTR_HI(0), 0},
        {AS_TRANSCFG_LO(0), AS_TRANSCFG_ADRMODE_LEGACY},
        {AS_TRANSCFG_HI(0), 0},
        {AS_COMMAND(0), AS_COMMAND_UPDATE},
    };
    checkWrites(rig.registers, expected, sizeof(expected) / sizeof(expected[0]));
    /* The FLUSH_MEM operation waits before LOCK, before FLUSH_MEM and again
     * for the flush to finish; UPDATE waits once more. */
    CHECK(rig.registers.statusReads == 4);
    CHECK(rig.registers.commandWrites == 3);
    CHECK(rig.registers.delays.empty());

    /* The last state is what a table walk would use, and the slot stays
     * allocated after enable. */
    CHECK(rig.registers.memory[AS_TRANSTAB_LO(0) / 4] == 0x40000007);
    CHECK(rig.registers.memory[AS_MEMATTR_LO(0) / 4] == 0x00888d88);
    CHECK(rig.registers.memory[AS_TRANSCFG_LO(0) / 4] ==
        AS_TRANSCFG_ADRMODE_LEGACY);
    CHECK(rig.mmu.isAllocated(as));
    CHECK(rig.mmu.release(as) == kIOReturnSuccess);
}

void testEnableWaitsForStatus()
{
    Rig rig;
    uint32_t as = 0;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    rig.registers.activeReads = 2;
    CHECK(rig.mmu.enable(as, rig.vm, 1000) == kIOReturnSuccess);
    /* Two extra reads on the first wait_ready, then the other three waits. */
    CHECK(rig.registers.statusReads == 6);
    CHECK(rig.registers.delays.size() == 2);
    for(uint64_t delay : rig.registers.delays)
        CHECK(delay == NativeMaliMmu::ReadyPollUs);
}

void testEnableRefusals()
{
    {
        /* AS_ACTIVE never clears: the LOCKADDR has been written, but no
         * command reaches the AS. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.activeReads = 0xFFFFFFFF;
        CHECK(rig.mmu.enable(as, rig.vm, 0) == kIOReturnTimeout);
        CHECK(rig.registers.writes.size() == 2);
        CHECK(rig.registers.writes[0].offset == AS_LOCKADDR_LO(as));
        CHECK(rig.registers.writes[1].offset == AS_LOCKADDR_HI(as));
        CHECK(rig.registers.statusReads == 1);
    }
    {
        Rig rig;
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
        CHECK(rig.registers.statusReads == 0);
    }
    {
        Rig rig;
        rig.mfgcfg.memory[0] = NativeMaliPower::MfgClockGateBg3d;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
    }
    {
        /* Present but never allocated. */
        Rig rig;
        CHECK(rig.mmu.enable(1, rig.vm) == kIOReturnBadArgument);
        CHECK(rig.registers.writes.empty());
        /* Allocated but not present on this GPU. */
        Rig small(0x5);
        uint32_t as = 0;
        CHECK(small.mmu.allocate(&as) == kIOReturnSuccess);
        CHECK(small.mmu.enable(1, small.vm) == kIOReturnBadArgument);
        CHECK(small.registers.writes.empty());
    }
    {
        /* The VM must have built its root table. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        NativeMaliVm empty;
        CHECK(rig.mmu.enable(as, empty) == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
        CHECK(rig.mmu.release(as) == kIOReturnSuccess);
    }
    {
        NativeMaliMmu cold;
        Rig rig;
        CHECK(cold.enable(0, rig.vm) == kIOReturnNotReady);
        CHECK(cold.disable(0) == kIOReturnNotReady);
    }
}

void testEnableFailure()
{
    {
        /* The first AS_COMMAND write fails; TRANSTAB must never be touched. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.failWriteAt = AS_COMMAND(0);
        CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnIOError);
        CHECK(rig.registers.memory[AS_TRANSTAB_LO(0) / 4] == 0);
        CHECK(rig.registers.commandWrites == 0);
    }
    {
        /* A configuration write fails after TRANSTAB landed: enable() disables
         * the AS best effort and still returns the original error. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.failWriteAt = AS_MEMATTR_HI(0);
        CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnIOError);
        CHECK(rig.registers.memory[AS_TRANSTAB_LO(0) / 4] == 0);
        CHECK(rig.registers.memory[AS_TRANSTAB_HI(0) / 4] == 0);
    }
    {
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.failReadAt = AS_STATUS(0);
        CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnIOError);
        CHECK(rig.registers.writes.size() == 2);
    }
    {
        /* A delay error is not a timeout and is returned unchanged. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.activeReads = 1;
        rig.registers.delayResult = kIOReturnNoResources;
        CHECK(rig.mmu.enable(as, rig.vm, 100) == kIOReturnNoResources);
    }
}

void testDisable()
{
    Rig rig;
    uint32_t as = 0;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    CHECK(rig.mmu.enable(as, rig.vm) == kIOReturnSuccess);
    rig.registers.writes.clear();
    rig.registers.statusReads = 0;
    rig.registers.commandWrites = 0;

    CHECK(rig.mmu.disable(as) == kIOReturnSuccess);
    const FakeRegisterBlock::Write expected[] = {
        {AS_LOCKADDR_LO(0), 63},
        {AS_LOCKADDR_HI(0), 0},
        {AS_COMMAND(0), AS_COMMAND_LOCK},
        {AS_COMMAND(0), AS_COMMAND_FLUSH_MEM},
        {AS_TRANSTAB_LO(0), 0},
        {AS_TRANSTAB_HI(0), 0},
        {AS_MEMATTR_LO(0), 0},
        {AS_MEMATTR_HI(0), 0},
        {AS_TRANSCFG_LO(0), AS_TRANSCFG_ADRMODE_UNMAPPED},
        {AS_TRANSCFG_HI(0), 0},
        {AS_COMMAND(0), AS_COMMAND_UPDATE},
    };
    checkWrites(rig.registers, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.registers.statusReads == 4);
    CHECK(rig.registers.commandWrites == 3);
    /* Disable does not free the slot; that is the caller's decision. */
    CHECK(rig.mmu.isAllocated(as));
    CHECK(rig.mmu.release(as) == kIOReturnSuccess);
}

void testDisableRefusals()
{
    {
        Rig rig(0x5);
        /* A present but unallocated slot may still be quiesced. */
        CHECK(rig.mmu.disable(2) == kIOReturnSuccess);
        CHECK(rig.registers.memory[AS_TRANSCFG_LO(2) / 4] ==
            AS_TRANSCFG_ADRMODE_UNMAPPED);
        /* A slot the GPU does not have is refused before any write. */
        rig.registers.writes.clear();
        CHECK(rig.mmu.disable(1) == kIOReturnBadArgument);
        CHECK(rig.registers.writes.empty());
        CHECK(rig.mmu.disable(32) == kIOReturnBadArgument);
    }
    {
        Rig rig;
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        CHECK(rig.mmu.disable(0) == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
    }
}

void testReadFault()
{
    Rig rig;
    const uint32_t status = (0x0000BEEFu << 16) | (1u << 10) | (2u << 8) | 0xC2;
    rig.registers.memory[AS_FAULTSTATUS(0) / 4] = status;
    rig.registers.memory[AS_FAULTADDRESS_LO(0) / 4] = 0x00001000;
    rig.registers.memory[AS_FAULTADDRESS_HI(0) / 4] = 0x00000003;
    rig.registers.readCount = 0;

    NativeMaliMmuFault fault;
    CHECK(rig.mmu.readFault(0, &fault) == kIOReturnSuccess);
    /* panfrost_mmu.c:897-899: status, then LO, then HI. */
    CHECK(rig.registers.readCount == 3);
    CHECK(fault.status == status);
    CHECK(fault.exception() == 0xC2);
    CHECK(fault.access() == 2);
    CHECK(fault.sourceId() == 0xBEEF);
    CHECK(fault.decoderFault());
    CHECK(fault.pageFault());
    CHECK(fault.address() == UINT64_C(0x300001000));
    /* Reporting a fault does not acknowledge it or write anything. */
    CHECK(rig.registers.writes.empty());

    /* Each AS has its own fault registers. */
    CHECK(rig.mmu.readFault(2, &fault) == kIOReturnSuccess);
    CHECK(fault.status == 0);
    CHECK(!fault.isFault());
    CHECK(fault.address() == 0);

    /* A slot the GPU does not have is never touched. */
    Rig small(0x5);
    small.registers.readCount = 0;
    CHECK(small.mmu.readFault(1, &fault) == kIOReturnBadArgument);
    CHECK(small.registers.readCount == 0);
    CHECK(small.mmu.readFault(32, &fault) == kIOReturnBadArgument);
    CHECK(small.registers.readCount == 0);
    CHECK(small.mmu.readFault(0, nullptr) == kIOReturnBadArgument);
    CHECK(small.registers.readCount == 0);
}

void testReadFaultRefusals()
{
    {
        /* prepare() alone is not enough: the present mask is still unknown. */
        Rig rig;
        NativeMaliMmu fresh;
        CHECK(fresh.prepare(&rig.registers, &rig.power) == kIOReturnSuccess);
        rig.registers.readCount = 0;
        NativeMaliMmuFault fault;
        CHECK(fresh.readFault(0, &fault) == kIOReturnNotReady);
        CHECK(rig.registers.readCount == 0);
    }
    {
        /* The M2 gate closes: zero reads. */
        Rig rig;
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        rig.registers.readCount = 0;
        NativeMaliMmuFault fault;
        CHECK(rig.mmu.readFault(0, &fault) == kIOReturnNotReady);
        CHECK(rig.registers.readCount == 0);
    }
    {
        NativeMaliMmu cold;
        NativeMaliMmuFault fault;
        CHECK(cold.readFault(0, &fault) == kIOReturnNotReady);
    }
    {
        /* A failed address read keeps the status that did arrive. */
        Rig rig;
        rig.registers.memory[AS_FAULTSTATUS(0) / 4] = 0x40;
        rig.registers.memory[AS_FAULTADDRESS_LO(0) / 4] = 0x1234;
        rig.registers.failReadAt = AS_FAULTADDRESS_HI(0);
        NativeMaliMmuFault fault;
        CHECK(rig.mmu.readFault(0, &fault) == kIOReturnIOError);
        CHECK(fault.status == 0x40);
        CHECK(fault.addressLo == 0x1234);
        CHECK(fault.addressHi == 0);
    }
}

void testFlushRange()
{
    Rig rig;
    uint32_t as = 0;
    CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
    rig.registers.writes.clear();
    rig.registers.statusReads = 0;
    rig.registers.commandWrites = 0;
    rig.registers.readCount = 0;

    /* A 128 KiB naturally aligned block: width 16, base 0x20000, so
     * AS_LOCKADDR = base | width = 0x20010. */
    CHECK(rig.mmu.flushRange(as, 0x20000, 0x20000) == kIOReturnSuccess);
    const FakeRegisterBlock::Write expected[] = {
        {AS_LOCKADDR_LO(0), 0x20010},
        {AS_LOCKADDR_HI(0), 0},
        {AS_COMMAND(0), AS_COMMAND_LOCK},
        {AS_COMMAND(0), AS_COMMAND_FLUSH_PT},
    };
    checkWrites(rig.registers, expected, sizeof(expected) / sizeof(expected[0]));
    /* wait_ready() before LOCK, before FLUSH_PT, and once for the flush. */
    CHECK(rig.registers.statusReads == 3);
    CHECK(rig.registers.commandWrites == 2);
    CHECK(rig.registers.delays.empty());

    /* A 4 KiB range still locks the minimum 32 KiB block at 0. */
    rig.registers.writes.clear();
    CHECK(rig.mmu.flushRange(as, 0x1000, 0x1000) == kIOReturnSuccess);
    CHECK(rig.registers.writes.size() == 4);
    CHECK(rig.registers.writes[0].offset == AS_LOCKADDR_LO(0));
    CHECK(rig.registers.writes[0].value == 14);
    CHECK(rig.registers.writes[2].value == AS_COMMAND_LOCK);
    CHECK(rig.registers.writes[3].value == AS_COMMAND_FLUSH_PT);

    /* The whole-space flush used by enable/disable is not reachable here:
     * `size` is the range the caller updated, and zero means nothing. */
    rig.registers.writes.clear();
    CHECK(rig.mmu.flushRange(as, 0, 0) == kIOReturnBadArgument);
    CHECK(rig.registers.writes.empty());
    CHECK(rig.mmu.flushRange(as, ~UINT64_C(0) - 1, 0x10) ==
        kIOReturnBadArgument);   /* wraps */
    CHECK(rig.registers.writes.empty());
}

void testFlushRangeRefusals()
{
    {
        /* Present but never allocated: enable() never programmed it. */
        Rig rig;
        rig.registers.writes.clear();
        CHECK(rig.mmu.flushRange(2, 0x1000, 0x1000) == kIOReturnBadArgument);
        CHECK(rig.registers.writes.empty());
        /* Allocated but not present on this GPU. */
        Rig small(0x5);
        uint32_t as = 0;
        CHECK(small.mmu.allocate(&as) == kIOReturnSuccess);
        small.registers.writes.clear();
        CHECK(small.mmu.flushRange(1, 0x1000, 0x1000) ==
            kIOReturnBadArgument);
        CHECK(small.registers.writes.empty());
        CHECK(small.mmu.flushRange(32, 0x1000, 0x1000) ==
            kIOReturnBadArgument);
        CHECK(small.registers.writes.empty());
    }
    {
        /* The M2 gate closes. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        rig.registers.readCount = 0;
        CHECK(rig.mmu.flushRange(as, 0x1000, 0x1000) == kIOReturnNotReady);
        CHECK(rig.registers.readCount == 0);
        CHECK(rig.registers.writes.empty());
    }
    {
        NativeMaliMmu cold;
        CHECK(cold.flushRange(0, 0x1000, 0x1000) == kIOReturnNotReady);
    }
    {
        /* AS_ACTIVE never clears: the range is locked but nothing is flushed. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.activeReads = 0xFFFFFFFF;
        CHECK(rig.mmu.flushRange(as, 0x1000, 0x1000, 0) == kIOReturnTimeout);
        CHECK(rig.registers.writes.size() == 2);
        CHECK(rig.registers.writes[0].offset == AS_LOCKADDR_LO(as));
        CHECK(rig.registers.writes[1].offset == AS_LOCKADDR_HI(as));
        CHECK(rig.registers.commandWrites == 0);
    }
    {
        /* A failed status read comes back unchanged. */
        Rig rig;
        uint32_t as = 0;
        CHECK(rig.mmu.allocate(&as) == kIOReturnSuccess);
        rig.registers.failReadAt = AS_STATUS(0);
        CHECK(rig.mmu.flushRange(as, 0x1000, 0x1000) == kIOReturnIOError);
        CHECK(rig.registers.writes.size() == 2);
    }
}

} // namespace

int main()
{
    testPrepare();
    testPresent();
    testAllocate();
    testResetAllocation();
    testReclaim();
    testLockRegionValue();
    testEnable();
    testEnableWaitsForStatus();
    testEnableRefusals();
    testEnableFailure();
    testDisable();
    testDisableRefusals();
    testReadFault();
    testReadFaultRefusals();
    testFlushRange();
    testFlushRangeRefusals();
    std::puts("PASS: MMU AS allocation, allocation reset and reclaim after a GPU reset, lock-region encoding, ready polling, enable/disable write order, AS fault reads, range FLUSH_PT, gate refusals and rollback");
    return 0;
}
