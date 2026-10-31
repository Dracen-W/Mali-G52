/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliGpuCore against fake registers.
 *
 * The feature words below are synthetic. This device has not reported real
 * feature values yet (the M1/M2 probes still need a booted XNU), and inventing
 * "plausible" hardware numbers would be exactly the kind of guess this project
 * forbids. What the test proves is the plumbing: the gate is checked before the
 * GPU is touched, each raw word lands in the right field, the 64-bit present
 * bitmaps are assembled from LO/HI, the counts are popcounts, and no write
 * happens. The real values will be recorded from probe() output.
 *
 * The reset and the power-on/off sequences are checked against the exact write
 * order of panfrost_gpu_soft_reset(), panfrost_gpu_power_on() and
 * panfrost_gpu_power_off(); FakeGpu models the READY/PWRTRANS handshake with a
 * configurable number of polls, so the timeouts and the rollback run without
 * hardware. */
#include "FakeMaliPlatform.h"
#include "NativeMaliGpuCore.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;

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
    gpu.memory[GPU_JS_PRESENT / 4] = 0x5;         /* slots 0 and 2 */
    gpu.memory[GPU_JS_FEATURES(0) / 4] = 0x1111;
    gpu.memory[GPU_JS_FEATURES(1) / 4] = 0x2222;
    gpu.memory[GPU_SHADER_PRESENT_LO / 4] = 0x0000000F;
    gpu.memory[GPU_SHADER_PRESENT_HI / 4] = 0x00000001;   /* 5 cores */
    gpu.memory[GPU_TILER_PRESENT_LO / 4] = 0x3;
    gpu.memory[GPU_TILER_PRESENT_HI / 4] = 0;             /* 2 tilers */
    gpu.memory[GPU_L2_PRESENT_LO / 4] = 0x1;
    gpu.memory[GPU_L2_PRESENT_HI / 4] = 0x2;              /* 2 L2, 2 groups */
    gpu.memory[GPU_STACK_PRESENT_LO / 4] = 0xF0;
    gpu.memory[GPU_STACK_PRESENT_HI / 4] = 0;             /* 4 stacks */
    gpu.memory[GPU_THREAD_TLS_ALLOC / 4] = 64;
}

/* Power sequencing only writes the LO halves, so these tests use present
 * bitmaps that fit in 32 bits. The feature words stay synthetic for the same
 * reason as fillGpu(). */
void fillPowerGpu(FakeRegisterBlock &gpu, uint32_t l2 = 0x1,
    uint32_t shader = 0xF, uint32_t tiler = 0x3)
{
    fillGpu(gpu);
    gpu.memory[GPU_L2_PRESENT_LO / 4] = l2;
    gpu.memory[GPU_L2_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_SHADER_PRESENT_LO / 4] = shader;
    gpu.memory[GPU_SHADER_PRESENT_HI / 4] = 0;
    gpu.memory[GPU_TILER_PRESENT_LO / 4] = tiler;
    gpu.memory[GPU_TILER_PRESENT_HI / 4] = 0;
}

struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeGpu gpu;
    NativeMaliPower power;
    NativeMaliGpuCore core;

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
        CHECK(core.prepare(&gpu, &power) == kIOReturnSuccess);
    }
};

void testPrepare()
{
    FakeRegisterBlock gpu;
    NativeMaliGpuCore core;
    NativeMaliPower lonely;

    CHECK(core.prepare(nullptr, &lonely) == kIOReturnBadArgument);
    CHECK(core.prepare(&gpu, nullptr) == kIOReturnBadArgument);
    CHECK(core.prepare(&gpu, &lonely) == kIOReturnNotReady);
    CHECK(!core.prepared());

    Rig rig;
    CHECK(rig.core.prepare(&rig.gpu, &rig.power) == kIOReturnSuccess);
    CHECK(rig.core.prepared());
    FakeRegisterBlock other_gpu;
    CHECK(rig.core.prepare(&other_gpu, &rig.power) == kIOReturnBusy);
    NativeMaliPower other_power;
    CHECK(rig.core.prepare(&rig.gpu, &other_power) == kIOReturnBusy);
}

