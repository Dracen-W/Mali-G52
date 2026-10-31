/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliUserClient: the pure client state machine above NativeMaliBoTable.
 *
 * These are the real NativeMaliVm page tables, the real NativeMaliMmu flush, the
 * real NativeMaliJobSlot submit/reap sequence and the real NativeMaliGpuCore
 * flush-id read; only the register bank, the page/buffer sources and the cache
 * maintainer are fakes (tests/FakeMaliMemory.h, tests/FakeMaliPlatform.h).
 *
 * What the tests pin down, and where each rule comes from:
 *   - Query/GetDeviceInfo report the geometry the service actually built, the
 *     M3 probe output, and the non-coherent flag (NativeMaliABI.h).
 *   - Submission only accepts a chain head inside one of this client's live
 *     mappings, reads GPU_LATEST_FLUSH_ID per submission and lets the slot, not
 *     the caller, choose the job slot (NativeMaliUserClient.h; panfrost_job.c).
 *   - A fence belongs to the client that was handed it. Waiting on, or using as
 *     a dependency, a fence another client received is refused before the slot
 *     is touched, so its completion cannot be stolen.
 *   - Completion is polled: DONE reaps, ERR reaps and faults the slot and the
 *     fault counter, a timeout leaves the job outstanding and is repeatable.
 *   - close() and releaseAll() are fail-closed: live objects and un-reaped jobs
 *     stop them, and a failed unmap leaves the object live so the call can be
 *     repeated (panfrost_mmu.c:500-524). A job of another client on the same
 *     slot is not this client's to wait for.
 */
#include "FakeMaliMemory.h"
#include "FakeMaliPlatform.h"
#include "NativeMaliAbiValidate.h"
#include "NativeMaliRegs.h"
#include "NativeMaliUserClient.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kShaderPresent = 0x1f;
constexpr uint32_t kFlushId = 0x2a;

/* The device-wide half injected at open: the pure client never owns a GPU
 * reset, it only forwards the ABI Reset selector to this target. */
struct FakeResetTarget final : NativeMaliResetTarget {
    size_t calls = 0;
    IOReturn result = kIOReturnSuccess;
    IOReturn resetDevice() override
    {
        ++calls;
        return result;
    }
};

struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeMmu registers;      /* the whole 0x13040000 window: GPU, JS, MMU */
    NativeMaliPower power;
    NativeMaliMmu mmu;
    TestPages pages;
    NativeMaliVm vm;
    FakeMemorySource source;
    FakeCache cache;
    NativeMaliJobSlot jobs;
    NativeMaliGpuCore gpu;
    NativeMaliUserClient client;
    FakeResetTarget reset;
    NativeMaliGpuObservation observation;
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
        CHECK(jobs.prepare(&registers, &power, &mmu) == kIOReturnSuccess);
        CHECK(jobs.setPresent(0x7, kShaderPresent) == kIOReturnSuccess);
        CHECK(gpu.prepare(&registers, &power) == kIOReturnSuccess);
        registers.memory[GPU_LATEST_FLUSH_ID / 4] = kFlushId;

        observation.gpuId = 0x70021000;
        observation.shaderPresent = kShaderPresent;
        observation.tilerPresent = 0x3;
        observation.maxThreads = 0x100;
        observation.maxWorkgroupSize = 0x200;
        observation.maxBarrierSize = 0x300;
        observation.l2Features = 0x11;
        observation.coreFeatures = 0x22;
        observation.tilerFeatures = 0x33;
    }

    IOReturn openClient(uint64_t maxBytes = 0, bool coherent = false)
    {
        return client.open(&source, coherent ? nullptr : &cache, coherent, &vm,
            &mmu, as, &jobs, &gpu, observation, maxBytes, &reset);
    }

    void openOrDie(uint64_t maxBytes = 0)
    {
        CHECK(openClient(maxBytes) == kIOReturnSuccess);
        CHECK(client.open());
    }

    uint64_t alloc(uint64_t bytes = 0x4000)
    {
        NativeMaliAbiBoAlloc request{bytes};
        NativeMaliAbiBoAllocReply reply{};
        CHECK(client.allocBo(request, &reply) == kIOReturnSuccess);
        CHECK(reply.handle != 0);
        return reply.handle;
    }

    uint64_t allocAt(uint64_t iova, uint64_t bytes = 0x4000)
    {
        const uint64_t handle = alloc(bytes);
        NativeMaliAbiBoMap request{};
        request.handle = handle;
        request.iova = iova;
        request.permissions = NativeMaliBoRead | NativeMaliBoWrite;
        NativeMaliAbiBoMapReply reply{};
        CHECK(client.mapBo(request, &reply) == kIOReturnSuccess);
        CHECK(reply.iova == iova);
        return handle;
    }

    /* Start a chain the way a client would: no dependency, no timeout. */
    uint64_t submitAt(uint64_t chain = 0x10000)
    {
        NativeMaliAbiSubmit request{};
        request.job_chain = chain;
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(client.submit(request, &reply) == kIOReturnSuccess);
        CHECK(reply.fence != 0);
        return reply.fence;
    }

    IOReturn submitWaiting(uint64_t wait, uint64_t timeout_ns,
        NativeMaliAbiSubmitReply *reply)
    {
        NativeMaliAbiSubmit request{};
        request.job_chain = 0x10000;
        request.wait_fence = wait;
        request.wait_timeout_ns = timeout_ns;
        return client.submit(request, reply);
    }

    void latchDone(uint32_t js)
    {
        registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(js);
    }

    /* The hardware reports ERR with a reason in JS_STATUS; everything above
     * MAX_NON_FAULT (0x3f) is a fault (NativeMaliFault.h). */
    void latchError(uint32_t js, uint32_t exception)
    {
        registers.memory[JS_STATUS(js) / 4] = exception;
        registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_ERR(js);
    }

    /* A job started behind this client's back: another client on the same
     * GPU. It uses the same slot object because the service shares it. */
    void foreign(uint32_t js)
    {
        NativeMaliFence fence{};
        CHECK(jobs.submit(js, 0x10000, as, 0, &fence) == kIOReturnSuccess);
        CHECK(fence.js == js);
    }

    void reapForeign(uint32_t js)
    {
        latchDone(js);
        NativeMaliJobCompletion completion;
        CHECK(jobs.waitForCompletion(js, &completion) == kIOReturnSuccess);
    }

    uint64_t finish(uint64_t fence)
    {
        const uint32_t js = NativeMaliAbiValidate::fenceSlot(fence);
        latchDone(js);
        NativeMaliAbiWaitFence wait{fence, 0};
        CHECK(client.waitFence(wait) == kIOReturnSuccess);
        return fence;
    }
};

