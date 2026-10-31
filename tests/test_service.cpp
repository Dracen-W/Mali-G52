/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliService: the device-wide owner of the address spaces, the shared
 * job slots and the reset sequence.
 *
 * Everything below the service is real except the register bank, the page and
 * buffer sources and the cache maintainer: NativeMaliBringup runs the actual
 * M1-M4 order, NativeMaliMmu programs real TRANSTAB values, NativeMaliVm builds
 * real LPAE tables and NativeMaliUserClient submits real chains.
 *
 * What this file pins down:
 *   - start() refuses before prepare(), runs bring-up once, records
 *     GPU_JS_PRESENT/GPU_SHADER_PRESENT, and refuses a second start;
 *   - open allocates the next free AS, prepares that client's tables, programs
 *     TRANSTAB, and registers the client; the number is the client's until it
 *     closes;
 *   - every failure after the AS was taken rolls back in the source's order
 *     (disable, give the slot back, release the tables) and returns the
 *     original error;
 *   - close releases the client, then disables and returns the AS, and a failed
 *     disable keeps the client registered so stop() keeps refusing;
 *   - stop is Busy while a client is open or a job is in flight, with zero
 *     writes;
 *   - reset is soft reset -> power on -> mmu_reset -> resetInterrupts ->
 *     abandonOutstanding -> noteReset -> reclaim+enable every client AS, and a
 *     failure anywhere makes clientUsable() false until stop()/start().
 */
#include "FakeMaliMemory.h"
#include "FakeMaliPlatform.h"
#include "NativeMaliAbiValidate.h"
#include "NativeMaliRegs.h"
#include "NativeMaliService.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

/* A page source that cannot hand out a table, to reach the rollback that has to
 * happen between "AS taken" and "TRANSTAB programmed". */
class FailingPages final : public NativeMaliPageSource {
public:
    IOReturn alloc(NativeMaliPage *) override { return kIOReturnNoMemory; }
    void release(const NativeMaliPage &) override {}
};

/* The same synthetic-but-measured GPU as tests/test_bringup.cpp: a G52 r1p0
 * with three address spaces and two of the three job slots present. */
void fillGpu(FakeRegisterBlock &gpu)
{
    gpu.memory[GPU_ID / 4] = 0x70021000;
    gpu.memory[GPU_L2_FEATURES / 4] = 0x01234567;
    gpu.memory[GPU_CORE_FEATURES / 4] = 0x89ABCDEF;
    gpu.memory[GPU_TILER_FEATURES / 4] = 0x0F0F0F0F;
    gpu.memory[GPU_MEM_FEATURES / 4] = 0x00000007;
    gpu.memory[GPU_MMU_FEATURES / 4] = (40u << 8) | 48u;
    gpu.memory[GPU_THREAD_FEATURES / 4] = 0x00000003;
    gpu.memory[GPU_THREAD_MAX_THREADS / 4] = 256;
    gpu.memory[GPU_THREAD_MAX_WORKGROUP_SIZE / 4] = 512;
    gpu.memory[GPU_THREAD_MAX_BARRIER_SIZE / 4] = 768;
    gpu.memory[GPU_AS_PRESENT / 4] = 0x7;
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
    gpu.memory[GPU_LATEST_FLUSH_ID / 4] = 0x2a;
}

/* A client on the heap: one NativeMaliUserClient is ~100 KiB of BO table, and a
 * rig holds several. The VMs live in the rig so their tables outlive the BO
 * tables but are released before the page source. */
struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeGpu gpu;
    NativeMaliService service;
    uint32_t sequence = 0;
    TestPages tables;
    FakeMemorySource buffers[4];
    FakeCache cache;
    NativeMaliVm vms[4];
    std::unique_ptr<NativeMaliUserClient> clients[4];

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
        CHECK(service.prepare(blocks) == kIOReturnSuccess);
        for(std::unique_ptr<NativeMaliUserClient> &client : clients)
            client = std::make_unique<NativeMaliUserClient>();
    }

    IOReturn start(NativeMaliBringupState *state = nullptr)
    {
        return service.start(state);
    }

    NativeMaliUserClient *open(size_t index, uint64_t maxBytes = 0,
        IOReturn *teardownError = nullptr)
    {
        CHECK(service.openClient(clients[index].get(), &vms[index], &tables,
            &buffers[index], &cache, false, maxBytes, teardownError) ==
            kIOReturnSuccess);
        return clients[index].get();
    }

    NativeMaliMmu &mmu() { return service.bringup().mmu(); }
};

