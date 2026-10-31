/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliJobSlot against fake registers.
 *
 * The MMU is the real NativeMaliMmu (already tested against panfrost_mmu.c) on
 * the same fake 0x13040000 window, so "the address space is allocated" is the
 * real bookkeeping and the JS registers sit where the GPU would see them. What
 * is fake is the register bank only; no hardware is touched.
 *
 * The expected sequence is panfrost_job_hw_submit() (panfrost_job.c:205-282):
 * JS_HEAD_NEXT_LO/HI, JS_AFFINITY_NEXT_LO/HI, JS_CONFIG_NEXT, JS_FLUSH_ID_NEXT
 * and finally JS_COMMAND_NEXT = START. G52 has HW_FEATURE_FLUSH_REDUCTION and
 * JOBCHAIN_DISAMBIGUATION (panfrost_features.h:73-83) and hw_issues_g52 is 0
 * (panfrost_issues.h:248), so JS_CONFIG carries the flush-reduction bit and not
 * START_MMU, and the chain flag alternates per submission. */
#include "FakeMaliPlatform.h"
#include "NativeMaliJobSlot.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;

struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    FakeMmu registers;      /* the whole 0x13040000 window: GPU, JS, MMU */
    NativeMaliPower power;
    NativeMaliMmu mmu;
    NativeMaliJobSlot slot;

    explicit Rig(uint32_t asPresent = 0x7, uint32_t jsPresent = 0x7,
        uint64_t shaderPresent = 0x1f)
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
        CHECK(slot.prepare(&registers, &power, &mmu) == kIOReturnSuccess);
        CHECK(slot.setPresent(jsPresent, shaderPresent) == kIOReturnSuccess);
    }

    uint32_t allocateAs(uint32_t *as)
    {
        CHECK(mmu.allocate(as) == kIOReturnSuccess);
        return *as;
    }

    /* Finish the job outstanding on `js` the way the hardware would, then drop
     * the completion's own writes so a test sees only what it is testing. */
    void finish(uint32_t js = 0)
    {
        registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(js);
        NativeMaliJobCompletion completion;
        CHECK(slot.waitForCompletion(js, &completion) == kIOReturnSuccess);
        registers.writes.clear();
        registers.delays.clear();
    }
};

void checkWrites(const FakeMmu &registers,
    const FakeRegisterBlock::Write *expected, size_t count)
{
    CHECK(registers.writes.size() == count);
    for(size_t i = 0; i < count; ++i) {
        CHECK(registers.writes[i].offset == expected[i].offset);
        CHECK(registers.writes[i].value == expected[i].value);
    }
}

void testPrepareAndPresent()
{
    FakeMmu registers;
    NativeMaliPower lonely;
    NativeMaliMmu mmu;
    NativeMaliJobSlot slot;

    CHECK(slot.prepare(nullptr, &lonely, &mmu) == kIOReturnBadArgument);
    CHECK(slot.prepare(&registers, nullptr, &mmu) == kIOReturnBadArgument);
    CHECK(slot.prepare(&registers, &lonely, nullptr) == kIOReturnBadArgument);
    /* Non-null but neither the power block nor the MMU is prepared. */
    CHECK(slot.prepare(&registers, &lonely, &mmu) == kIOReturnNotReady);
    CHECK(!slot.prepared());

    Rig rig;
    FakeMmu other;
    NativeMaliJobSlot cold;
    CHECK(cold.prepare(&rig.registers, &rig.power, &rig.mmu) ==
        kIOReturnSuccess);
    CHECK(cold.prepared());
    /* A second prepare with the same triple is a no-op; a different register
     * window is refused so a live slot is never moved. */
    CHECK(cold.prepare(&rig.registers, &rig.power, &rig.mmu) ==
        kIOReturnSuccess);
    CHECK(cold.prepare(&other, &rig.power, &rig.mmu) == kIOReturnBusy);
    CHECK(cold.prepare(&rig.registers, &rig.power, &rig.mmu) ==
        kIOReturnSuccess);

    /* setPresent needs prepare() first. */
    NativeMaliJobSlot fresh;
    CHECK(fresh.setPresent(0x7, 0x1f) == kIOReturnNotReady);
    CHECK(rig.slot.present() == 0x7);
    CHECK(rig.slot.shaderPresent() == 0x1f);

    /* An empty mask is a refusal, not a GPU with no slots. */
    NativeMaliJobSlot probeOnly;
    CHECK(probeOnly.prepare(&rig.registers, &rig.power, &rig.mmu) ==
        kIOReturnSuccess);
    CHECK(probeOnly.setPresent(0, 0x1f) == kIOReturnBadArgument);
    CHECK(probeOnly.setPresent(0x7, 0) == kIOReturnBadArgument);
    CHECK(probeOnly.present() == 0);
    CHECK(probeOnly.setPresent(0x7, 0x1f) == kIOReturnSuccess);
    /* A different pair afterwards would mean different hardware. */
    CHECK(probeOnly.setPresent(0x5, 0x1f) == kIOReturnBusy);
    CHECK(probeOnly.setPresent(0x7, 0x3) == kIOReturnBusy);
    CHECK(probeOnly.setPresent(0x7, 0x1f) == kIOReturnSuccess);

    /* A slot that never saw a probe refuses to start anything. */
    NativeMaliJobSlot unpresented;
    CHECK(unpresented.prepare(&rig.registers, &rig.power, &rig.mmu) ==
        kIOReturnSuccess);
    rig.registers.writes.clear();
    CHECK(unpresented.submit(0, 0x100000, 0, 0) == kIOReturnNotReady);
    CHECK(rig.registers.writes.empty());
}

/* panfrost_job_hw_submit(): cfg = as_nr | THREAD_PRI(8) | the two
 * CLEAN_INVALIDATE fields | ENABLE_FLUSH_REDUCTION, plus the chain flag.
 * G52 has both features, so the flag is the only variable part. */