void testProbe()
{
    Rig rig;
    fillGpu(rig.gpu);
    NativeMaliGpuObservation seen;
    CHECK(rig.core.probe(&seen) == kIOReturnSuccess);

    CHECK(seen.gpuId == 0x70021000);
    CHECK(seen.identity.product == 0x7002);
    CHECK(seen.identity.revision == 0x1000);
    CHECK(seen.identity.major == 1 && seen.identity.minor == 0 &&
        seen.identity.status == 0);
    CHECK(seen.identity.bifrost == 1);
    CHECK(seen.identity.matched == 0x7002);
    CHECK(seen.identity.model && seen.identity.model[0] == 'g' &&
        seen.identity.model[1] == '5' && seen.identity.model[2] == '2');
    CHECK(seen.identity.revision_specific == 0);

    CHECK(seen.l2Features == 0x01234567);
    CHECK(seen.coreFeatures == 0x89ABCDEF);
    CHECK(seen.tilerFeatures == 0x0F0F0F0F);
    CHECK(seen.memFeatures == 0x7);
    CHECK(seen.mmuFeatures == ((40u << 8) | 48u));
    CHECK(seen.ias == 48 && seen.oas == 40);
    CHECK(seen.threadFeatures == 3);
    CHECK(seen.maxThreads == 256);
    CHECK(seen.maxWorkgroupSize == 512);
    CHECK(seen.maxBarrierSize == 768);
    CHECK(seen.asPresent == 0x7);
    CHECK(seen.jsPresent == 0x5);
    CHECK(seen.jsCount == 2);
    CHECK(seen.jsFeatures[0] == 0x1111);
    CHECK(seen.jsFeatures[1] == 0x2222);
    for(size_t i = 2; i < 16; ++i)CHECK(seen.jsFeatures[i] == 0);
    CHECK(seen.shaderPresent == ((UINT64_C(1) << 32) | 0xF));
    CHECK(seen.coreCount == 5);
    CHECK(seen.tilerPresent == 0x3);
    CHECK(seen.tilerCount == 2);
    CHECK(seen.l2Present == ((UINT64_C(2) << 32) | 0x1));
    CHECK(seen.l2Count == 2);
    CHECK(seen.coreGroups == 2);
    CHECK(seen.stackPresent == 0xF0);
    CHECK(seen.stackCount == 4);
    CHECK(seen.threadTlsAlloc == 64);

    /* Reading is the whole probe: nothing was written, and the read set is
     * exactly the registers listed in the header. */
    CHECK(rig.gpu.writes.empty());
    CHECK(rig.gpu.readCount == 23);
}

void testGateBeforeGpu()
{
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnNotReady);
        CHECK(seen.gpuId == 0);
        CHECK(rig.gpu.readCount == 0);
        CHECK(rig.gpu.writes.empty());
    }
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.top.memory[NativeMaliPower::TopMfgClockConfig / 4] =
            NativeMaliPower::TopMfgMuxGate;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnNotReady);
        CHECK(rig.gpu.readCount == 0);
    }
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.mfgcfg.memory[0] = NativeMaliPower::MfgClockGateBg3d;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnNotReady);
        CHECK(rig.gpu.readCount == 0);
    }
}

void testUnknownId()
{
    Rig rig;
    fillGpu(rig.gpu);
    rig.gpu.memory[GPU_ID / 4] = 0x12340000;
    NativeMaliGpuObservation seen;
    CHECK(rig.core.probe(&seen) == kIOReturnUnsupported);
    /* The raw words are still filled: the caller logs what the chip claimed. */
    CHECK(seen.gpuId == 0x12340000);
    CHECK(seen.identity.product == 0x1234);
    CHECK(seen.identity.model == nullptr);
    CHECK(seen.l2Features == 0x01234567);
    CHECK(seen.coreCount == 5);
    CHECK(rig.gpu.writes.empty());
}

void testReadFailures()
{
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.gpu.failReadAt = GPU_ID;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnIOError);
        CHECK(seen.gpuId == 0);
    }
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.gpu.failReadAt = GPU_L2_FEATURES;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnIOError);
        /* The id was read before the failure. */
        CHECK(seen.gpuId == 0x70021000);
        CHECK(seen.identity.product == 0x7002);
    }
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.gpu.failReadAt = GPU_JS_FEATURES(1);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnIOError);
        CHECK(seen.jsFeatures[0] == 0x1111);
    }
    {
        Rig rig;
        fillGpu(rig.gpu);
        rig.gpu.failReadAt = GPU_SHADER_PRESENT_HI;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnIOError);
        CHECK(seen.shaderPresent == 0);   /* LO alone is not published */
    }
}