uint32_t seqOf(const FakeRegisterBlock &block, uint32_t offset, uint32_t value)
{
    for(const FakeRegisterBlock::Write &write : block.writes)
        if(write.offset == offset && write.value == value)return write.seq;
    return 0;
}

/* The newest matching write: start() itself performs a soft reset, so "the
 * reset under test" is the last one, not the first. */
uint32_t lastSeqOf(const FakeRegisterBlock &block, uint32_t offset,
    uint32_t value)
{
    for(size_t index = block.writes.size(); index-- > 0;)
        if(block.writes[index].offset == offset &&
            block.writes[index].value == value)
            return block.writes[index].seq;
    return 0;
}

size_t countWrites(const FakeRegisterBlock &block, uint32_t offset,
    uint32_t value)
{
    size_t count = 0;
    for(const FakeRegisterBlock::Write &write : block.writes)
        if(write.offset == offset && write.value == value)++count;
    return count;
}

void testPrepareAndStart()
{
    NativeMaliService fresh;
    NativeMaliBringupState state;
    CHECK(fresh.start(&state) == kIOReturnNotReady);
    NativeMaliBringupBlocks nothing;
    CHECK(fresh.prepare(nothing) == kIOReturnBadArgument);

    Rig rig;
    CHECK(rig.service.prepared());
    CHECK(!rig.service.started());
    CHECK(rig.service.clients() == 0);

    CHECK(rig.start(&state) == kIOReturnSuccess);
    CHECK(rig.service.started());
    CHECK(rig.service.jobs().present() == 0x5);
    CHECK(rig.service.jobs().shaderPresent() == 0xF);
    CHECK(rig.service.observation().gpuId == 0x70021000);
    CHECK(state.gpu.jsPresent == 0x5);
    CHECK(rig.mmu().present() == 0x7);
    CHECK(rig.mmu().allocatedMask() == 0);

    /* A second start is a refusal, not a second bring-up. */
    const size_t writes = rig.gpu.writes.size();
    CHECK(rig.start(nullptr) == kIOReturnBusy);
    CHECK(rig.gpu.writes.size() == writes);

    CHECK(rig.service.stop() == kIOReturnSuccess);
    CHECK(!rig.service.started());
    CHECK(rig.service.stop() == kIOReturnSuccess);   /* idempotent */
}