uint32_t fenceSlot(uint64_t fence)
{
    return NativeMaliAbiValidate::fenceSlot(fence);
}

uint32_t fenceSeqno(uint64_t fence)
{
    return NativeMaliAbiValidate::fenceSeqno(fence);
}

void testOpenAndInfo()
{
    Rig rig;

    /* Nothing works before open. Every reply is zeroed by the refusal. */
    {
        NativeMaliAbiBoAlloc alloc{0x4000};
        NativeMaliAbiBoAllocReply allocReply{0xdead};
        CHECK(rig.client.allocBo(alloc, &allocReply) == kIOReturnNotReady);
        CHECK(allocReply.handle == 0);
        NativeMaliAbiBoMap map{};
        map.handle = 1;
        map.iova = 0x1000;
        map.permissions = NativeMaliBoRead;
        NativeMaliAbiBoMapReply mapReply{0xdead};
        CHECK(rig.client.mapBo(map, &mapReply) == kIOReturnNotReady);
        CHECK(mapReply.iova == 0);
        CHECK(rig.client.freeBo(NativeMaliAbiBoFree{1}) == kIOReturnNotReady);
        NativeMaliAbiBoCpuMapReply cpuReply{0xdead, 0xdead, 0xdead};
        CHECK(rig.client.cpuMap(NativeMaliAbiBoCpuMap{1}, &cpuReply) ==
            kIOReturnNotReady);
        CHECK(cpuReply.token == 0 && cpuReply.bytes == 0);
        CHECK(rig.client.cpuUnmap(NativeMaliAbiBoCpuUnmap{1, 1, 0}) ==
            kIOReturnNotReady);

        NativeMaliAbiQuery query{};
        query.abi_version = NativeMaliAbiVersion;
        NativeMaliAbiQuery queryReply{0xdead, 0xdead, 0xdead, 0xdead, 0xdead};
        CHECK(rig.client.query(query, &queryReply) == kIOReturnNotReady);
        CHECK(queryReply.abi_version == 0 && queryReply.max_bo_bytes == 0);
        NativeMaliAbiDeviceInfo info{};
        std::memset(&info, 0xde, sizeof(info));
        CHECK(rig.client.deviceInfo(&info) == kIOReturnNotReady);
        CHECK(info.gpu_id == 0 && info.va_size == 0);

        NativeMaliAbiSubmit request{};
        request.job_chain = 0x10000;
        NativeMaliAbiSubmitReply submitReply{0xdead};
        CHECK(rig.client.submit(request, &submitReply) == kIOReturnNotReady);
        CHECK(submitReply.fence == 0);
        CHECK(rig.client.waitFence(NativeMaliAbiWaitFence{
            NativeMaliAbiValidate::packFence(0, 1), 0}) == kIOReturnNotReady);
        NativeMaliAbiStatus status{0xdead, 0xdead};
        CHECK(rig.client.status(&status) == kIOReturnNotReady);
        CHECK(status.faults == 0 && status.resets == 0);

        /* A reset before there is a client is not counted anywhere. */
        rig.client.noteReset();
        CHECK(rig.client.status(&status) == kIOReturnNotReady);
        CHECK(rig.client.close() == kIOReturnNotReady);
        CHECK(rig.client.releaseAll() == kIOReturnNotReady);
    }

    /* Argument refusals: the table's own open() checks are exercised in
     * test_bo_table; here the client's extra arguments. */
    CHECK(rig.client.open(&rig.source, &rig.cache, false, nullptr, &rig.mmu,
        rig.as, &rig.jobs, &rig.gpu, rig.observation, 0) ==
        kIOReturnBadArgument);
    CHECK(rig.client.open(&rig.source, &rig.cache, false, &rig.vm, &rig.mmu,
        rig.as, nullptr, &rig.gpu, rig.observation, 0) ==
        kIOReturnBadArgument);
    CHECK(rig.client.open(&rig.source, &rig.cache, false, &rig.vm, &rig.mmu,
        rig.as, &rig.jobs, nullptr, rig.observation, 0) ==
        kIOReturnBadArgument);
    CHECK(rig.client.open(nullptr, &rig.cache, false, &rig.vm, &rig.mmu,
        rig.as, &rig.jobs, &rig.gpu, rig.observation, 0) ==
        kIOReturnBadArgument);
    /* Non-coherent without a maintainer, and an address space the MMU does not
     * have. */
    CHECK(rig.client.open(&rig.source, nullptr, false, &rig.vm, &rig.mmu,
        rig.as, &rig.jobs, &rig.gpu, rig.observation, 0) ==
        kIOReturnBadArgument);
    CHECK(rig.client.open(&rig.source, &rig.cache, false, &rig.vm, &rig.mmu,
        4, &rig.jobs, &rig.gpu, rig.observation, 0) ==
        kIOReturnBadArgument);
    CHECK(!rig.client.open());

    rig.openOrDie(UINT64_C(0x200000));
    CHECK(rig.openClient() == kIOReturnBusy);

    /* Query: a version or flag the driver does not know is Unsupported, a
     * malformed one BadArgument, and a refusal never reports geometry. */
    {
        NativeMaliAbiQuery query{};
        query.abi_version = NativeMaliAbiVersion + 1;
        NativeMaliAbiQuery reply{0xdead, 0xdead, 0xdead, 0xdead, 0xdead};
        CHECK(rig.client.query(query, &reply) == kIOReturnUnsupported);
        CHECK(reply.abi_version == 0 && reply.input_bits == 0);
        query.abi_version = NativeMaliAbiVersion;
        query.flags = 1;
        CHECK(rig.client.query(query, &reply) == kIOReturnUnsupported);
        CHECK(reply.flags == 0);
        CHECK(rig.client.query(query, nullptr) == kIOReturnBadArgument);

        query.flags = 0;
        CHECK(rig.client.query(query, &reply) == kIOReturnSuccess);
        CHECK(reply.abi_version == NativeMaliAbiVersion);
        /* The tables the service built, not what the hardware could do. */
        CHECK(reply.input_bits == 48);
        CHECK(reply.output_bits == 40);
        CHECK(reply.flags == 0);
        CHECK(reply.max_bo_bytes == UINT64_C(0x200000));
    }

    /* GetDeviceInfo: the M3 probe output, the non-coherent flag and the whole
     * address space. */
    {
        NativeMaliAbiDeviceInfo info{};
        CHECK(rig.client.deviceInfo(nullptr) == kIOReturnBadArgument);
        CHECK(rig.client.deviceInfo(&info) == kIOReturnSuccess);
        CHECK(info.gpu_id == 0x70021000);
        CHECK(info.shader_present_lo == kShaderPresent);
        CHECK(info.shader_present_hi == 0);
        CHECK(info.tiler_present_lo == 0x3);
        CHECK(info.tiler_present_hi == 0);
        CHECK(info.thread_max_threads == 0x100);
        CHECK(info.max_workgroup_size == 0x200);
        CHECK(info.max_barrier_size == 0x300);
        CHECK(info.l2_features == 0x11);
        CHECK(info.core_features == 0x22);
        CHECK(info.tiler_features == 0x33);
        CHECK(info.flags == NativeMaliDeviceNonCoherent);
        CHECK(info.va_start == 0);
        CHECK(info.va_size == (UINT64_C(1) << 48));
    }

    /* A coherent client needs no maintainer and says so in the reply. */
    {
        Rig coherent;
        CHECK(coherent.openClient(0, true) == kIOReturnSuccess);
        NativeMaliAbiDeviceInfo info{};
        CHECK(coherent.client.deviceInfo(&info) == kIOReturnSuccess);
        CHECK(info.flags == 0);
        NativeMaliAbiQuery query{};
        query.abi_version = NativeMaliAbiVersion;
        NativeMaliAbiQuery reply{};
        CHECK(coherent.client.query(query, &reply) == kIOReturnSuccess);
        CHECK(reply.max_bo_bytes == NATIVE_MALI_ABI_MAX_BO_BYTES);
    }
}