void testTooManyJobSlots()
{
    Rig rig;
    fillGpu(rig.gpu);
    /* 17 bits does not fit panfrost's js_features[16]; refuse instead of
     * walking past the array or reading unrelated registers. */
    rig.gpu.memory[GPU_JS_PRESENT / 4] = 0x1FFFF;
    NativeMaliGpuObservation seen;
    CHECK(rig.core.probe(&seen) == kIOReturnUnsupported);
    CHECK(seen.jsCount == 17);
    CHECK(rig.gpu.writes.empty());
}

void testNotPrepared()
{
    NativeMaliGpuCore core;
    NativeMaliGpuObservation seen;
    CHECK(core.probe(&seen) == kIOReturnNotReady);
    Rig rig;
    CHECK(rig.core.probe(nullptr) == kIOReturnBadArgument);
}

void checkWrites(const FakeGpu &gpu, const FakeRegisterBlock::Write *expected,
    size_t count)
{
    CHECK(gpu.writes.size() == count);
    for(size_t i = 0; i < count; ++i) {
        CHECK(gpu.writes[i].offset == expected[i].offset);
        CHECK(gpu.writes[i].value == expected[i].value);
    }
}

void testSoftReset()
{
    Rig rig;
    CHECK(NativeMaliGpuCore::kWriteAuthorized);
    CHECK(rig.core.softReset() == kIOReturnSuccess);

    /* panfrost_gpu_soft_reset(), panfrost_gpu.c:62-108: mask, clear the
     * completed bit, request the reset, then clear everything and mask the
     * three sources panfrost keeps (error, perfcnt, clean-caches). */
    const FakeRegisterBlock::Write expected[] = {
        {GPU_INT_MASK, 0},
        {GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED},
        {GPU_CMD, GPU_CMD_SOFT_RESET},
        {GPU_INT_CLEAR, GPU_IRQ_MASK_ALL},
        {GPU_INT_MASK, GPU_IRQ_MASK_ERROR | GPU_IRQ_PERFCNT_SAMPLE_COMPLETED |
            GPU_IRQ_CLEAN_CACHES_COMPLETED},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.gpu.commandWrites == 1);
    /* The reset was already complete on the first RAWSTAT read. */
    CHECK(rig.gpu.readCount == 1);
    CHECK(rig.gpu.delays.empty());
    CHECK(rig.gpu.memory[GPU_INT_MASK / 4] ==
        (GPU_IRQ_MASK_ERROR | GPU_IRQ_PERFCNT_SAMPLE_COMPLETED |
            GPU_IRQ_CLEAN_CACHES_COMPLETED));
    CHECK((rig.gpu.memory[GPU_INT_RAWSTAT / 4] & GPU_IRQ_RESET_COMPLETED) == 0);
}

void testSoftResetPolls()
{
    Rig rig;
    rig.gpu.pollsBeforeReset = 3;
    CHECK(rig.core.softReset() == kIOReturnSuccess);
    /* Four RAWSTAT reads, three 10 us sleeps. */
    CHECK(rig.gpu.readCount == 4);
    CHECK(rig.gpu.delays.size() == 3);
    for(uint64_t delay : rig.gpu.delays)
        CHECK(delay == NativeMaliGpuCore::SoftResetPollUs);
    CHECK(rig.gpu.commandWrites == 1);
}

void testHardResetFallback()
{
    Rig rig;
    rig.gpu.softCompletes = false;
    rig.gpu.hardCompletes = true;
    rig.gpu.pollsBeforeReset = 1;   /* the hard reset needs two reads */
    CHECK(rig.core.softReset(20) == kIOReturnSuccess);

    const FakeRegisterBlock::Write expected[] = {
        {GPU_INT_MASK, 0},
        {GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED},
        {GPU_CMD, GPU_CMD_SOFT_RESET},
        {GPU_CMD, GPU_CMD_HARD_RESET},
        {GPU_INT_CLEAR, GPU_IRQ_MASK_ALL},
        {GPU_INT_MASK, GPU_IRQ_MASK_ERROR | GPU_IRQ_PERFCNT_SAMPLE_COMPLETED |
            GPU_IRQ_CLEAN_CACHES_COMPLETED},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.gpu.commandWrites == 2);
    /* Soft polls at 10 us, hard polls at 100 us. */
    CHECK(rig.gpu.delays.size() == 3);
    CHECK(rig.gpu.delays[0] == NativeMaliGpuCore::SoftResetPollUs);
    CHECK(rig.gpu.delays[1] == NativeMaliGpuCore::SoftResetPollUs);
    CHECK(rig.gpu.delays[2] == NativeMaliGpuCore::HardResetPollUs);
}