void testOpenAndClose()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);

    NativeMaliUserClient *first = rig.open(0);
    CHECK(first->open());
    CHECK(rig.service.clients() == 1);
    CHECK(rig.service.clientUsable(first));
    CHECK(rig.mmu().allocatedMask() == 0x1);

    /* The client's AS is programmed with its own tables: TRANSTAB before the
     * UPDATE that broadcasts it (panfrost_mmu.c:167-190). */
    const uint32_t transtab = seqOf(rig.gpu, AS_TRANSTAB_LO(0),
        static_cast<uint32_t>(rig.vms[0].transtab() & 0xFFFFFFFFu));
    const uint32_t update = seqOf(rig.gpu, AS_COMMAND(0), AS_COMMAND_UPDATE);
    CHECK(transtab != 0 && update != 0 && transtab < update);

    NativeMaliUserClient *second = rig.open(1);
    CHECK(second->open());
    CHECK(rig.mmu().allocatedMask() == 0x3);
    CHECK(seqOf(rig.gpu, AS_TRANSTAB_LO(1),
        static_cast<uint32_t>(rig.vms[1].transtab() & 0xFFFFFFFFu)) != 0);

    /* The same client twice is refused before anything is written. */
    {
        const size_t writes = rig.gpu.writes.size();
        CHECK(rig.service.openClient(second, &rig.vms[1], &rig.tables,
            &rig.buffers[1], &rig.cache, false, 0) == kIOReturnBusy);
        CHECK(rig.gpu.writes.size() == writes);
    }

    /* Nulls are refused. */
    CHECK(rig.service.openClient(nullptr, &rig.vms[2], &rig.tables,
        &rig.buffers[2], &rig.cache, false, 0) == kIOReturnBadArgument);
    CHECK(rig.service.openClient(rig.clients[2].get(), nullptr, &rig.tables,
        &rig.buffers[2], &rig.cache, false, 0) == kIOReturnBadArgument);
    CHECK(rig.service.openClient(rig.clients[2].get(), &rig.vms[2], nullptr,
        &rig.buffers[2], &rig.cache, false, 0) == kIOReturnBadArgument);
    CHECK(rig.service.openClient(rig.clients[2].get(), &rig.vms[2], &rig.tables,
        nullptr, &rig.cache, false, 0) == kIOReturnBadArgument);

    /* Three present slots: the third fits, the fourth has no address space. */
    NativeMaliUserClient *third = rig.open(2);
    CHECK(third->open());
    CHECK(rig.mmu().allocatedMask() == 0x7);
    {
        const size_t writes = rig.gpu.writes.size();
        CHECK(rig.service.openClient(rig.clients[3].get(), &rig.vms[3],
            &rig.tables, &rig.buffers[3], &rig.cache, false, 0) ==
            kIOReturnNoResources);
        CHECK(!rig.clients[3]->open());
        CHECK(rig.gpu.writes.size() == writes);
        CHECK(!rig.service.clientUsable(rig.clients[3].get()));
    }

    /* Closing releases the objects, disables the AS and gives the number back;
     * the tables go with it. */
    const uint64_t tablesOfFirst = rig.vms[0].tables();
    const size_t liveTables = rig.tables.live;
    CHECK(rig.service.closeClient(first) == kIOReturnSuccess);
    CHECK(!first->open());
    CHECK(rig.service.clients() == 2);
    CHECK(rig.mmu().allocatedMask() == 0x6);
    CHECK(!rig.vms[0].prepared());
    CHECK(rig.tables.live + tablesOfFirst == liveTables);
    CHECK(!rig.service.clientUsable(first));
    CHECK(rig.service.closeClient(first) == kIOReturnBadArgument);
    CHECK(rig.service.closeClient(rig.clients[3].get()) == kIOReturnBadArgument);

    /* A client that still holds a BO is released the same way: releaseAll()
     * drops it, then the AS goes (panfrost_mmu.c:722-743). */
    {
        NativeMaliAbiBoAlloc request{0x2000};
        NativeMaliAbiBoAllocReply reply{};
        CHECK(second->allocBo(request, &reply) == kIOReturnSuccess);
        CHECK(rig.buffers[1].allocs == 1);
        CHECK(rig.service.closeClient(second) == kIOReturnSuccess);
        CHECK(rig.buffers[1].releases == 1);
        CHECK(!second->open());
    }

    /* stop() refuses while an AS is still owned, with nothing written. */
    {
        const size_t gpuWrites = rig.gpu.writes.size();
        const size_t spmWrites = rig.spm.writes.size();
        CHECK(rig.service.stop() == kIOReturnBusy);
        CHECK(rig.gpu.writes.size() == gpuWrites);
        CHECK(rig.spm.writes.size() == spmWrites);
        CHECK(rig.service.started());
    }
    CHECK(rig.service.closeClient(third) == kIOReturnSuccess);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.service.clients() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
    CHECK(!rig.service.started());
}