void testBoForwarding()
{
    Rig rig;
    rig.openOrDie();

    /* The client is a pass-through: the table's own rules are tested in
     * test_bo_table, so this only proves the wrapper reaches it with the same
     * reply and the same ownership. */
    const uint64_t handle = rig.alloc(0x8000);
    {
        NativeMaliAbiBoMap request{};
        request.handle = handle;
        request.iova = 0x10000;
        request.permissions = NativeMaliBoRead;
        NativeMaliAbiBoMapReply reply{0xdead};
        CHECK(rig.client.mapBo(request, &reply) == kIOReturnSuccess);
        CHECK(reply.iova == 0x10000);
    }

    NativeMaliAbiBoCpuMapReply cpu{};
    CHECK(rig.client.cpuMap(NativeMaliAbiBoCpuMap{handle}, &cpu) ==
        kIOReturnSuccess);
    CHECK(cpu.token != 0);
    CHECK(cpu.bytes == 0x8000);
    CHECK(rig.client.memoryForToken(cpu.token) != nullptr);
    CHECK(rig.client.memoryForToken(cpu.token)->bytes() == 0x8000);
    CHECK(rig.client.memoryForToken(cpu.token + 1) == nullptr);
    CHECK(rig.client.cpuUnmap(NativeMaliAbiBoCpuUnmap{handle, cpu.token, 0}) ==
        kIOReturnSuccess);
    CHECK(rig.client.memoryForToken(cpu.token) == nullptr);

    CHECK(rig.client.liveObjects() == 1);
    CHECK(rig.client.freeBo(NativeMaliAbiBoFree{handle}) == kIOReturnSuccess);
    CHECK(rig.client.liveObjects() == 0);
    CHECK(rig.source.outstanding() == 0);
}

void testSubmitRefusals()
{
    Rig rig;
    rig.openOrDie();
    const uint64_t handle = rig.allocAt(0x10000);
    const size_t writes = rig.registers.writes.size();

    /* Not 64-byte aligned, zero, or outside every mapping: refused before the
     * slot or the GPU is touched. */
    {
        NativeMaliAbiSubmit request{};
        request.job_chain = 0x10020;   /* 32 bytes into the mapping */
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        request.job_chain = 0;
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        request.job_chain = 0x0FFC0;   /* just before the mapping */
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        request.job_chain = 0x14000;   /* just after it */
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
    }

    /* A malformed record: an unknown flag bit (Unsupported), a reserved word
     * set or a timeout without a fence (BadArgument). */
    {
        NativeMaliAbiSubmit request{};
        request.job_chain = 0x10000;
        request.flags = 1;
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.client.submit(request, &reply) == kIOReturnUnsupported);
        CHECK(reply.fence == 0);
        request.flags = 0;
        request.reserved = 1;
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        request.reserved = 0;
        request.wait_timeout_ns = 1000;
        CHECK(rig.client.submit(request, &reply) == kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        CHECK(rig.client.submit(request, nullptr) == kIOReturnBadArgument);
        /* A null reply is refused before anything is read. */
        request.wait_timeout_ns = 0;
        CHECK(rig.client.submit(request, nullptr) == kIOReturnBadArgument);
    }
    /* No refusal above reached a register. */
    CHECK(rig.registers.writes.size() == writes);

    /* The last 64 bytes of a mapping are inside it; the flush id is read and
     * written for this one. */
    {
        NativeMaliAbiSubmit request{};
        request.job_chain = 0x13FC0;
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.client.submit(request, &reply) == kIOReturnSuccess);
        CHECK(reply.fence != 0);
        CHECK(fenceSlot(reply.fence) == 0);
        rig.finish(reply.fence);
    }

    CHECK(rig.client.freeBo(NativeMaliAbiBoFree{handle}) == kIOReturnSuccess);
}