void testConfigValue()
{
    const uint32_t base = 0x80000 | 0x300 | 0x3000 | 0x4000;
    CHECK(base == 0x87300);
    CHECK(NativeMaliJobSlot::configValue(0, false) == 0x87300);
    CHECK(NativeMaliJobSlot::configValue(1, false) == 0x87301);
    CHECK(NativeMaliJobSlot::configValue(0, true) == 0x87b00);
    CHECK(NativeMaliJobSlot::configValue(0xf, true) == 0x87b0f);
    /* START_MMU (1 << 10) is not in the value: hw_issues_g52 is 0. */
    CHECK((NativeMaliJobSlot::configValue(3, false) & JS_CONFIG_START_MMU) == 0);
}

void testSubmitGolden()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    CHECK(as == 0);
    rig.registers.writes.clear();
    rig.registers.readCount = 0;

    /* First submission on slot 0: emit_seqno becomes 1 (odd), so the chain flag
     * is set, as it is for panfrost's first job on a slot. Affinity is the
     * whole shader mask and the flush id is whatever the caller read. */
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0x1234) ==
        kIOReturnSuccess);
    const FakeRegisterBlock::Write first[] = {
        {JS_HEAD_NEXT_LO(0), 0x00100000},
        {JS_HEAD_NEXT_HI(0), 0},
        {JS_AFFINITY_NEXT_LO(0), 0x1f},
        {JS_AFFINITY_NEXT_HI(0), 0},
        {JS_CONFIG_NEXT(0), 0x87b00},
        {JS_FLUSH_ID_NEXT(0), 0x1234},
        {JS_COMMAND_NEXT(0), JS_COMMAND_START},
    };
    checkWrites(rig.registers, first, sizeof(first) / sizeof(first[0]));
    /* Two reads: the staged-command check and the stale-event check. */
    CHECK(rig.registers.readCount == 2);
    CHECK(rig.slot.outstanding(0));

    /* Second submission on the same slot has to wait for the first to be
     * reaped; seqno 2 is even, so no chain flag. */
    CHECK(rig.slot.submit(0, UINT64_C(0x100040), as, 0x1235) == kIOReturnBusy);
    CHECK(rig.registers.writes.size() == 7);
    rig.finish(0);
    CHECK(!rig.slot.outstanding(0));
    CHECK(rig.slot.submit(0, UINT64_C(0x100040), as, 0x1235) ==
        kIOReturnSuccess);
    const FakeRegisterBlock::Write second[] = {
        {JS_HEAD_NEXT_LO(0), 0x00100040},
        {JS_HEAD_NEXT_HI(0), 0},
        {JS_AFFINITY_NEXT_LO(0), 0x1f},
        {JS_AFFINITY_NEXT_HI(0), 0},
        {JS_CONFIG_NEXT(0), 0x87300},
        {JS_FLUSH_ID_NEXT(0), 0x1235},
        {JS_COMMAND_NEXT(0), JS_COMMAND_START},
    };
    checkWrites(rig.registers, second, sizeof(second) / sizeof(second[0]));

    /* Third: odd again. */
    rig.finish(0);
    rig.registers.writes.clear();
    CHECK(rig.slot.submit(0, UINT64_C(0x100080), as, 0) == kIOReturnSuccess);
    CHECK(rig.registers.writes[4].offset == JS_CONFIG_NEXT(0));
    CHECK(rig.registers.writes[4].value == 0x87b00);

    /* The parity is per slot: slot 1's first submission sets the flag. */
    rig.registers.writes.clear();
    CHECK(rig.slot.submit(1, UINT64_C(0x200000), as, 0) == kIOReturnSuccess);
    CHECK(rig.registers.writes[0].offset == JS_HEAD_NEXT_LO(1));
    CHECK(rig.registers.writes[1].offset == JS_HEAD_NEXT_HI(1));
    CHECK(rig.registers.writes[4].offset == JS_CONFIG_NEXT(1));
    CHECK(rig.registers.writes[4].value == 0x87b00);
    CHECK(rig.registers.writes[6].offset == JS_COMMAND_NEXT(1));

    /* A head above 4 GiB has to land in both halves of the pair. */
    rig.finish(1);
    rig.registers.writes.clear();
    CHECK(rig.slot.submit(2, UINT64_C(0x700000000), as, 0) == kIOReturnSuccess);
    CHECK(rig.registers.writes[0].offset == JS_HEAD_NEXT_LO(2));
    CHECK(rig.registers.writes[0].value == 0);
    CHECK(rig.registers.writes[1].offset == JS_HEAD_NEXT_HI(2));
    CHECK(rig.registers.writes[1].value == 7);

    /* A shader mask above 32 bits goes into the affinity pair the same way. */
    Rig wide(0x7, 0x7, UINT64_C(0x000000030000000f));
    uint32_t wideAs = 0;
    wide.allocateAs(&wideAs);
    wide.registers.writes.clear();
    CHECK(wide.slot.submit(0, UINT64_C(0x100000), wideAs, 0x9) ==
        kIOReturnSuccess);
    CHECK(wide.registers.writes[2].offset == JS_AFFINITY_NEXT_LO(0));
    CHECK(wide.registers.writes[2].value == 0x0000000f);
    CHECK(wide.registers.writes[3].offset == JS_AFFINITY_NEXT_HI(0));
    CHECK(wide.registers.writes[3].value == 3);
}