/* Every refusal that happens after the AS was taken has to give it back. */
void testOpenRollback()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    const size_t liveBefore = rig.tables.live;

    /* enable() fails on its first command: nothing was programmed. */
    rig.gpu.failWriteAt = AS_COMMAND(0);
    IOReturn teardown = kIOReturnIOError;
    CHECK(rig.service.openClient(rig.clients[0].get(), &rig.vms[0], &rig.tables,
        &rig.buffers[0], &rig.cache, false, 0, &teardown) == kIOReturnIOError);
    CHECK(teardown == kIOReturnSuccess);
    CHECK(!rig.clients[0]->open());
    CHECK(rig.service.clients() == 0);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(!rig.vms[0].prepared());
    CHECK(rig.tables.live == liveBefore);
    CHECK(!rig.service.clientUsable(rig.clients[0].get()));

    /* A failure after TRANSTAB was written: enable() disables the partial AS,
     * then the slot and the tables go. */
    rig.gpu.failWriteAt = AS_MEMATTR_LO(0);
    const size_t writes = rig.gpu.writes.size();
    CHECK(rig.service.openClient(rig.clients[0].get(), &rig.vms[0], &rig.tables,
        &rig.buffers[0], &rig.cache, false, 0, &teardown) == kIOReturnIOError);
    CHECK(teardown == kIOReturnSuccess);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.tables.live == liveBefore);
    CHECK(countWrites(rig.gpu, AS_TRANSTAB_LO(0), 0) >= 1);
    CHECK(rig.gpu.writes.size() > writes);

    /* A page source that cannot allocate the root table: the AS must go back
     * before the client is told anything. */
    {
        FailingPages failing;
        IOReturn noTables = kIOReturnIOError;
        CHECK(rig.service.openClient(rig.clients[2].get(), &rig.vms[2],
            &failing, &rig.buffers[2], &rig.cache, false, 0, &noTables) ==
            kIOReturnNoMemory);
        CHECK(noTables == kIOReturnSuccess);
        CHECK(rig.mmu().allocatedMask() == 0);
        CHECK(rig.service.clients() == 0);
        CHECK(!rig.clients[2]->open());
        CHECK(!rig.vms[2].prepared());
    }

    /* The BO table refuses a non-coherent client with no cache maintainer.
     * That refusal happens after the AS was taken and programmed, so it is the
     * one path that exercises rollbackOpen(): disable, give the number back,
     * release the tables. */
    rig.gpu.failWriteAt = -1;
    {
        const size_t clears = countWrites(rig.gpu, AS_TRANSTAB_LO(0), 0);
        IOReturn noCache = kIOReturnIOError;
        CHECK(rig.service.openClient(rig.clients[1].get(), &rig.vms[1],
            &rig.tables, &rig.buffers[1], nullptr, false, 0, &noCache) ==
            kIOReturnBadArgument);
        CHECK(noCache == kIOReturnSuccess);
        CHECK(!rig.clients[1]->open());
        CHECK(rig.service.clients() == 0);
        CHECK(rig.mmu().allocatedMask() == 0);
        CHECK(!rig.vms[1].prepared());
        CHECK(rig.tables.live == liveBefore);
        /* The AS was disabled before it was given back: exactly one more
         * TRANSTAB clear than before this open. */
        CHECK(countWrites(rig.gpu, AS_TRANSTAB_LO(0), 0) == clears + 1);
    }

    /* The same client can be opened once the hardware cooperates. */
    NativeMaliUserClient *client = rig.open(0);
    CHECK(client->open() && rig.service.clientUsable(client));
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* A close that cannot disable the AS leaves the client registered and stop()
 * refusing, so the retry is the only way forward. */
void testCloseFailure()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);
    const size_t liveTables = rig.tables.live;
    CHECK(liveTables != 0);

    rig.gpu.failWriteAt = AS_COMMAND(0);
    CHECK(rig.service.closeClient(client) == kIOReturnIOError);
    CHECK(!client->open());          /* releaseAll() already ran */
    CHECK(rig.service.clients() == 1);
    CHECK(rig.mmu().allocatedMask() == 0x1);
    CHECK(rig.vms[0].prepared());    /* the GPU may still walk these tables */

    const size_t gpuWrites = rig.gpu.writes.size();
    CHECK(rig.service.stop() == kIOReturnBusy);
    CHECK(rig.gpu.writes.size() == gpuWrites);

    rig.gpu.failWriteAt = -1;
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(rig.service.clients() == 0);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(!rig.vms[0].prepared());
    CHECK(rig.tables.live == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* A block that cannot be reached (gpuReadable() false) has no MMU to flush and
 * no address space to disable, but it also cannot still be reading the client's
 * buffers. closeClient() must free them anyway, the way
 * panfrost_mmu_release_ctx() guards its disable with pm_runtime_active()
 * (panfrost_mmu.c:731-732). Without that, a failed reset whose power-on never
 * brought the block back leaves a client that can never close. */
void testCloseWhenBlockIsDown()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);

    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocated{};
    CHECK(client->allocBo(alloc, &allocated) == kIOReturnSuccess);
    NativeMaliAbiBoMap map{};
    map.handle = allocated.handle;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    NativeMaliAbiBoMapReply mapped{};
    CHECK(client->mapBo(map, &mapped) == kIOReturnSuccess);

    /* A failed status read is not the same answer as "the block is gone": the
     * state is unknown, so the client must survive the close and the error must
     * come back unchanged. */
    rig.spm.failReadAt = NativeMaliPower::SpmPowerStatus;
    CHECK(rig.service.closeClient(client) == kIOReturnIOError);
    CHECK(client->open());
    CHECK(rig.service.clients() == 1);
    CHECK(rig.mmu().allocatedMask() == 0x1);
    CHECK(rig.buffers[0].releases == 0);
    rig.spm.failReadAt = -1;

    /* SPM stops reporting the MFG domains: every path that needs the block
     * refuses before its first write. */
    rig.spm.deriveStatus = false;
    rig.spm.memory[NativeMaliPower::SpmPowerStatus / 4] = 0;
    rig.spm.memory[NativeMaliPower::SpmPowerStatus2 / 4] = 0;

    const size_t gpuWrites = rig.gpu.writes.size();
    CHECK(rig.service.reset() == kIOReturnNotReady);
    CHECK(rig.gpu.writes.size() == gpuWrites);   /* softReset() refused first */

    /* releaseAll()'s flush and disable() both need the block, so the close
     * takes the no-register path: the buffers are released, the AS number and
     * the page tables come back, and nothing is written. */
    const uint64_t tablesOfClient = rig.vms[0].tables();
    const size_t liveTables = rig.tables.live;
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(!client->open());
    CHECK(rig.buffers[0].releases == 1);
    CHECK(rig.service.clients() == 0);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(!rig.vms[0].prepared());
    CHECK(rig.tables.live + tablesOfClient == liveTables);
    CHECK(rig.gpu.writes.size() == gpuWrites);

    /* With the block readable again the teardown can finish normally. */
    rig.spm.deriveStatus = true;
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* A client with a job in flight is not closable: releaseAll() waits for it
 * first (NativeMaliUserClient.cpp), and stop() keeps refusing in the meantime.
 * The retry, after the GPU finished, reaps and tears down in one go. */
