/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliBringup: the M1-M4 order and its rollback, end to end.
 *
 * Every block here is the same fake the per-module tests use, wired to one
 * shared sequence counter, so this file can prove *cross-block* order - the
 * thing no single-module test can: that the MFG domains and the clock gate are
 * up before the first GPU write, that the GPU probe precedes any MMU
 * programming, and that the teardown runs the address space, then the GPU
 * blocks, then the domains.
 *
 * What this still cannot prove: any number on the real chip. The GPU feature
 * words are synthetic (tests/test_gpu_core.cpp explains why), and the reset and
 * power handshakes are the fakes' models of panfrost's expectations. */
#include "FakeMaliPlatform.h"
#include "NativeMaliBringup.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;

/* Real 4 KiB pages for the page tables, as tests/test_mmu.cpp uses. */
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
        std::memset(memory, 0, NativeMaliVmGranule);
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

/* The measured GPU id and synthetic feature words, same set as
 * tests/test_gpu_core.cpp: just enough for probe() to name a G52 r1p0 with
 * three address spaces and power-on masks that fit in the LO registers. */
void fillGpu(FakeRegisterBlock &gpu)
{
    gpu.memory[GPU_ID / 4] = 0x70021000;          /* G52 r1p0, measured */
    gpu.memory[GPU_L2_FEATURES / 4] = 0x01234567;
    gpu.memory[GPU_CORE_FEATURES / 4] = 0x89ABCDEF;
    gpu.memory[GPU_TILER_FEATURES / 4] = 0x0F0F0F0F;
    gpu.memory[GPU_MEM_FEATURES / 4] = 0x00000007;
    gpu.memory[GPU_MMU_FEATURES / 4] = (40u << 8) | 48u;   /* oas 40, ias 48 */
    gpu.memory[GPU_THREAD_FEATURES / 4] = 0x00000003;
    gpu.memory[GPU_THREAD_MAX_THREADS / 4] = 256;
    gpu.memory[GPU_THREAD_MAX_WORKGROUP_SIZE / 4] = 512;
    gpu.memory[GPU_THREAD_MAX_BARRIER_SIZE / 4] = 768;
    gpu.memory[GPU_AS_PRESENT / 4] = 0x7;         /* three address spaces */
    gpu.memory[GPU_JS_PRESENT / 4] = 0x5;
    gpu.memory[GPU_SHADER_PRESENT_LO / 4] = 0xF;
    gpu.memory[GPU_SHADER_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_TILER_PRESENT_LO / 4] = 0x3;
    gpu.memory[GPU_TILER_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_L2_PRESENT_LO / 4] = 0x1;
    gpu.memory[GPU_L2_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_STACK_PRESENT_LO / 4] = 0xF0;
    gpu.memory[GPU_STACK_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_THREAD_TLS_ALLOC / 4] = 64;
}

/* One device, starting from the state a bootloader leaves: every MFG domain
 * off, the mux on the wrong parent with its gate shut, the MFG clock gate shut,
 * both rails enabled at the vendor's lowest OPP. */
struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeGpu gpu;   /* the whole 0x13040000 window: GPU core *and* MMU */
    NativeMaliBringup bringup;
    uint32_t sequence = 0;

    Rig()
    {
        pwrap.setPmic(NativeMaliMt6358::VgpuEnable, 1);
        pwrap.setPmic(NativeMaliMt6358::VgpuVosel, 0);
        pwrap.setPmic(NativeMaliMt6358::VsramEnable, 1);
        pwrap.setPmic(NativeMaliMt6358::VsramVosel, 0);
        top.memory[NativeMaliPower::TopMfgClockConfig / 4] = 0x3 | 0x80;
        mfgcfg.memory[0] = NativeMaliPower::MfgClockGateBg3d;
        fillGpu(gpu);

        spm.sequence = &sequence;
        infra.sequence = &sequence;
        top.sequence = &sequence;
        mfgcfg.sequence = &sequence;
        pwrap.sequence = &sequence;
        gpu.sequence = &sequence;

        NativeMaliBringupBlocks blocks;
        blocks.power = {&spm, &infra, &top, &mfgcfg, &pwrap};
        blocks.gpu = &gpu;
        CHECK(bringup.prepare(blocks) == kIOReturnSuccess);
    }
    /* The GPU core, the job slots and the MMU share one 0x13040000 window, so
     * the fake GPU target is also the MMU target: every AS_* access lands in
     * its plain word bank and is recorded. tests/test_mmu.cpp is the file that
     * models AS_STATUS/JOB_INT behaviour; here only the order matters. */
    IOReturn start(NativeMaliBringupState *state,
        IOReturn *teardownError = nullptr)
    {
        return bringup.start(state, teardownError);
    }
};