void testSubmitRefusals()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);

    NativeMaliJobSlot cold;
    CHECK(cold.submit(0, 0x100000, 0, 0) == kIOReturnNotReady);

    rig.registers.writes.clear();
    /* Slot 3 does not exist for this driver (NUM_JOB_SLOTS is 3). */
    CHECK(rig.slot.submit(3, 0x100000, as, 0) == kIOReturnBadArgument);
    /* Slot 32 is off the end of the mask. */
    CHECK(rig.slot.submit(32, 0x100000, as, 0) == kIOReturnBadArgument);
    /* Address space 16 has no room in the low nibble of JS_CONFIG. */
    Rig present(0x7);
    uint32_t unused = 0;
    present.allocateAs(&unused);
    present.registers.writes.clear();
    CHECK(present.slot.submit(0, 0x100000, 16, 0) == kIOReturnBadArgument);
    /* Present in the MMU mask but not allocated. */
    CHECK(rig.slot.submit(0, 0x100000, 1, 0) == kIOReturnBadArgument);
    /* No chain head at all, and a head that is not 64-byte aligned. */
    CHECK(rig.slot.submit(0, 0, as, 0) == kIOReturnBadArgument);
    CHECK(rig.slot.submit(0, 0x100008, as, 0) == kIOReturnBadArgument);
    CHECK(rig.registers.writes.empty());

    /* A slot that is not in GPU_JS_PRESENT. */
    Rig sparse(0x7, 0x5);
    uint32_t sparseAs = 0;
    sparse.allocateAs(&sparseAs);
    sparse.registers.writes.clear();
    CHECK(sparse.slot.submit(0, 0x100000, sparseAs, 0) == kIOReturnSuccess);
    CHECK(sparse.slot.submit(1, 0x100000, sparseAs, 0) ==
        kIOReturnBadArgument);
    CHECK(sparse.slot.submit(2, 0x100000, sparseAs, 0) == kIOReturnSuccess);
    CHECK(sparse.registers.writes.size() == 14);

    /* Another command is staged: the source's WARN_ON, here a refusal. */
    rig.registers.writes.clear();
    rig.registers.memory[JS_COMMAND_NEXT(0) / 4] = JS_COMMAND_START;
    CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnBusy);
    CHECK(rig.registers.writes.empty());
    rig.registers.memory[JS_COMMAND_NEXT(0) / 4] = 0;

    /* The power gate is closed: no read of the slot either. */
    rig.registers.writes.clear();
    rig.registers.readCount = 0;
    rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
    CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnNotReady);
    CHECK(rig.registers.writes.empty());
    CHECK(rig.registers.readCount == 0);
    rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = kPowerOn;

    /* Still usable after every refusal, and the parity did not move. */
    rig.registers.writes.clear();
    CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnSuccess);
    CHECK(rig.registers.writes[4].value == 0x87b00);
}

void testRollback()
{
    {
        /* The flush-id write fails: the staged job is cancelled and the error
         * is returned unchanged. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        rig.registers.writes.clear();
        rig.registers.failWriteAt = JS_FLUSH_ID_NEXT(0);
        CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0x5) ==
            kIOReturnIOError);
        const FakeRegisterBlock::Write expected[] = {
            {JS_HEAD_NEXT_LO(0), 0x00100000},
            {JS_HEAD_NEXT_HI(0), 0},
            {JS_AFFINITY_NEXT_LO(0), 0x1f},
            {JS_AFFINITY_NEXT_HI(0), 0},
            {JS_CONFIG_NEXT(0), 0x87b00},
            {JS_FLUSH_ID_NEXT(0), 0x5},
            {JS_COMMAND_NEXT(0), JS_COMMAND_NOP},
        };
        checkWrites(rig.registers, expected, sizeof(expected) / sizeof(expected[0]));

        /* The chain parity did not advance, so the retry is the same chain. */
        rig.registers.failWriteAt = -1;
        rig.registers.writes.clear();
        CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0x5) ==
            kIOReturnSuccess);
        CHECK(rig.registers.writes[4].value == 0x87b00);
        CHECK(rig.registers.writes[6].value == JS_COMMAND_START);
    }
    {
        /* The first write fails: only it and the cancellation are seen. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        rig.registers.writes.clear();
        rig.registers.failWriteAt = JS_HEAD_NEXT_LO(0);
        CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) ==
            kIOReturnIOError);
        const FakeRegisterBlock::Write expected[] = {
            {JS_HEAD_NEXT_LO(0), 0x00100000},
            {JS_COMMAND_NEXT(0), JS_COMMAND_NOP},
        };
        checkWrites(rig.registers, expected, sizeof(expected) / sizeof(expected[0]));
    }
    {
        /* The START write itself fails; the cancel still goes out. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        rig.registers.writes.clear();
        rig.registers.failWriteAt = JS_COMMAND_NEXT(0);
        rig.registers.memory[JS_COMMAND_NEXT(0) / 4] = 0;
        CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) ==
            kIOReturnIOError);
        /* The failing START is recorded, then the NOP cancel. */
        CHECK(rig.registers.writes.size() == 8);
        CHECK(rig.registers.writes[5].offset == JS_FLUSH_ID_NEXT(0));
        CHECK(rig.registers.writes[6].offset == JS_COMMAND_NEXT(0));
        CHECK(rig.registers.writes[6].value == JS_COMMAND_START);
        CHECK(rig.registers.writes[7].offset == JS_COMMAND_NEXT(0));
        CHECK(rig.registers.writes[7].value == JS_COMMAND_NOP);
    }
}

/* waitForCompletion()'s success path: DONE is BIT(js) of JOB_INT_RAWSTAT and
 * only that bit is cleared (panfrost_jm_handle_irq(), panfrost_job.c:520-545).
 * The fake latches the event on a chosen status read, so the polling itself is
 * part of the test. */