void testCloseWaitsForJobs()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);

    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocated{};
    CHECK(client->allocBo(alloc, &allocated) == kIOReturnSuccess);
    NativeMaliAbiBoMap map{};
    map.handle = allocated.handle;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    NativeMaliAbiBoMapReply mapped{};
    CHECK(client->mapBo(map, &mapped) == kIOReturnSuccess);
    NativeMaliAbiSubmit submit{};
    submit.job_chain = 0x10000;
    NativeMaliAbiSubmitReply reply{};
    CHECK(client->submit(submit, &reply) == kIOReturnSuccess);
    const uint32_t js = NativeMaliAbiValidate::fenceSlot(reply.fence);

    const size_t gpuWrites = rig.gpu.writes.size();
    const size_t spmWrites = rig.spm.writes.size();
    CHECK(rig.service.closeClient(client) == kIOReturnTimeout);
    CHECK(client->open());
    CHECK(rig.service.clients() == 1);
    CHECK(rig.mmu().allocatedMask() == 0x1);
    CHECK(rig.service.stop() == kIOReturnBusy);
    CHECK(rig.gpu.writes.size() == gpuWrites);
    CHECK(rig.spm.writes.size() == spmWrites);

    rig.gpu.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(js);
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(!client->open());
    CHECK(rig.service.clients() == 0);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* stop() answers for the job slots it owns, independently of the address
 * space check. Getting there takes an owner bug on purpose - releasing the AS
 * while its chain is still queued - because through closeClient() the job is
 * always reaped first. The refusal must still write nothing. */