void testSlotChoice()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);

    /* Round-robin from the last slot used, one job at a time, and every job
     * carries the flush id read from GPU_LATEST_FLUSH_ID for that submission. */
    uint64_t fences[3]{};
    for(uint32_t i = 0; i < 3; ++i) {
        fences[i] = rig.submitAt();
        CHECK(fenceSlot(fences[i]) == i);
        CHECK(fenceSeqno(fences[i]) == 1);
        size_t flushWrites = 0;
        for(const auto &write : rig.registers.writes)
            if(write.offset == JS_FLUSH_ID_NEXT(i) &&
                write.value == kFlushId)
                ++flushWrites;
        CHECK(flushWrites == 1);
        rig.finish(fences[i]);
        CHECK(rig.jobs.fenceSignaled(NativeMaliFence{i, 1}));
    }

    /* Wrapped around: slot 0 again, next seqno. */
    const uint64_t second = rig.submitAt();
    CHECK(fenceSlot(second) == 0);
    CHECK(fenceSeqno(second) == 2);
    rig.latchError(0, 0x40);
    NativeMaliAbiWaitFence wait{second, 1000};
    CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
    CHECK(rig.jobs.faulted(0));

    /* A faulted slot needs a GPU reset, which this object does not perform, so
     * it is skipped. Fill the other two, reap them, and come back around: the
     * next submission must land on slot 1, not be refused by slot 0. */
    for(uint32_t i = 0; i < 2; ++i) {
        const uint64_t fence = rig.submitAt();
        CHECK(fenceSlot(fence) == i + 1);
        rig.finish(fence);
    }
    const uint64_t afterFault = rig.submitAt();
    CHECK(fenceSlot(afterFault) == 1);
    CHECK(fenceSeqno(afterFault) == 3);
}

void testFenceOwnership()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);

    /* Another client submitted on slot 1 first; its fence must be unusable
     * here even though the slot is busy. */
    rig.foreign(1);
    const uint64_t foreignFence = NativeMaliAbiValidate::packFence(1, 1);
    const size_t writes = rig.registers.writes.size();
    {
        NativeMaliAbiWaitFence wait{foreignFence, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnBadArgument);
        /* The refusal did not reap it or touch the slot. */
        CHECK(rig.jobs.outstanding(1));
        CHECK(!rig.jobs.fenceSignaled(NativeMaliFence{1, 1}));
        CHECK(rig.registers.writes.size() == writes);
    }
    {
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(foreignFence, 0, &reply) ==
            kIOReturnBadArgument);
        CHECK(reply.fence == 0);
        CHECK(rig.registers.writes.size() == writes);
    }

    /* A seqno this client was never handed, even on a slot it has used, is
     * refused; a seqno it has not reached yet cannot be waited on. */
    {
        NativeMaliAbiWaitFence wait{
            NativeMaliAbiValidate::packFence(0, 1), 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnBadArgument);
    }

    /* Once the foreign job completes it is still not this client's: signaled
     * does not transfer ownership. */
    rig.reapForeign(1);
    {
        NativeMaliAbiWaitFence wait{foreignFence, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnBadArgument);
    }

    /* The client's own fence, on the free slot 0. */
    const uint64_t own = rig.submitAt();
    CHECK(fenceSlot(own) == 0);
    CHECK(fenceSeqno(own) == 1);
    /* A higher seqno on an owned slot is still not this client's. */
    {
        NativeMaliAbiWaitFence wait{
            NativeMaliAbiValidate::packFence(0, 2), 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnBadArgument);
    }
    /* Zero, and a fence with a seqno of zero, are malformed. */
    CHECK(rig.client.waitFence(NativeMaliAbiWaitFence{0, 0}) ==
        kIOReturnBadArgument);
    CHECK(rig.client.waitFence(NativeMaliAbiWaitFence{
        NativeMaliAbiValidate::packFence(0, 0), 0}) == kIOReturnBadArgument);
}

void testDependency()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);

    /* The first job is outstanding; the second waits on it. The DONE is
     * already latched, so the wait reaps it, and the slot choice then starts
     * from the next slot. */
    const uint64_t first = rig.submitAt();
    CHECK(fenceSlot(first) == 0);
    rig.latchDone(0);
    uint64_t dependent = 0;
    {
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(first, 1000000, &reply) == kIOReturnSuccess);
        CHECK(reply.fence != 0);
        CHECK(fenceSlot(reply.fence) == 1);
        CHECK(fenceSeqno(reply.fence) == 1);
        CHECK(rig.jobs.fenceSignaled(NativeMaliFence{0, 1}));
        dependent = reply.fence;
    }

    /* A dependency that has not completed in the budget is a timeout, and no
     * job is started for it. */
    const uint64_t second = rig.submitAt();   /* slot 2, seqno 1 */
    CHECK(fenceSlot(second) == 2);
    {
        const size_t writes = rig.registers.writes.size();
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(second, 0, &reply) == kIOReturnTimeout);
        CHECK(reply.fence == 0);
        CHECK(rig.registers.writes.size() == writes);
        CHECK(rig.jobs.outstanding(2));
    }

    /* Free slot 1 from the first dependency, so the final submission has
     * exactly one free slot to land on. */
    rig.latchDone(1);
    {
        NativeMaliAbiWaitFence wait{dependent, 1000};
        CHECK(rig.client.waitFence(wait) == kIOReturnSuccess);
    }

    /* Repeat it with the job completed: the wait succeeds and the new job goes
     * to the slot that became free. */
    rig.latchDone(2);
    {
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(second, 1000000, &reply) == kIOReturnSuccess);
        CHECK(fenceSlot(reply.fence) == 0);
        CHECK(fenceSeqno(reply.fence) == 2);
    }

    /* A dependency that ends in a fault is not waited through: the error is
     * returned, the fault counted, and no new job started. */
    {
        const uint64_t doomed = rig.submitAt();   /* slot 1, seqno 2 */
        CHECK(fenceSlot(doomed) == 1);
        rig.latchError(1, 0x40);
        const size_t writes = rig.registers.writes.size();
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(doomed, 1000000, &reply) == kIOReturnIOError);
        CHECK(reply.fence == 0);
        /* The reap writes JOB_INT_CLEAR and cancels the staged command, but no
         * new chain head is written. */
        for(size_t i = writes; i < rig.registers.writes.size(); ++i) {
            const uint32_t offset = rig.registers.writes[i].offset;
            for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js)
                CHECK(offset != JS_HEAD_NEXT_LO(js));
        }
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 1);
    }
}