void testCompletionDone()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0x7) == kIOReturnSuccess);

    rig.registers.writes.clear();
    rig.registers.delays.clear();
    rig.registers.doneAfterReads = 3;
    rig.registers.doneWord = JOB_INT_MASK_DONE(0);
    NativeMaliJobCompletion completion;
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnSuccess);
    CHECK(completion.rawStatus == JOB_INT_MASK_DONE(0));
    CHECK(completion.jsStatus == 0);
    CHECK(completion.exception() == 0);
    CHECK(!completion.isFault());
    /* Three polls, two sleeps of the default 10 us. */
    CHECK(rig.registers.delays.size() == 2);
    CHECK(rig.registers.delays[0] == CompletionPollUs);
    CHECK(rig.registers.delays[1] == CompletionPollUs);
    /* One write, this slot's DONE bit. */
    CHECK(rig.registers.writes.size() == 1);
    CHECK(rig.registers.writes[0].offset == JOB_INT_CLEAR);
    CHECK(rig.registers.writes[0].value == JOB_INT_MASK_DONE(0));
    CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
        JOB_INT_MASK_DONE(0)) == 0);
    CHECK(!rig.slot.outstanding(0));
    CHECK(!rig.slot.faulted(0));

    /* Nothing left to reap. */
    rig.registers.readCount = 0;
    rig.registers.writes.clear();
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnNotReady);
    CHECK(completion.rawStatus == 0);
    CHECK(rig.registers.readCount == 0);
    CHECK(rig.registers.writes.empty());

    /* Another slot's event neither ends this wait nor is cleared by it. */
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(1);
    rig.registers.writes.clear();
    NativeMaliJobCompletion timed;
    CHECK(rig.slot.waitForCompletion(0, &timed, 25, 10) == kIOReturnTimeout);
    CHECK(timed.rawStatus == 0);
    CHECK(rig.slot.outstanding(0));
    CHECK(rig.registers.writes.empty());
    CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
        JOB_INT_MASK_DONE(1)) != 0);

    rig.registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(0);
    rig.registers.writes.clear();
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnSuccess);
    CHECK(rig.registers.writes.size() == 1);
    CHECK(rig.registers.writes[0].value == JOB_INT_MASK_DONE(0));
    CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
        JOB_INT_MASK_DONE(1)) != 0);
    CHECK(!rig.slot.outstanding(0));
}

/* Neither bit: a timeout writes nothing, keeps the job outstanding, and can be
 * repeated. A zero budget still reads once. */
void testCompletionTimeout()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    rig.registers.writes.clear();
    rig.registers.delays.clear();

    NativeMaliJobCompletion completion;
    completion.rawStatus = 0xFFFFFFFF;
    completion.jsStatus = 0xFFFFFFFF;
    CHECK(rig.slot.waitForCompletion(0, &completion, 25, 10) ==
        kIOReturnTimeout);
    CHECK(completion.rawStatus == 0);
    CHECK(completion.jsStatus == 0);
    /* Four polls, three sleeps: 10, 20, 30 >= 25 on the fourth read. */
    CHECK(rig.registers.delays.size() == 3);
    CHECK(rig.slot.outstanding(0));
    CHECK(!rig.slot.faulted(0));
    CHECK(rig.registers.writes.empty());

    rig.registers.readCount = 0;
    CHECK(rig.slot.waitForCompletion(0, &completion, 0, 10) ==
        kIOReturnTimeout);
    CHECK(rig.registers.readCount == 1);
    CHECK(rig.registers.delays.size() == 3);

    /* A delay failure is returned unchanged and the job is still outstanding. */
    rig.registers.delayResult = kIOReturnNoResources;
    CHECK(rig.slot.waitForCompletion(0, &completion, 100, 10) ==
        kIOReturnNoResources);
    CHECK(completion.rawStatus == 0);
    CHECK(rig.slot.outstanding(0));
    rig.registers.delayResult = kIOReturnSuccess;

    /* ... and the same slot still completes afterwards. */
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(0);
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnSuccess);
    CHECK(!rig.slot.outstanding(0));
}

/* ERR: the source reads JS_STATUS for the reason, cancels the staged next
 * command and clears the bit. The slot then stays closed until clearFault(). */
void testCompletionError()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_ERR(0);
    rig.registers.memory[JS_STATUS(0) / 4] = 0xC2;
    rig.registers.writes.clear();

    NativeMaliJobCompletion completion;
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
    CHECK(completion.rawStatus == JOB_INT_MASK_ERR(0));
    CHECK(completion.jsStatus == 0xC2);
    CHECK(completion.exception() == 0xC2);
    CHECK(completion.isFault());
    CHECK(std::strcmp(NativeMaliFault::exceptionName(completion.exception()),
        "TRANSLATION_FAULT_2") == 0);
    /* NOP cancel, then the clear of this slot's ERR bit. */
    CHECK(rig.registers.writes.size() == 2);
    CHECK(rig.registers.writes[0].offset == JS_COMMAND_NEXT(0));
    CHECK(rig.registers.writes[0].value == JS_COMMAND_NOP);
    CHECK(rig.registers.writes[1].offset == JOB_INT_CLEAR);
    CHECK(rig.registers.writes[1].value == JOB_INT_MASK_ERR(0));
    CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);
    CHECK(!rig.slot.outstanding(0));
    CHECK(rig.slot.faulted(0));

    /* A faulted slot refuses to start anything, without touching the GPU. */
    rig.registers.writes.clear();
    rig.registers.readCount = 0;
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnNotReady);
    CHECK(rig.registers.writes.empty());
    CHECK(rig.registers.readCount == 0);

    /* clearFault() is pure state: no register, and idempotent. */
    rig.registers.writes.clear();
    rig.registers.readCount = 0;
    CHECK(rig.slot.clearFault(0) == kIOReturnSuccess);
    CHECK(!rig.slot.faulted(0));
    CHECK(rig.slot.clearFault(0) == kIOReturnSuccess);
    CHECK(rig.slot.clearFault(3) == kIOReturnBadArgument);
    CHECK(rig.registers.writes.empty());
    CHECK(rig.registers.readCount == 0);

    /* The failed job consumed its sequence number (the source increments
     * emit_seqno when the fence is created, before the write sequence), so the
     * next submission is even and carries no chain flag. */
    rig.registers.writes.clear();
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    CHECK(rig.registers.writes[4].offset == JS_CONFIG_NEXT(0));
    CHECK(rig.registers.writes[4].value == 0x87300);

    /* DONE and ERR in the same word: the error wins and both bits are cleared.
     * 0x04 is TERMINATED, a non-fault exception code (panfrost_device.h:262-268),
     * so nothing is latched as a fault. */
    rig.registers.memory[JOB_INT_RAWSTAT / 4] =
        JOB_INT_MASK_DONE(0) | JOB_INT_MASK_ERR(0);
    rig.registers.memory[JS_STATUS(0) / 4] = 0x04;
    rig.registers.writes.clear();
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
    CHECK(completion.rawStatus ==
        (JOB_INT_MASK_DONE(0) | JOB_INT_MASK_ERR(0)));
    CHECK(completion.jsStatus == 0x04);
    CHECK(!completion.isFault());
    CHECK(rig.registers.writes.size() == 2);
    CHECK(rig.registers.writes[1].value ==
        (JOB_INT_MASK_DONE(0) | JOB_INT_MASK_ERR(0)));
    CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);
    CHECK(rig.slot.faulted(0));
    CHECK(rig.slot.clearFault(0) == kIOReturnSuccess);
}