void testHardResetTimesOut()
{
    Rig rig;
    rig.gpu.softCompletes = false;
    rig.gpu.hardCompletes = false;
    /* Zero budget: one RAWSTAT read per attempt, no sleeps, so this stays
     * fast while still exercising the same code path. */
    CHECK(rig.core.softReset(0) == kIOReturnTimeout);

    const FakeRegisterBlock::Write expected[] = {
        {GPU_INT_MASK, 0},
        {GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED},
        {GPU_CMD, GPU_CMD_SOFT_RESET},
        {GPU_CMD, GPU_CMD_HARD_RESET},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.gpu.commandWrites == 2);
    CHECK(rig.gpu.delays.empty());
    /* The interrupt mask is left cleared: the caller must stop, not continue
     * as if the block had reset. */
    CHECK(rig.gpu.memory[GPU_INT_MASK / 4] == 0);
}

void testResetRefusals()
{
    {
        Rig rig;
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        CHECK(rig.core.softReset() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
        CHECK(rig.gpu.readCount == 0);
    }
    {
        Rig rig;
        rig.mfgcfg.memory[0] = NativeMaliPower::MfgClockGateBg3d;
        CHECK(rig.core.softReset() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
    }
    {
        NativeMaliGpuCore core;
        CHECK(core.softReset() == kIOReturnNotReady);
    }
}

void testResetFailures()
{
    {
        Rig rig;
        rig.gpu.failWriteAt = GPU_INT_MASK;
        CHECK(rig.core.softReset() == kIOReturnIOError);
        CHECK(rig.gpu.writes.size() == 1);
        CHECK(rig.gpu.commandWrites == 0);
    }
    {
        Rig rig;
        rig.gpu.failWriteAt = GPU_INT_CLEAR;
        CHECK(rig.core.softReset() == kIOReturnIOError);
        CHECK(rig.gpu.writes.size() == 2);
        CHECK(rig.gpu.commandWrites == 0);
    }
    {
        Rig rig;
        rig.gpu.failWriteAt = GPU_CMD;
        CHECK(rig.core.softReset() == kIOReturnIOError);
        CHECK(rig.gpu.commandWrites == 0);
    }
    {
        Rig rig;
        rig.gpu.failReadAt = GPU_INT_RAWSTAT;
        CHECK(rig.core.softReset() == kIOReturnIOError);
        CHECK(rig.gpu.commandWrites == 1);
    }
    {
        Rig rig;
        rig.gpu.softCompletes = false;
        rig.gpu.delayResult = kIOReturnNoResources;
        CHECK(rig.core.softReset(100) == kIOReturnNoResources);
        CHECK(rig.gpu.commandWrites == 1);
    }
}

void testReadFault()
{
    {
        Rig rig;
        fillPowerGpu(rig.gpu);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
        rig.gpu.memory[GPU_FAULT_STATUS / 4] = 0x1234C2;
        rig.gpu.memory[GPU_FAULT_ADDRESS_LO / 4] = 0xFFFFF000;
        rig.gpu.memory[GPU_FAULT_ADDRESS_HI / 4] = 0x00000001;
        rig.gpu.readCount = 0;

        NativeMaliGpuFault fault;
        CHECK(rig.core.readFault(&fault) == kIOReturnSuccess);
        /* panfrost_gpu.c:32-35: status first, then HI, then LO. */
        CHECK(rig.gpu.readCount == 3);
        CHECK(fault.status == 0x1234C2);
        CHECK(fault.exception() == 0xC2);
        CHECK(fault.addressHi == 0x1);
        CHECK(fault.addressLo == 0xFFFFF000);
        CHECK(fault.address() == UINT64_C(0x1FFFFF000));
        CHECK(fault.isFault());
        CHECK(std::strcmp(NativeMaliFault::exceptionName(fault.exception()),
            "TRANSLATION_FAULT_2") == 0);
        /* Reading a fault must not acknowledge it or touch anything else. */
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* The M2 gate wins over everything: not even the fault registers. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        rig.gpu.readCount = 0;
        NativeMaliGpuFault fault;
        CHECK(rig.core.readFault(&fault) == kIOReturnNotReady);
        CHECK(rig.gpu.readCount == 0);
    }
    {
        /* Fields read before a failure are kept, as in probe(). */
        Rig rig;
        fillPowerGpu(rig.gpu);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
        rig.gpu.memory[GPU_FAULT_STATUS / 4] = 0x40;
        rig.gpu.memory[GPU_FAULT_ADDRESS_HI / 4] = 0x2;
        rig.gpu.failReadAt = GPU_FAULT_ADDRESS_LO;
        NativeMaliGpuFault fault;
        CHECK(rig.core.readFault(&fault) == kIOReturnIOError);
        CHECK(fault.status == 0x40);
        CHECK(fault.addressHi == 0x2);
        CHECK(fault.addressLo == 0);
        rig.gpu.failReadAt = -1;
    }
    {
        NativeMaliGpuCore cold;
        NativeMaliGpuFault fault;
        CHECK(cold.readFault(&fault) == kIOReturnNotReady);
        Rig rig;
        CHECK(rig.core.readFault(nullptr) == kIOReturnBadArgument);
    }
}

/* panfrost_device_reset() runs the reset and then the power-on; do the same and
 * forget the reset writes, so each power test sees only its own sequence. */
void armPower(Rig &rig){
    NativeMaliGpuObservation seen;
    CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
    CHECK(rig.core.softReset() == kIOReturnSuccess);
    rig.gpu.writes.clear();
    rig.gpu.readCount = 0;
    rig.gpu.delays.clear();
}

/* panfrost_gpu_get_latest_flush_id() (panfrost_gpu.c:561-573): one
 * GPU_LATEST_FLUSH_ID read while the GPU is usable. G52 has
 * HW_FEATURE_FLUSH_REDUCTION, so this is what a job's JS_FLUSH_ID_NEXT gets. */
void testLatestFlushId()
{
    {
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.memory[GPU_LATEST_FLUSH_ID / 4] = 0x0000ABCD;
        rig.gpu.readCount = 0;
        uint32_t flushId = 0xdeadbeef;
        CHECK(rig.core.latestFlushId(&flushId) == kIOReturnSuccess);
        CHECK(flushId == 0x0000ABCD);
        CHECK(rig.gpu.readCount == 1);
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* The gate wins over the register, and the failure is returned rather
         * than the source's 0, which a caller could not tell from a real id. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        rig.gpu.readCount = 0;
        uint32_t flushId = 0xdeadbeef;
        CHECK(rig.core.latestFlushId(&flushId) == kIOReturnNotReady);
        CHECK(flushId == 0);
        CHECK(rig.gpu.readCount == 0);
    }
    {
        /* A read failure is returned unchanged and leaves no id behind. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.failReadAt = GPU_LATEST_FLUSH_ID;
        uint32_t flushId = 0xdeadbeef;
        CHECK(rig.core.latestFlushId(&flushId) == kIOReturnIOError);
        CHECK(flushId == 0);
        rig.gpu.failReadAt = -1;
        CHECK(rig.core.latestFlushId(&flushId) == kIOReturnSuccess);
    }
    {
        NativeMaliGpuCore cold;
        uint32_t flushId = 0xdeadbeef;
        CHECK(cold.latestFlushId(&flushId) == kIOReturnNotReady);
        CHECK(flushId == 0);
        Rig rig;
        CHECK(rig.core.latestFlushId(nullptr) == kIOReturnBadArgument);
    }
}

void testPowerOn()
{
    Rig rig;
    fillPowerGpu(rig.gpu);
    armPower(rig);

    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    /* panfrost_gpu_power_on(), panfrost_gpu.c:455-484: L2, shader, tiler, each
     * PWRON with the present mask (shader and L2 narrowed to the first core
     * group). No tiler masking in the source. */
    const FakeRegisterBlock::Write expected[] = {
        {GPU_JM_CONFIG, 0x000F0000},
        {L2_PWRON_LO, 0x1},
        {SHADER_PWRON_LO, 0xF},
        {TILER_PWRON_LO, 0x3},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    /* Each READY already equalled its mask on the first read. */
    CHECK(rig.gpu.readCount == 3);
    CHECK(rig.gpu.delays.empty());
    CHECK(rig.gpu.memory[L2_READY_LO / 4] == 0x1);
    CHECK(rig.gpu.memory[SHADER_READY_LO / 4] == 0xF);
    CHECK(rig.gpu.memory[TILER_READY_LO / 4] == 0x3);

    /* Nothing is written after the last READY poll: the quirk write happens
     * before the first PWRON, as in the source. */
    const size_t writesAfterPower = rig.gpu.writes.size();
    CHECK(writesAfterPower == 4);
}

void testPowerOnFirstCoreGroup()
{
    /* Two L2s mean two core groups; panfrost_get_core_mask() keeps the first,
     * so only bit 0 of L2/shader is powered. The tiler bitmap is not narrowed
     * (panfrost_gpu.c:475). */
    Rig rig;
    fillPowerGpu(rig.gpu, 0x3, 0xF, 0x3);
    armPower(rig);

    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    const FakeRegisterBlock::Write expected[] = {
        {GPU_JM_CONFIG, 0x000F0000},
        {L2_PWRON_LO, 0x1},
        {SHADER_PWRON_LO, 0x1},
        {TILER_PWRON_LO, 0x3},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
}

void testPowerOnPolls()
{
    Rig rig;
    fillPowerGpu(rig.gpu);
    armPower(rig);
    rig.gpu.readyPolls = 2;

    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    /* Three READY registers, each needing three reads and two waits. The
     * source polls at 10 us for L2 and shader (panfrost_gpu.c:462,470) and
     * 10 us for the tiler (panfrost_gpu.c:478). */
    CHECK(rig.gpu.readCount == 9);
    CHECK(rig.gpu.delays.size() == 6);
    for(uint64_t delay : rig.gpu.delays)
        CHECK(delay == 10);
}

void testPowerRefusals()
{
    {
        /* No probe: the masks are unknown. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
        CHECK(rig.gpu.readCount == 0);
    }
    {
        /* Probe without a reset: panfrost always resets before powering on. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* A failed re-probe throws away the masks from the earlier one. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.failReadAt = GPU_L2_FEATURES;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnIOError);
        rig.gpu.failReadAt = -1;
        rig.gpu.writes.clear();
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* An id outside the pinned table never yields power masks. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        rig.gpu.memory[GPU_ID / 4] = 0x12345678;
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnUnsupported);
        CHECK(rig.core.softReset() == kIOReturnSuccess);
        rig.gpu.writes.clear();
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* The M2 gate closes after the reset: nothing may be written. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
        CHECK(rig.gpu.readCount == 0);
    }
    {
        /* Bits above 31 have no LO register: refuse instead of powering a
         * partial GPU. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        rig.gpu.memory[GPU_SHADER_PRESENT_HI / 4] = 1;
        armPower(rig);
        CHECK(rig.core.powerOn() == kIOReturnUnsupported);
        CHECK(rig.gpu.writes.empty());
        CHECK(rig.core.powerOff() == kIOReturnUnsupported);
        CHECK(rig.gpu.writes.empty());
    }
    {
        /* An empty present bitmap is a broken probe, not "no cores". */
        Rig rig;
        fillPowerGpu(rig.gpu, 0x1, 0x0, 0x3);
        armPower(rig);
        CHECK(rig.core.powerOn() == kIOReturnUnsupported);
        CHECK(rig.gpu.writes.empty());
    }
    {
        Rig rig;
        fillPowerGpu(rig.gpu, 0x0, 0xF, 0x3);
        armPower(rig);
        CHECK(rig.core.powerOn() == kIOReturnUnsupported);
        CHECK(rig.gpu.writes.empty());
    }
    {
        NativeMaliGpuCore cold;
        CHECK(cold.powerOn() == kIOReturnNotReady);
        CHECK(cold.powerOff() == kIOReturnNotReady);
    }
}

void testPowerOnRollback()
{
    {
        /* The shader never reports ready: L2 is switched back off. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.shaderCompletes = false;
        CHECK(rig.core.powerOn() == kIOReturnTimeout);
        const FakeRegisterBlock::Write expected[] = {
            {GPU_JM_CONFIG, 0x000F0000},
            {L2_PWRON_LO, 0x1},
            {SHADER_PWRON_LO, 0xF},
            {SHADER_PWROFF_LO, 0xF},
            {TILER_PWROFF_LO, 0x3},
            {L2_PWROFF_LO, 0x1},
        };
        checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
        /* The failed sequence needs a fresh reset before another attempt. */
        rig.gpu.writes.clear();
        CHECK(rig.core.powerOn() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
        rig.gpu.shaderCompletes = true;
        CHECK(rig.core.softReset() == kIOReturnSuccess);
        CHECK(rig.core.powerOn() == kIOReturnSuccess);
    }
    {
        /* A tiler timeout still quiesces L2 and shader, in reverse order. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.tilerCompletes = false;
        CHECK(rig.core.powerOn() == kIOReturnTimeout);
        const FakeRegisterBlock::Write expected[] = {
            {GPU_JM_CONFIG, 0x000F0000},
            {L2_PWRON_LO, 0x1},
            {SHADER_PWRON_LO, 0xF},
            {TILER_PWRON_LO, 0x3},
            {SHADER_PWROFF_LO, 0xF},
            {TILER_PWROFF_LO, 0x3},
            {L2_PWROFF_LO, 0x1},
        };
        checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    }
    {
        /* The very first write fails: the quiesce still runs so the caller
         * knows every block was told to go down. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.failWriteAt = L2_PWRON_LO;
        CHECK(rig.core.powerOn() == kIOReturnIOError);
        const FakeRegisterBlock::Write expected[] = {
            {GPU_JM_CONFIG, 0x000F0000},
            {L2_PWRON_LO, 0x1},
            {SHADER_PWROFF_LO, 0xF},
            {TILER_PWROFF_LO, 0x3},
            {L2_PWROFF_LO, 0x1},
        };
        checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    }
    {
        /* A failure during the quiesce does not replace the original timeout,
         * and the remaining blocks are still told to go down. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.tilerCompletes = false;
        rig.gpu.failWriteAt = SHADER_PWROFF_LO;
        CHECK(rig.core.powerOn() == kIOReturnTimeout);
        const FakeRegisterBlock::Write expected[] = {
            {GPU_JM_CONFIG, 0x000F0000},
            {L2_PWRON_LO, 0x1},
            {SHADER_PWRON_LO, 0xF},
            {TILER_PWRON_LO, 0x3},
            {SHADER_PWROFF_LO, 0xF},
            {TILER_PWROFF_LO, 0x3},
            {L2_PWROFF_LO, 0x1},
        };
        checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    }
    {
        /* A delay error is returned unchanged (not as a timeout), after the
         * quiesce. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.readyPolls = 2;
        rig.gpu.delayResult = kIOReturnNoResources;
        CHECK(rig.core.powerOn() == kIOReturnNoResources);
        CHECK(rig.gpu.writes[0].offset == GPU_JM_CONFIG);
        CHECK(rig.gpu.writes[1].offset == L2_PWRON_LO);
        CHECK(rig.gpu.writes[2].offset == SHADER_PWROFF_LO);
    }
    {
        /* The quirk write itself fails: nothing has been powered, so nothing
         * is switched off and no PWRON is attempted. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.gpu.failWriteAt = GPU_JM_CONFIG;
        CHECK(rig.core.powerOn() == kIOReturnIOError);
        const FakeRegisterBlock::Write expected[] = {
            {GPU_JM_CONFIG, 0x000F0000},
        };
        checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
        rig.gpu.failWriteAt = -1;
        /* No reset was consumed by the quirk, so a retry works. */
        CHECK(rig.core.powerOn() == kIOReturnSuccess);
    }
}

void testPowerOff()
{
    Rig rig;
    fillPowerGpu(rig.gpu);
    armPower(rig);
    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    rig.gpu.writes.clear();
    rig.gpu.readCount = 0;
    rig.gpu.delays.clear();

    CHECK(rig.core.powerOff() == kIOReturnSuccess);
    /* panfrost_gpu_power_off(), panfrost_gpu.c:486-504: shader, tiler, L2,
     * each with the full present mask (not the core mask). */
    const FakeRegisterBlock::Write expected[] = {
        {SHADER_PWROFF_LO, 0xF},
        {TILER_PWROFF_LO, 0x3},
        {L2_PWROFF_LO, 0x1},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.gpu.readCount == 3);
    CHECK(rig.gpu.delays.empty());
    CHECK(rig.gpu.memory[SHADER_PWRTRANS_LO / 4] == 0);
    CHECK(rig.gpu.memory[TILER_PWRTRANS_LO / 4] == 0);
    CHECK(rig.gpu.memory[L2_PWRTRANS_LO / 4] == 0);

    /* Power-off means the next bring-up starts with a reset. */
    CHECK(rig.core.powerOn() == kIOReturnNotReady);
    CHECK(rig.core.softReset() == kIOReturnSuccess);
    CHECK(rig.core.powerOn() == kIOReturnSuccess);
}

void testPowerOffPolls()
{
    Rig rig;
    fillPowerGpu(rig.gpu);
    armPower(rig);
    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    rig.gpu.writes.clear();
    rig.gpu.readCount = 0;
    rig.gpu.delays.clear();
    rig.gpu.transitionPolls = 2;

    CHECK(rig.core.powerOff() == kIOReturnSuccess);
    /* Three transition registers, each three reads; shader and tiler wait 1 us
     * between reads, while the L2 poll asks for no delay at all
     * (panfrost_gpu.c:498). */
    CHECK(rig.gpu.readCount == 9);
    CHECK(rig.gpu.delays.size() == 4);
    for(uint64_t delay : rig.gpu.delays)
        CHECK(delay == 1);
}

void testPowerOffTimeout()
{
    Rig rig;
    fillPowerGpu(rig.gpu);
    armPower(rig);
    CHECK(rig.core.powerOn() == kIOReturnSuccess);
    rig.gpu.writes.clear();
    rig.gpu.transitionsSettle = false;

    /* The shader transition never clears, but tiler and L2 are still told to
     * go down; the first error is what comes back. */
    CHECK(rig.core.powerOff() == kIOReturnTimeout);
    const FakeRegisterBlock::Write expected[] = {
        {SHADER_PWROFF_LO, 0xF},
        {TILER_PWROFF_LO, 0x3},
        {L2_PWROFF_LO, 0x1},
    };
    checkWrites(rig.gpu, expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(rig.core.powerOn() == kIOReturnNotReady);
}

void testPowerOffRefusals()
{
    {
        /* Probe alone is enough to quiesce; a suspend does not need a reset. */
        Rig rig;
        fillPowerGpu(rig.gpu);
        NativeMaliGpuObservation seen;
        CHECK(rig.core.probe(&seen) == kIOReturnSuccess);
        rig.gpu.writes.clear();
        CHECK(rig.core.powerOff() == kIOReturnSuccess);
        CHECK(rig.gpu.writes.size() == 3);
    }
    {
        Rig rig;
        fillPowerGpu(rig.gpu);
        armPower(rig);
        rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
        CHECK(rig.core.powerOff() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
        CHECK(rig.gpu.readCount == 0);
    }
    {
        Rig rig;
        fillPowerGpu(rig.gpu);
        CHECK(rig.core.powerOff() == kIOReturnNotReady);
        CHECK(rig.gpu.writes.empty());
    }
}

} // namespace

int main()
{
    testPrepare();
    testProbe();
    testGateBeforeGpu();
    testUnknownId();
    testReadFailures();
    testTooManyJobSlots();
    testNotPrepared();
    testSoftReset();
    testSoftResetPolls();
    testHardResetFallback();
    testHardResetTimesOut();
    testResetRefusals();
    testResetFailures();
    testReadFault();
    testLatestFlushId();
    testPowerOn();
    testPowerOnFirstCoreGroup();
    testPowerOnPolls();
    testPowerRefusals();
    testPowerOnRollback();
    testPowerOff();
    testPowerOffPolls();
    testPowerOffTimeout();
    testPowerOffRefusals();
    std::puts("PASS: GPU feature probe behind the M2 gate, soft/hard reset order and polling, L2/shader/tiler power-on and power-off order, fault register reads, the latest flush id, timeouts, rollback and write-gate refusals");
    return 0;
}