/* Sequence number of the first write of `value` to `offset` in `block`, or 0. */
uint32_t seqOf(const FakeRegisterBlock &block, uint32_t offset, uint32_t value)
{
    for(const FakeRegisterBlock::Write &write : block.writes)
        if(write.offset == offset && write.value == value)return write.seq;
    return 0;
}

uint32_t firstSeq(const FakeRegisterBlock &block)
{
    return block.writes.empty() ? 0 : block.writes.front().seq;
}

/* How many times `offset` was written with `value`. */
size_t countWrites(const FakeRegisterBlock &block, uint32_t offset,
    uint32_t value)
{
    size_t count = 0;
    for(const FakeRegisterBlock::Write &write : block.writes)
        if(write.offset == offset && write.value == value)++count;
    return count;
}

/* The sequence number of the last such write. */
uint32_t lastSeqOf(const FakeRegisterBlock &block, uint32_t offset,
    uint32_t value)
{
    uint32_t seq = 0;
    for(const FakeRegisterBlock::Write &write : block.writes)
        if(write.offset == offset && write.value == value)seq = write.seq;
    return seq;
}

uint32_t lastSeq(const FakeRegisterBlock &block)
{
    return block.writes.empty() ? 0 : block.writes.back().seq;
}

void testPrepare()
{
    NativeMaliBringup bringup;
    NativeMaliBringupBlocks nothing;
    CHECK(bringup.prepare(nothing) == kIOReturnBadArgument);
    CHECK(!bringup.prepared());

    NativeMaliBringupState state;
    CHECK(bringup.start(&state) == kIOReturnNotReady);

    Rig rig;
    CHECK(rig.bringup.prepared());
    CHECK(!rig.bringup.started());
    CHECK(rig.bringup.mmu().allocatedMask() == 0);
}