void testCompletion()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);

    const uint64_t fence = rig.submitAt();
    CHECK(fenceSlot(fence) == 0);
    CHECK(fenceSeqno(fence) == 1);

    /* Nothing has completed: a zero budget polls once, leaves the job in
     * flight and can be repeated. */
    {
        NativeMaliAbiWaitFence wait{fence, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnTimeout);
        CHECK(rig.jobs.outstanding(0));
        CHECK(!rig.jobs.fenceSignaled(NativeMaliFence{0, 1}));
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 0 && status.resets == 0);
    }

    /* DONE reaps, and a second wait on the same fence is answered from the
     * slot's completed seqno without another poll. */
    rig.latchDone(0);
    {
        NativeMaliAbiWaitFence wait{fence, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnSuccess);
        CHECK(rig.jobs.fenceSignaled(NativeMaliFence{0, 1}));
        CHECK(rig.client.waitFence(wait) == kIOReturnSuccess);
    }

    /* ERR also reaps, and a fault (exception above 0x3f) advances the fault
     * counter. */
    const uint64_t errored = rig.submitAt();   /* slot 1 */
    CHECK(fenceSlot(errored) == 1);
    rig.latchError(1, 0x40);
    {
        NativeMaliAbiWaitFence wait{errored, 1000};
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
        CHECK(rig.jobs.faulted(1));
        CHECK(rig.jobs.fenceSignaled(NativeMaliFence{1, 1}));
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 1);
        CHECK(status.resets == 0);
    }

    /* An ERR that is not a fault (MAX_NON_FAULT and below) still reaps and is
     * still IOError, but is not a lost context. */
    const uint64_t stopped = rig.submitAt();   /* slot 2 */
    CHECK(fenceSlot(stopped) == 2);
    rig.latchError(2, 0x1);
    {
        NativeMaliAbiWaitFence wait{stopped, 1000};
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 1);
    }

    /* Faulted slots are skipped; slot 0 is free again and its seqno moves. */
    const uint64_t next = rig.submitAt();
    CHECK(fenceSlot(next) == 0);
    CHECK(fenceSeqno(next) == 2);
    rig.finish(next);

    /* The service reports a GPU reset; that is the context-loss counter. */
    rig.client.noteReset();
    {
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 1);
        CHECK(status.resets == 1);
    }
}

void testCloseAndRetire()
{
    Rig rig;
    rig.openOrDie();
    const uint64_t handle = rig.allocAt(0x10000);

    /* A live object blocks close; retiring it and reaping the job clears the
     * way. */
    CHECK(rig.client.close() == kIOReturnBusy);
    const uint64_t fence = rig.submitAt();
    /* Retire the object while the job is in flight: the table does not track
     * jobs, so this succeeds, and close() is the last line of defence. */
    CHECK(rig.client.freeBo(NativeMaliAbiBoFree{handle}) == kIOReturnSuccess);
    CHECK(rig.client.liveObjects() == 0);
    CHECK(rig.client.close() == kIOReturnBusy);
    CHECK(rig.client.open());

    rig.latchDone(NativeMaliAbiValidate::fenceSlot(fence));
    {
        NativeMaliAbiWaitFence wait{fence, 1000};
        CHECK(rig.client.waitFence(wait) == kIOReturnSuccess);
    }
    CHECK(rig.client.close() == kIOReturnSuccess);
    CHECK(!rig.client.open());
    CHECK(rig.source.outstanding() == 0);

    /* Closed means closed for every entry point. */
    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocReply{0xdead};
    CHECK(rig.client.allocBo(alloc, &allocReply) == kIOReturnNotReady);
    CHECK(allocReply.handle == 0);
    NativeMaliAbiQuery query{};
    query.abi_version = NativeMaliAbiVersion;
    NativeMaliAbiQuery queryReply{};
    CHECK(rig.client.query(query, &queryReply) == kIOReturnNotReady);
    NativeMaliAbiDeviceInfo info{};
    CHECK(rig.client.deviceInfo(&info) == kIOReturnNotReady);
    NativeMaliAbiStatus status{};
    CHECK(rig.client.status(&status) == kIOReturnNotReady);
    CHECK(rig.client.close() == kIOReturnNotReady);
    CHECK(rig.client.releaseAll() == kIOReturnNotReady);

    /* A reset after close is not counted into a reopened client: open() starts
     * the counters again. */
    rig.client.noteReset();
    CHECK(rig.openClient() == kIOReturnSuccess);
    CHECK(rig.client.status(&status) == kIOReturnSuccess);
    CHECK(status.faults == 0 && status.resets == 0);
}

void testReleaseRollback()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);
    rig.allocAt(0x20000);

    /* An outstanding job blocks teardown and nothing is freed: the chain could
     * still read those pages. */
    const uint64_t fence = rig.submitAt();
    {
        CHECK(rig.client.releaseAll(0) == kIOReturnTimeout);
        CHECK(rig.client.open());
        CHECK(rig.client.liveObjects() == 2);
        CHECK(rig.source.outstanding() == 2);
        CHECK(rig.jobs.outstanding(NativeMaliAbiValidate::fenceSlot(fence)));
    }

    /* Once it is done, teardown waits for it, then unmaps. A failed flush
     * leaves the first object live (the leaves are clear, so the retry repeats
     * only the flush) and the second untouched. */
    rig.latchDone(NativeMaliAbiValidate::fenceSlot(fence));
    rig.registers.failWriteAt = AS_COMMAND(rig.as);
    {
        CHECK(rig.client.releaseAll() == kIOReturnIOError);
        CHECK(rig.client.open());
        CHECK(rig.client.liveObjects() == 2);
        CHECK(rig.source.outstanding() == 2);
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x10000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);
        CHECK(rig.vm.descriptor(0x20000, &leaf) == kIOReturnSuccess);
        CHECK(leaf != 0);
    }

    rig.registers.failWriteAt = -1;
    CHECK(rig.client.releaseAll() == kIOReturnSuccess);
    CHECK(!rig.client.open());
    CHECK(rig.client.liveObjects() == 0);
    CHECK(rig.source.outstanding() == 0);
    {
        uint64_t leaf = 0xdead;
        CHECK(rig.vm.descriptor(0x20000, &leaf) == kIOReturnSuccess);
        CHECK(leaf == 0);
    }

    /* A job of another client on the shared slot is not this client's to wait
     * for: teardown must not block on it. */
    {
        Rig shared;
        shared.openOrDie();
        shared.foreign(2);
        CHECK(shared.client.releaseAll() == kIOReturnSuccess);
        CHECK(!shared.client.open());
        /* The foreign job is still the other client's. */
        CHECK(shared.jobs.outstanding(2));
        shared.reapForeign(2);
    }
}