/* A latched DONE from before this job (a GPU reset can leave one behind;
 * panfrost_jm_reset_interrupts() clears every job event for that reason) must
 * not be mistaken for this job's completion, and another slot's event must be
 * left alone. */
void testStaleEvent()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    rig.registers.memory[JOB_INT_RAWSTAT / 4] =
        JOB_INT_MASK_DONE(0) | JOB_INT_MASK_ERR(0);
    rig.registers.writes.clear();

    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    /* One extra write, ahead of the head: exactly this slot's two bits. */
    CHECK(rig.registers.writes.size() == 8);
    CHECK(rig.registers.writes[0].offset == JOB_INT_CLEAR);
    CHECK(rig.registers.writes[0].value ==
        (JOB_INT_MASK_DONE(0) | JOB_INT_MASK_ERR(0)));
    CHECK(rig.registers.writes[1].offset == JS_HEAD_NEXT_LO(0));
    CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);

    /* The cleared event is not a completion: the wait times out. */
    rig.registers.writes.clear();
    NativeMaliJobCompletion completion;
    CHECK(rig.slot.waitForCompletion(0, &completion, 15, 10) ==
        kIOReturnTimeout);
    CHECK(rig.slot.outstanding(0));
    CHECK(!rig.slot.faulted(0));
    CHECK(rig.registers.writes.empty());

    /* Slot 1's latched event is none of slot 0's business. */
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(1);
    rig.registers.writes.clear();
    CHECK(rig.slot.waitForCompletion(0, &completion, 15, 10) ==
        kIOReturnTimeout);
    CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
        JOB_INT_MASK_DONE(1)) != 0);
    CHECK(rig.registers.writes.empty());
}

void testCompletionRefusals()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);
    NativeMaliJobCompletion completion;
    completion.rawStatus = 0xFFFFFFFF;

    /* No completion pointer, no wait. */
    CHECK(rig.slot.waitForCompletion(0, nullptr) == kIOReturnBadArgument);
    /* Not prepared. */
    NativeMaliJobSlot cold;
    CHECK(cold.waitForCompletion(0, &completion) == kIOReturnNotReady);
    /* Slot 3 does not exist, slot 32 is off the mask, and a slot that is not in
     * GPU_JS_PRESENT is refused even though it exists. */
    Rig sparse(0x7, 0x5);
    uint32_t sparseAs = 0;
    sparse.allocateAs(&sparseAs);
    CHECK(sparse.slot.submit(0, 0x100000, sparseAs, 0) == kIOReturnSuccess);
    sparse.registers.writes.clear();
    sparse.registers.readCount = 0;
    CHECK(sparse.slot.waitForCompletion(3, &completion) == kIOReturnBadArgument);
    CHECK(sparse.slot.waitForCompletion(32, &completion) ==
        kIOReturnBadArgument);
    CHECK(sparse.slot.waitForCompletion(1, &completion) ==
        kIOReturnBadArgument);
    CHECK(sparse.registers.readCount == 0);
    CHECK(sparse.registers.writes.empty());

    /* Nothing submitted on slot 1: refused without touching the GPU. */
    rig.registers.readCount = 0;
    CHECK(rig.slot.waitForCompletion(1, &completion) == kIOReturnNotReady);
    CHECK(completion.rawStatus == 0);
    CHECK(rig.registers.readCount == 0);

    /* The power gate wins over the outstanding job, and gives up no read. */
    CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnSuccess);
    rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = 0;
    rig.registers.readCount = 0;
    rig.registers.writes.clear();
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnNotReady);
    CHECK(rig.registers.readCount == 0);
    CHECK(rig.registers.writes.empty());
    CHECK(rig.slot.outstanding(0));
    rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] = kPowerOn;

    /* Still waiting for that same job once the gate is back. */
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(0);
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnSuccess);
    CHECK(!rig.slot.outstanding(0));
}

/* A failed teardown write is not hidden: the call reports it, the job stays
 * outstanding (or unfaulted), and the retry repeats the same steps. */