/* The whole point of this file: the order, across every block. */
void testStartOrder()
{
    Rig rig;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnSuccess);

    /* M2 before M3: the mux gate opens, then the MFG clock gate, and only then
     * does the first GPU write happen. (The probe's reads are gated by
     * gpuReadable(), so a wrong order would have failed start() anyway.) */
    const uint32_t mux = seqOf(rig.top, NativeMaliPower::TopMfgClockConfigClear,
        NativeMaliPower::TopMfgMuxGate);
    const uint32_t gate = seqOf(rig.mfgcfg, NativeMaliPower::MfgClockGateClear,
        NativeMaliPower::MfgClockGateBg3d);
    const uint32_t firstGpuWrite = firstSeq(rig.gpu);
    CHECK(mux != 0 && gate != 0 && firstGpuWrite != 0);
    CHECK(mux < gate);
    CHECK(gate < firstGpuWrite);

    /* M2 in the vendor's order: buck (PMIC, no bus write), then the mux parent,
     * then MTCMOS, then the clock gate. The domains have to be up before the
     * gate opens, and the whole block before the GPU is written. */
    uint32_t firstDomain = 0;
    for(const FakeRegisterBlock::Write &write : rig.spm.writes)
        if((write.value & kPowerOn) == kPowerOn) {
            firstDomain = write.seq;
            break;
        }
    CHECK(firstDomain != 0);
    CHECK(mux < firstDomain);
    CHECK(firstDomain < gate);

    /* M3: the first GPU write is the interrupt mask reset from
     * panfrost_gpu_soft_reset(); power-on comes after the reset, L2 -> shader
     * -> tiler. */
    CHECK(rig.gpu.writes.front().offset == GPU_INT_MASK);
    CHECK(rig.gpu.writes.front().value == 0);
    const uint32_t reset = seqOf(rig.gpu, GPU_CMD, 1);   /* GPU_CMD_SOFT_RESET */
    const uint32_t l2 = seqOf(rig.gpu, L2_PWRON_LO, 0x1);
    const uint32_t shader = seqOf(rig.gpu, SHADER_PWRON_LO, 0xF);
    const uint32_t tiler = seqOf(rig.gpu, TILER_PWRON_LO, 0x3);
    CHECK(reset != 0 && l2 != 0 && shader != 0 && tiler != 0);
    CHECK(reset < l2);
    CHECK(l2 < shader);
    CHECK(shader < tiler);

    /* M4 records the slot mask and forgets stale allocations; it programs no
     * address space, because the page tables and their owner are not here yet.
     * No AS register in the window may be written. */
    for(const FakeRegisterBlock::Write &write : rig.gpu.writes)
        CHECK(write.offset < MMU_BASE || write.offset >= MMU_BASE + 0x400);
    CHECK(rig.bringup.mmu().allocatedMask() == 0);
    CHECK(rig.bringup.mmu().present() == 0x7);

    /* The state the caller gets back is the two probes. */
    CHECK(state.gpu.identity.model != nullptr);
    CHECK(std::strcmp(state.gpu.identity.model, "g52") == 0);
    CHECK(state.gpu.identity.matched == 0x7002);
    CHECK(state.gpu.gpuId == 0x70021000);
    CHECK(state.gpu.asPresent == 0x7);
    CHECK(state.gpu.ias == 48 && state.gpu.oas == 40);
    /* The M1 probe runs *before* the writes, so these are the bootloader's
     * values (selector 0), not the fixed OPP bringUp() then programs. */
    CHECK(state.power.vgpu.microvolts == 500000);
    CHECK(state.power.vsram.microvolts == 500000);
    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VgpuVosel) == 18);
    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VsramVosel) == 56);
    CHECK(rig.bringup.started());
    CHECK(rig.bringup.observation().gpuId == 0x70021000);

    /* Everything the GPU window needs is up. */
    uint32_t missing = 0xFFFF;
    CHECK(rig.bringup.power().gpuReadable(&missing) == kIOReturnSuccess);
    CHECK(missing == 0);

    /* A second start() while the block is up is a refusal, not a second
     * bring-up. */
    CHECK(rig.start(nullptr) == kIOReturnBusy);
}

/* The owner of the page tables takes an AS from the MMU and programs it; the
 * bring-up object neither allocates one nor tracks one. */
void testAddressSpaceLifecycle()
{
    Rig rig;
    TestPages pages;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnSuccess);

    uint32_t as = 0xFFFFFFFF;
    CHECK(rig.bringup.mmu().allocate(&as) == kIOReturnSuccess);
    CHECK(as == 0);                       /* ffz(as_alloc_mask) */

    NativeMaliVm vm;
    CHECK(vm.prepare(&pages, state.gpu.ias, state.gpu.oas, false) ==
        kIOReturnSuccess);

    CHECK(rig.bringup.mmu().enable(as, vm) == kIOReturnSuccess);

    /* panfrost_mmu_enable(): the AS registers were programmed, TRANSTAB before
     * the UPDATE that broadcasts it. */
    const uint32_t transtab = seqOf(rig.gpu, AS_TRANSTAB_LO(0),
        static_cast<uint32_t>(vm.transtab() & 0xFFFFFFFFu));
    const uint32_t update = seqOf(rig.gpu, AS_COMMAND(0), AS_COMMAND_UPDATE);
    CHECK(transtab != 0);
    CHECK(update != 0);
    CHECK(transtab < update);

    /* Asking for a second slot takes the next free bit, not the same one. */
    uint32_t second = 0xFFFFFFFF;
    CHECK(rig.bringup.mmu().allocate(&second) == kIOReturnSuccess);
    CHECK(second == 1);
    CHECK(rig.bringup.mmu().allocatedMask() == 0x3);
    CHECK(rig.bringup.mmu().release(second) == kIOReturnSuccess);

    /* While the AS is allocated, the block does not go down. */
    const size_t gpuWrites = rig.gpu.writes.size();
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnBusy);
    CHECK(rig.gpu.writes.size() == gpuWrites);
    CHECK(rig.bringup.started());

    /* A disabled but still allocated slot is still the owner's to give back. */
    CHECK(rig.bringup.mmu().disable(as) == kIOReturnSuccess);
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnBusy);
    CHECK(rig.bringup.mmu().release(as) == kIOReturnSuccess);

    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);
    CHECK(!rig.bringup.started());
}