void testReset()
{
    Rig rig;
    rig.openOrDie();
    rig.allocAt(0x10000);

    const uint64_t lost = rig.submitAt();   /* slot 0, seqno 1 */
    CHECK(fenceSlot(lost) == 0);
    CHECK(rig.jobs.latestSeqno(0) > rig.jobs.completedSeqno(0));

    /* The service sees the fault and resets. noteReset() remembers the seqno
     * that was in flight; waiting on it is an error fence (EIO in panfrost),
     * not a fault and not a timeout. */
    rig.client.noteReset();
    {
        const size_t writes = rig.registers.writes.size();
        NativeMaliAbiWaitFence wait{lost, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
        CHECK(rig.registers.writes.size() == writes);
        /* Repeating does not make it succeed or hang. */
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 0);   /* a reset is not a job fault */
        CHECK(status.resets == 1);
    }

    /* The reset destroyed the job; the bring-up/slot path drops the state. A
     * dependency on the dead fence is refused, not silently submitted. */
    rig.jobs.abandonOutstanding();
    CHECK(!rig.jobs.outstanding(0));
    {
        NativeMaliAbiSubmitReply reply{0xdead};
        CHECK(rig.submitWaiting(lost, 0, &reply) == kIOReturnIOError);
        CHECK(reply.fence == 0);
    }

    /* Every slot is usable again; the seqnos continue rather than rewind. */
    const uint64_t next = rig.submitAt();
    CHECK(fenceSlot(next) == 1);
    rig.finish(next);
    const uint64_t third = rig.submitAt();
    CHECK(fenceSlot(third) == 2);
    rig.finish(third);
    const uint64_t back = rig.submitAt();
    CHECK(fenceSlot(back) == 0);
    CHECK(fenceSeqno(back) == 2);
    rig.finish(back);

    /* Reusing the slot does not revive the lost fence: seqnos are monotonic,
     * so the dead marker can only ever match that one job. */
    {
        NativeMaliAbiWaitFence wait{lost, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
    }

    /* A reset after that job was reaped kills nothing: the fence is answered
     * from the completed seqno and no dead marker is left. */
    rig.client.noteReset();
    {
        NativeMaliAbiWaitFence wait{back, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnSuccess);
    }
    {
        NativeMaliAbiStatus status{};
        CHECK(rig.client.status(&status) == kIOReturnSuccess);
        CHECK(status.faults == 0);
        CHECK(status.resets == 2);
    }
    /* That reset killed nothing, so it must not erase the older dead marker:
     * the destroyed fence is still an error, never Success. */
    {
        NativeMaliAbiWaitFence wait{lost, 0};
        CHECK(rig.client.waitFence(wait) == kIOReturnIOError);
    }

    /* Teardown after a reset does not wait for a job the reset destroyed. */
    CHECK(rig.client.releaseAll() == kIOReturnSuccess);
    CHECK(!rig.client.open());
    CHECK(rig.source.outstanding() == 0);

    /* Order does not matter: latest > completed survives abandonOutstanding(),
     * so a noteReset() after the state drop still marks the lost fence. */
    {
        Rig late;
        late.openOrDie();
        late.allocAt(0x10000);
        const uint64_t fence = late.submitAt();
        late.jobs.abandonOutstanding();
        late.client.noteReset();
        NativeMaliAbiWaitFence wait{fence, 0};
        CHECK(late.client.waitFence(wait) == kIOReturnIOError);
        CHECK(late.client.releaseAll() == kIOReturnSuccess);
    }
}

} // namespace

/* The pure external-method half of the future IOUserClient: each selector has
 * exactly one input and one output record, and a refusal cannot leave a
 * believable reply behind. */
bool everyByte(const void *buffer, size_t size, uint8_t value)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    for(size_t index = 0; index < size; ++index)
        if(bytes[index] != value)return false;
    return true;
}