void testStopRefusesOutstandingJob()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    uint32_t as = 0;
    CHECK(rig.mmu().allocate(&as) == kIOReturnSuccess);

    NativeMaliFence fence{};
    CHECK(rig.service.jobs().submit(0, UINT64_C(0x100000), as, 0, &fence) ==
        kIOReturnSuccess);
    CHECK(rig.service.jobs().outstanding(0));

    /* A buggy owner gives the number back early; the AS check can no longer
     * see the job. */
    CHECK(rig.mmu().release(as) == kIOReturnSuccess);
    const size_t gpuWrites = rig.gpu.writes.size();
    const size_t spmWrites = rig.spm.writes.size();
    CHECK(rig.service.stop() == kIOReturnBusy);
    CHECK(rig.gpu.writes.size() == gpuWrites);
    CHECK(rig.spm.writes.size() == spmWrites);
    CHECK(rig.service.started());

    /* Once the slot is quiet, stop() goes through. */
    rig.gpu.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(0);
    NativeMaliJobCompletion completion;
    CHECK(rig.service.jobs().waitForCompletion(0, &completion) ==
        kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

void testOpenNeedsStart()
{
    Rig rig;
    CHECK(rig.service.openClient(rig.clients[0].get(), &rig.vms[0], &rig.tables,
        &rig.buffers[0], &rig.cache, false, 0) == kIOReturnNotReady);
    CHECK(!rig.clients[0]->open());
    CHECK(rig.tables.live == 0);
}

void testReset()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *first = rig.open(0);
    NativeMaliUserClient *second = rig.open(1);

    /* A real submission through the client: mapped BO, chain head inside it,
     * GPU_LATEST_FLUSH_ID read per submission. */
    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocated{};
    CHECK(first->allocBo(alloc, &allocated) == kIOReturnSuccess);
    NativeMaliAbiBoMap map{};
    map.handle = allocated.handle;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    NativeMaliAbiBoMapReply mapped{};
    CHECK(first->mapBo(map, &mapped) == kIOReturnSuccess);

    NativeMaliAbiSubmit submit{};
    submit.job_chain = 0x10000;
    NativeMaliAbiSubmitReply reply{};
    CHECK(first->submit(submit, &reply) == kIOReturnSuccess);
    CHECK(reply.fence != 0);
    const uint64_t lost = reply.fence;
    const uint32_t js = NativeMaliAbiValidate::fenceSlot(lost);
    CHECK(rig.service.jobs().outstanding(js));
    CHECK(rig.service.clientUsable(first));
    CHECK(rig.service.clientUsable(second));

    const size_t resetsBefore = rig.gpu.commandWrites;
    const size_t transtabsBefore = countWrites(rig.gpu, AS_TRANSTAB_LO(0),
        static_cast<uint32_t>(rig.vms[0].transtab() & 0xFFFFFFFFu));

    CHECK(rig.service.reset() == kIOReturnSuccess);

    /* soft reset, then the palette clear, then every slot dropped. */
    CHECK(rig.gpu.commandWrites == resetsBefore + 1);
    /* The job manager was quiesced first: NOP then SOFT_STOP on both present
     * slots (GPU_JS_PRESENT is 0x5), before the reset command. Slot 1 is not
     * present and must not be written. */
    CHECK(countWrites(rig.gpu, JS_COMMAND_NEXT(0), JS_COMMAND_NOP) == 1);
    CHECK(countWrites(rig.gpu, JS_COMMAND(0), JS_COMMAND_SOFT_STOP) == 1);
    CHECK(countWrites(rig.gpu, JS_COMMAND_NEXT(2), JS_COMMAND_NOP) == 1);
    CHECK(countWrites(rig.gpu, JS_COMMAND(2), JS_COMMAND_SOFT_STOP) == 1);
    CHECK(countWrites(rig.gpu, JS_COMMAND_NEXT(1), JS_COMMAND_NOP) == 0);
    CHECK(countWrites(rig.gpu, JS_COMMAND(1), JS_COMMAND_SOFT_STOP) == 0);
    {
        const uint32_t softStop = lastSeqOf(rig.gpu, JS_COMMAND(2),
            JS_COMMAND_SOFT_STOP);
        const uint32_t reset = lastSeqOf(rig.gpu, FakeGpu::Command,
            FakeGpu::CommandSoftReset);
        CHECK(softStop != 0 && reset != 0 && softStop < reset);
        CHECK(softStop < lastSeqOf(rig.gpu, JOB_INT_CLEAR,
            NativeMaliAllJobInterrupts));
    }
    CHECK(rig.service.softStopTimeouts() == 0);
    CHECK(countWrites(rig.gpu, JOB_INT_CLEAR, NativeMaliAllJobInterrupts) == 1);
    CHECK(!rig.service.jobs().outstanding(js));
    CHECK(rig.gpu.memory[JOB_INT_RAWSTAT / 4] == 0);

    /* Every client kept its AS number and got its tables programmed again. */
    CHECK(rig.mmu().allocatedMask() == 0x3);
    CHECK(countWrites(rig.gpu, AS_TRANSTAB_LO(0),
        static_cast<uint32_t>(rig.vms[0].transtab() & 0xFFFFFFFFu)) ==
        transtabsBefore + 1);
    CHECK(seqOf(rig.gpu, AS_TRANSTAB_LO(1),
        static_cast<uint32_t>(rig.vms[1].transtab() & 0xFFFFFFFFu)) != 0);
    CHECK(rig.service.clientUsable(first));
    CHECK(rig.service.clientUsable(second));

    /* The in-flight fence is dead: IOError, not a fault, not a timeout. */
    {
        NativeMaliAbiWaitFence wait{lost, 0};
        CHECK(first->waitFence(wait) == kIOReturnIOError);
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(first->status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 0);
        CHECK(status.resets == 1);
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(second->status(&status) == kIOReturnSuccess);
        CHECK(status.resets == 1);   /* told even though it had no job */
    }

    /* The slots are usable again: the round-robin moves to the next slot, not
     * back to the destroyed fence. */
    CHECK(first->submit(submit, &reply) == kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::fenceSlot(reply.fence) == 2);
    CHECK(NativeMaliAbiValidate::fenceSeqno(reply.fence) == 1);
    rig.gpu.memory[JOB_INT_RAWSTAT / 4] |=
        JOB_INT_MASK_DONE(NativeMaliAbiValidate::fenceSlot(reply.fence));
    NativeMaliAbiWaitFence wait{reply.fence, 0};
    CHECK(first->waitFence(wait) == kIOReturnSuccess);

    /* A reset with no job in flight destroys nothing but still reprograms. */
    CHECK(rig.service.reset() == kIOReturnSuccess);
    CHECK(first->status(nullptr) == kIOReturnBadArgument);
    {
        NativeMaliAbiStatus status{};
        CHECK(first->status(&status) == kIOReturnSuccess);
        CHECK(status.resets == 2);
        CHECK(status.faults == 0);
    }

    /* Close after a reset: the BOs are gone from the client's view, but the
     * mapping and the pages are still valid, so releaseAll() unmaps normally. */
    CHECK(rig.service.closeClient(first) == kIOReturnSuccess);
    CHECK(rig.service.closeClient(second) == kIOReturnSuccess);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* A reset without clients still does the hardware half and drops stale jobs. */
void testResetWithoutClients()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    const size_t resetsBefore = rig.gpu.commandWrites;
    CHECK(rig.service.reset() == kIOReturnSuccess);
    CHECK(rig.gpu.commandWrites == resetsBefore + 1);
    CHECK(countWrites(rig.gpu, JOB_INT_CLEAR, NativeMaliAllJobInterrupts) == 1);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* panfrost logs "Soft-stop failed" and resets anyway (panfrost_job.c:708-709);
 * this service does the same but counts it. A register error in the quiesce is
 * not a timeout: the reset is aborted before the reset line is touched. */
void testResetSoftStop()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);

    /* A chain the fake never stops: JS_STATE stays set through SOFT_STOP. */
    rig.gpu.memory[JOB_INT_JS_STATE / 4] = 0x1;
    rig.gpu.softStopStuck = true;
    const size_t resetsBefore = rig.gpu.commandWrites;
    const size_t delaysBefore = rig.gpu.delays.size();
    CHECK(rig.service.reset() == kIOReturnSuccess);
    CHECK(rig.gpu.commandWrites == resetsBefore + 1);
    CHECK(rig.service.softStopTimeouts() == 1);
    CHECK(rig.gpu.delays.size() - delaysBefore ==
        SoftStopTimeoutUs / SoftStopPollUs);
    /* The soft reset that followed cleared the job-manager state anyway. */
    CHECK(rig.gpu.memory[JOB_INT_JS_STATE / 4] == 0);
    CHECK(rig.service.clientUsable(client));

    /* An idle manager adds no timeout: the counter only moves on a stuck
     * reset. */
    rig.gpu.softStopStuck = false;
    CHECK(rig.service.reset() == kIOReturnSuccess);
    CHECK(rig.service.softStopTimeouts() == 1);

    /* A write error in the quiesce aborts the reset before the reset line. */
    rig.gpu.failWriteAt = JS_COMMAND(0);
    const size_t resets = rig.gpu.commandWrites;
    CHECK(rig.service.reset() == kIOReturnIOError);
    CHECK(rig.gpu.commandWrites == resets);
    CHECK(!rig.service.clientUsable(client));
    rig.gpu.failWriteAt = -1;
    CHECK(rig.service.reset() == kIOReturnNotReady);

    /* The documented way out also clears the counter. */
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
    CHECK(rig.start() == kIOReturnSuccess);
    CHECK(rig.service.softStopTimeouts() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* The reset worker's own failure: the hardware was reset, so the clients' jobs
 * are dead, but the service must stop claiming they are usable. */
void testResetInterruptFailure()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);

    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocated{};
    CHECK(client->allocBo(alloc, &allocated) == kIOReturnSuccess);
    NativeMaliAbiBoMap map{};
    map.handle = allocated.handle;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    NativeMaliAbiBoMapReply mapped{};
    CHECK(client->mapBo(map, &mapped) == kIOReturnSuccess);
    NativeMaliAbiSubmit submit{};
    submit.job_chain = 0x10000;
    NativeMaliAbiSubmitReply reply{};
    CHECK(client->submit(submit, &reply) == kIOReturnSuccess);
    const uint64_t lost = reply.fence;

    rig.gpu.failWriteAt = JOB_INT_CLEAR;
    CHECK(rig.service.reset() == kIOReturnIOError);
    CHECK(!rig.service.clientUsable(client));

    /* The jobs are gone even though the palette clear failed. */
    CHECK(!rig.service.jobs().outstanding(
        NativeMaliAbiValidate::fenceSlot(lost)));
    {
        NativeMaliAbiWaitFence wait{lost, 0};
        CHECK(client->waitFence(wait) == kIOReturnIOError);
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(client->status(&status) == kIOReturnSuccess);
        CHECK(status.resets == 1);
    }

    /* No new client on a GPU whose state is not trusted, and no second reset:
     * it would report success while nothing is usable. */
    CHECK(rig.service.openClient(rig.clients[1].get(), &rig.vms[1], &rig.tables,
        &rig.buffers[1], &rig.cache, false, 0) == kIOReturnNotReady);
    CHECK(!rig.clients[1]->open());
    {
        const size_t gpuWrites = rig.gpu.writes.size();
        CHECK(rig.service.reset() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.size() == gpuWrites);
        CHECK(!rig.service.clientUsable(client));
    }

    /* The way out is close, stop, start. */
    rig.gpu.failWriteAt = -1;
    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *again = rig.open(1);
    CHECK(rig.service.clientUsable(again));
    CHECK(rig.service.closeClient(again) == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* If one client's AS cannot be reprogrammed, the service reports the error and
 * refuses to pretend anything is usable; the other AS numbers are still
 * allocated so stop() cannot power the block down under them. */
void testResetReenableFailure()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *first = rig.open(0);
    NativeMaliUserClient *second = rig.open(1);

    rig.gpu.failWriteAt = AS_TRANSTAB_LO(1);
    CHECK(rig.service.reset() == kIOReturnIOError);
    CHECK(!rig.service.clientUsable(first));
    CHECK(!rig.service.clientUsable(second));
    CHECK(rig.mmu().allocatedMask() == 0x3);

    rig.gpu.failWriteAt = -1;
    CHECK(rig.service.closeClient(first) == kIOReturnSuccess);
    CHECK(rig.service.closeClient(second) == kIOReturnSuccess);
    CHECK(rig.mmu().allocatedMask() == 0);
    CHECK(rig.service.stop() == kIOReturnSuccess);
    CHECK(rig.start() == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

/* The ABI Reset selector is the trigger the glue needed: it runs the same
 * device-wide reset through the target the service injects at openClient(), and
 * a failure leaves the service refusing every later reset. */
void testResetThroughAbi()
{
    Rig rig;
    CHECK(rig.start() == kIOReturnSuccess);
    NativeMaliUserClient *client = rig.open(0);

    const size_t resets = rig.gpu.commandWrites;
    const size_t writes = rig.gpu.writes.size();
    CHECK(client->dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
        kIOReturnSuccess);
    CHECK(rig.gpu.commandWrites == resets + 1);
    CHECK(rig.gpu.writes.size() > writes);
    CHECK(rig.service.clientUsable(client));
    {
        NativeMaliAbiStatus status{};
        CHECK(client->status(&status) == kIOReturnSuccess);
        CHECK(status.resets == 1);
    }

    /* A failed reset is returned to the caller, and the service's latch makes
     * the next attempt NotReady instead of reporting a success it cannot keep. */
    rig.gpu.failWriteAt = JOB_INT_CLEAR;
    CHECK(client->dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
        kIOReturnIOError);
    rig.gpu.failWriteAt = -1;
    const size_t afterFailure = rig.gpu.writes.size();
    CHECK(client->dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
        kIOReturnNotReady);
    CHECK(rig.gpu.writes.size() == afterFailure);

    CHECK(rig.service.closeClient(client) == kIOReturnSuccess);
    CHECK(rig.service.stop() == kIOReturnSuccess);
}

} // namespace

int main()
{
    testPrepareAndStart();
    testOpenAndClose();
    testOpenRollback();
    testCloseFailure();
    testCloseWhenBlockIsDown();
    testCloseWaitsForJobs();
    testStopRefusesOutstandingJob();
    testOpenNeedsStart();
    testReset();
    testResetWithoutClients();
    testResetSoftStop();
    testResetInterruptFailure();
    testResetReenableFailure();
    testResetThroughAbi();
    std::puts("PASS: the service's start/stop gates, one address space per open client with its tables programmed, rollback of every open failure, close that keeps the AS until it is really disabled, waits for the client's jobs, and frees them without registers when the block is down, stop refusing an open client or a job whose AS was given back early, a reset that quiesces the job manager (counting a soft-stop timeout, aborting on a register error), clears the palette, kills only the in-flight fences and reprograms every client AS, and the ABI Reset selector that reaches that same reset");
    return 0;
}