/* MMU refusals: a VM that was never prepared, a slot that was never taken, and
 * allocation before start() recorded GPU_AS_PRESENT. None of them leaves a
 * half-programmed AS behind. */
void testAddressSpaceRefusals()
{
    Rig rig;
    NativeMaliBringupState state;
    /* `pages` outlives `vm`: the VM releases its tables back into it. */
    TestPages pages;
    CHECK(rig.start(&state) == kIOReturnSuccess);

    NativeMaliVm vm;
    uint32_t as = 0;
    CHECK(rig.bringup.mmu().allocate(&as) == kIOReturnSuccess);
    const size_t writes = rig.gpu.writes.size();
    CHECK(rig.bringup.mmu().enable(as, vm) == kIOReturnNotReady);
    CHECK(rig.gpu.writes.size() == writes);

    /* Slot 1 was never allocated. */
    CHECK(vm.prepare(&pages, state.gpu.ias, state.gpu.oas, false) ==
        kIOReturnSuccess);
    CHECK(rig.bringup.mmu().enable(1, vm) == kIOReturnBadArgument);
    CHECK(rig.bringup.mmu().release(as) == kIOReturnSuccess);

    /* Before start() there is no present mask to allocate from. */
    Rig fresh;
    uint32_t neither = 0;
    CHECK(fresh.bringup.mmu().allocate(&neither) == kIOReturnNotReady);
}

/* Teardown order, the reverse of panfrost_device_fini(): the AS is disabled by
 * its owner, then the GPU blocks go off, then the MFG domains (gate before
 * domains inside M2). */
void testStopOrder()
{
    Rig rig;
    TestPages pages;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnSuccess);
    uint32_t as = 0;
    CHECK(rig.bringup.mmu().allocate(&as) == kIOReturnSuccess);
    NativeMaliVm vm;
    CHECK(vm.prepare(&pages, state.gpu.ias, state.gpu.oas, false) ==
        kIOReturnSuccess);
    CHECK(rig.bringup.mmu().enable(as, vm) == kIOReturnSuccess);

    const size_t gpuWrites = rig.gpu.writes.size();
    CHECK(countWrites(rig.gpu, AS_COMMAND(0), AS_COMMAND_UPDATE) == 1);
    CHECK(rig.bringup.mmu().disable(as) == kIOReturnSuccess);
    CHECK(rig.bringup.mmu().release(as) == kIOReturnSuccess);
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);

    /* The AS was disabled, not just released: a second UPDATE was broadcast... */
    CHECK(countWrites(rig.gpu, AS_COMMAND(0), AS_COMMAND_UPDATE) == 2);
    CHECK(countWrites(rig.gpu, AS_TRANSTAB_LO(0), 0) >= 1);
    const uint32_t lastMmu = lastSeqOf(rig.gpu, AS_COMMAND(0),
        AS_COMMAND_UPDATE);
    /* ...before the GPU blocks were powered off... */
    const uint32_t firstOff = seqOf(rig.gpu, SHADER_PWROFF_LO, 0xF);
    CHECK(firstOff != 0);
    CHECK(lastMmu < firstOff);
    /* ...which happened before the clock gate closed... */
    const uint32_t gateClose = seqOf(rig.mfgcfg, NativeMaliPower::MfgClockGateSet,
        NativeMaliPower::MfgClockGateBg3d);
    CHECK(gateClose != 0);
    CHECK(rig.gpu.writes.size() > gpuWrites);
    CHECK(lastSeq(rig.gpu) < gateClose);
    /* ...and the gate closed before the first domain went away. */
    uint32_t firstDown = 0;
    for(const FakeRegisterBlock::Write &write : rig.spm.writes)
        if((write.value & kPowerOn) == 0) {
            firstDown = write.seq;
            break;
        }
    CHECK(firstDown != 0);
    CHECK(gateClose < firstDown);

    CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
    CHECK(!rig.bringup.started());
    CHECK(rig.bringup.mmu().allocatedMask() == 0);
    CHECK(rig.bringup.mmu().present() == 0x7);

    /* Idempotent: nothing is up, so nothing is written and nothing fails. */
    const size_t spmWrites = rig.spm.writes.size();
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);
    CHECK(rig.spm.writes.size() == spmWrites);
}