void testCompletionRollback()
{
    {
        /* Clearing DONE fails. The caller learns what the hardware said and
         * that the job is not reaped yet. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnSuccess);
        rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(0);
        rig.registers.writes.clear();
        rig.registers.failWriteAt = JOB_INT_CLEAR;
        NativeMaliJobCompletion completion;
        CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
        CHECK(completion.rawStatus == JOB_INT_MASK_DONE(0));
        CHECK(rig.slot.outstanding(0));
        CHECK(!rig.slot.faulted(0));
        CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
            JOB_INT_MASK_DONE(0)) != 0);

        rig.registers.failWriteAt = -1;
        CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnSuccess);
        CHECK(!rig.slot.outstanding(0));
        CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);
    }
    {
        /* The NOP cancel fails after ERR: the reason is still reported, the slot
         * is not latched as faulted, and the ERR bit is still set for the
         * retry. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnSuccess);
        rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_ERR(0);
        rig.registers.memory[JS_STATUS(0) / 4] = 0x42;
        rig.registers.failWriteAt = JS_COMMAND_NEXT(0);
        NativeMaliJobCompletion completion;
        CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
        CHECK(completion.jsStatus == 0x42);
        CHECK(rig.slot.outstanding(0));
        CHECK(!rig.slot.faulted(0));
        CHECK((rig.registers.memory[JOB_INT_RAWSTAT / 4] &
            JOB_INT_MASK_ERR(0)) != 0);

        rig.registers.failWriteAt = -1;
        CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
        CHECK(completion.jsStatus == 0x42);
        CHECK(!rig.slot.outstanding(0));
        CHECK(rig.slot.faulted(0));
        CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);
        /* clearFault() refuses while a job is outstanding; here it is not. */
        CHECK(rig.slot.clearFault(0) == kIOReturnSuccess);
    }
    {
        /* clearFault() refuses while a job is still in flight, and the latch is
         * untouched either way. */
        Rig rig;
        uint32_t as = 0;
        rig.allocateAs(&as);
        CHECK(rig.slot.submit(0, 0x100000, as, 0) == kIOReturnSuccess);
        CHECK(!rig.slot.faulted(0));
        CHECK(rig.slot.clearFault(0) == kIOReturnBusy);
        CHECK(!rig.slot.faulted(0));
        rig.finish(0);
        CHECK(rig.slot.clearFault(0) == kIOReturnSuccess);
    }
}

/* Software fences: the per-slot seqno panfrost puts in fence->seqno
 * (panfrost_job.c:106). A fence only answers "is that job still in flight";
 * whether it succeeded is in the completion the reaper got. */
void testFences()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);

    NativeMaliFence fence{9, 9};
    CHECK(rig.slot.submit(0, 0x100000, as, 0, &fence) == kIOReturnSuccess);
    CHECK(fence.valid());
    CHECK(fence.js == 0);
    CHECK(fence.seqno == 1);              /* panfrost's first job is 1 */
    CHECK(rig.slot.latestSeqno(0) == 1);
    CHECK(rig.slot.completedSeqno(0) == 0);
    CHECK(!rig.slot.fenceSignaled(fence));

    rig.finish(0);
    CHECK(rig.slot.completedSeqno(0) == 1);
    CHECK(rig.slot.fenceSignaled(fence));

    /* The next job on the slot is a new fence; the old one stays signaled. */
    NativeMaliFence second;
    CHECK(rig.slot.submit(0, 0x100000, as, 0, &second) == kIOReturnSuccess);
    CHECK(second.seqno == 2);
    CHECK(rig.slot.fenceSignaled(fence));
    CHECK(!rig.slot.fenceSignaled(second));
    CHECK(rig.slot.completedSeqno(0) == 1);
    rig.finish(0);
    CHECK(rig.slot.fenceSignaled(second));
    CHECK(rig.slot.completedSeqno(0) == 2);

    /* Slots count independently. */
    CHECK(rig.slot.latestSeqno(1) == 0);
    CHECK(rig.slot.completedSeqno(1) == 0);

    /* A refusal zeroes the out parameter, so a caller cannot mistake the fence
     * it passed in for the job that did not start. */
    NativeMaliFence refused;
    CHECK(rig.slot.submit(0, 0x100000, as, 0, &refused) == kIOReturnSuccess);
    NativeMaliFence busy{4, 4};
    CHECK(rig.slot.submit(0, 0x100000, as, 0, &busy) == kIOReturnBusy);
    CHECK(!busy.valid());

    /* Nothing is signaled while the job is still outstanding. */
    CHECK(!rig.slot.fenceSignaled(refused));
    rig.finish(0);
    CHECK(rig.slot.fenceSignaled(refused));

    /* A faulted job reaps its fence too: the wait is over, the reason is in
     * the completion. */
    NativeMaliFence faulted;
    CHECK(rig.slot.submit(1, 0x100000, as, 0, &faulted) == kIOReturnSuccess);
    CHECK(faulted.seqno == 1);
    rig.registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_ERR(1);
    rig.registers.memory[JS_STATUS(1) / 4] = 0xC2;
    NativeMaliJobCompletion completion;
    CHECK(rig.slot.waitForCompletion(1, &completion) == kIOReturnIOError);
    CHECK(completion.isFault());
    CHECK(rig.slot.fenceSignaled(faulted));
    CHECK(rig.slot.completedSeqno(1) == 1);

    /* A timeout is not a completion: the fence stays unsignaled, and the retry
     * that reaps the job then signals it. */
    NativeMaliFence timed;
    CHECK(rig.slot.submit(2, 0x100000, as, 0, &timed) == kIOReturnSuccess);
    NativeMaliJobCompletion late;
    CHECK(rig.slot.waitForCompletion(2, &late, 30, 10) == kIOReturnTimeout);
    CHECK(!rig.slot.fenceSignaled(timed));
    CHECK(rig.slot.completedSeqno(2) == 0);
    rig.finish(2);
    CHECK(rig.slot.fenceSignaled(timed));
    CHECK(rig.slot.completedSeqno(2) == 1);

    /* clearFault() does not rewind the seqno: panfrost never resets
     * emit_seqno either, and the number is only ever compared. */
    CHECK(rig.slot.clearFault(1) == kIOReturnSuccess);
    CHECK(rig.slot.latestSeqno(1) == 1);
    NativeMaliFence afterFault;
    CHECK(rig.slot.submit(1, 0x100000, as, 0, &afterFault) == kIOReturnSuccess);
    CHECK(afterFault.seqno == 2);
    CHECK(rig.slot.fenceSignaled(faulted));
    CHECK(!rig.slot.fenceSignaled(afterFault));

    /* Invalid fences are never signaled, including one "from the future" and
     * one for a slot this driver does not use. */
    CHECK(!rig.slot.fenceSignaled(NativeMaliFence{}));
    CHECK(!rig.slot.fenceSignaled(NativeMaliFence{0, 0}));
    /* One past what was ever submitted on slot 0 is not a fence this driver
     * handed out, so it must not read as signaled even though the slot has
     * reaped earlier jobs. */
    CHECK(!rig.slot.fenceSignaled(
        NativeMaliFence{0, rig.slot.latestSeqno(0) + 1}));
    CHECK(!rig.slot.fenceSignaled(NativeMaliFence{NativeMaliJobSlotCount, 1}));
    CHECK(rig.slot.latestSeqno(NativeMaliJobSlotCount) == 0);
    CHECK(rig.slot.completedSeqno(NativeMaliJobSlotCount) == 0);
}