void testDispatch()
{
    typedef NativeMaliUserClient Client;

    /* The table is the ABI: every selector, both sizes, and one past the end
     * is unsupported. */
    CHECK(Client::inputSize(NativeMaliMethodQuery) ==
        sizeof(NativeMaliAbiQuery));
    CHECK(Client::outputSize(NativeMaliMethodQuery) ==
        sizeof(NativeMaliAbiQuery));
    CHECK(Client::inputSize(NativeMaliMethodGetDeviceInfo) == 0);
    CHECK(Client::outputSize(NativeMaliMethodGetDeviceInfo) ==
        sizeof(NativeMaliAbiDeviceInfo));
    CHECK(Client::inputSize(NativeMaliMethodBoAlloc) ==
        sizeof(NativeMaliAbiBoAlloc));
    CHECK(Client::outputSize(NativeMaliMethodBoAlloc) ==
        sizeof(NativeMaliAbiBoAllocReply));
    CHECK(Client::inputSize(NativeMaliMethodBoMap) == sizeof(NativeMaliAbiBoMap));
    CHECK(Client::outputSize(NativeMaliMethodBoMap) ==
        sizeof(NativeMaliAbiBoMapReply));
    CHECK(Client::inputSize(NativeMaliMethodBoFree) ==
        sizeof(NativeMaliAbiBoFree));
    CHECK(Client::outputSize(NativeMaliMethodBoFree) == 0);
    CHECK(Client::inputSize(NativeMaliMethodBoCpuMap) ==
        sizeof(NativeMaliAbiBoCpuMap));
    CHECK(Client::outputSize(NativeMaliMethodBoCpuMap) ==
        sizeof(NativeMaliAbiBoCpuMapReply));
    CHECK(Client::inputSize(NativeMaliMethodBoCpuUnmap) ==
        sizeof(NativeMaliAbiBoCpuUnmap));
    CHECK(Client::outputSize(NativeMaliMethodBoCpuUnmap) == 0);
    CHECK(Client::inputSize(NativeMaliMethodSubmit) ==
        sizeof(NativeMaliAbiSubmit));
    CHECK(Client::outputSize(NativeMaliMethodSubmit) ==
        sizeof(NativeMaliAbiSubmitReply));
    CHECK(Client::inputSize(NativeMaliMethodWaitFence) ==
        sizeof(NativeMaliAbiWaitFence));
    CHECK(Client::outputSize(NativeMaliMethodWaitFence) == 0);
    CHECK(Client::inputSize(NativeMaliMethodGetStatus) == 0);
    CHECK(Client::outputSize(NativeMaliMethodGetStatus) ==
        sizeof(NativeMaliAbiStatus));
    /* Reset has no records: the selector is the whole request. */
    CHECK(Client::inputSize(NativeMaliMethodReset) == 0);
    CHECK(Client::outputSize(NativeMaliMethodReset) == 0);
    CHECK(Client::inputSize(0) == Client::kUnsupportedSize);
    CHECK(Client::outputSize(0) == Client::kUnsupportedSize);
    CHECK(Client::inputSize(NativeMaliMethodLast + 1) ==
        Client::kUnsupportedSize);
    CHECK(Client::outputSize(NativeMaliMethodLast + 1) ==
        Client::kUnsupportedSize);

    Rig rig;
    rig.openOrDie(NATIVE_MALI_ABI_MAX_BO_BYTES);

    /* Unknown selector: unsupported, and not one output byte changes. */
    uint8_t canary[sizeof(NativeMaliAbiDeviceInfo)];
    memset(canary, 0xa5, sizeof(canary));
    CHECK(rig.client.dispatch(0, nullptr, 0, canary, sizeof(canary)) ==
        kIOReturnUnsupported);
    CHECK(rig.client.dispatch(NativeMaliMethodLast + 1, nullptr, 0, canary,
        sizeof(canary)) == kIOReturnUnsupported);
    CHECK(everyByte(canary, sizeof(canary), 0xa5));

    /* Reset carries no records and goes to the device-wide target the service
     * injected; its error is returned unchanged and a wrong size never reaches
     * the target. */
    CHECK(rig.reset.calls == 0);
    CHECK(rig.client.dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
        kIOReturnSuccess);
    CHECK(rig.reset.calls == 1);
    CHECK(rig.client.dispatch(NativeMaliMethodReset, nullptr, 1, nullptr, 0) ==
        kIOReturnBadArgument);
    CHECK(rig.client.dispatch(NativeMaliMethodReset, nullptr, 0, canary,
        sizeof(canary)) == kIOReturnBadArgument);
    CHECK(rig.reset.calls == 1);
    rig.reset.result = kIOReturnIOError;
    CHECK(rig.client.dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
        kIOReturnIOError);
    CHECK(rig.reset.calls == 2);
    rig.reset.result = kIOReturnSuccess;

    /* Sizes are exact in both directions, and a required record cannot be
     * null. Every refusal leaves the output untouched. */
    CHECK(rig.client.dispatch(NativeMaliMethodGetDeviceInfo, nullptr, 1, canary,
        sizeof(canary)) == kIOReturnBadArgument);
    CHECK(rig.client.dispatch(NativeMaliMethodGetDeviceInfo, nullptr, 0, canary,
        sizeof(canary) - 1) == kIOReturnBadArgument);
    CHECK(everyByte(canary, sizeof(canary), 0xa5));

    /* One byte too large in either direction is refused as well: the check is
     * an exact size, not a minimum. */
    {
        uint8_t big[sizeof(NativeMaliAbiDeviceInfo) + 1];
        NativeMaliAbiBoAllocReply alloc_reply;
        memset(big, 0xa5, sizeof(big));
        memset(&alloc_reply, 0xa5, sizeof(alloc_reply));
        CHECK(rig.client.dispatch(NativeMaliMethodGetDeviceInfo, nullptr, 0, big,
            sizeof(big)) == kIOReturnBadArgument);
        CHECK(rig.client.dispatch(NativeMaliMethodGetDeviceInfo, nullptr, 1, big,
            sizeof(NativeMaliAbiDeviceInfo)) == kIOReturnBadArgument);
        CHECK(rig.client.dispatch(NativeMaliMethodBoAlloc, big,
            sizeof(NativeMaliAbiBoAlloc) + 1, &alloc_reply,
            sizeof(alloc_reply)) == kIOReturnBadArgument);
        CHECK(everyByte(big, sizeof(big), 0xa5));
        CHECK(everyByte(&alloc_reply, sizeof(alloc_reply), 0xa5));
    }
    {
        NativeMaliAbiQuery request{};
        request.abi_version = NativeMaliAbiVersion;
        CHECK(rig.client.dispatch(NativeMaliMethodQuery, nullptr,
            sizeof(request), &request, sizeof(request)) ==
            kIOReturnBadArgument);
        CHECK(rig.client.dispatch(NativeMaliMethodGetStatus, nullptr, 0,
            nullptr, sizeof(NativeMaliAbiStatus)) == kIOReturnBadArgument);
    }

    /* A successful reply is exactly what the typed method returns. */
    {
        NativeMaliAbiDeviceInfo direct{};
        CHECK(rig.client.deviceInfo(&direct) == kIOReturnSuccess);
        memset(canary, 0, sizeof(canary));
        CHECK(rig.client.dispatch(NativeMaliMethodGetDeviceInfo, nullptr, 0,
            canary, sizeof(canary)) == kIOReturnSuccess);
        CHECK(memcmp(&direct, canary, sizeof(direct)) == 0);
    }

    /* Query: a good version answers the geometry, a bad one is unsupported
     * before the client is asked and leaves the reply as it was. */
    {
        NativeMaliAbiQuery request{};
        request.abi_version = NativeMaliAbiVersion;
        NativeMaliAbiQuery reply{};
        CHECK(rig.client.dispatch(NativeMaliMethodQuery, &request,
            sizeof(request), &reply, sizeof(reply)) == kIOReturnSuccess);
        CHECK(reply.abi_version == NativeMaliAbiVersion);
        CHECK(reply.input_bits == 48);
        CHECK(reply.output_bits == 40);
        CHECK(reply.flags == 0);
        CHECK(reply.max_bo_bytes == NATIVE_MALI_ABI_MAX_BO_BYTES);

        request.abi_version = NativeMaliAbiVersion + 1;
        memset(&reply, 0x5a, sizeof(reply));
        CHECK(rig.client.dispatch(NativeMaliMethodQuery, &request,
            sizeof(request), &reply, sizeof(reply)) == kIOReturnUnsupported);
        CHECK(everyByte(&reply, sizeof(reply), 0x5a));
    }

    /* A full BO lifecycle through the same entry point. */
    NativeMaliAbiBoAlloc alloc{0x4000};
    NativeMaliAbiBoAllocReply allocated{};
    CHECK(rig.client.dispatch(NativeMaliMethodBoAlloc, &alloc, sizeof(alloc),
        &allocated, sizeof(allocated)) == kIOReturnSuccess);
    CHECK(allocated.handle != 0);
    {
        NativeMaliAbiBoAlloc bad{0};
        NativeMaliAbiBoAllocReply untouched;
        memset(&untouched, 0x5a, sizeof(untouched));
        CHECK(rig.client.dispatch(NativeMaliMethodBoAlloc, &bad, sizeof(bad),
            &untouched, sizeof(untouched)) == kIOReturnBadArgument);
        CHECK(everyByte(&untouched, sizeof(untouched), 0x5a));
    }

    NativeMaliAbiBoMap map{};
    map.handle = allocated.handle;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    NativeMaliAbiBoMapReply mapped{};
    CHECK(rig.client.dispatch(NativeMaliMethodBoMap, &map, sizeof(map),
        &mapped, sizeof(mapped)) == kIOReturnSuccess);
    CHECK(mapped.iova == map.iova);

    /* A chain outside the mapping is refused, and the reply stays untouched. */
    {
        NativeMaliAbiSubmit bad{};
        bad.job_chain = 0x800000;
        NativeMaliAbiSubmitReply untouched;
        memset(&untouched, 0x5a, sizeof(untouched));
        CHECK(rig.client.dispatch(NativeMaliMethodSubmit, &bad, sizeof(bad),
            &untouched, sizeof(untouched)) == kIOReturnBadArgument);
        CHECK(everyByte(&untouched, sizeof(untouched), 0x5a));
    }

    NativeMaliAbiSubmit submit{};
    submit.job_chain = 0x10000;
    NativeMaliAbiSubmitReply submitted{};
    CHECK(rig.client.dispatch(NativeMaliMethodSubmit, &submit, sizeof(submit),
        &submitted, sizeof(submitted)) == kIOReturnSuccess);
    CHECK(submitted.fence != 0);

    NativeMaliAbiBoCpuMap cpu_map{};
    cpu_map.handle = allocated.handle;
    NativeMaliAbiBoCpuMapReply cpu_mapped{};
    CHECK(rig.client.dispatch(NativeMaliMethodBoCpuMap, &cpu_map,
        sizeof(cpu_map), &cpu_mapped, sizeof(cpu_mapped)) ==
        kIOReturnSuccess);
    CHECK(cpu_mapped.token != 0);
    CHECK(cpu_mapped.bytes == alloc.bytes);
    {
        NativeMaliAbiBoCpuUnmap cpu_unmap{};
        cpu_unmap.handle = allocated.handle;
        cpu_unmap.token = cpu_mapped.token;
        CHECK(rig.client.dispatch(NativeMaliMethodBoCpuUnmap, &cpu_unmap,
            sizeof(cpu_unmap), nullptr, 0) == kIOReturnSuccess);
    }

    /* WaitFence has no output record; latch the completion first. */
    rig.latchDone(fenceSlot(submitted.fence));
    {
        NativeMaliAbiWaitFence wait{submitted.fence, 0};
        CHECK(rig.client.dispatch(NativeMaliMethodWaitFence, &wait,
            sizeof(wait), nullptr, 0) == kIOReturnSuccess);
    }

    /* A reset that killed nothing still bumps the counter the wire record must
     * carry, so a missing copy in this branch cannot pass on a zeroed reply. */
    rig.client.noteReset();

    {
        NativeMaliAbiStatus direct{};
        NativeMaliAbiStatus via{};
        CHECK(rig.client.status(&direct) == kIOReturnSuccess);
        CHECK(direct.resets == 1);
        CHECK(rig.client.dispatch(NativeMaliMethodGetStatus, nullptr, 0, &via,
            sizeof(via)) == kIOReturnSuccess);
        CHECK(memcmp(&direct, &via, sizeof(direct)) == 0);
        CHECK(via.faults == 0);
        CHECK(via.resets == 1);
    }

    NativeMaliAbiBoFree free_request{};
    free_request.handle = allocated.handle;
    CHECK(rig.client.dispatch(NativeMaliMethodBoFree, &free_request,
        sizeof(free_request), nullptr, 0) == kIOReturnSuccess);

    /* Not open: the typed method's NotReady is returned, and the reply is still
     * untouched. */
    {
        NativeMaliUserClient cold;
        NativeMaliAbiStatus untouched;
        memset(&untouched, 0x5a, sizeof(untouched));
        CHECK(cold.dispatch(NativeMaliMethodGetStatus, nullptr, 0, &untouched,
            sizeof(untouched)) == kIOReturnNotReady);
        CHECK(everyByte(&untouched, sizeof(untouched), 0x5a));
        /* Closed means no method runs, Reset included; the target is not
         * touched. */
        CHECK(cold.dispatch(NativeMaliMethodReset, nullptr, 0, nullptr, 0) ==
            kIOReturnNotReady);
    }

    /* A client opened without a reset target cannot perform the device-wide
     * method: Unsupported, not a silent success. */
    {
        Rig bare;
        CHECK(bare.client.open(&bare.source, &bare.cache, false, &bare.vm,
            &bare.mmu, bare.as, &bare.jobs, &bare.gpu, bare.observation, 0,
            nullptr) == kIOReturnSuccess);
        CHECK(bare.client.dispatch(NativeMaliMethodReset, nullptr, 0, nullptr,
            0) == kIOReturnUnsupported);
        CHECK(bare.client.close() == kIOReturnSuccess);
    }
}

int main()
{
    testOpenAndInfo();
    testBoForwarding();
    testSubmitRefusals();
    testSlotChoice();
    testFenceOwnership();
    testDependency();
    testCompletion();
    testCloseAndRetire();
    testReleaseRollback();
    testReset();
    testDispatch();
    std::puts("PASS: user client open/Query/GetDeviceInfo, BO forwarding, chain "
        "coverage, round-robin slot choice with flush-id read, fence ownership, "
        "dependency wait, DONE/ERR/timeout reaping with fault counting, "
        "close and teardown that wait and roll back, a GPU reset that turns an "
        "in-flight fence into an error fence without touching the slot, and the "
        "external-method dispatch with exact record sizes and untouched outputs");
    return 0;
}