/* panfrost_device_runtime_suspend() refuses to suspend with a job in flight. */
void testStopRefusesOutstandingJobs()
{
    Rig rig;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnSuccess);
    const size_t gpuWrites = rig.gpu.writes.size();
    const size_t spmWrites = rig.spm.writes.size();
    IOReturn teardown = kIOReturnSuccess;
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Outstanding, &teardown) ==
        kIOReturnBusy);
    CHECK(teardown == kIOReturnSuccess);
    CHECK(rig.gpu.writes.size() == gpuWrites);
    CHECK(rig.spm.writes.size() == spmWrites);
    CHECK(rig.bringup.started());
}

/* An unknown GPU id is reported as Unsupported with the identity fields filled,
 * and the block is powered back down: a GPU this driver does not know must not
 * be left up. */
void testUnknownGpuRollsBack()
{
    Rig rig;
    rig.gpu.memory[GPU_ID / 4] = 0x99990000;
    NativeMaliBringupState state;
    IOReturn teardown = kIOReturnIOError;
    CHECK(rig.start(&state, &teardown) == kIOReturnUnsupported);
    CHECK(state.gpu.gpuId == 0x99990000);
    CHECK(state.power.vgpu.microvolts == 500000);   /* the M1 probe did run */
    CHECK(teardown == kIOReturnSuccess);

    CHECK(!rig.bringup.started());
    CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
    const uint32_t controls[] = {
        NativeMaliPower::SpmMfgAsyncPowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgCore1PowerCon,
    };
    for(uint32_t control : controls)
        CHECK((rig.spm.memory[control / 4] & kPowerOn) == 0);
}

/* The M1 probe is a precondition, not a diagnostic: if the SPM/PMIC reads fail,
 * not one configuration register is written. */
void testProbeFailureWritesNothing()
{
    Rig rig;
    rig.spm.failReadAt = NativeMaliPower::SpmPowerStatus;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnIOError);
    CHECK(rig.spm.writes.empty());
    CHECK(rig.infra.writes.empty());
    CHECK(rig.top.writes.empty());
    CHECK(rig.mfgcfg.writes.empty());
    CHECK(rig.pwrap.writes.empty());
    CHECK(rig.gpu.writes.empty());
    CHECK(!rig.bringup.started());
}

void testResetAndPowerOnRollBack()
{
    {
        /* Neither reset completes: softReset() falls back to a hard reset and
         * then gives up, and the M2 block still comes back down. */
        Rig rig;
        rig.gpu.softCompletes = false;
        rig.gpu.hardCompletes = false;
        NativeMaliBringupState state;
        IOReturn teardown = kIOReturnIOError;
        CHECK(rig.start(&state, &teardown) == kIOReturnTimeout);
        CHECK(teardown == kIOReturnSuccess);
        CHECK(!rig.bringup.started());
        CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
        CHECK((rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] &
            kPowerOn) == 0);
    }
    {
        /* Reset fine, L2 never reports ready: powerOn() quiesces itself and
         * start() still takes the domains down. */
        Rig rig;
        rig.gpu.l2Completes = false;
        NativeMaliBringupState state;
        CHECK(rig.start(&state) == kIOReturnTimeout);
        CHECK(!rig.bringup.started());
        CHECK((rig.spm.memory[NativeMaliPower::SpmMfgCore1PowerCon / 4] &
            kPowerOn) == 0);
        /* Every PWROFF was still attempted: shader and tiler were never on,
         * but the sequence does not skip them. */
        CHECK(rig.gpu.countWrites(SHADER_PWROFF_LO) >= 1);
        CHECK(rig.gpu.countWrites(TILER_PWROFF_LO) >= 1);
    }
}

/* GPU_AS_PRESENT == 0 means the block answered with "no address space", which
 * cannot be turned into a working AS. It is a refusal, not an allocation. */
void testNoAddressSpaceRollsBack()
{
    Rig rig;
    rig.gpu.memory[GPU_AS_PRESENT / 4] = 0;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnBadArgument);
    CHECK(!rig.bringup.started());
    CHECK((rig.spm.memory[NativeMaliPower::SpmMfgAsyncPowerCon / 4] &
        kPowerOn) == 0);
}