/* The job-manager half of panfrost_reset() (panfrost_job.c:697-709): every
 * present slot gets JS_COMMAND_NEXT = NOP then JS_COMMAND = SOFT_STOP, then
 * JOB_INT_JS_STATE is polled every 10 us for at most 10 ms. */
void testQuiesce()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);

    /* Never prepared: nothing to quiesce and no write. A prepared slot that was
     * never told GPU_JS_PRESENT is refused the same way. */
    {
        NativeMaliJobSlot cold;
        CHECK(cold.quiesce() == kIOReturnNotReady);
        CHECK(cold.prepare(&rig.registers, &rig.power, &rig.mmu) ==
            kIOReturnSuccess);
        rig.registers.writes.clear();
        CHECK(cold.quiesce() == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
    }

    /* An idle job manager: NOP then SOFT_STOP for each present slot, in slot
     * order, and no poll delay because JS_STATE is already clear. */
    const uint32_t expected[] = {
        JS_COMMAND_NEXT(0), JS_COMMAND(0),
        JS_COMMAND_NEXT(1), JS_COMMAND(1),
        JS_COMMAND_NEXT(2), JS_COMMAND(2),
    };
    const uint32_t expectedValues[] = {
        JS_COMMAND_NOP, JS_COMMAND_SOFT_STOP,
        JS_COMMAND_NOP, JS_COMMAND_SOFT_STOP,
        JS_COMMAND_NOP, JS_COMMAND_SOFT_STOP,
    };
    rig.registers.writes.clear();
    rig.registers.delays.clear();
    CHECK(rig.slot.quiesce() == kIOReturnSuccess);
    CHECK(rig.registers.writes.size() == 6);
    for(size_t i = 0; i < 6; ++i) {
        CHECK(rig.registers.writes[i].offset == expected[i]);
        CHECK(rig.registers.writes[i].value == expectedValues[i]);
    }
    CHECK(rig.registers.delays.empty());

    /* Only GPU_JS_PRESENT slots are written: slot 1 does not exist here. */
    {
        Rig partial(0x7, 0x5);
        partial.allocateAs(&as);
        partial.registers.writes.clear();
        CHECK(partial.slot.quiesce() == kIOReturnSuccess);
        CHECK(partial.registers.writes.size() == 4);
        CHECK(partial.registers.writes[0].offset == JS_COMMAND_NEXT(0));
        CHECK(partial.registers.writes[1].offset == JS_COMMAND(0));
        CHECK(partial.registers.writes[2].offset == JS_COMMAND_NEXT(2));
        CHECK(partial.registers.writes[3].offset == JS_COMMAND(2));
    }

    /* A running chain: START set the slot's JS_STATE bit, SOFT_STOP clears it,
     * so the first poll already succeeds. */
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    CHECK((rig.registers.memory[JOB_INT_JS_STATE / 4] & 0x1) == 0x1);
    rig.registers.writes.clear();
    rig.registers.delays.clear();
    CHECK(rig.slot.quiesce() == kIOReturnSuccess);
    CHECK((rig.registers.memory[JOB_INT_JS_STATE / 4] & 0x1) == 0);
    CHECK(rig.registers.delays.empty());

    /* A soft-stop that does not take effect is still retired by a DONE event:
     * panfrost_active_slots() masks the slot out for the rest of the wait
     * (panfrost_job.c:642-655). */
    CHECK(rig.slot.submit(1, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    rig.registers.softStopStuck = true;
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_DONE(1);
    rig.registers.writes.clear();
    rig.registers.delays.clear();
    CHECK(rig.slot.quiesce() == kIOReturnSuccess);
    CHECK((rig.registers.memory[JOB_INT_JS_STATE / 4] & 0x2) == 0x2);
    CHECK(rig.registers.delays.empty());

    /* A stuck slot without an event runs the whole budget: 10 ms in 10 us
     * steps, then kIOReturnTimeout. The source logs that and resets anyway. */
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = 0;
    rig.registers.delays.clear();
    CHECK(rig.slot.quiesce() == kIOReturnTimeout);
    CHECK(rig.registers.delays.size() == SoftStopTimeoutUs / SoftStopPollUs);
    for(uint64_t delay : rig.registers.delays)
        CHECK(delay == SoftStopPollUs);
    CHECK((rig.registers.memory[JOB_INT_JS_STATE / 4] & 0x2) == 0x2);
    rig.registers.softStopStuck = false;

    /* A failed write or a failed JS_STATE read is returned unchanged, and a GPU
     * that is not readable is refused before the first write. */
    rig.registers.failWriteAt = JS_COMMAND(1);
    CHECK(rig.slot.quiesce() == kIOReturnIOError);
    rig.registers.failWriteAt = -1;
    rig.registers.failReadAt = JOB_INT_JS_STATE;
    CHECK(rig.slot.quiesce() == kIOReturnIOError);
    rig.registers.failReadAt = -1;
    {
        const uint32_t control = NativeMaliPower::SpmMfgCore0PowerCon;
        rig.spm.memory[control / 4] = 0;
        rig.registers.writes.clear();
        CHECK(rig.slot.quiesce() == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
        rig.spm.memory[control / 4] = kPowerOn;
    }
}

/* A GPU reset: panfrost_jm_reset_interrupts() clears every job event in one
 * write, and the scheduler's part (stop every queue, signal the in-flight
 * fences with an error) has no counterpart here, so abandonOutstanding() drops
 * the slot state and keeps the seqnos. */
void testReset()
{
    Rig rig;
    uint32_t as = 0;
    rig.allocateAs(&as);

    /* A slot that was never prepared, or prepared but never told which slots
     * exist, has nothing to clear and must not write a job register. */
    {
        NativeMaliJobSlot cold;
        CHECK(cold.resetInterrupts() == kIOReturnNotReady);
        CHECK(cold.prepare(&rig.registers, &rig.power, &rig.mmu) ==
            kIOReturnSuccess);
        rig.registers.writes.clear();
        CHECK(cold.resetInterrupts() == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
    }

    /* One write clears every DONE and ERR bit of the three slots. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js)
        rig.registers.memory[JOB_INT_RAWSTAT / 4] |=
            JOB_INT_MASK_DONE(js) | JOB_INT_MASK_ERR(js);
    rig.registers.writes.clear();
    CHECK(NativeMaliAllJobInterrupts == 0x70007);
    CHECK(rig.slot.resetInterrupts() == kIOReturnSuccess);
    CHECK(rig.registers.writes.size() == 1);
    CHECK(rig.registers.writes[0].offset == JOB_INT_CLEAR);
    CHECK(rig.registers.writes[0].value == NativeMaliAllJobInterrupts);
    CHECK(rig.registers.memory[JOB_INT_RAWSTAT / 4] == 0);

    /* A failed write is returned unchanged; a gate failure writes nothing. */
    rig.registers.failWriteAt = JOB_INT_CLEAR;
    CHECK(rig.slot.resetInterrupts() == kIOReturnIOError);
    rig.registers.failWriteAt = -1;
    {
        const uint32_t control = NativeMaliPower::SpmMfgCore0PowerCon;
        rig.spm.memory[control / 4] = 0;
        rig.registers.writes.clear();
        CHECK(rig.slot.resetInterrupts() == kIOReturnNotReady);
        CHECK(rig.registers.writes.empty());
        rig.spm.memory[control / 4] = kPowerOn;
    }

    /* One slot faulted after ERR, two in flight. */
    NativeMaliJobCompletion completion;
    CHECK(rig.slot.submit(0, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    rig.registers.memory[JOB_INT_RAWSTAT / 4] = JOB_INT_MASK_ERR(0);
    rig.registers.memory[JS_STATUS(0) / 4] = 0x40;
    CHECK(rig.slot.waitForCompletion(0, &completion) == kIOReturnIOError);
    CHECK(rig.slot.faulted(0));
    CHECK(rig.slot.submit(1, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    CHECK(rig.slot.submit(2, UINT64_C(0x100000), as, 0) == kIOReturnSuccess);
    CHECK(rig.slot.outstanding(1) && rig.slot.outstanding(2));
    const uint32_t latest0 = rig.slot.latestSeqno(0);
    const uint32_t completed0 = rig.slot.completedSeqno(0);
    const uint32_t latest1 = rig.slot.latestSeqno(1);
    const uint32_t latest2 = rig.slot.latestSeqno(2);

    /* Pure state drop: no register, no read, every slot available again. The
     * fault latch goes, the seqnos do not. */
    rig.registers.writes.clear();
    const uint32_t reads = rig.registers.readCount;
    rig.slot.abandonOutstanding();
    CHECK(!rig.slot.outstanding(0) && !rig.slot.outstanding(1) &&
        !rig.slot.outstanding(2));
    CHECK(!rig.slot.faulted(0));
    CHECK(rig.slot.latestSeqno(0) == latest0);
    CHECK(rig.slot.completedSeqno(0) == completed0);
    CHECK(rig.slot.latestSeqno(1) == latest1);
    CHECK(rig.slot.latestSeqno(2) == latest2);
    /* The faulted job was reaped and stays signaled; the two destroyed ones
     * were never reaped and never become signaled. */
    CHECK(rig.slot.fenceSignaled(NativeMaliFence{0, latest0}));
    CHECK(!rig.slot.fenceSignaled(NativeMaliFence{1, latest1}));
    CHECK(!rig.slot.fenceSignaled(NativeMaliFence{2, latest2}));
    CHECK(rig.registers.writes.empty());
    CHECK(rig.registers.readCount == reads);

    /* Idempotent. */
    rig.slot.abandonOutstanding();
    CHECK(rig.registers.writes.empty());

    /* Every slot accepts a job again, and the seqno continues instead of
     * rewinding. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js) {
        CHECK(rig.slot.submit(js, UINT64_C(0x100000), as, 0) ==
            kIOReturnSuccess);
        CHECK(rig.slot.latestSeqno(js) == 2);
        CHECK(rig.slot.outstanding(js));
        rig.registers.memory[JOB_INT_RAWSTAT / 4] |= JOB_INT_MASK_DONE(js);
        CHECK(rig.slot.waitForCompletion(js, &completion) == kIOReturnSuccess);
    }
}

} // namespace

int main()
{
    testPrepareAndPresent();
    testConfigValue();
    testSubmitGolden();
    testSubmitRefusals();
    testRollback();
    testCompletionDone();
    testCompletionTimeout();
    testCompletionError();
    testStaleEvent();
    testCompletionRefusals();
    testCompletionRollback();
    testFences();
    testQuiesce();
    testReset();
    std::puts("PASS: JS slot submit order and golden JS_CONFIG, per-slot chain-flag "
        "parity, affinity and head LO/HI words, present/allocated/alignment "
        "refusals, the staged-command Busy case, the NOP rollback, polled "
        "DONE/ERR completion with per-slot clearing, timeouts that keep the job "
        "outstanding, the stale-event clear at submit, the fault latch, "
        "software fence seqnos that signal only on a reap, the reset palette clear "
        "and the pure slot-state drop, the NOP/SOFT_STOP quiesce with its "
        "JS_STATE poll and timeout, and every teardown rollback");
    return 0;
}