/* A slot whose disable failed is *not* "free": while the GPU is still powered
 * it could be walking those tables. stop() refuses with nothing written until
 * the owner gets the disable and the release through, and only then powers the
 * block down. That is what keeps a retry honest. */
void testStopRefusesAnAddressSpaceThatIsStillOwned()
{
    Rig rig;
    TestPages pages;
    NativeMaliBringupState state;
    CHECK(rig.start(&state) == kIOReturnSuccess);
    uint32_t as = 0;
    CHECK(rig.bringup.mmu().allocate(&as) == kIOReturnSuccess);
    NativeMaliVm vm;
    CHECK(vm.prepare(&pages, state.gpu.ias, state.gpu.oas, false) ==
        kIOReturnSuccess);
    CHECK(rig.bringup.mmu().enable(as, vm) == kIOReturnSuccess);

    /* The disable itself fails. The slot stays ours and the block stays up. */
    rig.gpu.failWriteAt = AS_COMMAND(0);
    CHECK(rig.bringup.mmu().disable(as) != kIOReturnSuccess);
    const size_t gpuWrites = rig.gpu.writes.size();
    const size_t spmWrites = rig.spm.writes.size();
    IOReturn teardown = kIOReturnIOError;
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle, &teardown) ==
        kIOReturnBusy);
    CHECK(teardown == kIOReturnSuccess);      /* a refusal, not a rollback failure */
    CHECK(rig.gpu.writes.size() == gpuWrites);
    CHECK(rig.spm.writes.size() == spmWrites);
    CHECK(rig.bringup.started());
    CHECK(rig.bringup.mmu().allocatedMask() == 0x1);

    /* The retry, with the hardware cooperating, goes all the way down. */
    rig.gpu.failWriteAt = -1;
    CHECK(rig.bringup.mmu().disable(as) == kIOReturnSuccess);
    CHECK(rig.bringup.mmu().release(as) == kIOReturnSuccess);
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);
    CHECK(!rig.bringup.started());
    CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
    CHECK((rig.spm.memory[NativeMaliPower::SpmMfgCore1PowerCon / 4] &
        kPowerOn) == 0);

    /* And a fresh start comes up clean, with the bitmap reset by M4. */
    CHECK(rig.start(&state) == kIOReturnSuccess);
    CHECK(rig.bringup.mmu().allocatedMask() == 0);
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);
}

/* The original error is what start() returns, even when the rollback fails too;
 * the rollback's own error goes to the out parameter, so neither is lost. */
void testRollbackErrorIsReportedSeparately()
{
    Rig rig;
    rig.gpu.memory[GPU_ID / 4] = 0x99990000;      /* Unsupported after M2 */
    rig.mfgcfg.failWriteAt = NativeMaliPower::MfgClockGateSet;
    NativeMaliBringupState state;
    IOReturn teardown = kIOReturnSuccess;
    CHECK(rig.start(&state, &teardown) == kIOReturnUnsupported);
    CHECK(teardown == kIOReturnIOError);
    /* The failure left the block marked as up, so a retried stop() is the way
     * out and start() refuses in the meantime. */
    CHECK(rig.start(nullptr) == kIOReturnBusy);
    rig.mfgcfg.failWriteAt = -1;
    CHECK(rig.bringup.stop(NativeMaliBringup::Jobs::Idle) == kIOReturnSuccess);
    CHECK(!rig.bringup.started());
}

} // namespace

int main()
{
    testPrepare();
    testStartOrder();
    testAddressSpaceLifecycle();
    testAddressSpaceRefusals();
    testStopOrder();
    testStopRefusesOutstandingJobs();
    testStopRefusesAnAddressSpaceThatIsStillOwned();
    testUnknownGpuRollsBack();
    testProbeFailureWritesNothing();
    testResetAndPowerOnRollBack();
    testNoAddressSpaceRollsBack();
    testRollbackErrorIsReportedSeparately();
    std::puts("PASS: M1 read before write, M2->M3->M4 order across blocks, the AS staying with its owner (allocate, enable, disable, release), reverse teardown, refusal with jobs outstanding or an AS still owned, and rollback of every failure stage with the original error preserved");
    return 0;
}
